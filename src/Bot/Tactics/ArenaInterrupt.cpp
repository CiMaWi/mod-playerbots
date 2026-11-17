/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "ArenaInterrupt.h"

#include <algorithm>
#include <array>

#include "AiFactory.h"
#include "AiObjectContext.h"
#include "ArenaInterruptConfig.h"
#include "Battleground.h"
#include "BotTactics.h"
#include "BotTacticsInternal.h"
#include "FocusState.h"
#include "Log.h"
#include "NaturalReaction.h"
#include "PerfMonitor.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "PvpTestCounters.h"
#include "SkillStrategy.h"
#include "SpellAuraEffects.h"
#include "SpellAuras.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "TacticIds.h"

namespace
{
uint8 InterruptHpPct(Unit const* unit)
{
    return static_cast<uint8>(std::clamp(static_cast<int32>(unit->GetHealthPct() + 0.5f), 0, 100));
}
}  // namespace

// The end of a hold episode: tactics.intHoldPaid and intHoldVain (harness matches only; count false for the match's
// end, which may come after the harness took the record) and its trace line
void NoteInterruptHoldEnd(PlayerbotAI* botAI, InterruptHoldEnd end, uint32 heldMs, char const* cause, uint32 nowMs,
                          bool count = true)
{
    if (end == InterruptHoldEnd::None)
        return;

    if (count && end == InterruptHoldEnd::Paid)
        botAI->CountTactic(TacticCounter::IntHoldPaid);
    else if (count && end == InterruptHoldEnd::Vain)
        botAI->CountTactic(TacticCounter::IntHoldVain);

    LOG_TRACE("playerbots.reaction", "interrupt hold end {} by {} skill {} held {} at {}", InterruptHoldEndName(end),
              cause, botAI->GetPvpSkill(), heldMs, nowMs);
}

namespace
{
// The bot's spec for its warrior kicks (InterruptKickFacts)
InterruptWarriorSpec InterruptWarriorSpecOf(Player* bot)
{
    if (bot->getClass() != CLASS_WARRIOR)
        return InterruptWarriorSpec::None;

    switch (AiFactory::GetPlayerSpecTab(bot))
    {
        case WARRIOR_TAB_ARMS:
            return InterruptWarriorSpec::Arms;
        case WARRIOR_TAB_FURY:
            return InterruptWarriorSpec::Fury;
        default:
            return InterruptWarriorSpec::Protection;
    }
}

// A new match (or the bot's first fight): the open episode ends (not counted), the kick spells come from the bot's
// spell book (once per match, with the gear worn at the start), ready as their cooldowns say: only kicks the bot casts
// at an enemy (CastsKickSpell: aimed at one enemy, the item requirement met, the warrior spec's kick)
void StartInterruptMatch(PlayerbotAI* botAI, Player* bot, InterruptBotState& state, uint32 matchId, uint32 nowMs)
{
    uint32 const heldMs = state.Hold.HeldMs(nowMs);
    NoteInterruptHoldEnd(botAI, state.StartMatch(matchId, nowMs), heldMs, "match", nowMs, false);
    uint8 const spec = bot->GetActiveSpec();
    InterruptKickFacts facts;
    facts.Warrior = InterruptWarriorSpecOf(bot);
    facts.ArmsSwap = facts.Warrior == InterruptWarriorSpec::Arms &&
                     sPlayerbotAIConfig.TacticInterrupt.ArmsPummel != 0 &&
                     botAI->GetAiObjectContext()->GetValue<uint32>("spell id", "berserker stance")->Get() != 0;
    for (auto const& [spellId, spell] : bot->GetSpellMap())
    {
        if (!spell || spell->State == PLAYERSPELL_REMOVED || !spell->Active || !spell->IsInSpec(spec))
            continue;

        SpellInfo const* const info = sSpellMgr->GetSpellInfo(spellId);
        if (!info || info->IsPassive() || InterruptUseOf(info) != InterruptUse::Kick)
            continue;

        facts.Aimed = InterruptKickAimed(info);
        facts.ItemFits = bot->HasItemFitToSpellRequirements(info);
        facts.NeedsShield = info->EquippedItemClass == ITEM_CLASS_ARMOR &&
                            (info->EquippedItemSubClassMask & (1 << ITEM_SUBCLASS_ARMOR_SHIELD)) != 0;
        if (CastsKickSpell(facts))
            state.AddKickSpell(spellId, nowMs + bot->GetSpellCooldownDelay(spellId));
    }
}
}  // namespace

// G5c-c: at the bot's checks while the Interrupt tactic follows (InterruptFollows): the match (StartInterruptMatch at
// a new one) and, under HoldPolicy 1 in a fight, every HOLD_TREND_SAMPLE_MS the enemy players' health bars and their
// cast heal bars (at most HOLD_TREND_UNITS units, one Classify per cast)
void FollowInterruptFight(PlayerbotAI* botAI, Player* bot, uint32 nowMs)
{
    InterruptConfig const& config = sPlayerbotAIConfig.TacticInterrupt;
    if (!botAI->TacticEnabledHere(TacticId::Interrupt))
        return;

    InterruptBotState& state = botAI->GetTactics().GetInterruptState();
    Battleground* const battleground = bot->GetBattleground();
    uint32 const matchId =
        battleground && battleground->GetStatus() == STATUS_IN_PROGRESS ? battleground->GetInstanceID() : 0;
    if (!state.MatchStarted || matchId != state.MatchId)
    {
        if (PerfMonitor::IsEnabled())
        {
            PerfMonitorScope scope(sPerfMonitor.acquire(PERF_MON_TOTAL, "ArenaInterrupt::StartMatch"));
            StartInterruptMatch(botAI, bot, state, matchId, nowMs);
        }
        else
            StartInterruptMatch(botAI, bot, state, matchId, nowMs);
    }

    if (config.HoldPolicy == 0 || !state.HasKickSpell() || !bot->IsInCombat() ||
        nowMs - state.TrendSampleMs < HOLD_TREND_SAMPLE_MS)
        return;

    PerfMonitorScope scope(
        PerfMonitor::IsEnabled() ? sPerfMonitor.acquire(PERF_MON_TOTAL, "ArenaInterrupt::TrendSample") : nullptr);
    state.TrendSampleMs = nowMs;
    uint32 units = 0;
    for (ObjectGuid const& guid : botAI->GetAiObjectContext()->GetValue<GuidVector>("fight enemies")->Get())
    {
        if (units >= HOLD_TREND_UNITS)
            break;

        Unit* const unit = botAI->GetUnit(guid);
        if (!unit || !unit->IsInWorld() || !unit->IsAlive() || !unit->IsPlayer())
            continue;

        ++units;
        InterruptEnemyTrack& track = state.Track(guid.GetCounter(), nowMs);
        track.Note(InterruptHpPct(unit), nowMs);
        InterruptBar bar;
        if (!ReadInterruptBar(unit, bar))
            continue;

        uint32 const startMs = nowMs - bar.ElapsedMs;
        uint32 const gap = startMs > track.BarStartMs ? startMs - track.BarStartMs : track.BarStartMs - startMs;
        if (bar.Info->Id == track.BarSpellId && gap <= bar.CastMs / 2)
            continue;

        track.BarSpellId = bar.Info->Id;
        track.BarStartMs = startMs;
        if (ArenaInterrupt::Classify(InterruptSpellFactsOf(bar.Info)) != ArenaInterrupt::Kind::Heal)
            continue;

        track.HealSpellId = bar.Info->Id;
        track.HealCastMs = bar.CastMs;
        track.HealStartMs = startMs;
        state.NoteHoldHealBar();
    }
}

InterruptHit ReadInterruptHit(Player* bot, SpellInfo const* spellInfo, Unit* target)
{
    InterruptHit hit;
    if (target == bot || !bot->IsHostileTo(target))
        return hit;

    hit.Use = InterruptUseOf(spellInfo);
    if (hit.Use == InterruptUse::None)
        return hit;

    InterruptBar bar;
    hit.Bar = ReadInterruptBar(target, bar);
    if (!hit.Bar)
        return hit;

    hit.CastKind = ArenaInterrupt::Classify(InterruptSpellFactsOf(bar.Info));
    hit.SpellId = bar.Info->Id;
    hit.CastMs = bar.CastMs;
    hit.ElapsedMs = bar.ElapsedMs;
    return hit;
}

namespace
{
// Another enemy of the fight in view (spell range and line of sight) casting a heal (at most TACTIC_FIGHT_ENEMIES_MAX
// units; at a damage interrupt only)
bool OtherEnemyHealing(PlayerbotAI* botAI, Unit* target)
{
    Player* const bot = botAI->GetBot();
    for (ObjectGuid const& guid : botAI->GetAiObjectContext()->GetValue<GuidVector>("fight enemies")->Get())
    {
        Unit* const unit = botAI->GetUnit(guid);
        InterruptBar bar;
        if (unit && unit != target && unit->IsAlive() && ReadInterruptBar(unit, bar) &&
            ArenaInterrupt::Classify(InterruptSpellFactsOf(bar.Info)) == ArenaInterrupt::Kind::Heal &&
            bot->IsWithinDistInMap(unit, sPlayerbotAIConfig.SpellDistance) && bot->IsWithinLOSInMap(unit))
            return true;
    }

    return false;
}
}  // namespace

// tactics.intHeal/intCc/intDamage/intLate/intWasted (harness matches only): a kick at no bar is wasted (the cast was
// faked or ended), a stun at none is ordinary CC; a damage interrupt while another enemy heals is wasted too
void CountInterruptHit(PlayerbotAI* botAI, Unit* target, InterruptHit const& hit, uint32 nowMs)
{
    if (hit.Use == InterruptUse::None)
        return;

    if (!hit.Bar)
    {
        if (hit.Use == InterruptUse::Kick)
            botAI->CountTactic(TacticCounter::IntWasted);

        return;
    }

    switch (hit.CastKind)
    {
        case ArenaInterrupt::Kind::Heal:
            botAI->CountTactic(TacticCounter::IntHeal);
            if (hit.ElapsedMs * 100 >= hit.CastMs * ArenaInterrupt::LATE_SHARE_PCT)
                botAI->CountTactic(TacticCounter::IntLate);

            break;
        case ArenaInterrupt::Kind::Cc:
            botAI->CountTactic(TacticCounter::IntCc);
            break;
        case ArenaInterrupt::Kind::Damage:
            botAI->CountTactic(TacticCounter::IntDamage);
            if (OtherEnemyHealing(botAI, target))
                botAI->CountTactic(TacticCounter::IntWasted);

            break;
        default:
            break;
    }

    // Which delay set the kick: "commit" (the commit point; the kick went within BOUND_TOLERANCE_MS of the first check
    // that let it go), "reaction" (the reaction model alone: no commit point, or it ran past the commit), or
    // "commit+reaction" where the model counts from the first query and not from the bar's start (a bar older than
    // REACTION_MAX_STAMP_AGE_MS then): the kick then lands at commit plus reaction; "off" without the tactic's
    // decision
    char const* bound = "off";
    if (botAI->TacticEnabledHere(TacticId::Interrupt))
    {
        InterruptCastSlot const* slot = botAI->GetTactics().GetInterruptState().Latest(target->GetGUID().GetCounter());
        if (slot && slot->SpellId == hit.SpellId && slot->ActiveAtMs != 0)
        {
            bool const firstQueryStamp = slot->ActiveAtMs - slot->StartMs > REACTION_MAX_STAMP_AGE_MS;
            if (slot->Decision.CommitMs == 0)
                bound = "reaction";
            else if (firstQueryStamp)
                bound = "commit+reaction";
            else
                bound = nowMs - slot->ActiveAtMs <= ArenaInterrupt::BOUND_TOLERANCE_MS ? "commit" : "reaction";
        }
    }

    LOG_TRACE("playerbots.reaction",
              "interrupt kick skill {} use {} on {} spell {} kind {} elapsed {} cast {} bound {} at {}",
              botAI->GetPvpSkill(), hit.Use == InterruptUse::Kick ? "kick" : "stun", target->GetGUID().GetCounter(),
              hit.SpellId, ArenaInterrupt::KindName(hit.CastKind), hit.ElapsedMs, hit.CastMs, bound, nowMs);
}

// G5c-c: the bot's kick is used (its cooldown, InterruptBotState::NoteKickSpell); a heal it stops pays the open hold
// episode
void NoteInterruptKick(PlayerbotAI* botAI, Player* bot, SpellInfo const* spellInfo, InterruptHit const& hit,
                       uint32 nowMs)
{
    InterruptBotState& state = botAI->GetTactics().GetInterruptState();
    uint32 cooldownMs = bot->GetSpellCooldownDelay(spellInfo->Id);
    if (cooldownMs == 0)
        cooldownMs = std::max(spellInfo->RecoveryTime, spellInfo->CategoryRecoveryTime);

    state.NoteKickSpell(spellInfo->Id, nowMs, cooldownMs);
    if (hit.Bar && hit.CastKind == ArenaInterrupt::Kind::Heal)
    {
        uint32 const heldMs = state.Hold.HeldMs(nowMs);
        NoteInterruptHoldEnd(botAI, state.NoteHoldHealKick(), heldMs, "kick", nowMs);
    }
}

namespace
{
// G5b: whether another enemy in reach and sight is a healer worth holding the kick for (casting a heal now, or a
// healer class whose last casts in the opponent memory include a heal; DT.12), and how many enemies cast. At most
// TACTIC_FIGHT_ENEMIES_MAX units, once per enemy cast; the sight test only for a candidate healer.
bool InterruptHealerInReach(PlayerbotAI* botAI, Player* bot, Unit* caster, bool wantHealer, uint8& casting)
{
    OpponentMemory const& memory = botAI->GetTactics().GetOpponentMemory();
    auto const healSpell = [](uint32 spellId)
    {
        return spellId != 0 && ArenaInterrupt::Classify(InterruptSpellFactsOf(sSpellMgr->GetSpellInfo(spellId))) ==
                                   ArenaInterrupt::Kind::Heal;
    };

    bool healer = false;
    casting = 0;
    for (ObjectGuid const& guid : botAI->GetAiObjectContext()->GetValue<GuidVector>("fight enemies")->Get())
    {
        Unit* const unit = botAI->GetUnit(guid);
        if (!unit || !unit->IsInWorld() || !unit->IsAlive())
            continue;

        InterruptBar bar;
        bool const barShown = ReadInterruptBar(unit, bar);
        if (barShown)
            ++casting;

        if (!wantHealer || healer || unit == caster || CcUnitControlled(unit))
            continue;

        bool healing =
            barShown && ArenaInterrupt::Classify(InterruptSpellFactsOf(bar.Info)) == ArenaInterrupt::Kind::Heal;
        if (!healing && unit->IsPlayer() && InterruptHealerClass(unit->getClass()))
        {
            OpponentMemorySlot const* const slot = memory.Find(guid.GetCounter());
            healing = slot && (healSpell(slot->SpellId) || healSpell(slot->PrevSpellId));
        }

        healer =
            healing && bot->IsWithinDistInMap(unit, sPlayerbotAIConfig.SpellDistance) && bot->IsWithinLOSInMap(unit);
    }

    return healer;
}

// G5c-c: an instant heal or absorb of the healer on one of the units (auras with a duration are public, DT.12), put on
// within seenMs, that the bot recognises (KnowsSpell); a heal over time of a cast heal (Regrowth) is no instant
bool InterruptInstantCover(std::array<Unit*, TACTIC_FIGHT_ENEMIES_MAX> const& units, uint32 count, ObjectGuid healer,
                           ArenaInterrupt::InterruptHoldParams const& numbers, uint64 botGuid, uint32 seenMs)
{
    constexpr std::array<AuraType, 3> Types = {SPELL_AURA_PERIODIC_HEAL, SPELL_AURA_OBS_MOD_HEALTH,
                                               SPELL_AURA_SCHOOL_ABSORB};
    for (uint32 index = 0; index < count; ++index)
    {
        for (AuraType const type : Types)
        {
            for (AuraEffect const* effect : units[index]->GetAuraEffectsByType(type))
            {
                if (effect->GetCasterGUID() != healer)
                    continue;

                Aura const* const aura = effect->GetBase();
                SpellInfo const* const info = aura->GetSpellInfo();
                int32 const maxMs = aura->GetMaxDuration();
                if (maxMs <= 0 || info->CalcCastTime() > 0 ||
                    static_cast<uint32>(std::max(0, maxMs - aura->GetDuration())) > seenMs)
                    continue;

                if (ArenaInterrupt::KnowsSpell(numbers, botGuid, info->Id))
                    return true;
            }
        }
    }

    return false;
}

// G5c-c (HoldPolicy 1): what the bot sees for the hold on a damage cast, in one pass over the fight's enemies (at most
// TACTIC_FIGHT_ENEMIES_MAX units, once per enemy cast; health bars are public): the lowest projected health of the
// caster's team and its trend (the sampler's tracks), and the first healer other than the caster in reach and sight
// that the bot has seen healing (a cast heal's bar now or followed, a heal in the opponent memory, or an instant heal
// of its own the bot recognises), its instant cover and the cast time the bot believes its next cast heal takes. The
// sight test only for a candidate healer. Returns the enemies casting.
uint8 FillInterruptHold(PlayerbotAI* botAI, Player* bot, Unit* caster, ArenaInterrupt::Params const& params,
                        InterruptBotState const& state, uint32 nowMs, ArenaInterrupt::InterruptHoldInput& hold)
{
    InterruptConfig const& config = sPlayerbotAIConfig.TacticInterrupt;
    ArenaInterrupt::InterruptHoldParams const& numbers = params.Hold;
    uint64 const botGuid = bot->GetGUID().GetCounter();
    std::array<Unit*, TACTIC_FIGHT_ENEMIES_MAX> units{};
    uint32 count = 0;
    uint8 casting = 0;
    uint32 lowest = 101;
    for (ObjectGuid const& guid : botAI->GetAiObjectContext()->GetValue<GuidVector>("fight enemies")->Get())
    {
        if (count >= TACTIC_FIGHT_ENEMIES_MAX)
            break;

        Unit* const unit = botAI->GetUnit(guid);
        if (!unit || !unit->IsInWorld() || !unit->IsAlive())
            continue;

        units[count++] = unit;
        InterruptBar bar;
        if (ReadInterruptBar(unit, bar))
            ++casting;

        if (!unit->IsPlayer())
            continue;

        uint8 const hpPct = InterruptHpPct(unit);
        InterruptEnemyTrack const* const track = state.FindTrack(guid.GetCounter());
        float const loss = track ? track->LossPctPerS(hpPct, nowMs) : 0.0f;
        uint8 const projected = ArenaInterrupt::HoldProjectedHpPct(numbers, hpPct, loss);
        if (projected < lowest || (projected == lowest && hpPct < hold.FriendHpPct))
        {
            lowest = projected;
            hold.FriendHpPct = hpPct;
            hold.FriendLossPctPerS = loss;
        }
    }

    OpponentMemory const& memory = botAI->GetTactics().GetOpponentMemory();
    for (uint32 index = 0; index < count; ++index)
    {
        Unit* const unit = units[index];
        if (unit == caster || !unit->IsPlayer() || !InterruptHealerClass(unit->getClass()) || CcUnitControlled(unit) ||
            !bot->IsWithinDistInMap(unit, sPlayerbotAIConfig.SpellDistance))
            continue;

        // The cast heal seen last: its bar now, the sampler's, else the opponent memory's (its start unknown)
        uint32 const guid = unit->GetGUID().GetCounter();
        uint32 healSpell = 0;
        uint32 healCastMs = 0;
        uint32 healStartMs = 0;
        bool startKnown = false;
        InterruptBar bar;
        InterruptEnemyTrack const* const track = state.FindTrack(guid);
        if (ReadInterruptBar(unit, bar) &&
            ArenaInterrupt::Classify(InterruptSpellFactsOf(bar.Info)) == ArenaInterrupt::Kind::Heal)
        {
            healSpell = bar.Info->Id;
            healCastMs = bar.CastMs;
            healStartMs = nowMs - bar.ElapsedMs;
            startKnown = true;
        }
        else if (track && track->HealSpellId != 0)
        {
            healSpell = track->HealSpellId;
            healCastMs = track->HealCastMs;
            healStartMs = track->HealStartMs;
            startKnown = true;
        }
        else if (OpponentMemorySlot const* const slot = memory.Find(guid))
        {
            for (uint32 const spellId : {slot->SpellId, slot->PrevSpellId})
            {
                SpellInfo const* const info = spellId ? sSpellMgr->GetSpellInfo(spellId) : nullptr;
                if (info && ArenaInterrupt::Classify(InterruptSpellFactsOf(info)) == ArenaInterrupt::Kind::Heal)
                {
                    healSpell = spellId;
                    healCastMs = info->CalcCastTime();
                    break;
                }
            }
        }

        bool const cover =
            InterruptInstantCover(units, count, unit->GetGUID(), numbers, botGuid, config.HoldInstantSeenMs);
        if ((healSpell == 0 && !cover) || !bot->IsWithinLOSInMap(unit))
            continue;

        bool const known = healSpell != 0 && ArenaInterrupt::KnowsSpell(numbers, botGuid, healSpell);
        hold.HealerSeen = true;
        hold.InstantCover = cover;
        hold.RecentCastHeal = startKnown && known && nowMs - healStartMs <= config.HoldRecentCastHealMs;
        hold.HealCastMs = ArenaInterrupt::BelievedHealCastMs(numbers, known, healCastMs,
                                                             InterruptHealerCastPriorMs(unit->getClass()));
        break;
    }

    return casting;
}
}  // namespace

bool BotTactics::DecideInterrupt(Unit* enemy)
{
    InterruptBar bar;
    if (!ReadInterruptBar(enemy, bar))
        return true;  // no bar to judge (an instant in flight): as without the tactic

    uint32 const nowMs = getMSTime();
    uint32 const enemyGuid = enemy->GetGUID().GetCounter();
    uint32 const startMs = nowMs - bar.ElapsedMs;
    InterruptBotState& state = GetInterruptState();
    InterruptCastSlot* slot = state.Find(enemyGuid, bar.Info->Id, startMs, bar.CastMs / 2);
    if (!slot)
    {
        // One decision per cast: the draw comes from the event's hash (G0h), so it holds across checks
        PlayerbotAIConfig const& config = sPlayerbotAIConfig;
        // G5c-c: while the tactic follows the kick (HoldPolicy 1, the attention gap) a cast is judged only once the
        // bot's kick is ready, so a cast first seen during the cooldown is judged when the kick returns
        bool const follows = InterruptFollows(config.TacticInterrupt);
        if (follows && !state.KickReady(nowMs))
            return false;

        slot = &state.Take(nowMs);
        slot->Guid = enemyGuid;
        slot->SpellId = bar.Info->Id;
        slot->StartMs = startMs;
        slot->CastMs = bar.CastMs;
        // 13e-decide (Interrupt.SeeCancelLag): the bar's end reaches the bot with its reaction delay
        if (config.TacticInterrupt.SeeCancelLag)
        {
            ReactionParams const& reaction = config.NaturalReaction;
            slot->SeeLagMs = ArenaInterrupt::HoldReactionMs(reactionState, reactionTypes, ReactionEase::Common,
                                                            reaction.PingMs + reaction.DisplayMs, reaction.MinMs);
        }
        uint64 const hash = ReactionEventHash(bot->GetGUID().GetCounter(),
                                              static_cast<uint8>(SkillNoticeCategory::Interrupt), startMs) ^
                            enemyGuid;
        ArenaInterrupt::Params const params = ArenaInterrupt::ComputeParams(
            botAI->GetPvpSkill(), attributes.Get(BotAttribute::Patience), attributes.Get(BotAttribute::Knowledge),
            config.SkillGate.CurveExponent, config.TacticInterrupt);
        ArenaInterrupt::Input input;
        input.CastMs = bar.CastMs;
        input.Deep =
            ArenaInterrupt::UsesDeep(TacticActive(TacticId::Interrupt), HasFullSkillAbility(), reactionTypes, hash);
        if (follows)
        {
            input.ReadyMs = state.KickReadyMs(nowMs);
            input.ReadyHash = ArenaInterrupt::AttentionHash(bot->GetGUID().GetCounter(), state.KickReadyAtMs);
        }

        bool const holding = params.Hold.Enable;
        if (holding)
        {
            uint32 const heldMs = state.Hold.HeldMs(nowMs);
            NoteInterruptHoldEnd(botAI, state.ResetHold(nowMs, params.Hold.ResetMs), heldMs, "reset", nowMs);
        }

        bool healer = false;
        if (input.Deep)
        {
            input.CastKind = ArenaInterrupt::Classify(InterruptSpellFactsOf(bar.Info));
            input.TargetHpPct = InterruptBarTargetHpPct(enemy, bar);
            input.FakeRate = opponentMemory.FakeRate(enemyGuid, config.TacticInterrupt.CancelMinCasts);
            uint8 casting = 0;
            if (holding && input.CastKind == ArenaInterrupt::Kind::Damage)
            {
                PerfMonitorScope scope(PerfMonitor::IsEnabled()
                                           ? sPerfMonitor.acquire(PERF_MON_TOTAL, "ArenaInterrupt::HoldScan")
                                           : nullptr);
                ArenaInterrupt::InterruptHoldInput& hold = input.Hold;
                casting = FillInterruptHold(botAI, bot, enemy, params, state, nowMs, hold);
                ReactionParams const& reaction = config.NaturalReaction;
                hold.ReactionMs = ArenaInterrupt::HoldReactionMs(reactionState, reactionTypes, ReactionEase::Common,
                                                                 reaction.PingMs + reaction.DisplayMs, reaction.MinMs);
                hold.HeldMs = state.Hold.HeldMs(nowMs);
                hold.Expired = state.Hold.Expired;
                healer = hold.HealerSeen;
            }
            else
            {
                bool const wantHealer = !holding && input.CastKind == ArenaInterrupt::Kind::Damage &&
                                        config.TacticInterrupt.HoldForHealer != 0;
                healer = InterruptHealerInReach(botAI, bot, enemy, wantHealer, casting);
                input.HealerInRange = healer;
            }

            if (holding && input.CastKind == ArenaInterrupt::Kind::Heal)
                state.NoteHoldHealBar();

            slot->Casting = std::max<uint8>(1, casting);
        }

        slot->Deep = input.Deep;
        slot->CastKind = input.CastKind;
        slot->Decision = ArenaInterrupt::Decide(input, params, hash);
        if (input.Deep)
            ++state.Decided[static_cast<uint32>(input.CastKind)];
        else
            ++state.Shortcuts;

        if (!slot->Decision.Kick)
            ++state.Skips;

        // G5c-c: the hold episode and tactics.intHold, intHoldExpired, intHoldEpisodes
        if (holding)
        {
            uint32 const episodes = state.HoldEpisodes;
            if (slot->Decision.Why == ArenaInterrupt::Reason::Held)
            {
                state.NoteHeld(nowMs);
                botAI->CountTactic(TacticCounter::IntHold);
            }
            else if (slot->Decision.Why == ArenaInterrupt::Reason::HoldExpired && state.NoteHoldExpired(nowMs))
                botAI->CountTactic(TacticCounter::IntHoldExpired);

            if (state.HoldEpisodes != episodes)
                botAI->CountTactic(TacticCounter::IntHoldEpisodes);
        }

        // G5c-c's fields come after G5b's (the analysis joins by position): the use draw, the attention gap and how
        // long the kick was ready (-1: not followed), then the hold's inputs and parts (their defaults unless judged)
        ArenaInterrupt::Decision const& decision = slot->Decision;
        ArenaInterrupt::InterruptHoldInput const& hold = input.Hold;
        LOG_TRACE("playerbots.reaction",
                  "interrupt decide skill {} enemy {} spell {} cast {} elapsed {} depth {} kind {} kick {} commit {} "
                  "share {:.2f} why {} healer {} fake {:.2f} casting {} at {} use {:.2f} gap {} ready {} hp {} loss "
                  "{:.1f} proj {} high {:.2f} need {:.2f} recent {} cover {} healcast {} react {} reach {} motive "
                  "{:.2f} held {} expired {} focus {}",
                  botAI->GetPvpSkill(), enemyGuid, slot->SpellId, bar.CastMs, bar.ElapsedMs,
                  input.Deep ? "deep" : "shortcut", input.Deep ? ArenaInterrupt::KindName(input.CastKind) : "-",
                  decision.Kick ? 1 : 0, decision.CommitMs, decision.Share, ArenaInterrupt::ReasonName(decision.Why),
                  healer ? 1 : 0, input.FakeRate, slot->Casting, nowMs, decision.UseDraw, decision.AttentionGapMs,
                  input.ReadyMs == ArenaInterrupt::ATTENTION_UNKNOWN_MS ? -1 : static_cast<int64>(input.ReadyMs),
                  hold.FriendHpPct, hold.FriendLossPctPerS, decision.Hold.ProjectedHpPct, decision.Hold.HpHigh,
                  decision.Hold.Need, hold.RecentCastHeal ? 1 : 0, hold.InstantCover ? 1 : 0, hold.HealCastMs,
                  hold.ReactionMs, decision.Hold.Reachable ? 1 : 0, decision.Hold.Motivation, hold.HeldMs,
                  hold.Expired ? 1 : 0, focusState && focusState->Target.Is(enemy->GetGUID()) ? 1 : 0);
    }

    slot->SeenMs = nowMs;
    if (!slot->Decision.Kick || bar.ElapsedMs < slot->Decision.CommitMs)
        return false;

    if (slot->ActiveAtMs == 0)
    {
        slot->ActiveAtMs = nowMs;
        ++state.Kicks;
        // tactics stats: the tactic acts when the bot judged the cast
        if (slot->Deep)
            NoteTacticUse(TacticId::Interrupt);
    }

    return true;
}
