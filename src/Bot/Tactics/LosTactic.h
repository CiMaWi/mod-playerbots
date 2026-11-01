/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_LOSTACTIC_H
#define _PLAYERBOT_LOSTACTIC_H

#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <vector>

#include "ArenaCover.h"
#include "ArenaCoverConfig.h"
#include "Common.h"
#include "SplitMix64.h"
#include "TacticGate.h"

// The Los tactic (G2e, PLAN 3.15, DT.9), the pure parts: a ranged bot that cannot see its target, or a healer that
// cannot see an alive partner, moves to a point that sees it again (a pillar's ring in an arena, else the first point
// of the path that sees it) instead of walking into melee or not healing. The game side is LosTacticStrategy.cpp: a
// sight refresh at most once per second (at most RaysPerSecond rays: the target or the partners, then the held point),
// the triggers "target out of los" and "partner out of los" and the action "regain los" (a cover search at most every
// SearchIntervalMs, at most MaxRaysPerSearch rays). The skill shapes it (LosConfig): known from KnownFromSkill
// (knowledge), late by LateMs (speed, log-normal per episode), into the open by IntoOpenPct (skill), hidden from the
// enemy caster from HidesFromCasterFromSkill (wisdom); a healer below EffortMin stays put. The guards: a point is
// held at least HoldMs, at most MaxRelocs relocations per RelocWindowMs (G2f's pillar play shares the ring); a regain
// not done in TimeoutMs gives up until the bot sees again. DT.12: positions and rays only, no other bot's AI state.

constexpr uint32 LOS_REFRESH_MS = 1000;       // the sight refresh (the cover plan's "at most once per second")
constexpr float LOS_PARTNER_YARDS = 40.0f;    // partners further away are not watched (out of heal range)
constexpr float LOS_MAX_MOVE_YARDS = 15.0f;   // a cover point at most this far (FindCoverPoint's estimated walk)
constexpr float LOS_PATH_FACTOR = 1.6f;       // the mmap path to it at most this times LOS_MAX_MOVE_YARDS
constexpr float LOS_FLOOR_DZ = 1.5f;          // the ground at a cover point within this of its pillar's floor
constexpr float LOS_MELEE_YARDS = 8.0f;       // a path point this near the enemy target is melee: never taken
constexpr float LOS_AT_POINT_YARDS = 1.5f;    // the bot stands on its point
constexpr uint32 LOS_LOST_GAP_CAP_MS = 2000;  // one refresh adds at most this to partnerLosLostMs
constexpr uint32 LOS_DUE_MS = 1000;           // a due regain is acted on within this, else dropped
constexpr uint32 LOS_RING = 8;                // the relocation ring (MaxRelocs is clamped to it)
constexpr float LOS_LATE_SIGMA = 0.35f;       // the lateness' log-normal spread
constexpr float LOS_LATE_MIN_FACTOR = 0.4f;   // ... clamped to this ...
constexpr float LOS_LATE_MAX_FACTOR = 2.5f;   // ... and this times the mean
constexpr uint64 LOS_SALT_LATE = 0x6A09E667F3BCC909ULL;
constexpr uint64 LOS_SALT_OPEN = 0x3C6EF372FE94F82BULL;

enum class LosWant : uint8
{
    None,
    Target,   // a ranged bot cannot see its target
    Partner,  // a healer cannot see an alive partner
};

inline char const* LosWantName(LosWant want)
{
    switch (want)
    {
        case LosWant::Target:
            return "target";
        case LosWant::Partner:
            return "partner";
        default:
            return "none";
    }
}

// One bot's numbers (from LosConfig and its attributes at each refresh)
struct LosParams
{
    bool Known = true;            // the bot regains LOS on purpose at all
    uint32 LateMs = 0;            // the mean wait before it moves
    uint32 IntoOpenPct = 0;       // the chance per episode that it walks the path into the open (no cover)
    bool HidesFromCaster = true;  // the cover point also hides it from the nearest enemy caster
    bool HealerStays = false;     // a low-effort healer does not move for a partner
    uint32 HoldMs = 2500;
    uint32 MaxRelocs = 3;
    uint32 RelocWindowMs = 10000;
    uint32 SearchIntervalMs = 1500;
    uint32 MaxRays = 40;
    uint32 RaysPerRefresh = 5;
    uint32 TimeoutMs = 6000;
};

// What the numbers read of a bot (PlayerbotAI: the skill, three attributes, the effort; full ability = skill 0 or
// PlayerbotAI::HasFullSkillAbility)
struct LosGateInputs
{
    uint8 Skill = 0;
    uint8 Knowledge = 0;
    uint8 Speed = 0;
    uint8 Wisdom = 0;
    uint8 Effort = SKILL_GATE_FULL;
    bool FullAbility = true;
    float Exponent = 1.0f;
};

inline LosParams ComputeLosParams(LosConfig const& config, LosGateInputs const& in)
{
    bool const full = in.FullAbility || in.Skill == 0;
    LosParams params;
    params.Known = full || in.Knowledge >= config.KnownFromSkill;
    params.LateMs = SkillLerpRounded(config.LateMsAtSkill1, config.LateMsAtSkill99,
                                     full ? 1.0f : SkillPosition(in.Speed), in.Exponent);
    params.IntoOpenPct = std::min<uint32>(SkillLerpRounded(config.IntoOpenPctAtSkill1, config.IntoOpenPctAtSkill99,
                                                           full ? 1.0f : SkillPosition(in.Skill), in.Exponent),
                                          100);
    params.HidesFromCaster = full || in.Wisdom >= config.HidesFromCasterFromSkill;
    params.HealerStays = !full && in.Effort < config.EffortMin;
    params.HoldMs = config.HoldMs;
    params.MaxRelocs = std::clamp<uint32>(config.MaxRelocs, 1, LOS_RING);
    params.RelocWindowMs = config.RelocWindowMs;
    params.SearchIntervalMs = config.SearchIntervalMs;
    params.MaxRays = std::max<uint32>(config.MaxRaysPerSearch, 1);
    params.RaysPerRefresh = std::max<uint32>(config.RaysPerSecond, 1);
    params.TimeoutMs = config.TimeoutMs;
    return params;
}

// The wait of one episode: the mean times a log-normal factor (sigma LOS_LATE_SIGMA, clamped), from a hash of the bot
// and the episode (no RNG state; the same episode always waits the same)
inline uint32 LosEpisodeLateMs(uint32 meanMs, uint64 guid, uint32 episode)
{
    uint64 const hash = SplitMix64(guid ^ SplitMix64(LOS_SALT_LATE + episode));
    float sum = 0.0f;
    for (uint32 part = 0; part < 4; ++part)
        sum += static_cast<float>((hash >> (16 * part)) & 0xFFFFu) / 65535.0f;

    // four uniforms: mean 2, sd 1/sqrt(3); about a standard normal
    float const z = (sum - 2.0f) * std::sqrt(3.0f);
    float const factor = std::clamp(std::exp(LOS_LATE_SIGMA * z), LOS_LATE_MIN_FACTOR, LOS_LATE_MAX_FACTOR);
    return static_cast<uint32>(static_cast<float>(meanMs) * factor + 0.5f);
}

// Does this episode's regain walk into the open (the first point of the path that sees, no cover)?
inline bool LosEpisodeIntoOpen(uint32 pct, uint64 guid, uint32 episode)
{
    return pct > 0 && SplitMix64(guid ^ SplitMix64(LOS_SALT_OPEN + episode)) % 100 < pct;
}

// The last relocations (G2e's regains, G2f's pillar steps): hold a point at least HoldMs, at most MaxRelocs per window
struct LosRelocRing
{
    std::array<uint32, LOS_RING> Ms{};
    uint32 Count = 0;  // relocations noted (the ring holds the last LOS_RING)

    uint32 LastMs() const { return Count ? Ms[(Count - 1) % LOS_RING] : 0; }

    uint32 InWindow(uint32 nowMs, uint32 windowMs) const
    {
        uint32 inWindow = 0;
        for (uint32 index = 0; index < std::min(Count, LOS_RING); ++index)
            inWindow += nowMs - Ms[index] < windowMs ? 1 : 0;

        return inWindow;
    }

    void Note(uint32 nowMs) { Ms[Count++ % LOS_RING] = nowMs; }
};

enum class LosGuard : uint8
{
    Ok,
    Hold,    // the last point is younger than HoldMs
    Window,  // MaxRelocs relocations in the window already
};

inline LosGuard LosRelocGuard(LosRelocRing const& ring, uint32 nowMs, LosParams const& params)
{
    if (ring.Count && nowMs - ring.LastMs() < params.HoldMs)
        return LosGuard::Hold;

    if (ring.InWindow(nowMs, params.RelocWindowMs) >= params.MaxRelocs)
        return LosGuard::Window;

    return LosGuard::Ok;
}

// A partner as the healer's refresh sees it (the group, read directly: "party member to heal" drops a partner out of
// LOS, PartyMemberToHeal::Check)
struct LosPartner
{
    uint64 Guid = 0;
    bool Alive = false;  // alive and no ghost
    float Yards = 0.0f;
    float HealthPct = 100.0f;
};

// The partners worth a ray, in the order to test them: alive, within LOS_PARTNER_YARDS, the most hurt first (a dead
// partner is never watched, so a healer never regains LOS to a corpse)
inline void LosPartnerOrder(std::vector<LosPartner>& partners)
{
    partners.erase(std::remove_if(partners.begin(), partners.end(), [](LosPartner const& partner)
                                  { return !partner.Alive || partner.Yards > LOS_PARTNER_YARDS; }),
                   partners.end());
    std::stable_sort(partners.begin(), partners.end(),
                     [](LosPartner const& a, LosPartner const& b) { return a.HealthPct < b.HealthPct; });
}

// The ms one refresh adds to partnerLosLostMs: the time since the last refresh while a partner was out of sight there
// (capped: a gap in the refreshes, a death or the gates, is not counted whole)
inline uint32 LosLostGapMs(bool partnerLost, uint32 lastRefreshMs, uint32 nowMs)
{
    if (!partnerLost || !lastRefreshMs)
        return 0;

    return std::min(nowMs - lastRefreshMs, LOS_LOST_GAP_CAP_MS);
}

// No regain before the gates open, nor in Ring of Valor's first RvIdleMs (the elevators carry the teams), counted
// from the bot's first look at the started match (late rather than early, as G2c's hops)
inline bool LosArenaIdle(bool inProgress, bool ringOfValor, uint32 sinceMs, uint32 nowMs, uint32 rvIdleMs)
{
    return !inProgress || (ringOfValor && nowMs - sinceMs < rvIdleMs);
}

// A ranged bot that knows the tactic does not walk into its target on lost LOS (ArenaTactics' branch, which runs only
// with a victim): the regain replaces it. The rest of ArenaTactics (the walk to the centre out of combat) stays.
inline bool LosParksArenaTactics(bool ranged, bool known, bool inCombat, bool hasVictim)
{
    return ranged && known && inCombat && hasVictim;
}

// The per-bot state (the value "los state"; the bot's own update, tactics show on the world thread)
struct LosBotState
{
    LosParams Params;
    // the sight refresh
    uint32 RefreshMs = 0;
    LosWant Want = LosWant::None;  // what the bot could not see at the last refresh
    uint64 SeeGuid = 0;            // that unit (raw)
    bool PartnerLost = false;      // a healer: an alive partner out of sight at the last refresh
    bool PlanHolds = false;        // the held point still does its job (refresh)
    // the episode (one lost unit until it is seen again)
    uint32 LostSinceMs = 0;
    uint32 Episode = 0;
    uint32 EpisodeLateMs = 0;
    bool EpisodeIntoOpen = false;
    bool GaveUp = false;
    uint32 RegainStartMs = 0;  // 0: not regaining
    // the trigger's answer for the action
    uint8 Due = 0;  // LosDue
    uint32 DueMs = 0;
    bool Guarding = false;  // the guards refuse now (counted once per refusal)
    // the arena's start
    uint32 ArenaInstance = 0;
    uint32 ArenaSinceMs = 0;
    LosRelocRing Relocs;
    // what it did (tactics show)
    uint32 Regains = 0;
    uint32 Regained = 0;
    uint32 Timeouts = 0;
    uint32 Guarded = 0;
    uint32 Searches = 0;
    uint32 SearchRays = 0;
    uint32 LastSearchRays = 0;
    uint32 CoverPoints = 0;
    uint32 PathPoints = 0;
    uint32 NoPoint = 0;
    uint32 PartnerLostMs = 0;
};

enum class LosEvent : uint8
{
    None,
    Lost,      // a new episode
    Changed,   // a new episode while a regain ran (another unit lost)
    Regained,  // seen again while a regain ran
    Timeout,   // the regain took TimeoutMs: given up until seen again
};

// One refresh's answer: want None = the bot sees everything it watches
inline LosEvent LosNoteSight(LosBotState& state, LosWant want, uint64 seeGuid, uint64 guid, uint32 nowMs)
{
    LosParams const& params = state.Params;
    bool const regaining = state.RegainStartMs != 0;
    if (want == LosWant::None)
    {
        state.Want = LosWant::None;
        state.SeeGuid = 0;
        state.LostSinceMs = 0;
        state.RegainStartMs = 0;
        state.GaveUp = false;
        if (!regaining)
            return LosEvent::None;

        ++state.Regained;
        return LosEvent::Regained;
    }

    if (!state.LostSinceMs || state.Want != want || state.SeeGuid != seeGuid)
    {
        state.Want = want;
        state.SeeGuid = seeGuid;
        state.LostSinceMs = std::max(nowMs, 1u);
        ++state.Episode;
        state.EpisodeLateMs = LosEpisodeLateMs(params.LateMs, guid, state.Episode);
        state.EpisodeIntoOpen = LosEpisodeIntoOpen(params.IntoOpenPct, guid, state.Episode);
        state.GaveUp = false;
        state.RegainStartMs = 0;
        return regaining ? LosEvent::Changed : LosEvent::Lost;
    }

    if (regaining && nowMs - state.RegainStartMs >= params.TimeoutMs)
    {
        state.RegainStartMs = 0;
        state.GaveUp = true;
        ++state.Timeouts;
        return LosEvent::Timeout;
    }

    return LosEvent::None;
}

enum class LosDue : uint8
{
    No,
    Search,   // look for a point (and move)
    Resume,   // walk on to the held point (stopped on the way)
    Guarded,  // a search is due, the guards refuse it
};

inline char const* LosDueName(LosDue due)
{
    switch (due)
    {
        case LosDue::Search:
            return "search";
        case LosDue::Resume:
            return "resume";
        case LosDue::Guarded:
            return "guarded";
        default:
            return "no";
    }
}

// Is a regain due now? (lastSearchMs: CoverBotState::LastSearchMs; atPoint and moving: the bot on its held point,
// the bot moving)
inline LosDue LosRegainDue(LosBotState const& state, bool atPoint, bool moving, uint32 lastSearchMs, uint32 nowMs)
{
    LosParams const& params = state.Params;
    if (!params.Known || state.Want == LosWant::None || !state.LostSinceMs || state.GaveUp)
        return LosDue::No;

    if (state.Want == LosWant::Partner && params.HealerStays)
        return LosDue::No;

    if (nowMs - state.LostSinceMs < state.EpisodeLateMs)
        return LosDue::No;

    if (state.RegainStartMs && state.PlanHolds)
        return !moving && !atPoint ? LosDue::Resume : LosDue::No;

    if (lastSearchMs && nowMs - lastSearchMs < params.SearchIntervalMs)
        return LosDue::No;

    return LosRelocGuard(state.Relocs, nowMs, params) == LosGuard::Ok ? LosDue::Search : LosDue::Guarded;
}

// The selftest (tactics selftest los; the offline harness runs the same checks)
template <typename Check>
void RunLosChecks(Check&& check)
{
    LosConfig const config;
    auto inputs = [](uint8 skill, uint8 effort = SKILL_GATE_FULL)
    {
        LosGateInputs in;
        in.Skill = skill;
        in.Knowledge = skill;
        in.Speed = skill;
        in.Wisdom = skill;
        in.Effort = effort;
        in.FullAbility = skill == 0;
        return in;
    };

    // the numbers by skill
    LosParams const full = ComputeLosParams(config, inputs(0));
    check(full.Known && full.LateMs == config.LateMsAtSkill99 && full.IntoOpenPct == config.IntoOpenPctAtSkill99 &&
              full.HidesFromCaster && !full.HealerStays,
          "full ability: known, fastest, never into the open, hides, never stays");
    LosParams const low = ComputeLosParams(config, inputs(20));
    check(!low.Known, "skill 20 below KnownFromSkill 25: not known");
    LosParams const mid = ComputeLosParams(config, inputs(50));
    check(mid.Known && !mid.HidesFromCaster && mid.LateMs < config.LateMsAtSkill1 &&
              mid.LateMs > config.LateMsAtSkill99 && mid.IntoOpenPct > 0 &&
              mid.IntoOpenPct < config.IntoOpenPctAtSkill1,
          "skill 50: known, late and into the open between the ends, no hiding from the caster");
    LosParams const high = ComputeLosParams(config, inputs(80));
    check(high.Known && high.HidesFromCaster && high.LateMs < mid.LateMs && high.IntoOpenPct <= mid.IntoOpenPct,
          "skill 80: hides from the caster, faster and less into the open than skill 50");
    check(ComputeLosParams(config, inputs(60, 10)).HealerStays && !ComputeLosParams(config, inputs(60, 30)).HealerStays,
          "effort below EffortMin: the healer stays");
    LosConfig wide = config;
    wide.MaxRelocs = 100;
    wide.RaysPerSecond = 0;
    LosParams const clamped = ComputeLosParams(wide, inputs(80));
    check(clamped.MaxRelocs == LOS_RING && clamped.RaysPerRefresh == 1,
          "MaxRelocs clamped to the ring, rays at least 1");

    // lateness: bounded, deterministic, about log-normal
    bool bounded = true;
    float sum = 0.0f;
    for (uint32 episode = 1; episode <= 2000; ++episode)
    {
        uint32 const late = LosEpisodeLateMs(1000, 0x1234 + episode * 7919ULL, episode);
        bounded = bounded && late >= 400 && late <= 2500;
        sum += static_cast<float>(late);
    }
    float const mean = sum / 2000.0f;
    check(bounded, "lateness within 0.4-2.5 x the mean");
    check(mean > 950.0f && mean < 1200.0f, "lateness mean about exp(sigma^2 / 2) x the mean: " + std::to_string(mean));
    check(LosEpisodeLateMs(1000, 77, 3) == LosEpisodeLateMs(1000, 77, 3) && LosEpisodeLateMs(0, 77, 3) == 0,
          "lateness deterministic, 0 at mean 0");

    // into the open: never at 0, always at 100, about the share between
    uint32 open = 0;
    bool never = true;
    bool always = true;
    for (uint32 episode = 1; episode <= 2000; ++episode)
    {
        open += LosEpisodeIntoOpen(25, 99, episode) ? 1 : 0;
        never = never && !LosEpisodeIntoOpen(0, 99, episode);
        always = always && LosEpisodeIntoOpen(100, 99, episode);
    }
    check(never && always, "into the open: never at 0 %, always at 100 %");
    check(open > 400 && open < 600, "into the open at 25 %: " + std::to_string(open) + " of 2000");

    // the relocation guards
    LosParams guards = high;
    LosRelocRing ring;
    check(LosRelocGuard(ring, 5000, guards) == LosGuard::Ok, "guard: no relocation yet");
    ring.Note(5000);
    check(LosRelocGuard(ring, 6000, guards) == LosGuard::Hold, "guard: the point is held 2.5 s");
    check(LosRelocGuard(ring, 7600, guards) == LosGuard::Ok, "guard: free after the hold");
    ring.Note(7600);
    ring.Note(10200);
    check(LosRelocGuard(ring, 13000, guards) == LosGuard::Window, "guard: 3 relocations in 10 s");
    check(LosRelocGuard(ring, 15100, guards) == LosGuard::Ok, "guard: the window moves on");
    for (uint32 k = 0; k < 20; ++k)
        ring.Note(20000 + 3000 * k);
    check(ring.Count == 23 && ring.LastMs() == 77000 && ring.InWindow(77000, 10000) == 4,
          "guard ring wraps, keeps the last relocations");

    // the partners: dead or far partners are never watched, the most hurt first
    std::vector<LosPartner> partners = {
        {1, true, 20.0f, 80.0f}, {2, false, 10.0f, 0.0f}, {3, true, 50.0f, 10.0f}, {4, true, 30.0f, 40.0f}};
    LosPartnerOrder(partners);
    check(partners.size() == 2 && partners[0].Guid == 4 && partners[1].Guid == 1,
          "partners: the dead and the far one dropped, the most hurt first");
    std::vector<LosPartner> dead = {{9, false, 5.0f, 0.0f}};
    LosPartnerOrder(dead);
    check(dead.empty(), "a dead partner: nothing to watch, so no regain");
    check(LosLostGapMs(true, 1000, 2000) == 1000 && LosLostGapMs(true, 1000, 9000) == LOS_LOST_GAP_CAP_MS &&
              LosLostGapMs(false, 1000, 2000) == 0 && LosLostGapMs(true, 0, 2000) == 0,
          "partner lost ms: the refresh gap, capped, only while lost");

    // the arena's start and the walk into the target
    check(LosArenaIdle(false, false, 0, 1000, 21000) && LosArenaIdle(true, true, 1000, 21000, 21000) &&
              !LosArenaIdle(true, true, 1000, 22000, 21000) && !LosArenaIdle(true, false, 1000, 1001, 21000),
          "idle before the gates and in Ring of Valor's first 21 s");
    check(LosParksArenaTactics(true, true, true, true) && !LosParksArenaTactics(false, true, true, true) &&
              !LosParksArenaTactics(true, false, true, true) && !LosParksArenaTactics(true, true, false, true),
          "ArenaTactics' walk parked for known ranged bots in combat only");

    // the episode
    LosBotState state;
    state.Params = high;
    state.Params.LateMs = 500;
    check(LosNoteSight(state, LosWant::Target, 42, 7, 10000) == LosEvent::Lost && state.Episode == 1 &&
              state.LostSinceMs == 10000 && state.EpisodeLateMs >= 200,
          "episode: lost starts one");
    check(LosRegainDue(state, false, false, 0, 10000 + state.EpisodeLateMs - 1) == LosDue::No,
          "due: not before the lateness");
    check(LosRegainDue(state, false, false, 0, 10000 + state.EpisodeLateMs) == LosDue::Search,
          "due: a search after the lateness");
    check(LosNoteSight(state, LosWant::Target, 42, 7, 11000) == LosEvent::None && state.Episode == 1,
          "episode: the same unit lost again is the same episode");
    uint32 const start = 10000 + LOS_RING * 1000;
    state.RegainStartMs = start;
    state.Relocs.Note(start);
    state.PlanHolds = true;
    check(LosRegainDue(state, false, false, start, start + 100) == LosDue::Resume &&
              LosRegainDue(state, false, true, start, start + 100) == LosDue::No &&
              LosRegainDue(state, true, false, start, start + 100) == LosDue::No,
          "due: resume to the held point only when stopped short of it");
    state.PlanHolds = false;
    check(LosRegainDue(state, false, false, start, start + 1000) == LosDue::No,
          "due: a failed point waits for the search interval");
    check(LosRegainDue(state, false, false, start, start + 2000) == LosDue::Guarded,
          "due: a new point inside the hold time is guarded");
    check(LosRegainDue(state, false, false, start, start + 2600) == LosDue::Search, "due: a new point after the hold");
    check(LosNoteSight(state, LosWant::Target, 42, 7, start + 6000) == LosEvent::Timeout && state.GaveUp &&
              state.Timeouts == 1 && !state.RegainStartMs,
          "episode: a regain of TimeoutMs times out");
    check(LosRegainDue(state, false, false, 0, start + 9000) == LosDue::No, "due: given up until seen again");
    check(LosNoteSight(state, LosWant::Target, 42, 7, start + 9000) == LosEvent::None && state.GaveUp,
          "episode: still lost, still given up");
    check(
        LosNoteSight(state, LosWant::None, 0, 7, start + 9500) == LosEvent::None && !state.GaveUp && !state.LostSinceMs,
        "episode: seen again ends it");
    LosNoteSight(state, LosWant::Partner, 43, 7, start + 10000);
    state.RegainStartMs = start + 12000;
    check(LosNoteSight(state, LosWant::Partner, 44, 7, start + 12500) == LosEvent::Changed && state.Episode == 3 &&
              !state.RegainStartMs,
          "episode: another partner lost is a new episode");
    state.RegainStartMs = start + 13000;
    check(LosNoteSight(state, LosWant::None, 0, 7, start + 14000) == LosEvent::Regained && state.Regained == 1,
          "episode: seen again during a regain counts it");
    LosBotState stays;
    stays.Params = ComputeLosParams(config, inputs(60, 10));
    LosNoteSight(stays, LosWant::Partner, 5, 7, 1000);
    check(LosRegainDue(stays, false, false, 0, 60000) == LosDue::No, "due: a low-effort healer stays");
    LosBotState unknown;
    unknown.Params = low;
    LosNoteSight(unknown, LosWant::Target, 5, 7, 1000);
    check(LosRegainDue(unknown, false, false, 0, 60000) == LosDue::No, "due: an unknown tactic never regains");

    // the cover point that sees again (Ring of Valor's table, all pillars up): the target behind the west pillar
    ArenaCoverTable const* rv = FindArenaCoverTable(COVER_MAP_RING_OF_VALOR);
    check(rv != nullptr, "Ring of Valor's table");
    if (!rv)
        return;

    auto los = [rv](CoverVec const& a, CoverVec const& b)
    { return !CoverSegmentBlocked(*rv, COVER_ALL_GATES, a, b, 0.0f); };
    CoverPillar const& west = rv->Pillars[1];
    CoverVec const self{west.X + 8.0f, west.Y, west.Z};
    CoverVec const target{west.X - 12.0f, west.Y, west.Z};
    CoverVec const caster{west.X + 2.0f, west.Y + 14.0f, west.Z};
    check(!los(CoverEye(self, COVER_EYE_YARDS), CoverEye(target, COVER_EYE_YARDS)), "the case: the target is hidden");
    CoverSearchParams search;
    search.RequireHidden = false;
    search.MaxRays = high.MaxRays;
    CoverSearchResult const seen =
        FindCoverPoint(*rv, COVER_ALL_GATES, self, target, &target, LOS_MAX_MOVE_YARDS, los, search);
    check(seen.Found && seen.Sees && seen.Moved <= LOS_MAX_MOVE_YARDS && seen.Rays <= search.MaxRays &&
              los(CoverEye(seen.Point, COVER_EYE_YARDS), CoverEye(target, COVER_EYE_YARDS)),
          "regain: a ring point that sees the target, within the walk and the rays");
    search.PreferHidden = true;
    CoverSearchResult const hidden =
        FindCoverPoint(*rv, COVER_ALL_GATES, self, caster, &target, LOS_MAX_MOVE_YARDS, los, search);
    check(hidden.Found && hidden.Hidden &&
              los(CoverEye(hidden.Point, COVER_EYE_YARDS), CoverEye(target, COVER_EYE_YARDS)),
          "regain at high skill: the point also hides from the caster");
}

#endif
