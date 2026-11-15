/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "PvpTestCounters.h"

#include <atomic>
#include <chrono>

#include "Action.h"
#include "AiObjectContext.h"
#include "BotTacticsInternal.h"
#include "CcTacticStrategy.h"
#include "MatchTrace.h"
#include "PlayerbotAI.h"
#include "SkillStrategy.h"
#include "SpellInfo.h"

static_assert(PVPTEST_NOTICE_CATEGORY_COUNT == SkillNoticeCounters::Categories,
              "the executed counts have one slot per notice category");

PvpTestCounterBlock const& PlayerbotAI::GetPvpTestCounters() const
{
    static PvpTestCounterBlock const none{};
    return pvpTestCounters ? *pvpTestCounters : none;
}

void PlayerbotAI::AddPvpTestOwnBreak()
{
    if (pvpTestCounters)
        ++pvpTestCounters->Cc.OwnBreaks;
}

void PlayerbotAI::AddPvpTestPartnerDispel(uint32 ageMs)
{
    if (!pvpTestCounters)
        return;

    ++pvpTestCounters->Cc.PartnerDispel;
    pvpTestCounters->Tactics[static_cast<uint32>(TacticCounter::PartnerDispelMs)] += ageMs;
}

void PlayerbotAI::AddPvpTestTactic(TacticCounter counter, uint32 amount)
{
    if (pvpTestCounters)
        pvpTestCounters->Tactics[static_cast<uint32>(counter)] += amount;
}

// The DR group a cast counts under. A trap is cast as a summon spell without a group of its own (the spell only puts
// the trap down); its crowd control is the spell the trap's game object casts when it springs (ArenaCcSpells.h).
uint32 GetPvpTestCastGroup(SpellInfo const* spellInfo)
{
    return GetDiminishingReturnsGroupForSpell(GetCcAuraSpell(spellInfo), false);
}

void PlayerbotAI::SetPvpTestCounting(bool on)
{
    // G5c-c: an open hold episode never spans two records (a forced Interrupt mask ends here, so the match end's own
    // close may never run); not counted
    if (interruptState)
    {
        uint32 const nowMs = getMSTime();
        uint32 const heldMs = interruptState->Hold.HeldMs(nowMs);
        NoteInterruptHoldEnd(this, interruptState->EndMatch(), heldMs, "match", nowMs, false);
    }

    if (on)
    {
        // The block before the flag: a count that sees the flag finds it
        if (pvpTestCounters)
            *pvpTestCounters = PvpTestCounterBlock();
        else
            pvpTestCounters = std::make_unique<PvpTestCounterBlock>();

        pvpTestCounting = true;
        return;
    }

    pvpTestCounting = false;

    // A tactic mask the harness forced lasts for the match only, Avoid.EnsureTrinket's trinket too (G5a)
    CcRestoreTrinket(this);
    ResetTacticMask();
}

namespace
{
constexpr uint32 SPELL_PVP_TRINKET = 42292;
constexpr uint32 SPELL_EVERY_MAN_FOR_HIMSELF = 59752;
constexpr uint32 SPELL_WILL_OF_THE_FORSAKEN = 7744;
constexpr uint32 SPELL_ESCAPE_ARTIST = 20589;
}  // namespace

void PlayerbotAI::CountPvpTestCast(uint32 spellId, uint32 drGroup, bool atImmune)
{
    // Called while counting only, so the block exists
    if (!pvpTestCounters)
        return;

    PvpTestCcCounters& cc = pvpTestCounters->Cc;
    if (IsPvpTestCcGroup(drGroup))
    {
        ++cc.Cast[drGroup];
        if (atImmune)
            ++cc.AtImmune;
    }

    // The trinket and the racials have no DR group; "pvp trinket" (G5a, an item use) counts its use here too
    if (spellId == SPELL_PVP_TRINKET)
        ++cc.Trinket;
    else if (spellId == SPELL_EVERY_MAN_FOR_HIMSELF || spellId == SPELL_WILL_OF_THE_FORSAKEN ||
             spellId == SPELL_ESCAPE_ARTIST)
        ++cc.RacialBreak;
    else
        return;

    // G5a: tactics.breakWasted and breakMs of a break the Trinket tactic answered
    CcNoteBreakCast(this, spellId);
}

void PlayerbotAI::CountPvpTestReceivedCc(uint32 drGroup)
{
    if (pvpTestCounting && pvpTestCounters && IsPvpTestCcGroup(drGroup))
        ++pvpTestCounters->Cc.Received[drGroup];
}

namespace
{
// The cost of the counting (M9 review): the actions that went through BeginExecutedCount and the time of the name
// lookup, which is timed in one call out of CountSampleEvery per thread (so the other calls pay no atomic and no clock)
constexpr uint32 CountSampleEvery = 8;
std::atomic<uint64> countingCalls{0};
std::atomic<uint64> countingSamples{0};
std::atomic<uint64> countingSampledNs{0};
}  // namespace

PlayerbotAI::CountingStats PlayerbotAI::GetCountingStats()
{
    return CountingStats{countingCalls.load(), countingSamples.load(), countingSampledNs.load()};
}

uint8 PlayerbotAI::BeginExecutedCount(Action* action, ObjectGuid& previousTarget)
{
    static thread_local uint32 tick = 0;
    SkillNoticeCategory category;
    if (++tick % CountSampleEvery == 0)
    {
        auto const start = std::chrono::steady_clock::now();
        category = SkillNoticeCategoryOf(action->getName());
        auto const elapsed = std::chrono::steady_clock::now() - start;
        countingCalls.fetch_add(CountSampleEvery, std::memory_order_relaxed);
        countingSamples.fetch_add(1, std::memory_order_relaxed);
        countingSampledNs.fetch_add(
            static_cast<uint64>(std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count()),
            std::memory_order_relaxed);
    }
    else
        category = SkillNoticeCategoryOf(action->getName());

    // A movement (avoid aoe) or a positioning (regain los, break los, pillar step) is a reaction only, with no
    // executed count
    if (category == SkillNoticeCategory::None || category == SkillNoticeCategory::Movement ||
        category == SkillNoticeCategory::Positioning)
        return 0;

    if (category == SkillNoticeCategory::TargetSwitch)
    {
        Unit* const current = aiObjectContext->GetValue<Unit*>("current target")->Get();
        previousTarget = current ? current->GetGUID() : ObjectGuid::Empty;
    }

    return static_cast<uint8>(category);
}

void PlayerbotAI::EndExecutedCount(uint8 category, ObjectGuid previousTarget)
{
    ObjectGuid target;
    if (category == static_cast<uint8>(SkillNoticeCategory::TargetSwitch))
    {
        // An assist that keeps the target is not a switch
        Unit* const current = aiObjectContext->GetValue<Unit*>("current target")->Get();
        if (!current || current->GetGUID() == previousTarget)
            return;

        target = current->GetGUID();
    }

    if (pvpTestCounters)
        ++pvpTestCounters->Executed[category - 1];

    // A traced arena keeps the action (M10; one test while no trace runs)
    MatchTrace::OnAction(bot, category, target);
}
