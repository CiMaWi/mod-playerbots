/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "ArenaFocusTarget.h"

#include "AiObjectContext.h"
#include "Battleground.h"
#include "BotTactics.h"
#include "BotTacticsInternal.h"
#include "CcState.h"
#include "FocusState.h"
#include "Group.h"
#include "Log.h"
#include "NaturalReaction.h"
#include "PerfMonitor.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "PvpTestCounters.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "TacticIds.h"

namespace
{
// G4e: an enemy the bot has seen heal (a heal bar now, or a heal among its last casts in the opponent memory; DT.12)
bool FocusSeenHealing(PlayerbotAI* botAI, Unit* unit)
{
    InterruptBar bar;
    if (ReadInterruptBar(unit, bar) &&
        ArenaInterrupt::Classify(InterruptSpellFactsOf(bar.Info)) == ArenaInterrupt::Kind::Heal)
        return true;

    auto const healSpell = [](uint32 spellId)
    {
        return spellId != 0 && ArenaInterrupt::Classify(InterruptSpellFactsOf(sSpellMgr->GetSpellInfo(spellId))) ==
                                   ArenaInterrupt::Kind::Heal;
    };
    OpponentMemorySlot const* const slot = botAI->GetTactics().GetOpponentMemory().Find(unit->GetGUID().GetCounter());
    return slot && (healSpell(slot->SpellId) || healSpell(slot->PrevSpellId));
}

// A focus candidate (G4e is PvP: players only, no creature): in the bot's map, alive, attackable and not its own target
bool FocusCandidate(Player* bot, Unit* unit, ObjectGuid own)
{
    return unit && unit->IsPlayer() && unit->IsInWorld() && unit->IsInMap(bot) && unit->IsAlive() &&
           unit->GetGUID() != own && bot->IsValidAttackTarget(unit);
}
}  // namespace

// G4e: the focus target at the bot's checks while the FocusTarget tactic is on here (or a focus is left): cleared at
// a new match and while the tactic or the Dexterity gate fails; picked again at once when the moon, the own target or
// the CC target changes or the focus is gone, else every FOCUS_TARGET_PICK_MS (the healer scan: the fight's enemies,
// at most TACTIC_FIGHT_ENEMIES_MAX units)
void FollowFocusTarget(PlayerbotAI* botAI, Player* bot, uint32 nowMs)
{
    // Every call (the change check too; the picks are tactics show's `picks` and the `focus pick` lines)
    PerfMonitorScope scope(PerfMonitor::IsEnabled() ? sPerfMonitor.acquire(PERF_MON_TOTAL, "FocusTarget::Pick")
                                                    : nullptr);
    FocusTargetState& focus = botAI->GetTactics().GetFocusState().Target;
    Battleground* const battleground = bot->GetBattleground();
    uint32 const matchId =
        battleground && battleground->GetStatus() == STATUS_IN_PROGRESS ? battleground->GetInstanceID() : 0;
    if (matchId != focus.Pick.MatchId)
    {
        focus.Pick.StartMatch(matchId);
        focus.NextPickMs = nowMs;
    }

    PlayerbotAIConfig const& config = sPlayerbotAIConfig;
    uint8 const skill = botAI->GetPvpSkill();
    if (!botAI->GetTactics().TacticActive(TacticId::FocusTarget) ||
        !FocusTargetCapable(skill, botAI->GetTactics().GetAttribute(BotAttribute::Dexterity),
                            config.NaturalReaction.Ease.FocusDexterity))
    {
        focus.Pick.Set(0, FocusSource::None, nowMs);
        return;
    }

    Group* const group = bot->GetGroup();
    ObjectGuid const moon = group && botAI->GetTacticContext() == TacticContext::Arena
                                ? group->GetTargetIcon(FOCUS_ICON_MOON)
                                : ObjectGuid::Empty;
    Unit* const current = botAI->GetAiObjectContext()->GetValue<Unit*>("current target")->Get();
    ObjectGuid const own = current ? current->GetGUID() : ObjectGuid::Empty;
    CcBotState const& cc = botAI->GetTactics().GetCcState();
    ObjectGuid const ccTarget = FocusBefore(nowMs, cc.CcTargetUntilMs) ? cc.CcTarget : ObjectGuid::Empty;
    Unit* const kept = focus.Pick.Guid ? botAI->GetUnit(ObjectGuid(focus.Pick.Guid)) : nullptr;
    if (FocusBefore(nowMs, focus.NextPickMs) && moon == focus.SeenMoon && own == focus.SeenOwn &&
        ccTarget == focus.SeenCc && (!focus.Pick.Guid || FocusCandidate(bot, kept, own)))
        return;

    focus.NextPickMs = nowMs + FOCUS_TARGET_PICK_MS;
    focus.SeenMoon = moon;
    focus.SeenOwn = own;
    focus.SeenCc = ccTarget;

    // The healer: the kept focus while it still is one, else the first healer-class player of the fight seen healing
    // (once seen, for the rest of the match)
    Unit* healer = nullptr;
    for (ObjectGuid const& guid : botAI->GetAiObjectContext()->GetValue<GuidVector>("fight enemies")->Get())
    {
        Unit* const unit = botAI->GetUnit(guid);
        if (!FocusCandidate(bot, unit, own) || !InterruptHealerClass(unit->getClass()))
            continue;

        if (!focus.Pick.Healers.Has(guid.GetRawValue()))
        {
            if (!FocusSeenHealing(botAI, unit))
                continue;

            focus.Pick.Healers.Add(guid.GetRawValue());
        }

        if (!healer || unit == kept)
            healer = unit;

        if (unit == kept)
            break;
    }

    Unit* const moonUnit = moon.IsEmpty() ? nullptr : botAI->GetUnit(moon);
    Unit* const ccUnit = ccTarget.IsEmpty() ? nullptr : botAI->GetUnit(ccTarget);
    FocusPickInput input;
    input.Moon = FocusCandidate(bot, moonUnit, own);
    input.Healer = healer != nullptr;
    input.Cc = FocusCandidate(bot, ccUnit, own);
    if (input.Moon && input.Healer)
    {
        // Teamwork: the plan's moon over the healer, one draw per match and moon (skill 0: Teamwork 99)
        uint8 const teamwork = skill == 0 ? 99 : botAI->GetTactics().GetAttribute(BotAttribute::Teamwork);
        uint64 const hash =
            SplitMix64(bot->GetGUID().GetCounter()) ^ (static_cast<uint64>(matchId) << 32) ^ moon.GetCounter();
        input.TakesMoon = FocusTakesMoon(FocusMoonShare(teamwork, config.TacticFocus.FocusTargetMoonPctAtTeamwork1,
                                                        config.TacticFocus.FocusTargetMoonPctAtTeamwork99),
                                         hash);
    }

    FocusSource const source = PickFocusSource(input);
    Unit* pick = nullptr;
    if (source == FocusSource::Moon)
        pick = moonUnit;
    else if (source == FocusSource::Healer)
        pick = healer;
    else if (source == FocusSource::Cc)
        pick = ccUnit;

    if (focus.Pick.Set(pick ? pick->GetGUID().GetRawValue() : 0, source, nowMs))
        LOG_TRACE("playerbots.reaction", "focus pick skill {} source {} enemy {} own {} moon {} at {}", skill,
                  FocusSourceName(source), pick ? pick->GetGUID().GetCounter() : 0, own.GetCounter(), moon.GetCounter(),
                  nowMs);
}

// G4e: a kick or a CC on an enemy other than the bot's target: on its focus target (tactics.focusKick, focusCc) or a
// switch kick (tactics.switchKick; any bot, so the arm without the tactic counts too); counted while the harness
// counts, kept for tactics show, one trace line each (the own target and the selection before and after the cast; the
// class by the own target before it)
void NoteFocusAct(PlayerbotAI* botAI, Player* bot, SpellInfo const* spellInfo, Unit* target, ObjectGuid selectionBefore,
                  ObjectGuid ownBefore, bool counting, uint32 drGroup)
{
    if (!bot->IsHostileTo(target))
        return;

    bool const kick = InterruptUseOf(spellInfo) == InterruptUse::Kick;
    bool const cc = !kick && IsPvpTestCcGroup(counting ? drGroup : GetPvpTestCastGroup(spellInfo));
    if (!kick && !cc)
        return;

    Unit* const current = botAI->GetAiObjectContext()->GetValue<Unit*>("current target")->Get();
    ObjectGuid const ownAfter = current ? current->GetGUID() : ObjectGuid::Empty;
    FocusTargetState& focus = botAI->GetTactics().GetFocusState().Target;
    FocusAct const act = ClassifyFocusAct(target->GetGUID().GetRawValue(), ownBefore.GetRawValue(), focus.Pick.Guid);
    if (act == FocusAct::None || (act == FocusAct::Switch && !kick))
        return;

    if (act == FocusAct::Focus)
    {
        ++(kick ? focus.Kicks : focus.Ccs);
        if (counting)
            botAI->CountTactic(kick ? TacticCounter::FocusKick : TacticCounter::FocusCc);
    }
    else
    {
        ++focus.SwitchKicks;
        if (counting)
            botAI->CountTactic(TacticCounter::SwitchKick);
    }

    LOG_TRACE("playerbots.reaction",
              "focus act {} kind {} skill {} enemy {} spell {} source {} own before {} after {} sel before {} after {} "
              "at {}",
              act == FocusAct::Focus ? "focus" : "switch", kick ? "kick" : "cc", botAI->GetPvpSkill(),
              target->GetGUID().GetCounter(), spellInfo->Id,
              act == FocusAct::Focus ? FocusSourceName(focus.Pick.Source) : "-", ownBefore.GetCounter(),
              ownAfter.GetCounter(), selectionBefore.GetCounter(), bot->GetTarget().GetCounter(), getMSTime());
}
