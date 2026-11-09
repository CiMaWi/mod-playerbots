/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_WORLDLOGRECORDER_H
#define _PLAYERBOT_WORLDLOGRECORDER_H

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "WorldLogFormat.h"

// WL8, the Player-free part of the recorder: a bot's rings of its last minutes (a position a second, the actions its
// non-combat and dead engines executed, its events), the triggers that make them a case, and the case's recorder rows.
// A push is O(1), formats nothing and allocates nothing: words are interned once (WordTable) and the rings hold ids.
// Only the bot's own thread pushes and dumps; RequestCase may come from any thread (an atomic). Header-only so
// tests/world-log-selftest.cpp checks the same code the server runs (on a virtual millisecond clock).
namespace WorldLogFormat
{
// Playerbots.WorldLog.Recorder* defaults and bounds; the trigger thresholds are fixed
constexpr uint32_t RECORDER_SECONDS = 300;
constexpr uint32_t RECORDER_ACTIONS = 128;
constexpr uint32_t RECORDER_EVENTS = 64;
constexpr uint32_t RECORDER_COOLDOWN_SECONDS = 600;
constexpr uint32_t MAX_RECORDER_SECONDS = 3600;
constexpr uint32_t MIN_RECORDER_ROWS = 1;
constexpr uint32_t MAX_RECORDER_ROWS = 4096;
constexpr uint32_t MAX_RECORDER_COOLDOWN_SECONDS = 86400;
constexpr uint32_t STILL_SECONDS = 300;
constexpr float STILL_YARDS = 3.0f;
constexpr uint32_t DEATH_WINDOW_SECONDS = 600;
constexpr uint32_t FAIL_STREAK = 10;
constexpr uint32_t POSITION_MS = 1000;
constexpr size_t ACTION_CACHE_SLOTS = 64;
constexpr size_t ACTION_CACHE_SHIFT = 4;
constexpr size_t WORD_CAPACITY = 4096;
constexpr uint16_t WORD_UNKNOWN = 0;
constexpr char const* WORD_UNKNOWN_TEXT = "?";
constexpr size_t RECORDER_ROW_RESERVE = 64;
// NewRpgStatus RPG_IDLE and RPG_REST (PlayerbotAIConfig.h; WorldLogRecorder.cpp checks them)
constexpr int32_t RECORDER_RPG_IDLE = 0;
constexpr int32_t RECORDER_RPG_REST = 7;
// The last-resort teleports (Honesty Teleport sources) that make a case; the managers' moves (mgr_*) do not
constexpr std::array<std::string_view, 3> LAST_RESORT_TELEPORTS = {"spirit_healer_fallback", "ghost_to_corpse",
                                                                   "repop_graveyard"};

struct RecorderSettings
{
    uint32_t seconds = RECORDER_SECONDS;  // 0 = recorder off
    uint32_t actions = RECORDER_ACTIONS;
    uint32_t events = RECORDER_EVENTS;
    uint32_t cooldownSeconds = RECORDER_COOLDOWN_SECONDS;
};

inline RecorderSettings ClampRecorderSettings(uint32_t seconds, uint32_t actions, uint32_t events,
                                              uint32_t cooldownSeconds)
{
    RecorderSettings settings;
    settings.seconds = std::min(seconds, MAX_RECORDER_SECONDS);
    settings.actions = std::clamp(actions, MIN_RECORDER_ROWS, MAX_RECORDER_ROWS);
    settings.events = std::clamp(events, MIN_RECORDER_ROWS, MAX_RECORDER_ROWS);
    settings.cooldownSeconds = std::min(cooldownSeconds, MAX_RECORDER_COOLDOWN_SECONDS);
    return settings;
}

// Words (action and trigger names, event types and sources, detail words) as 16-bit ids. The words live in slots
// fixed at construction, each written once under the mutex before its id is handed out, so Word() needs no lock for
// an id the caller got from Intern (on any thread, through the mutex). A full table gives WORD_UNKNOWN ("?").
class WordTable
{
public:
    explicit WordTable(size_t capacity = WORD_CAPACITY) : _words(std::max<size_t>(capacity, 1))
    {
        _words[WORD_UNKNOWN] = WORD_UNKNOWN_TEXT;
    }

    WordTable(WordTable const&) = delete;
    WordTable& operator=(WordTable const&) = delete;

    uint16_t Intern(std::string_view word)
    {
        std::lock_guard<std::mutex> guard(_mutex);
        auto const found = _ids.find(word);
        if (found != _ids.end())
            return found->second;

        if (_count >= _words.size())
        {
            ++_overflow;
            return WORD_UNKNOWN;
        }

        uint16_t const id = static_cast<uint16_t>(_count++);
        _words[id] = std::string(word);
        _ids.emplace(std::string_view(_words[id]), id);
        return id;
    }

    std::string_view Word(uint16_t id) const
    {
        return id < _words.size() ? std::string_view(_words[id]) : std::string_view(WORD_UNKNOWN_TEXT);
    }

    size_t Count() const
    {
        std::lock_guard<std::mutex> guard(_mutex);
        return _count;
    }

    uint64_t Overflow() const
    {
        std::lock_guard<std::mutex> guard(_mutex);
        return _overflow;
    }

private:
    mutable std::mutex _mutex;
    std::vector<std::string> _words;  // never resized: the keys below view into it
    std::map<std::string_view, uint16_t> _ids;
    size_t _count = 1;  // id 0 is "?"
    uint64_t _overflow = 0;
};

// Entries, oldest first in their ring; ms is the server's millisecond clock (getMSTime, wraps after 49 days)
struct RecorderPosition
{
    uint32_t ms = 0;
    float x = 0.0f;
    float y = 0.0f;
    uint16_t map = 0;
    int16_t rpg = 0;
};

struct RecorderAction
{
    uint32_t ms = 0;
    uint16_t name = WORD_UNKNOWN;
    uint16_t trigger = WORD_UNKNOWN;
    bool result = false;
};

struct RecorderEvent
{
    int64_t amount = 0;
    uint32_t ms = 0;
    uint32_t id = 0;
    uint16_t type = WORD_UNKNOWN;
    uint16_t source = WORD_UNKNOWN;
};

// A fixed ring, allocated once; Push overwrites the oldest entry when full
template <typename T>
class RecorderRing
{
public:
    void Allocate(size_t capacity)
    {
        _items.assign(capacity, T{});
        _next = 0;
        _count = 0;
    }

    void Push(T const& item)
    {
        if (_items.empty())
            return;

        _items[_next] = item;
        if (++_next == _items.size())
            _next = 0;
        _count = std::min(_count + 1, _items.size());
    }

    size_t Count() const { return _count; }
    size_t Capacity() const { return _items.size(); }
    T const& FromOldest(size_t index) const { return _items[(_next + _items.size() - _count + index) % _items.size()]; }

private:
    std::vector<T> _items;
    size_t _next = 0;
    size_t _count = 0;
};

// Why a case was dumped: the trigger row's name
enum class CaseTrigger : uint8_t
{
    None,
    Stuck,
    Abort,
    LegFailed,
    Death2,
    Teleport,
    Still,
    FailStreak,
    Mark
};

constexpr size_t CASE_TRIGGER_COUNT = 9;
constexpr std::array<std::string_view, CASE_TRIGGER_COUNT> CASE_TRIGGER_WORDS = {
    "none", "stuck", "abort", "leg_failed", "death2", "teleport", "still", "fail_streak", "mark"};

constexpr std::string_view CaseTriggerWord(CaseTrigger trigger)
{
    size_t const index = static_cast<size_t>(trigger);
    return index < CASE_TRIGGER_COUNT ? CASE_TRIGGER_WORDS[index] : std::string_view(EventLog::SOURCE_OTHER);
}

struct EventCase
{
    CaseTrigger trigger = CaseTrigger::None;
    std::string_view detail;
};

// The case an event makes by itself (a second death needs the recorder's memory): a Stuck row (detail = the cause
// word), a Travel abort or leg failure (detail = the word after "abort:" or "leg_failed:"), a last-resort teleport
// (detail = the site)
inline EventCase ClassifyEvent(std::string_view type, std::string_view source)
{
    constexpr std::string_view ABORT_PREFIX = "abort:";
    constexpr std::string_view LEG_FAILED_PREFIX = "leg_failed:";
    if (type == "Stuck")
        return {CaseTrigger::Stuck, source};

    if (type == "Travel")
    {
        if (source.substr(0, ABORT_PREFIX.size()) == ABORT_PREFIX)
            return {CaseTrigger::Abort, source.substr(ABORT_PREFIX.size())};
        if (source.substr(0, LEG_FAILED_PREFIX.size()) == LEG_FAILED_PREFIX)
            return {CaseTrigger::LegFailed, source.substr(LEG_FAILED_PREFIX.size())};
        return {};
    }

    if (type == "HonestyTeleport" &&
        std::find(LAST_RESORT_TELEPORTS.begin(), LAST_RESORT_TELEPORTS.end(), source) != LAST_RESORT_TELEPORTS.end())
        return {CaseTrigger::Teleport, source};

    return {};
}

// The "no movement" watch applies to a living bot on the ground (no flight, no transport) busy with an RPG status
// other than idle and rest
inline bool StillWatched(bool alive, bool flying, bool onTransport, int32_t rpgStatus)
{
    return alive && !flying && !onTransport && rpgStatus != RECORDER_RPG_IDLE && rpgStatus != RECORDER_RPG_REST;
}

// The ring entries' memory for these settings, without the Recorder object itself
inline size_t RingArrayBytes(RecorderSettings const& settings)
{
    if (!settings.seconds)
        return 0;

    return settings.seconds * sizeof(RecorderPosition) + settings.actions * sizeof(RecorderAction) +
           settings.events * sizeof(RecorderEvent);
}

// One bot's recorder. Unset until the first position push allocates it from the settings of that moment (seconds 0
// leaves it off for good; a reload affects later rings only).
class Recorder
{
public:
    struct ActionWords
    {
        uint16_t name = WORD_UNKNOWN;
        uint16_t trigger = WORD_UNKNOWN;
    };

    bool Unset() const { return _state == State::Unset; }
    bool On() const { return _state == State::On; }

    // Allocates the rings once; false when the settings turn the recorder off
    bool Allocate(RecorderSettings const& settings)
    {
        if (!Unset())
            return On();

        if (!settings.seconds)
        {
            _state = State::Off;
            return false;
        }

        _positions.Allocate(settings.seconds);
        _actions.Allocate(settings.actions);
        _events.Allocate(settings.events);
        _windowMs = settings.seconds * POSITION_MS;
        _cooldownMs = settings.cooldownSeconds * POSITION_MS;
        _state = State::On;
        return true;
    }

    // One compare between two positions; true while unset, so the first push allocates
    bool PositionDue(uint32_t nowMs) const
    {
        return _state == State::Unset || (_state == State::On && static_cast<int32_t>(nowMs - _nextPositionMs) >= 0);
    }

    // The guard against a second thread (an event from the world thread in its window): a push or dump that finds
    // the recorder busy is skipped
    bool Enter() { return !_busy.test_and_set(std::memory_order_acquire); }
    void Leave() { _busy.clear(std::memory_order_release); }

    // Any thread: ask for a case at the bot's next position push (a user mark, the console). why must be a literal.
    void RequestCase(char const* why) { _requested.store(why, std::memory_order_relaxed); }

    // A position, the mark request and the "no movement" watch. True when a case is due.
    bool PushPosition(WordTable& words, RecorderPosition const& position, bool watched)
    {
        _positions.Push(position);
        _nextPositionMs = position.ms + POSITION_MS;
        _here = position;

        if (_requested.load(std::memory_order_relaxed))
            if (char const* why = _requested.exchange(nullptr, std::memory_order_relaxed))
                return Fire(CaseTrigger::Mark, words.Intern(why), 0, position.ms);

        if (!watched)
        {
            _anchored = false;
            return false;
        }

        float const dx = position.x - _anchor.x;
        float const dy = position.y - _anchor.y;
        if (!_anchored || position.map != _anchor.map || dx * dx + dy * dy >= STILL_YARDS * STILL_YARDS)
        {
            _anchor = position;
            _anchored = true;
            return false;
        }

        uint32_t const stillMs = position.ms - _anchor.ms;
        if (stillMs < STILL_SECONDS * POSITION_MS)
            return false;

        _anchor = position;  // one case per still stretch
        return Fire(CaseTrigger::Still, WORD_UNKNOWN, stillMs / POSITION_MS, position.ms);
    }

    // The action's interned name and trigger from a small direct-mapped cache keyed by the action object and the
    // trigger name's hash; false on a miss (the caller interns and calls CacheAction)
    bool CachedAction(void const* action, size_t triggerHash, ActionWords& out) const
    {
        ActionCacheSlot const& slot = _cache[CacheSlot(action)];
        if (slot.action != action || slot.triggerHash != triggerHash)
            return false;

        out = slot.words;
        return true;
    }

    void CacheAction(void const* action, size_t triggerHash, ActionWords words)
    {
        _cache[CacheSlot(action)] = ActionCacheSlot{action, triggerHash, words};
    }

    // An executed action and its result; the same action failing FAIL_STREAK times in a row (no success, no other
    // action between) makes one case per streak. True when a case is due.
    bool PushAction(uint32_t nowMs, ActionWords words, bool result)
    {
        _actions.Push(RecorderAction{nowMs, words.name, words.trigger, result});
        if (result || words.name != _failName)
        {
            _failName = words.name;
            _failCount = 0;
        }

        if (result || ++_failCount != FAIL_STREAK)
            return false;

        return Fire(CaseTrigger::FailStreak, words.name, FAIL_STREAK, nowMs);
    }

    // An event of the bot (type and source as words). True when a case is due.
    bool PushEvent(WordTable& words, uint32_t nowMs, std::string_view type, std::string_view source, uint32_t id,
                   int64_t amount)
    {
        _events.Push(RecorderEvent{amount, nowMs, id, words.Intern(type), words.Intern(source)});
        if (type == "Death")
        {
            bool const second = _deathSeen && nowMs - _lastDeathMs < DEATH_WINDOW_SECONDS * POSITION_MS;
            uint32_t const apart = nowMs - _lastDeathMs;
            _deathSeen = true;
            _lastDeathMs = nowMs;
            return second && Fire(CaseTrigger::Death2, WORD_UNKNOWN, apart / POSITION_MS, nowMs);
        }

        EventCase const eventCase = ClassifyEvent(type, source);
        if (eventCase.trigger == CaseTrigger::None)
            return false;

        return Fire(eventCase.trigger, words.Intern(eventCase.detail), 0, nowMs);
    }

    // The due case as recorder rows (case, t, bot, kind, name, detail, a, x, y, map): the entries of the last
    // RecorderSeconds interleaved by time, oldest first (pos before event before action at the same ms), then one
    // trigger row at the latest position. Action and event rows carry the latest position at or before them (the
    // first one in the window before it). t = wallMs minus the entry's age on the server clock. Returns the rows;
    // 0 when no case was due.
    uint32_t AppendCase(std::string& out, WordTable const& words, uint64_t caseId, uint64_t bot, uint32_t nowMs,
                        uint64_t wallMs)
    {
        if (!_due)
            return 0;

        size_t const firstPosition = FirstInWindow(_positions, nowMs);
        size_t const firstAction = FirstInWindow(_actions, nowMs);
        size_t const firstEvent = FirstInWindow(_events, nowMs);
        size_t positionIndex = firstPosition;
        size_t actionIndex = firstAction;
        size_t eventIndex = firstEvent;
        RecorderPosition at = positionIndex < _positions.Count() ? _positions.FromOldest(positionIndex) : _here;
        out.reserve(out.size() + RECORDER_ROW_RESERVE * (_positions.Count() - firstPosition + _actions.Count() -
                                                         firstAction + _events.Count() - firstEvent + 1));
        uint32_t rows = 0;
        while (true)
        {
            uint32_t const positionAge = AgeAt(_positions, positionIndex, nowMs);
            uint32_t const eventAge = AgeAt(_events, eventIndex, nowMs);
            uint32_t const actionAge = AgeAt(_actions, actionIndex, nowMs);
            if (positionIndex < _positions.Count() && positionAge >= eventAge && positionAge >= actionAge)
            {
                at = _positions.FromOldest(positionIndex++);
                AppendRow(out, caseId, wallMs - positionAge, bot, "pos", {}, {}, at.rpg, at);
            }
            else if (eventIndex < _events.Count() && eventAge >= actionAge)
            {
                RecorderEvent const& event = _events.FromOldest(eventIndex++);
                std::string detail(words.Word(event.source));
                detail.push_back(';');
                AppendNumber(detail, event.amount);
                AppendRow(out, caseId, wallMs - eventAge, bot, "event", words.Word(event.type), detail, event.id, at);
            }
            else if (actionIndex < _actions.Count())
            {
                RecorderAction const& action = _actions.FromOldest(actionIndex++);
                AppendRow(out, caseId, wallMs - actionAge, bot, "action", words.Word(action.name),
                          words.Word(action.trigger), action.result ? 1 : 0, at);
            }
            else
                break;

            ++rows;
        }

        AppendRow(out, caseId, wallMs, bot, "trigger", CaseTriggerWord(_dueTrigger),
                  _dueDetail == WORD_UNKNOWN ? std::string_view() : words.Word(_dueDetail), _dueA, _here);
        _due = false;
        _dumped = true;
        _lastCaseMs = nowMs;
        ++_cases;
        return rows + 1;
    }

    bool CaseDue() const { return _due; }
    uint64_t Cases() const { return _cases; }
    uint64_t Suppressed() const { return _suppressed; }
    RecorderPosition const& Here() const { return _here; }

private:
    enum class State : uint8_t
    {
        Unset,
        Off,
        On
    };

    struct ActionCacheSlot
    {
        void const* action = nullptr;
        size_t triggerHash = 0;
        ActionWords words;
    };

    static size_t CacheSlot(void const* action)
    {
        return (reinterpret_cast<uintptr_t>(action) >> ACTION_CACHE_SHIFT) % ACTION_CACHE_SLOTS;
    }

    // At most one case per cooldown; a trigger inside it is counted and dropped
    bool Fire(CaseTrigger trigger, uint16_t detail, int64_t a, uint32_t nowMs)
    {
        if (_due || (_dumped && nowMs - _lastCaseMs < _cooldownMs))
        {
            ++_suppressed;
            return false;
        }

        _due = true;
        _dueTrigger = trigger;
        _dueDetail = detail;
        _dueA = a;
        return true;
    }

    // The age of the ring's entry at index (the oldest-first index), 0 past its end: a finished ring never wins
    template <typename T>
    static uint32_t AgeAt(RecorderRing<T> const& ring, size_t index, uint32_t nowMs)
    {
        return index < ring.Count() ? nowMs - ring.FromOldest(index).ms : 0;
    }

    template <typename T>
    size_t FirstInWindow(RecorderRing<T> const& ring, uint32_t nowMs) const
    {
        size_t index = 0;
        while (index < ring.Count() && nowMs - ring.FromOldest(index).ms > _windowMs)
            ++index;

        return index;
    }

    static void AppendRow(std::string& out, uint64_t caseId, uint64_t t, uint64_t bot, std::string_view kind,
                          std::string_view name, std::string_view detail, int64_t a, RecorderPosition const& at)
    {
        AppendNumber(out, caseId);
        AppendSeparator(out);
        AppendNumber(out, t);
        AppendSeparator(out);
        AppendNumber(out, bot);
        AppendSeparator(out);
        EventLog::AppendField(out, kind);
        AppendSeparator(out);
        EventLog::AppendField(out, name);
        AppendSeparator(out);
        EventLog::AppendField(out, detail);
        AppendSeparator(out);
        AppendNumber(out, a);
        AppendSeparator(out);
        AppendPosition(out, at.x);
        AppendSeparator(out);
        AppendPosition(out, at.y);
        AppendSeparator(out);
        AppendNumber(out, at.map);
        out.push_back(ROW_END);
    }

    State _state = State::Unset;
    std::atomic_flag _busy;  // clear (C++20)
    std::atomic<char const*> _requested{nullptr};
    RecorderRing<RecorderPosition> _positions;
    RecorderRing<RecorderAction> _actions;
    RecorderRing<RecorderEvent> _events;
    std::array<ActionCacheSlot, ACTION_CACHE_SLOTS> _cache{};
    uint32_t _windowMs = RECORDER_SECONDS * POSITION_MS;
    uint32_t _cooldownMs = RECORDER_COOLDOWN_SECONDS * POSITION_MS;
    uint32_t _nextPositionMs = 0;
    RecorderPosition _here;
    RecorderPosition _anchor;
    bool _anchored = false;
    uint16_t _failName = WORD_UNKNOWN;
    uint32_t _failCount = 0;
    bool _deathSeen = false;
    uint32_t _lastDeathMs = 0;
    bool _due = false;
    CaseTrigger _dueTrigger = CaseTrigger::None;
    uint16_t _dueDetail = WORD_UNKNOWN;
    int64_t _dueA = 0;
    bool _dumped = false;
    uint32_t _lastCaseMs = 0;
    uint64_t _cases = 0;
    uint64_t _suppressed = 0;
};
}  // namespace WorldLogFormat

#endif
