/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "NaturalCast.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <vector>

#include "BotTactics.h"
#include "BotTacticsInternal.h"
#include "CellImpl.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Group.h"
#include "Log.h"
#include "NaturalReaction.h"
#include "ObjectAccessor.h"
#include "PerfMonitor.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "PvpTestCounters.h"
#include "Spell.h"
#include "SpellInfo.h"
#include "TacticIds.h"

namespace
{
// G5e: an enemy player in view that could kick the bot's cast now (DT.12, what a player sees: its class, its reach,
// its facing or its target)
bool FakeKickerInView(Player* bot, Player* enemy, FakeParams const& params)
{
    if (!enemy || !enemy->IsInWorld() || !enemy->IsAlive() || !enemy->IsInMap(bot))
        return false;

    uint32 const reach = FakeKickerReachYards(enemy->getClass(), params);
    if (reach == 0 || !bot->IsWithinDistInMap(enemy, static_cast<float>(reach)))
        return false;

    return FakeKickerSees(enemy->HasInArc(static_cast<float>(M_PI), bot), enemy->GetTarget() == bot->GetGUID(), params);
}

// 13e-polish: a cast outside the fight (a conjure, an item, a pet summon, an enchant, a trade skill) is no bait
bool FakeUtilityCast(SpellInfo const* info)
{
    for (uint8 i = EFFECT_0; i <= EFFECT_2; ++i)
    {
        switch (info->Effects[i].Effect)
        {
            case SPELL_EFFECT_CREATE_ITEM:
            case SPELL_EFFECT_CREATE_ITEM_2:
            case SPELL_EFFECT_CREATE_RANDOM_ITEM:
            case SPELL_EFFECT_CREATE_MANA_GEM:
            case SPELL_EFFECT_TRADE_SKILL:
            case SPELL_EFFECT_ENCHANT_ITEM:
            case SPELL_EFFECT_ENCHANT_ITEM_TEMPORARY:
            case SPELL_EFFECT_ENCHANT_HELD_ITEM:
            case SPELL_EFFECT_SUMMON_PET:
            case SPELL_EFFECT_SUMMON_OBJECT_WILD:
            case SPELL_EFFECT_FEED_PET:
                return true;
            default:
                break;
        }
    }

    return false;
}
}  // namespace

// G5e: the bot's cast began (CastSpell). A new long generic cast ends any pending fake and may become one: the fight's
// enemies in view that could kick it, without those whose kick the bot remembers used lately; a fake's cancel instant
// goes to the pre-gate stepper. Anything else (an instant, a channel) leaves a pending fake alone.
void DecideFakeCast(PlayerbotAI* botAI, Player* bot, uint32 spellId, uint32 nowMs)
{
    Spell const* const spell = bot->GetCurrentSpell(CURRENT_GENERIC_SPELL);
    SpellInfo const* const info = spell ? spell->GetSpellInfo() : nullptr;
    if (!info || info->Id != spellId || spell->getState() != SPELL_STATE_PREPARING)
        return;

    FakeBotState& state = botAI->GetTactics().GetFakeState();
    state.ClearPending();
    if (!botAI->TacticEnabledHere(TacticId::Fake))
        return;

    FakeParams const& params = state.Params;
    FakeInput input;
    input.CastMs = static_cast<uint32>(std::max(spell->GetCastTime(), 0));
    input.Kickable = (info->InterruptFlags & SPELL_INTERRUPT_FLAG_INTERRUPT) != 0;
    input.Fight = bot->IsInCombat() && !FakeUtilityCast(info);
    if (FakeCastVerdict(params, input.CastMs, input.Kickable, input.Fight) != FakeVerdict::Fake)
        return;

    PerfMonitorScope scope(PerfMonitor::IsEnabled() ? sPerfMonitor.acquire(PERF_MON_TOTAL, "NaturalCast::Decide")
                                                    : nullptr);
    OpponentMemory const& memory = botAI->GetTactics().GetOpponentMemory();
    uint64 kickerGuid = 0;
    uint8 kickerClass = 0;
    for (ObjectGuid const& guid : botAI->GetAiObjectContext()->GetValue<GuidVector>("fight enemies")->Get())
    {
        Unit* const unit = botAI->GetUnit(guid);
        Player* const enemy = unit ? unit->ToPlayer() : nullptr;
        if (!FakeKickerInView(bot, enemy, params))
            continue;

        ++input.KickersSeen;
        if (!FakeKickerLive(memory.KickSeenWithin(guid.GetCounter(), nowMs, params.KickSeenMs), params.RemembersKicks))
            continue;

        if (input.KickersLive++ == 0)
        {
            kickerGuid = guid.GetRawValue();
            kickerClass = enemy->getClass();
        }
    }

    // 13e-decide (Fake.HealGuardPct): no fake heal while its target, or (the only healer) any friend, is low
    if (input.KickersLive != 0 && ArenaInterrupt::Classify(InterruptSpellFactsOf(info)) == ArenaInterrupt::Kind::Heal)
    {
        auto const pct = [](Unit const* unit) { return static_cast<uint32>(unit->GetHealthPct() + 0.5f); };
        Unit const* const healed = spell->m_targets.GetUnitTarget();
        input.Heal = true;
        input.HealTargetHpPct = pct(healed ? healed : bot);
        uint32 healers = 0;
        uint32 lowest = pct(bot);
        if (Group* const group = bot->GetGroup())
        {
            for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
            {
                Player* const member = ref->GetSource();
                if (!member || !member->IsInMap(bot) || !member->IsAlive())
                    continue;

                healers += PlayerbotAI::IsHeal(member) ? 1 : 0;
                lowest = std::min(lowest, pct(member));
            }
        }

        input.OnlyHealer = healers <= 1;
        input.LowestFriendHpPct = lowest;
    }

    input.NowMs = nowMs;
    input.NextFakeAtMs = state.NextFakeAtMs;
    input.MinuteStartMs = state.MinuteStartMs;
    input.MinuteFakes = state.MinuteFakes;
    input.Hash = FakeCastHash(bot->GetGUID().GetCounter(), nowMs, spellId);
    FakeVerdict const verdict = DecideFake(params, input);
    state.LastVerdict = verdict;
    if (verdict == FakeVerdict::NoKicker)
        return;

    ++state.Decisions;
    if (verdict != FakeVerdict::Fake)
    {
        LOG_TRACE("playerbots",
                  "Fake: {} casts {} ({}) of {} ms for real ({}), kickers {} in view, {} live, skill {}, at {} ms",
                  bot->GetName(), info->SpellName[0], spellId, input.CastMs, FakeVerdictName(verdict),
                  input.KickersSeen, input.KickersLive, botAI->GetPvpSkill(), nowMs);
        return;
    }

    uint32 const cancelMs = PickCancelMs(params, input.CastMs, input.Hash);
    state.SpellId = spellId;
    state.CastStartMs = nowMs;
    state.CastMs = input.CastMs;
    state.CancelAtMs = nowMs + cancelMs;
    state.KickerGuid = kickerGuid;
    state.KickerClass = kickerClass;
    state.Hash = input.Hash;
    botAI->GetTactics().SetNaturalPreGate(true);
    LOG_TRACE("playerbots",
              "Fake: {} plans to cancel {} ({}) at {} of {} ms ({} %), kicker {} class {} ({} in view, {} live), "
              "skill {}, at {} ms",
              bot->GetName(), info->SpellName[0], spellId, cancelMs, input.CastMs, cancelMs * 100 / input.CastMs,
              ObjectGuid(kickerGuid).GetCounter(), kickerClass, input.KickersSeen, input.KickersLive,
              botAI->GetPvpSkill(), nowMs);
}

// G5e: the pending fake at the pre-gate point. The cast gone (kicked, interrupted, replaced) ends it; at its instant
// the cast is cancelled while the kicker still threatens it (in view, its kick not seen used), else it finishes.
FakeStep StepFakeCast(PlayerbotAI* botAI, Player* bot, FakeBotState& state, uint32 nowMs, uint32 tickMs)
{
    Spell const* const spell = bot->GetCurrentSpell(CURRENT_GENERIC_SPELL);
    SpellInfo const* const info = spell ? spell->GetSpellInfo() : nullptr;
    if (!info || info->Id != state.SpellId || spell->getState() != SPELL_STATE_PREPARING)
    {
        ++state.Gone;
        LOG_TRACE("playerbots", "Fake: {} cast {} ended {} ms in, before its cancel at {} ms, at {} ms", bot->GetName(),
                  state.SpellId, nowMs - state.CastStartMs, state.CancelAtMs - state.CastStartMs, nowMs);
        state.ClearPending();
        return FakeStep::Done;
    }

    if (!FakeCancelDue(state.CancelAtMs, nowMs, tickMs))
        return FakeStep::Pending;

    FakeParams const& params = state.Params;
    ObjectGuid const kickerGuid(state.KickerGuid);
    Player* const kicker = ObjectAccessor::GetPlayer(*bot, kickerGuid);
    bool const threat =
        FakeKickerInView(bot, kicker, params) && FakeKickerLive(botAI->GetTactics().GetOpponentMemory().KickSeenWithin(
                                                                    kickerGuid.GetCounter(), nowMs, params.KickSeenMs),
                                                                params.RemembersKicks);
    if (!threat)
    {
        ++state.Finished;
        LOG_TRACE("playerbots", "Fake: {} lets {} ({}) finish, the kicker {} no longer threatens it, at {} ms",
                  bot->GetName(), info->SpellName[0], state.SpellId, kickerGuid.GetCounter(), nowMs);
        state.ClearPending();
        return FakeStep::Done;
    }

    uint32 const spellId = state.SpellId;
    uint32 const castMs = state.CastMs;
    uint32 const plannedMs = state.CancelAtMs - state.CastStartMs;
    bot->InterruptSpell(CURRENT_GENERIC_SPELL, false, false, true);
    state.NoteCancel(nowMs, NextFakeGapMs(params, state.Hash));
    botAI->CountTactic(TacticCounter::Fakes);
    LOG_TRACE("playerbots",
              "Fake cancel: {} cancelled {} ({}) at {} of {} ms ({} %, planned {} ms, error {} ms), kicker {}, skill "
              "{}, next fake from {} ms, at {} ms",
              bot->GetName(), info->SpellName[0], spellId, plannedMs + state.LastErrorMs, castMs, state.LastCancelPct,
              plannedMs, state.LastErrorMs, kickerGuid.GetCounter(), botAI->GetPvpSkill(), state.NextFakeAtMs, nowMs);
    return FakeStep::Cancelled;
}

// G5e review note 1: set once any bot's tactic mask holds Fake (SetTacticMask: the config mask at login or reload, a
// forced mask), never cleared; until then a kick needs no search for bots that note it
std::atomic<bool> fakeTacticSeen{false};

// G5e (NaturalCast.h): a kick aimed at one enemy, cast by any player (OnPlayerSpellCast, the caster's map thread). The
// filter comes first (the tactics on, the Fake tactic in some bot's mask, a kick, a player target on the caster's
// map); then the players around the target on that map (the same thread) that are hostile to the caster and bots
// with the Fake tactic on here note it in their own kick memory, and the target counts tactics.kicksBaited when the
// kick came within BaitedWindowMs of its fake cancel. No unit pointer is kept; nothing of the caster's AI is read but
// its skill for the trace line.
void NoteFakeKickCast(Player* caster, Spell* spell)
{
    if (!sPlayerbotAIConfig.Tactics.Enable || !fakeTacticSeen.load(std::memory_order_relaxed) || !caster || !spell)
        return;

    SpellInfo const* const info = spell->GetSpellInfo();
    if (!info || InterruptUseOf(info) != InterruptUse::Kick || !InterruptKickAimed(info))
        return;

    Unit* const target = spell->m_targets.GetUnitTarget();
    if (!target || !target->IsPlayer() || !target->IsInWorld() || !target->IsInMap(caster))
        return;

    PerfMonitorScope scope(PerfMonitor::IsEnabled() ? sPerfMonitor.acquire(PERF_MON_TOTAL, "NaturalCast::KickSeen")
                                                    : nullptr);
    uint32 const nowMs = getMSTime();
    std::vector<Player*> seers;
    Acore::AnyPlayerInObjectRangeCheck check(target, FAKE_KICK_SEEN_YARDS);
    Acore::PlayerListSearcher<Acore::AnyPlayerInObjectRangeCheck> searcher(target, seers, check);
    Cell::VisitObjects(target, searcher, FAKE_KICK_SEEN_YARDS);
    for (Player* const seer : seers)
    {
        if (seer == caster || !seer->IsHostileTo(caster))
            continue;

        PlayerbotAI* const botAI = GET_PLAYERBOT_AI(seer);
        if (!botAI || !botAI->TacticEnabledHere(TacticId::Fake))
            continue;

        botAI->GetTactics().GetOpponentMemory().NoteKick(caster->GetGUID().GetCounter(), info->Id, nowMs);
        botAI->CountTactic(TacticCounter::KickSeen);
        FakeBotState& state = botAI->GetTactics().GetFakeState();
        ++state.KicksSeen;
        if (seer != target || !FakeBaited(state.LastCancelMs, nowMs, state.Params.BaitedWindowMs))
            continue;

        uint32 const afterMs = nowMs - state.LastCancelMs;
        state.LastCancelMs = 0;
        ++state.Baited;
        botAI->CountTactic(TacticCounter::KicksBaited);
        PlayerbotAI* const kickerAI = GET_PLAYERBOT_AI(caster);
        LOG_TRACE(
            "playerbots",
            "Fake baited: {} kicked {} with {} ({}) {} ms after its fake cancel, kicker skill {}, skill {}, at {} "
            "ms",
            caster->GetName(), seer->GetName(), info->SpellName[0], info->Id, afterMs,
            kickerAI ? kickerAI->GetPvpSkill() : 0, botAI->GetPvpSkill(), nowMs);
    }
}
