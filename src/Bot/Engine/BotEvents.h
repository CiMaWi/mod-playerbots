/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_BOTEVENTS_H
#define _PLAYERBOT_BOTEVENTS_H

#include <algorithm>
#include <cstdint>
#include <string>

// The bot's scheduler (questing WQ56): when a bot's AI looks next. A bot sleeps until something wakes it:
//  - an event: what happened to it or around it (the server's hooks in BotEvents.cpp, the state edges its own update
//    sees) is noted as a bit, and its next update turns the bits into a wake;
//  - a timer: the end of what it waits for and can foresee (its cast, its global cooldown, a cooldown, its energy);
//  - the fallback: the regular check, for everything no event tells (an enemy that moved, a ground effect).
// What the AI then does is decided as before, by the engines' triggers and their relevance: an event says when to
// look, never what to do. A check goes on while its actions take no time (PlayerbotAI::RunWorkChain).
// Pure rules: no bots, no server, so tests/bot-events-selftest.cpp drives them.
namespace BotEvents
{
enum Kind : std::uint32_t
{
    CombatBegin = 1u << 0,  // the bot entered combat (its update's edge)
    CombatEnd = 1u << 1,    // it left combat
    Damage = 1u << 2,       // it took damage
    AuraOn = 1u << 3,       // another unit's aura came onto it (a debuff, a control, a buff)
    AuraOwn = 1u << 4,      // an aura of its own came onto it (a proc, its own buff)
    AuraOff = 1u << 5,      // an aura left it (a control's end, a buff run out)
    AuraGone = 1u << 6,     // an aura it had put on another unit ended (a damage-over-time effect, a control)
    EnemyCast = 1u << 7,    // a unit it fights began a spell, or one began a spell at it
    AllyHurt = 1u << 8,     // a group member's health fell through a band, or it died
    Kill = 1u << 9,         // a unit it fought died (its kill, its group's, its kill credit)
    Hit = 1u << 10,         // its own hit landed (a rage user's rage comes with it)
    CastEnd = 1u << 11,     // its cast or channel ended: done, failed or interrupted (the edge)
    MoveEnd = 1u << 12,     // it stopped moving (the edge)
    VictimGone = 1u << 13,  // the unit it attacked is no longer its victim (the edge)
    Packet = 1u << 14,      // a packet it waits for came (the loot window)
    Died = 1u << 15,        // it died
    KindEnd = 1u << 16
};

// What ends a wait an action asked for too (a meal, a pause), unless the bot casts: as upstream's combat edges did
constexpr std::uint32_t URGENT = CombatBegin | CombatEnd | Died;
// What comes unforeseen: the bot looks one reaction time after it. The rest (the end of its own action, what it
// waits for, what it has watched coming) wakes it at once
constexpr std::uint32_t SURPRISE = Damage | AuraOn | EnemyCast | AllyHurt;

// The reaction time to a surprise where no reaction model times the bot (NaturalReaction.h does in its contexts,
// per event; there the wake is at once): a simple visual reaction, times the bot's skill stretch
constexpr std::uint32_t REACT_MIN_MS = 150;
constexpr std::uint32_t REACT_MAX_MS = 350;
// The regular check in combat outside the natural cadence's contexts (it was 500 ms as the only wake)
constexpr std::uint32_t FALLBACK_COMBAT_MS = 1000;
// A standing bot with work whose action neither took time nor named its wait looks again this soon
constexpr std::uint32_t STEP_CHECK_MS = 250;
// A timer's wake is never sooner than this (a wait estimated a little short does not spin)
constexpr std::uint32_t TIMER_MIN_MS = 50;
// A group hears of a member's damage when its health falls through one of these bands (percent of the maximum)
constexpr std::uint32_t HEALTH_BAND_PCT = 10;
// A check goes on at most this many passes
constexpr std::uint32_t CHAIN_PASSES = 6;

struct Wake
{
    bool any = false;
    bool urgent = false;     // the wait an action asked for ends too
    std::uint32_t inMs = 0;  // the bot looks this long after its update saw the events
};

// The wake a set of events asks for. reactMs: the bot's reaction time to a surprise, 0 where a model times it.
// One foreseen event among them and the bot looks at once
inline Wake WakeFor(std::uint32_t events, std::uint32_t reactMs)
{
    Wake wake;
    if (!events)
        return wake;
    wake.any = true;
    wake.urgent = (events & URGENT) != 0;
    wake.inMs = (events & ~SURPRISE) ? 0 : reactMs;
    return wake;
}

// The delay to the next check after a wake: never before the wait an action asked for is over, never later than the
// check already due
inline std::uint32_t WakeDelayMs(std::uint32_t nextMs, std::uint32_t holdMs, std::uint32_t inMs)
{
    return std::min(nextMs, std::max(holdMs, inMs));
}

// The sleep after a check: the first timer, or the fallback when none comes by then (the fallback gets the bot's
// stagger on top, a timer is exact). timerMs: UINT32_MAX for none
inline std::uint32_t SleepMs(std::uint32_t fallbackMs, std::uint32_t timerMs, bool& byTimer)
{
    byTimer = timerMs <= fallbackMs;
    return byTimer ? std::max(timerMs, TIMER_MIN_MS) : fallbackMs;
}

// A hit that takes the health through a band's edge, or to 0
inline bool CrossesHealthBand(std::uint32_t health, std::uint32_t damage, std::uint32_t maxHealth)
{
    if (!maxHealth || !damage || !health)
        return false;
    if (damage >= health)
        return true;
    std::uint64_t const before = static_cast<std::uint64_t>(health) * 100 / maxHealth / HEALTH_BAND_PCT;
    std::uint64_t const after = static_cast<std::uint64_t>(health - damage) * 100 / maxHealth / HEALTH_BAND_PCT;
    return before != after;
}

// The wait for a power that comes with time (energy): 0 when it is there or does not come
inline std::uint32_t PowerWaitMs(std::uint32_t have, std::uint32_t need, float perSecond)
{
    if (have >= need || perSecond <= 0.0f)
        return 0;
    return static_cast<std::uint32_t>(static_cast<float>(need - have) * 1000.0f / perSecond) + TIMER_MIN_MS;
}

// Whether a check goes on with another pass. fresh: the pass ran an action other than the pass before it, or one
// that asked to go on; tookTime: the action left a wait, a cast, a global cooldown, or out of combat a move
inline bool ChainGoesOn(std::uint32_t pass, bool fresh, bool tookTime)
{
    return fresh && !tookTime && pass < CHAIN_PASSES;
}

inline char const* Name(std::uint32_t bit)
{
    switch (bit)
    {
        case CombatBegin:
            return "combat";
        case CombatEnd:
            return "combat end";
        case Damage:
            return "damage";
        case AuraOn:
            return "aura";
        case AuraOwn:
            return "own aura";
        case AuraOff:
            return "aura off";
        case AuraGone:
            return "aura gone";
        case EnemyCast:
            return "enemy cast";
        case AllyHurt:
            return "ally hurt";
        case Kill:
            return "kill";
        case Hit:
            return "hit";
        case CastEnd:
            return "cast end";
        case MoveEnd:
            return "move end";
        case VictimGone:
            return "victim gone";
        case Packet:
            return "packet";
        case Died:
            return "died";
        default:
            return "?";
    }
}

// "damage+aura", "none" for 0
inline std::string Names(std::uint32_t events)
{
    std::string out;
    for (std::uint32_t bit = 1; bit < KindEnd; bit <<= 1)
    {
        if (!(events & bit))
            continue;
        if (!out.empty())
            out += '+';
        out += Name(bit);
    }
    return out.empty() ? "none" : out;
}

// What woke the bot's last check (the watch's idle line, the scheduler's counts)
enum class Cause : std::uint8_t
{
    Fallback,  // the regular check
    Event,
    Timer,
    Step,  // the standing work's short check
    Hold   // the end of a wait an action asked for
};

inline char const* CauseName(Cause cause)
{
    switch (cause)
    {
        case Cause::Event:
            return "event";
        case Cause::Timer:
            return "timer";
        case Cause::Step:
            return "step";
        case Cause::Hold:
            return "wait";
        default:
            return "regular";
    }
}
}  // namespace BotEvents

// The server's hooks that note events on bots (BotEvents.cpp)
void AddBotEventScripts();

#endif
