/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_VISITLIFECYCLESELFTEST_H
#define _PLAYERBOT_VISITLIFECYCLESELFTEST_H

#include <array>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

#include "VisitLifecycle.h"

// The lifecycle harness of the mailbox and bank visits (VisitLifecycle.h), run by tests/visit-lifecycle-selftest.cpp.
// A model of one bot's two visit slots, wired as the live code wires the shared rules: the watch in every AI update
// (PlayerbotAI::UpdateAI), the stepper in the out-of-combat engine (StepMailVisit, StepBankVisit), the ends before a
// far teleport's or a reset's AfterReset, at a near teleport and at a logout. Every state is crossed with every event,
// then seeded random sequences look for a counterexample to the invariant; the wiring before WB7c must yield one.
namespace BankMail
{
namespace VisitLifecycleDetail
{
constexpr uint32_t UPDATE_MS = 1000;      // one AI update
constexpr uint32_t MAIL_LIMIT_MS = 6000;  // the visits' deadlines in the model
constexpr uint32_t BANK_LIMIT_MS = 4000;
constexpr uint32_t SEARCH_SEED = 20261113;
constexpr uint32_t SEARCH_SEQUENCES = 20000;
constexpr uint32_t SEARCH_LENGTH = 40;
constexpr uint32_t UPDATE_SHARE_PERCENT = 40;  // of the search's events

enum class Event : uint8_t
{
    StartMail,  // the GM command (a refused start writes nothing)
    StartBank,
    Update,       // one AI update: the time moves on by UPDATE_MS
    CombatEnter,  // the bot enters combat (it may stay in it)
    CombatLeave,
    Die,
    Revive,
    FarTeleport,
    NearTeleport,  // the bot lands elsewhere on the same map (a test's tele, a hearth on the continent)
    Reset,
    MapChange,  // the bot comes to another map, or back
    Logout,
    Login,
    ReachSpot,  // the stepper finds nothing left to do at the mailbox or banker
    LoseSpot,   // the mailbox or banker is not there
    Count
};
constexpr size_t EVENT_COUNT = static_cast<size_t>(Event::Count);

inline char const* EventWord(Event event)
{
    static char const* const words[EVENT_COUNT] = {"start_mail",    "start_bank", "update",     "combat_enter",
                                                   "combat_leave",  "die",        "revive",     "far_teleport",
                                                   "near_teleport", "reset",      "map_change", "logout",
                                                   "login",         "reach_spot", "lose_spot"};
    return words[static_cast<size_t>(event)];
}

// Which hooks the live code has: today's, the wiring before WB7c (no watch outside the engine, AfterReset and the
// AI's deletion drop a visit without a row, the mail command did not look at a bank visit), or the one before WBL2 (a
// near teleport left the visit running: the walk went on from wherever the bot landed, up to the 15-minute deadline,
// and held its one visit slot; WBL2 run 1, a trip visit under a test hold teleported to Orgrimmar)
struct Wiring
{
    bool watchEveryUpdate = true;
    bool endBeforeDrop = true;
    bool oneAtATime = true;
    bool endAtNearTeleport = true;
};
constexpr Wiring TODAY{true, true, true, true};
constexpr Wiring BEFORE_WB7C{false, false, false, false};
constexpr Wiring BEFORE_WBL2{true, true, true, false};

struct Slot
{
    bool runs = false;
    uint32_t id = 0;
    uint32_t startedMs = 0;
    uint32_t limitMs = 0;
    uint32_t mapId = 0;
    bool atSpot = false;
    bool spotLost = false;
    bool displaced = false;  // a teleport took the bot off the walk the visit planned from its start
};

class BotModel
{
public:
    explicit BotModel(Wiring wiring) : _wiring(wiring) {}

    void Apply(Event event)
    {
        switch (event)
        {
            case Event::StartMail:
                Start(VisitKind::Mail);
                break;
            case Event::StartBank:
                Start(VisitKind::Bank);
                break;
            case Event::Update:
                Update();
                break;
            case Event::CombatEnter:
                _combat = _online && _alive;
                break;
            case Event::CombatLeave:
                _combat = false;
                break;
            case Event::Die:
                if (!_online)
                    break;
                _alive = false;
                _combat = false;
                break;
            case Event::Revive:
                _alive = true;
                break;
            case Event::FarTeleport:
                if (!_online)
                    break;
                EndOrDrop(VisitEnd::Teleport);
                _mapId ^= 1;
                break;
            case Event::NearTeleport:
                if (_online)
                    EndAtNearTeleport();
                break;
            case Event::Reset:
                if (_online)
                    EndOrDrop(VisitEnd::Reset);
                break;
            case Event::MapChange:
                if (_online)
                    _mapId ^= 1;
                break;
            case Event::Logout:
                if (!_online)
                    break;
                EndOrDrop(VisitEnd::Logout);
                _online = false;
                _combat = false;
                break;
            case Event::Login:
                _online = true;
                break;
            case Event::ReachSpot:
                for (Slot& slot : _slots)
                    slot.atSpot = slot.runs;
                break;
            case Event::LoseSpot:
                for (Slot& slot : _slots)
                    slot.spotLost = slot.runs;
                break;
            default:
                break;
        }
        CheckNow();
    }

    // The bot comes back online, alive and out of combat, and updates until every visit has had time to end
    void Drain()
    {
        Apply(Event::Login);
        Apply(Event::Revive);
        Apply(Event::CombatLeave);
        for (uint32_t i = 0; i * UPDATE_MS <= MAIL_LIMIT_MS + BANK_LIMIT_MS && Runs(); ++i)
            Apply(Event::Update);
        if (Runs())
            Fault("a visit never ends");
        for (size_t id = 0; id < _rows.size(); ++id)
            if (_rows[id] != 1)
                Fault("visit " + std::to_string(id) + " wrote " + std::to_string(_rows[id]) + " rows");
    }

    bool Runs() const { return _slots[0].runs || _slots[1].runs; }
    std::string const& FaultText() const { return _fault; }
    char const* LastRefusal() const { return _lastRefusal; }
    char const* LastWord() const { return _lastWord; }
    uint32_t Ended(VisitKind kind, VisitEnd end) const
    {
        return _ended[static_cast<size_t>(kind)][static_cast<size_t>(end)];
    }

private:
    Slot& SlotOf(VisitKind kind) { return _slots[static_cast<size_t>(kind)]; }
    uint32_t LimitOf(VisitKind kind) const { return kind == VisitKind::Mail ? MAIL_LIMIT_MS : BANK_LIMIT_MS; }

    // The GM command and Begin*Visit (BankMailCommands.cpp, BankCommands.cpp); before WB7c the mail side did not look
    // at a bank visit
    void Start(VisitKind kind)
    {
        _lastRefusal = nullptr;
        if (!_online || !_alive)
            return;
        bool const bankSeen = _wiring.oneAtATime || kind == VisitKind::Bank;
        _lastRefusal = VisitStartRefusal(kind, _slots[0].runs, bankSeen && _slots[1].runs);
        if (_lastRefusal)
            return;
        Slot& slot = SlotOf(kind);
        slot = Slot{};
        slot.runs = true;
        slot.id = static_cast<uint32_t>(_rows.size());
        slot.startedMs = _nowMs;
        slot.limitMs = LimitOf(kind);
        slot.mapId = _mapId;
        _rows.push_back(0);
        ++_started[static_cast<size_t>(kind)];
    }

    // PlayerbotAI::UpdateAI: the watch (BankMail::WatchVisits) whatever engine runs, then the out-of-combat engine's
    // stepper, which watches too (StepMailVisit, StepBankVisit)
    void Update()
    {
        if (!_online)
            return;
        _nowMs += UPDATE_MS;
        for (VisitKind kind : {VisitKind::Mail, VisitKind::Bank})
        {
            Slot& slot = SlotOf(kind);
            if (slot.runs && _wiring.watchEveryUpdate)
                if (std::optional<VisitEnd> const end = WatchVisit(WatchIn(slot)))
                    End(kind, *end);
            if (!slot.runs || !_alive || _combat)
                continue;
            if (std::optional<VisitEnd> const end = StepperWatch(slot))
                End(kind, *end);
            else if (slot.spotLost)
                End(kind, VisitEnd::NoSpot);
            else if (slot.atSpot)
                End(kind, VisitEnd::Done);
        }
    }

    VisitWatchIn WatchIn(Slot const& slot) const
    {
        return VisitWatchIn{_alive, _combat, slot.mapId == _mapId, _nowMs - slot.startedMs, slot.limitMs};
    }

    // Before WB7c the stepper checked the deadline and then the map (a map change counted as no spot)
    std::optional<VisitEnd> StepperWatch(Slot const& slot) const
    {
        if (_wiring.watchEveryUpdate)
            return WatchVisit(WatchIn(slot));
        if (_nowMs - slot.startedMs >= slot.limitMs)
            return VisitEnd::Timeout;
        if (slot.mapId != _mapId)
            return VisitEnd::NoSpot;
        return std::nullopt;
    }

    // A far teleport, a reset or a logout: today the visit ends with its row first; before WB7c AfterReset (or the
    // AI's deletion) dropped it
    void EndOrDrop(VisitEnd end)
    {
        for (VisitKind kind : {VisitKind::Mail, VisitKind::Bank})
        {
            if (!SlotOf(kind).runs)
                continue;
            if (_wiring.endBeforeDrop)
                End(kind, end);
            else
                SlotOf(kind) = Slot{};
        }
    }

    // PlayerbotAI::HandleTeleportAck's near branch: today the visit ends with its row; before WBL2 it ran on
    void EndAtNearTeleport()
    {
        for (VisitKind kind : {VisitKind::Mail, VisitKind::Bank})
        {
            if (!SlotOf(kind).runs)
                continue;
            if (_wiring.endAtNearTeleport)
                End(kind, VisitEnd::Teleport);
            else
                SlotOf(kind).displaced = true;
        }
    }

    // EndVisit in MailVisit.cpp and BankVisit.cpp: one row, one counter, the state cleared
    void End(VisitKind kind, VisitEnd end)
    {
        Slot& slot = SlotOf(kind);
        ++_rows[slot.id];
        ++_ended[static_cast<size_t>(kind)][static_cast<size_t>(end)];
        _lastWord = VisitEndWord(kind, end);
        slot = Slot{};
    }

    void CheckNow()
    {
        if (_slots[0].runs && _slots[1].runs)
            Fault("a mailbox and a bank visit run at once");
        for (VisitKind kind : {VisitKind::Mail, VisitKind::Bank})
        {
            size_t const k = static_cast<size_t>(kind);
            Slot const& slot = _slots[k];
            if (slot.runs && !_online)
                Fault("a visit runs while its bot is offline");
            // A visit walks only from where it started, the spot in reach (WB.12): after a teleport it has no walk
            if (slot.runs && slot.displaced)
                Fault("a visit runs on after a teleport took its bot off the walk");
            // Time moves only in an update, and every update watches: an open visit is within its deadline
            if (slot.runs && _nowMs - slot.startedMs >= slot.limitMs)
                Fault(std::string(kind == VisitKind::Mail ? "mail" : "bank") +
                      " visit past its deadline plus an update");
            uint32_t ended = 0;
            for (uint32_t count : _ended[k])
                ended += count;
            if (_started[k] != ended + (slot.runs ? 1 : 0))
                Fault(std::string(kind == VisitKind::Mail ? "mail" : "bank") + " started " +
                      std::to_string(_started[k]) + " != ended " + std::to_string(ended) + " + open " +
                      std::to_string(slot.runs ? 1 : 0) + " (a visit ended without a row and a counter)");
        }
    }

    void Fault(std::string const& what)
    {
        if (_fault.empty())
            _fault = what;
    }

    Wiring _wiring;
    std::array<Slot, 2> _slots{};
    bool _online = true;
    bool _alive = true;
    bool _combat = false;
    uint32_t _mapId = 0;
    uint32_t _nowMs = 0;
    std::array<uint32_t, 2> _started{};
    std::array<std::array<uint32_t, VISIT_END_COUNT>, 2> _ended{};
    std::vector<uint32_t> _rows;  // rows written per visit id
    std::string _fault;
    char const* _lastRefusal = nullptr;
    char const* _lastWord = nullptr;
};

struct Result
{
    uint32_t checks = 0;
    uint32_t failed = 0;
};

class Checker
{
public:
    Checker(std::vector<std::string>& lines, Result& result) : _lines(lines), _result(result) {}

    void Check(bool ok, std::string const& what)
    {
        ++_result.checks;
        if (ok)
            return;
        ++_result.failed;
        _lines.push_back("FAIL: " + what);
    }

    void Note(std::string const& line) { _lines.push_back(line); }

private:
    std::vector<std::string>& _lines;
    Result& _result;
};

inline std::string Describe(std::vector<Event> const& events)
{
    std::string text;
    for (Event event : events)
        text += (text.empty() ? "" : ", ") + std::string(EventWord(event));
    return text;
}

// Runs the events, then drains; the first broken rule, or empty
inline std::string Run(Wiring wiring, std::vector<Event> const& events)
{
    BotModel bot(wiring);
    for (Event event : events)
        bot.Apply(event);
    bot.Drain();
    return bot.FaultText();
}

// The states a bot's visits can be in, each reached from a fresh bot by its events
struct State
{
    std::string name;
    std::vector<Event> events;
};

inline std::vector<State> States()
{
    std::vector<State> states;
    states.push_back({"idle", {}});
    states.push_back({"idle offline", {Event::Logout}});
    struct Condition
    {
        char const* name;
        std::vector<Event> events;
    };
    std::vector<Condition> const conditions = {{"calm", {}},
                                               {"in combat", {Event::CombatEnter}},
                                               {"dead", {Event::Die}},
                                               {"on another map", {Event::MapChange}}};
    for (Event start : {Event::StartMail, Event::StartBank})
        for (char const* place : {"walking", "at the spot", "spot lost"})
            for (bool nearDeadline : {false, true})
                for (Condition const& condition : conditions)
                {
                    std::vector<Event> events = {start};
                    // Walking calm, a visit lives limit - 1 updates; the bank's deadline is the shorter
                    for (uint32_t i = 0; nearDeadline && (i + 2) * UPDATE_MS <= BANK_LIMIT_MS; ++i)
                        events.push_back(Event::Update);
                    if (place[0] == 'a')
                        events.push_back(Event::ReachSpot);
                    else if (place[0] == 's')
                        events.push_back(Event::LoseSpot);
                    events.insert(events.end(), condition.events.begin(), condition.events.end());
                    states.push_back({std::string(start == Event::StartMail ? "mail " : "bank ") + place +
                                          (nearDeadline ? ", near its deadline, " : ", fresh, ") + condition.name,
                                      events});
                }
    return states;
}

// Every state crossed with every event, each case drained and checked
inline void CheckEveryStateAndEvent(Checker& checker)
{
    std::vector<State> const states = States();
    uint32_t cases = 0;
    for (State const& state : states)
    {
        uint32_t faults = 0;
        for (size_t e = 0; e < EVENT_COUNT; ++e)
        {
            std::vector<Event> events = state.events;
            events.push_back(static_cast<Event>(e));
            std::string const fault = Run(TODAY, events);
            ++cases;
            faults += fault.empty() ? 0 : 1;
            checker.Check(fault.empty(), state.name + " + " + EventWord(static_cast<Event>(e)) + ": " + fault + " [" +
                                             Describe(events) + "]");
        }
        checker.Note("state " + state.name + ": " + std::to_string(EVENT_COUNT) + " events, " + std::to_string(faults) +
                     " faults");
    }
    checker.Note("every state and event: " + std::to_string(states.size()) + " states x " +
                 std::to_string(EVENT_COUNT) + " events = " + std::to_string(cases) + " cases");
}

// The end each interruption writes, and one visit at a time
inline void CheckWords(Checker& checker)
{
    auto const endOf = [](std::vector<Event> const& events, VisitKind kind, VisitEnd end)
    {
        BotModel bot(TODAY);
        for (Event event : events)
            bot.Apply(event);
        return bot.Ended(kind, end) == 1 && bot.FaultText().empty();
    };
    std::vector<Event> pastDeadline = {Event::StartMail, Event::CombatEnter};
    for (uint32_t i = 0; i * UPDATE_MS < MAIL_LIMIT_MS; ++i)
        pastDeadline.push_back(Event::Update);
    checker.Check(endOf(pastDeadline, VisitKind::Mail, VisitEnd::Combat), "in combat past the deadline: combat");
    checker.Check(endOf({Event::StartBank, Event::FarTeleport}, VisitKind::Bank, VisitEnd::Teleport),
                  "a far teleport: teleport");
    checker.Check(endOf({Event::StartMail, Event::NearTeleport}, VisitKind::Mail, VisitEnd::Teleport),
                  "a near teleport: teleport");
    checker.Check(endOf({Event::StartMail, Event::Reset}, VisitKind::Mail, VisitEnd::Reset), "a reset: reset");
    checker.Check(endOf({Event::StartBank, Event::Die, Event::Update}, VisitKind::Bank, VisitEnd::Death),
                  "death: death");
    checker.Check(endOf({Event::StartMail, Event::MapChange, Event::Update}, VisitKind::Mail, VisitEnd::MapChange),
                  "a map change: map_change");
    checker.Check(endOf({Event::StartBank, Event::Logout}, VisitKind::Bank, VisitEnd::Logout), "a logout: logout");
    checker.Check(endOf({Event::StartMail, Event::ReachSpot, Event::Update}, VisitKind::Mail, VisitEnd::Done),
                  "at the spot: done");

    BotModel bot(TODAY);
    bot.Apply(Event::StartBank);
    bot.Apply(Event::StartMail);
    std::string const mailRefusal = bot.LastRefusal() ? bot.LastRefusal() : "";
    bot.Apply(Event::StartBank);
    std::string const bankRefusal = bot.LastRefusal() ? bot.LastRefusal() : "";
    checker.Note("a mail start during a bank visit: refused, " + mailRefusal + "; a second bank start: refused, " +
                 bankRefusal);
    checker.Check(mailRefusal == "a bank visit runs" && bankRefusal == "a visit runs already" &&
                      VisitStartRefusal(VisitKind::Bank, true, false) == std::string("a mailbox visit runs"),
                  "one visit at a time, each refusal with its reason");

    checker.Check(WithinVisitReach(VISIT_COMMAND_REACH_YARDS - 1.0f) && WithinVisitReach(VISIT_COMMAND_REACH_YARDS) &&
                      !WithinVisitReach(VISIT_COMMAND_REACH_YARDS + 1.0f),
                  "the command's reach: within passes, beyond refuses");
    checker.Note("reach " + std::to_string(static_cast<int>(VISIT_COMMAND_REACH_YARDS)) +
                 " yd: " + std::to_string(static_cast<int>(VISIT_COMMAND_REACH_YARDS - 1.0f)) + " yd passes, " +
                 std::to_string(static_cast<int>(VISIT_COMMAND_REACH_YARDS + 1.0f)) + " yd refused, " + VISIT_TOO_FAR);
}

// Seeded random sequences: the first counterexample of a wiring, printed in full, or empty
inline std::string Search(Wiring wiring, uint32_t& tried)
{
    std::mt19937 random(SEARCH_SEED);
    std::uniform_int_distribution<uint32_t> length(1, SEARCH_LENGTH);
    std::uniform_int_distribution<uint32_t> percent(0, 99);
    std::uniform_int_distribution<uint32_t> other(0, EVENT_COUNT - 2);
    for (tried = 1; tried <= SEARCH_SEQUENCES; ++tried)
    {
        std::vector<Event> events;
        for (uint32_t i = length(random); i > 0; --i)
        {
            uint32_t const pick = other(random);
            Event const event = static_cast<Event>(pick >= static_cast<uint32_t>(Event::Update) ? pick + 1 : pick);
            events.push_back(percent(random) < UPDATE_SHARE_PERCENT ? Event::Update : event);
        }
        std::string const fault = Run(wiring, events);
        if (!fault.empty())
            return "sequence " + std::to_string(tried) + " [" + Describe(events) + "]: " + fault;
    }
    tried = SEARCH_SEQUENCES;
    return "";
}

inline void CheckSearch(Checker& checker)
{
    uint32_t tried = 0;
    std::string const before = Search(BEFORE_WB7C, tried);
    checker.Note("search, wiring before WB7c (seed " + std::to_string(SEARCH_SEED) +
                 "): " + (before.empty() ? "no counterexample" : "counterexample at " + before));
    checker.Check(!before.empty(), "the search finds the counterexample of the wiring before WB7c");

    std::string const beforeWbl2 = Search(BEFORE_WBL2, tried);
    checker.Note("search, wiring before WBL2: " +
                 (beforeWbl2.empty() ? std::string("no counterexample") : "counterexample at " + beforeWbl2));
    checker.Check(!beforeWbl2.empty(), "the search finds the counterexample of the wiring before WBL2");

    std::string const today = Search(TODAY, tried);
    checker.Note("search, today's wiring: seed " + std::to_string(SEARCH_SEED) + ", " + std::to_string(tried) +
                 " sequences of 1.." + std::to_string(SEARCH_LENGTH) + " events, " +
                 (today.empty() ? "no counterexample" : "counterexample at " + today));
    checker.Check(today.empty(), "no counterexample: " + today);
}
}  // namespace VisitLifecycleDetail

// Runs every scenario; returns the checks run and failed
inline VisitLifecycleDetail::Result RunVisitLifecycleSelftest(std::vector<std::string>& lines)
{
    using namespace VisitLifecycleDetail;
    Result result;
    Checker checker(lines, result);
    CheckWords(checker);
    CheckEveryStateAndEvent(checker);
    CheckSearch(checker);
    lines.push_back("VISIT lifecycle selftest: " + std::to_string(result.checks) + " checks, " +
                    std::to_string(result.failed) + " failed" + (result.failed ? "" : " (PASS)"));
    return result;
}
}  // namespace BankMail

#endif
