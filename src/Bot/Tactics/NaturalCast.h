/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_NATURALCAST_H
#define _PLAYERBOT_NATURALCAST_H

#include <algorithm>
#include <cmath>
#include <string>

#include "BotAttributes.h"
#include "Common.h"
#include "NaturalCastConfig.h"
#include "NaturalReaction.h"
#include "SplitMix64.h"

// Fake casts (G5e), the pure parts. A bot that knows fakes (Knowledge from KnownFromSkill) and starts a long cast a
// kick can interrupt looks at the enemy players it sees: one whose class carries a kick, within that kick's reach,
// facing or targeting the bot, is a kicker, unless the bot remembers seeing its kick used lately (from
// KickMemoryFromSkill; the kicks come from the combat log, OnPlayerSpellCast). With a kicker in view the bot fakes with
// a chance by Knowledge, at most every RepeatMs (Patience) and MaxPerMinute, and draws the cancel point of the bar
// from Patience with a log-normal spread; the pre-gate stepper cancels the cast at that instant if the kicker still
// sees it, else the cast finishes. Nothing tells a kicker it faces a fake (DT.12): its own memory of seen cancels is
// all it has. Every draw comes from a hash of the bot, the cast's start and the spell (no RNG state). The game side is
// in PlayerbotAI.cpp; the numbers are FakeConfig's.

// The player classes (SharedDefines.h Classes) whose kick is aimed at one enemy
constexpr uint8 FAKE_CLASS_WARRIOR = 1;       // Pummel, Shield Bash
constexpr uint8 FAKE_CLASS_HUNTER = 3;        // Silencing Shot (Marksmanship)
constexpr uint8 FAKE_CLASS_ROGUE = 4;         // Kick
constexpr uint8 FAKE_CLASS_PRIEST = 5;        // Silence (Shadow)
constexpr uint8 FAKE_CLASS_DEATH_KNIGHT = 6;  // Mind Freeze
constexpr uint8 FAKE_CLASS_SHAMAN = 7;        // Wind Shear
constexpr uint8 FAKE_CLASS_MAGE = 8;          // Counterspell

constexpr uint32 FAKE_MELEE_KICK_YARDS = 5;
constexpr uint32 FAKE_WIND_SHEAR_YARDS = 25;
constexpr uint32 FAKE_SPELL_KICK_YARDS = 30;
constexpr uint32 FAKE_SILENCING_SHOT_YARDS = 35;

// The skills the chance is lerped between (ChancePctAtSkill70/99)
constexpr uint32 FAKE_CHANCE_LOW_SKILL = 70;
constexpr uint32 FAKE_CHANCE_HIGH_SKILL = 99;
constexpr uint32 FAKE_MINUTE_MS = 60000;
// The cancel may come up to half a tick early (the error centred on the instant), never more than half of this
constexpr uint32 FAKE_MAX_TICK_MS = 100;
// The combat log's reach for a kick seen used (around its target)
constexpr float FAKE_KICK_SEEN_YARDS = 40.0f;

constexpr uint64 FAKE_CHANCE_SALT = 0x243F6A8885A308D3ULL;
constexpr uint64 FAKE_CANCEL_SALT = 0x13198A2E03707344ULL;
constexpr uint64 FAKE_REPEAT_SALT = 0xA4093822299F31D0ULL;

// The kick's reach of a class in yards (0: no kick aimed at one enemy). Paladins, druids and warlocks (the
// felhunter's Spell Lock is a pet's) have none.
inline uint32 FakeKickRangeYards(uint8 playerClass)
{
    switch (playerClass)
    {
        case FAKE_CLASS_WARRIOR:
        case FAKE_CLASS_ROGUE:
        case FAKE_CLASS_DEATH_KNIGHT:
            return FAKE_MELEE_KICK_YARDS;
        case FAKE_CLASS_SHAMAN:
            return FAKE_WIND_SHEAR_YARDS;
        case FAKE_CLASS_PRIEST:
        case FAKE_CLASS_MAGE:
            return FAKE_SPELL_KICK_YARDS;
        case FAKE_CLASS_HUNTER:
            return FAKE_SILENCING_SHOT_YARDS;
        default:
            return 0;
    }
}

// The Fake tactic's numbers for one bot (ComputeFakeParams, cached by PlayerbotAI::SetPvpSkill)
struct FakeParams
{
    bool Known = false;    // Knowledge reaches KnownFromSkill (skill 0: full ability)
    uint32 ChancePct = 0;  // the chance to fake a bait with a kicker in view
    uint32 MinCastMs = 1500;
    uint32 CancelPct = 55;       // the median cancel point, per cent of the bar (Patience)
    uint32 CancelSigmaPct = 15;  // its log-normal spread
    uint32 CancelMinMs = 300;
    uint32 CancelMinLeftMs = 250;
    uint32 RepeatMs = 7000;  // the median gap to the next fake (Patience)
    uint32 RepeatJitterPct = 30;
    uint32 MaxPerMinute = 6;
    uint32 KickerRangeYards = 30;  // the farthest kick counted (a class's reach is capped at it)
    uint32 KickerSlackYards = 3;
    bool KickerNeedFacing = true;
    bool RemembersKicks = true;  // Knowledge reaches KickMemoryFromSkill: kicks seen used take their kicker out
    uint32 KickSeenMs = 8000;
    uint32 BaitedWindowMs = 300;
    uint32 HealGuardPct = 70;
};

inline uint32 FakePct(float value) { return static_cast<uint32>(std::clamp(value, 0.0f, 100.0f) + 0.5f); }

inline float FakeLerp(uint32 atLow, uint32 atHigh, float position)
{
    return static_cast<float>(atLow) + (static_cast<float>(atHigh) - static_cast<float>(atLow)) * position;
}

// Knowledge carries whether the bot knows fakes, its chance and its memory of kicks (addon-level knowledge, DT.12);
// Patience the cancel point (patient bots go deeper into the bar) and the gap to the next fake. With the attribute
// defaults (abilities = skill, traits 50) it is the plain skill.
inline FakeParams ComputeFakeParams(uint8 skill, BotAttributeValues const& attributes, FakeConfig const& config)
{
    bool const full = skill == 0;
    uint32 const knowledge = full ? FAKE_CHANCE_HIGH_SKILL : attributes.Get(BotAttribute::Knowledge);
    float const patience =
        full ? 0.5f : static_cast<float>(std::min<uint8>(attributes.Get(BotAttribute::Patience), 100)) / 100.0f;
    float const chancePosition =
        std::clamp((static_cast<float>(knowledge) - static_cast<float>(FAKE_CHANCE_LOW_SKILL)) /
                       static_cast<float>(FAKE_CHANCE_HIGH_SKILL - FAKE_CHANCE_LOW_SKILL),
                   0.0f, 1.0f);

    FakeParams params;
    params.Known = knowledge >= config.KnownFromSkill;
    params.ChancePct = FakePct(FakeLerp(config.ChancePctAtSkill70, config.ChancePctAtSkill99, chancePosition));
    params.MinCastMs = config.MinCastMs;
    params.CancelPct =
        std::clamp(FakePct(FakeLerp(config.CancelPctAtPatience0, config.CancelPctAtPatience100, patience)), 1u, 99u);
    params.CancelSigmaPct = config.CancelSigmaPct;
    params.CancelMinMs = config.CancelMinMs;
    params.CancelMinLeftMs = config.CancelMinLeftMs;
    params.RepeatMs = static_cast<uint32>(
        std::max(0.0f, FakeLerp(config.RepeatMsAtPatience0, config.RepeatMsAtPatience100, patience) + 0.5f));
    params.RepeatJitterPct = std::min(config.RepeatJitterPct, 100u);
    params.MaxPerMinute = config.MaxPerMinute;
    params.KickerRangeYards = config.KickerRangeYards;
    params.KickerSlackYards = config.KickerRangeSlackYards;
    params.KickerNeedFacing = config.KickerNeedFacing != 0;
    params.RemembersKicks = knowledge >= config.KickMemoryFromSkill;
    params.KickSeenMs = config.KickSeenMs;
    params.BaitedWindowMs = config.BaitedWindowMs;
    params.HealGuardPct = std::min(config.HealGuardPct, 100u);
    return params;
}

// Why the bot fakes a cast or not
enum class FakeVerdict : uint8
{
    Fake,         // a kicker in view: the cast is cancelled at the drawn point
    Unknown,      // Knowledge below KnownFromSkill
    NotKickable,  // no kick interrupts the cast
    Short,        // shorter than MinCastMs, or no room between CancelMinMs and CancelMinLeftMs
    NoKicker,     // no enemy player in view could kick it
    KickSeen,     // every kicker in view was seen to use its kick within KickSeenMs (remembered)
    Repeat,       // too soon after the last fake
    Rate,         // MaxPerMinute fakes in the last minute
    Chance,       // the draw said no
    Urgent        // 13e-decide: a heal while its target (or, the only healer, any friend) is below HealGuardPct
};

inline char const* FakeVerdictName(FakeVerdict verdict)
{
    switch (verdict)
    {
        case FakeVerdict::Fake:
            return "fake";
        case FakeVerdict::Unknown:
            return "unknown";
        case FakeVerdict::NotKickable:
            return "not kickable";
        case FakeVerdict::Short:
            return "short";
        case FakeVerdict::NoKicker:
            return "no kicker";
        case FakeVerdict::KickSeen:
            return "kick seen";
        case FakeVerdict::Repeat:
            return "repeat";
        case FakeVerdict::Rate:
            return "rate";
        case FakeVerdict::Chance:
            return "chance";
        case FakeVerdict::Urgent:
            return "urgent";
    }

    return "?";
}

// The cast itself: Fake when it may be a bait (the kickers are looked at only then)
inline FakeVerdict FakeCastVerdict(FakeParams const& params, uint32 castMs, bool kickable)
{
    if (!params.Known)
        return FakeVerdict::Unknown;

    if (!kickable)
        return FakeVerdict::NotKickable;

    if (castMs < params.MinCastMs || castMs < params.CancelMinMs + params.CancelMinLeftMs)
        return FakeVerdict::Short;

    return FakeVerdict::Fake;
}

// The reach at which an enemy of this class could kick the bot (0: none): its kick's range, capped at
// KickerRangeYards, plus the slack
inline uint32 FakeKickerReachYards(uint8 playerClass, FakeParams const& params)
{
    uint32 const range = FakeKickRangeYards(playerClass);
    return range == 0 ? 0 : std::min(range, params.KickerRangeYards) + params.KickerSlackYards;
}

// The enemy in reach looks like it could kick: it targets the bot or faces it (or facing is not asked for)
inline bool FakeKickerSees(bool facing, bool targeting, FakeParams const& params)
{
    return targeting || facing || !params.KickerNeedFacing;
}

// A kicker in view counts unless the bot remembers its kick used lately
inline bool FakeKickerLive(bool kickSeenRecently, bool remembersKicks) { return !(kickSeenRecently && remembersKicks); }

// The fakes in the minute that began at minuteStartMs (0 once that minute is over)
inline uint32 FakeMinuteFakes(uint32 minuteStartMs, uint32 minuteFakes, uint32 nowMs)
{
    return minuteStartMs != 0 && nowMs - minuteStartMs < FAKE_MINUTE_MS ? minuteFakes : 0;
}

struct FakeInput
{
    uint32 CastMs = 0;
    bool Kickable = true;
    uint32 KickersSeen = 0;  // enemy players in reach, facing or targeting the bot
    uint32 KickersLive = 0;  // of them, those whose kick the bot does not remember used lately
    uint32 NowMs = 0;
    uint32 NextFakeAtMs = 0;  // 0: no gap
    uint32 MinuteStartMs = 0;
    uint32 MinuteFakes = 0;
    uint64 Hash = 0;  // FakeCastHash
    // 13e-decide: a heal, its target's health, the caster the group's only healer, the lowest friend's health
    bool Heal = false;
    uint32 HealTargetHpPct = 100;
    bool OnlyHealer = false;
    uint32 LowestFriendHpPct = 100;
};

// 13e-decide: a heal is urgent (never faked) while its target, or for the only healer any friend, is below the guard
inline bool FakeHealUrgent(FakeParams const& params, FakeInput const& input)
{
    return input.Heal && (input.HealTargetHpPct < params.HealGuardPct ||
                          (input.OnlyHealer && input.LowestFriendHpPct < params.HealGuardPct));
}

inline FakeVerdict DecideFake(FakeParams const& params, FakeInput const& input)
{
    FakeVerdict const cast = FakeCastVerdict(params, input.CastMs, input.Kickable);
    if (cast != FakeVerdict::Fake)
        return cast;

    if (input.KickersSeen == 0)
        return FakeVerdict::NoKicker;

    if (input.KickersLive == 0)
        return FakeVerdict::KickSeen;

    if (FakeHealUrgent(params, input))
        return FakeVerdict::Urgent;

    if (input.NextFakeAtMs != 0 && static_cast<int32>(input.NextFakeAtMs - input.NowMs) > 0)
        return FakeVerdict::Repeat;

    if (FakeMinuteFakes(input.MinuteStartMs, input.MinuteFakes, input.NowMs) >= params.MaxPerMinute)
        return FakeVerdict::Rate;

    if (UnitFromHash(SplitMix64(input.Hash ^ FAKE_CHANCE_SALT)) * 100.0 >= static_cast<double>(params.ChancePct))
        return FakeVerdict::Chance;

    return FakeVerdict::Fake;
}

// One hash per cast: the bot, the cast's start and the spell
inline uint64 FakeCastHash(uint64 botGuid, uint32 startMs, uint32 spellId)
{
    return SplitMix64((botGuid << 32) ^ (static_cast<uint64>(spellId) << 8) ^ startMs);
}

// The cancel's instant, in ms after the cast's start: CancelPct of the bar with a log-normal spread, between
// CancelMinMs and CancelMinLeftMs before the end
inline uint32 PickCancelMs(FakeParams const& params, uint32 castMs, uint64 hash)
{
    uint32 const latest = castMs > params.CancelMinLeftMs ? castMs - params.CancelMinLeftMs : 0;
    uint32 const earliest = std::min(params.CancelMinMs, latest);
    double const sigma = static_cast<double>(params.CancelSigmaPct) / 100.0;
    double const z = Probit(UnitFromHash(SplitMix64(hash ^ FAKE_CANCEL_SALT)));
    double const ms = static_cast<double>(castMs) * static_cast<double>(params.CancelPct) / 100.0 * std::exp(sigma * z);
    return static_cast<uint32>(std::clamp(ms + 0.5, static_cast<double>(earliest), static_cast<double>(latest)));
}

// The gap to the next fake: RepeatMs with an even jitter of RepeatJitterPct either way
inline uint32 NextFakeGapMs(FakeParams const& params, uint64 hash)
{
    double const u = UnitFromHash(SplitMix64(hash ^ FAKE_REPEAT_SALT));
    double const jitter = static_cast<double>(params.RepeatJitterPct) / 100.0;
    return static_cast<uint32>(std::max(0.0, static_cast<double>(params.RepeatMs) * (1.0 + jitter * (2.0 * u - 1.0))) +
                               0.5);
}

// The pre-gate stepper cancels at the update nearest the instant: up to half a tick early, so the error is centred
inline bool FakeCancelDue(uint32 cancelAtMs, uint32 nowMs, uint32 tickMs)
{
    return static_cast<int32>(cancelAtMs - nowMs) <= static_cast<int32>(std::min(tickMs, FAKE_MAX_TICK_MS) / 2);
}

// An enemy kick at the bot within windowMs after its fake cancel was baited
inline bool FakeBaited(uint32 lastCancelMs, uint32 nowMs, uint32 windowMs)
{
    return lastCancelMs != 0 && nowMs - lastCancelMs <= windowMs;
}

// The per-bot state (PlayerbotAI::GetFakeState; the bot's own update and, for the kicks seen, the casts on its map)
struct FakeBotState
{
    FakeParams Params;
    // The pending fake (SpellId 0: none): the bot's own cast and the cancel's instant
    uint32 SpellId = 0;
    uint32 CastStartMs = 0;
    uint32 CastMs = 0;
    uint32 CancelAtMs = 0;
    uint64 KickerGuid = 0;  // the kicker in view at the decision (raw guid), looked at again at the cancel
    uint8 KickerClass = 0;
    uint64 Hash = 0;
    // The pace
    uint32 NextFakeAtMs = 0;
    uint32 MinuteStartMs = 0;
    uint32 MinuteFakes = 0;
    uint32 LastCancelMs = 0;  // the last fake cancel not yet baited (tactics.kicksBaited)
    uint32 LastFakeMs = 0;    // the last fake cancel (tactics show)
    // What the bot did (tactics show)
    uint32 Decisions = 0;  // long kickable casts with a kicker in view
    uint32 Fakes = 0;
    uint32 Finished = 0;  // planned fakes let finish: the kicker gone or turned away, or its kick seen used
    uint32 Gone = 0;      // planned fakes whose cast ended before the cancel (kicked, interrupted, replaced)
    uint32 Baited = 0;
    uint32 KicksSeen = 0;
    FakeVerdict LastVerdict = FakeVerdict::NoKicker;
    uint32 LastCancelPct = 0;  // the last cancel's point, per cent of the bar
    int32 LastErrorMs = 0;     // the last cancel against its planned instant (late > 0)

    bool Pending() const { return SpellId != 0; }

    void ClearPending()
    {
        SpellId = 0;
        CancelAtMs = 0;
        KickerGuid = 0;
        KickerClass = 0;
    }

    // The cast is cancelled now: the pace moves on and the baited window opens
    void NoteCancel(uint32 nowMs, uint32 gapMs)
    {
        if (FakeMinuteFakes(MinuteStartMs, MinuteFakes, nowMs) == 0)
        {
            MinuteStartMs = std::max(nowMs, 1u);
            MinuteFakes = 0;
        }

        ++MinuteFakes;
        ++Fakes;
        NextFakeAtMs = std::max(nowMs + gapMs, 1u);
        LastCancelMs = std::max(nowMs, 1u);
        LastFakeMs = LastCancelMs;
        LastCancelPct = CastMs ? std::min((nowMs - CastStartMs) * 100 / CastMs, 100u) : 0;
        LastErrorMs = static_cast<int32>(nowMs - CancelAtMs);
        ClearPending();
    }
};

// tactics selftest fake (also the offline harness): the pure policy over many hashes
template <typename Check>
void RunFakeChecks(Check&& check)
{
    FakeConfig const config;
    auto const attributesAt = [](uint8 ability, uint8 patience)
    {
        BotAttributeValues values;
        for (uint32 index = 0; index < BOT_ATTRIBUTE_COUNT; ++index)
            values.Value[index] = index >= BOT_ABILITY_COUNT ? 50 : ability;

        values.Value[static_cast<uint32>(BotAttribute::Patience)] = patience;
        return values;
    };

    FakeParams const p99 = ComputeFakeParams(99, attributesAt(99, 50), config);
    FakeParams const p80 = ComputeFakeParams(80, attributesAt(80, 50), config);
    FakeParams const p70 = ComputeFakeParams(70, attributesAt(70, 50), config);
    FakeParams const p60 = ComputeFakeParams(60, attributesAt(60, 50), config);
    FakeParams const p0 = ComputeFakeParams(0, attributesAt(0, 50), config);
    FakeParams const impatient = ComputeFakeParams(99, attributesAt(99, 0), config);
    FakeParams const patient = ComputeFakeParams(99, attributesAt(99, 100), config);

    // Who knows fakes, and the chance by Knowledge
    check(p99.Known && p80.Known && p70.Known && !p60.Known, "fakes are known from skill 70");
    check(p0.Known && p0.ChancePct == config.ChancePctAtSkill99, "skill 0 is full ability");
    check(p70.ChancePct == config.ChancePctAtSkill70 && p99.ChancePct == config.ChancePctAtSkill99,
          "the chance is 40 % at skill 70 and 90 % at 99");
    check(p80.ChancePct > p70.ChancePct && p80.ChancePct < p99.ChancePct, "the chance rises with skill");
    check(p99.RemembersKicks && ComputeFakeParams(39, attributesAt(39, 50), config).RemembersKicks == false,
          "kicks seen used are remembered from skill 40");
    check(impatient.CancelPct == config.CancelPctAtPatience0 && patient.CancelPct == config.CancelPctAtPatience100,
          "the cancel point is 35 % of the bar at Patience 0 and 75 % at 100");
    check(impatient.RepeatMs == config.RepeatMsAtPatience0 && patient.RepeatMs == config.RepeatMsAtPatience100,
          "the gap is 4 s at Patience 0 and 10 s at 100");

    // The cast
    check(FakeCastVerdict(p99, 2500, true) == FakeVerdict::Fake, "a kickable 2.5 s cast may be a bait");
    check(FakeCastVerdict(p99, 1000, true) == FakeVerdict::Short, "a 1 s cast is no bait");
    check(FakeCastVerdict(p99, 2500, false) == FakeVerdict::NotKickable, "a cast no kick interrupts is no bait");
    check(FakeCastVerdict(p60, 2500, true) == FakeVerdict::Unknown, "skill 60 does not fake");
    FakeParams roomless = p99;
    roomless.MinCastMs = 100;
    check(FakeCastVerdict(roomless, 500, true) == FakeVerdict::Short, "no room between CancelMinMs and the end");

    // The kicker in view
    check(FakeKickerReachYards(FAKE_CLASS_ROGUE, p99) == 8 && FakeKickerReachYards(FAKE_CLASS_MAGE, p99) == 33 &&
              FakeKickerReachYards(FAKE_CLASS_SHAMAN, p99) == 28 && FakeKickerReachYards(FAKE_CLASS_HUNTER, p99) == 33,
          "kick reach: melee 5, Wind Shear 25, Counterspell 30, Silencing Shot capped at 30, plus 3");
    check(FakeKickerReachYards(2, p99) == 0 && FakeKickerReachYards(11, p99) == 0 && FakeKickerReachYards(9, p99) == 0,
          "paladins, druids and warlocks are no kickers");
    check(FakeKickerSees(false, true, p99) && FakeKickerSees(true, false, p99) && !FakeKickerSees(false, false, p99),
          "a kicker faces or targets the bot");
    FakeParams anyFacing = p99;
    anyFacing.KickerNeedFacing = false;
    check(FakeKickerSees(false, false, anyFacing), "KickerNeedFacing 0: any kicker in reach");
    check(FakeKickerLive(false, true) && !FakeKickerLive(true, true) && FakeKickerLive(true, false),
          "a kick seen used takes the kicker out only for a bot that remembers");

    // No fake without an observed kicker, over many casts; with one the share follows the chance
    uint32 const startMs = 100000;
    uint32 fakesWithout = 0;
    uint32 fakes99 = 0;
    uint32 fakes70 = 0;
    uint32 fakesUnknown = 0;
    uint32 const casts = 4000;
    for (uint32 index = 0; index < casts; ++index)
    {
        FakeInput input;
        input.CastMs = 2500;
        input.NowMs = startMs + index * 37;
        input.Hash = FakeCastHash(1000 + index % 17, input.NowMs, 116 + index % 5);
        input.KickersSeen = 0;
        input.KickersLive = 0;
        fakesWithout += DecideFake(p99, input) == FakeVerdict::Fake ? 1 : 0;
        input.KickersSeen = 1;
        input.KickersLive = 1;
        fakes99 += DecideFake(p99, input) == FakeVerdict::Fake ? 1 : 0;
        fakes70 += DecideFake(p70, input) == FakeVerdict::Fake ? 1 : 0;
        fakesUnknown += DecideFake(p60, input) == FakeVerdict::Fake ? 1 : 0;
    }

    check(fakesWithout == 0, "no fake without an observed kicker (" + std::to_string(casts) + " casts)");
    check(fakesUnknown == 0, "no fake below skill 70");
    check(fakes99 * 100 >= casts * (p99.ChancePct - 3) && fakes99 * 100 <= casts * (p99.ChancePct + 3),
          "skill 99 fakes about 90 % of the casts with a kicker (" + std::to_string(fakes99) + ")");
    check(fakes70 * 100 >= casts * (p70.ChancePct - 3) && fakes70 * 100 <= casts * (p70.ChancePct + 3),
          "skill 70 fakes about 40 % of them (" + std::to_string(fakes70) + ")");

    // Memory expiry: a kick seen used takes its kicker out for KickSeenMs, then it counts again
    OpponentMemory memory;
    uint32 const kicker = 4242;
    uint32 const seenMs = 200000;
    memory.NoteKick(kicker, 1766, seenMs);
    auto const live = [&](uint32 atMs, FakeParams const& params)
    { return FakeKickerLive(memory.KickSeenWithin(kicker, atMs, params.KickSeenMs), params.RemembersKicks); };
    check(!live(seenMs + 100, p99), "a kick seen 100 ms ago: the kicker is out");
    check(!live(seenMs + config.KickSeenMs, p99), "a kick seen KickSeenMs ago: still out");
    check(live(seenMs + config.KickSeenMs + 1, p99), "after KickSeenMs the kicker counts again");
    check(!memory.KickSeenWithin(7, seenMs + 100, config.KickSeenMs), "another enemy's kick is not this one's");
    memory.Expire(seenMs + config.KickSeenMs + 1, 120000, config.KickSeenMs);
    check(!memory.AnyKickSeenWithin(seenMs + config.KickSeenMs + 1, 60000), "Expire forgets the kick");
    FakeInput remembered;
    remembered.CastMs = 2500;
    remembered.KickersSeen = 1;
    remembered.KickersLive = 0;
    remembered.NowMs = startMs;
    check(DecideFake(p99, remembered) == FakeVerdict::KickSeen, "only a kicker whose kick was seen used: no fake");

    // Cancel times spread over the bar, inside their bounds; Patience moves them
    auto const cancelStats =
        [&](FakeParams const& params, uint32 castMs, double& mean, double& sd, uint32& low, uint32& high)
    {
        double sum = 0.0;
        double squares = 0.0;
        low = castMs;
        high = 0;
        uint32 const draws = 3000;
        for (uint32 index = 0; index < draws; ++index)
        {
            uint32 const ms = PickCancelMs(params, castMs, FakeCastHash(77 + index % 13, 5000 + index * 101, 133));
            sum += ms;
            squares += static_cast<double>(ms) * ms;
            low = std::min(low, ms);
            high = std::max(high, ms);
        }

        mean = sum / draws;
        sd = std::sqrt(std::max(0.0, squares / draws - mean * mean));
    };
    double mean = 0.0;
    double sd = 0.0;
    uint32 low = 0;
    uint32 high = 0;
    cancelStats(p99, 2500, mean, sd, low, high);
    check(low >= config.CancelMinMs && high <= 2500 - config.CancelMinLeftMs,
          "cancels stay between CancelMinMs and CancelMinLeftMs before the end");
    check(mean > 2500 * 0.50 && mean < 2500 * 0.62, "the mean cancel is near 55 % of the bar at Patience 50 (" +
                                                        std::to_string(static_cast<uint32>(mean)) + " ms)");
    check(sd > 120.0, "cancel times spread (sd " + std::to_string(static_cast<uint32>(sd)) + " ms)");
    check(high - low > 600, "cancel times cover a wide part of the bar");
    double meanImpatient = 0.0;
    double meanPatient = 0.0;
    cancelStats(impatient, 2500, meanImpatient, sd, low, high);
    cancelStats(patient, 2500, meanPatient, sd, low, high);
    check(meanImpatient + 500.0 < meanPatient, "patient bots cancel later in the bar");
    check(PickCancelMs(p99, 550, 12345) >= 300 && PickCancelMs(p99, 550, 12345) <= 300,
          "a bar with just enough room cancels at its only point");

    // The pace: the gap and the minute cap
    uint32 gapLow = UINT32_MAX;
    uint32 gapHigh = 0;
    for (uint32 index = 0; index < 1000; ++index)
    {
        uint32 const gap = NextFakeGapMs(p99, FakeCastHash(3, index * 13, 9));
        gapLow = std::min(gapLow, gap);
        gapHigh = std::max(gapHigh, gap);
    }

    check(gapLow >= p99.RepeatMs * 70 / 100 && gapHigh <= p99.RepeatMs * 130 / 100 + 1 && gapHigh - gapLow > 2000,
          "the gap to the next fake is RepeatMs +- 30 %");
    FakeBotState state;
    state.Params = p99;
    state.SpellId = 116;
    state.CastStartMs = startMs;
    state.CastMs = 2500;
    state.CancelAtMs = startMs + 1400;
    state.NoteCancel(startMs + 1410, 7000);
    check(!state.Pending() && state.Fakes == 1 && state.LastCancelPct == 56 && state.LastErrorMs == 10,
          "a cancel ends the pending fake and keeps its point and error");
    FakeInput again;
    again.CastMs = 2500;
    again.KickersSeen = 1;
    again.KickersLive = 1;
    again.NextFakeAtMs = state.NextFakeAtMs;
    again.MinuteStartMs = state.MinuteStartMs;
    again.MinuteFakes = state.MinuteFakes;
    again.NowMs = startMs + 3000;
    check(DecideFake(p99, again) == FakeVerdict::Repeat, "no second fake within the gap");
    again.NowMs = state.NextFakeAtMs;
    again.MinuteFakes = p99.MaxPerMinute;
    check(DecideFake(p99, again) == FakeVerdict::Rate, "no fake past MaxPerMinute in the minute");
    again.NowMs = state.MinuteStartMs + FAKE_MINUTE_MS;
    check(FakeMinuteFakes(again.MinuteStartMs, again.MinuteFakes, again.NowMs) == 0, "the minute's count resets");

    // The cancel's instant at the pre-gate and the baited window
    check(FakeCancelDue(1000, 1000, 40) && FakeCancelDue(1000, 980, 40) && !FakeCancelDue(1000, 979, 40),
          "the cancel comes up to half a tick early");
    check(!FakeCancelDue(1000, 900, 500) && FakeCancelDue(1000, 950, 500), "half a tick is capped at 50 ms");
    check(FakeCancelDue(5, UINT32_MAX - 10, 20) == false && FakeCancelDue(UINT32_MAX - 5, 3, 20),
          "the instant survives the clock's wrap");
    check(FakeBaited(1000, 1300, 300) && !FakeBaited(1000, 1301, 300) && !FakeBaited(0, 100, 300),
          "a kick within 300 ms after the cancel is baited");

    // 13e-decide (HealGuardPct): no fake heal while it is urgent
    {
        FakeInput heal;
        heal.CastMs = 2500;
        heal.KickersSeen = 1;
        heal.KickersLive = 1;
        heal.NowMs = 100000;
        heal.Heal = true;
        heal.HealTargetHpPct = 60;
        bool const lowTarget = DecideFake(p99, heal) == FakeVerdict::Urgent;
        heal.HealTargetHpPct = 70;
        bool const atGuard = DecideFake(p99, heal) != FakeVerdict::Urgent;
        heal.HealTargetHpPct = 90;
        heal.LowestFriendHpPct = 50;
        heal.OnlyHealer = true;
        bool const lowFriend = DecideFake(p99, heal) == FakeVerdict::Urgent;
        heal.OnlyHealer = false;
        bool const otherHealer = DecideFake(p99, heal) != FakeVerdict::Urgent;
        heal.Heal = false;
        heal.HealTargetHpPct = 10;
        heal.OnlyHealer = true;
        bool const damage = DecideFake(p99, heal) != FakeVerdict::Urgent;
        check(lowTarget && atGuard && lowFriend && otherHealer && damage && p99.HealGuardPct == 70,
              "no fake heal on a target below 70 %, nor as the only healer with a friend below 70 %; damage casts as "
              "before");
    }
}

// The game side (PlayerbotAI.cpp), from PlayerbotsPlayerScript::OnPlayerSpellCast on the caster's map thread: a kick
// aimed at one enemy goes into the kick memory of the bots on that map that see it (FAKE_KICK_SEEN_YARDS around its
// target, the caster's enemies, the Fake tactic on here) and counts tactics.kicksBaited on a target that just faked
class Player;
class Spell;
void NoteFakeKickCast(Player* caster, Spell* spell);

#endif
