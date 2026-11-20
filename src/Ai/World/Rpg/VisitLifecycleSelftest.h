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

// The lifecycle harness of the mailbox, bank and craft visits (VisitLifecycle.h), run by
// tests/visit-lifecycle-selftest.cpp. A model of one bot's three visit slots, wired as the live code wires the shared
// rules: the watch in every AI update (PlayerbotAI::UpdateAI), the stepper in the out-of-combat engine (StepMailVisit,
// StepBankVisit, StepCraftVisit), the ends before a far teleport's or a reset's AfterReset, at a near teleport and at a
// logout. The craft visit casts: each cast is current until it ends by itself, fails, is cancelled by the core (death,
// a teleport, a map change, a logout) or by the visit's end, or never ends (the step bound ends the visit). Every state
// is crossed with every event, then seeded random sequences look for a counterexample to the invariant; the wirings
// before WB7c, WBL2 and WR7 must yield one. The kinds the visit driver runs (Visit.h, consolidation CS28) share its
// one state; the wiring of separate states ends every sequence as the one state does.
namespace BankMail
{
namespace VisitLifecycleDetail
{
constexpr uint32_t UPDATE_MS = 1000;      // one AI update
constexpr uint32_t MAIL_LIMIT_MS = 6000;  // the visits' deadlines in the model
constexpr uint32_t BANK_LIMIT_MS = 4000;
constexpr uint32_t CRAFT_LIMIT_MS = 8000;
constexpr uint32_t CAST_BOUND_MS = 3000;  // a cast's step bound (StepCraftVisit: VisitStepSeconds or the cast time)
constexpr uint32_t CRAFT_CASTS = 2;       // the craft visit's queue
constexpr uint32_t SEARCH_SEED = 20261113;
constexpr uint32_t SEARCH_SEQUENCES = 20000;
constexpr uint32_t SEARCH_LENGTH = 40;
constexpr uint32_t UPDATE_SHARE_PERCENT = 40;  // of the search's events

enum class Event : uint8_t
{
    StartMail,  // the GM command (a refused start writes nothing)
    StartBank,
    StartCraft,
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
    LoseSpot,   // the mailbox, banker or station is not there
    CastEnds,   // the current craft cast ends by itself: the product is in the bags
    CastFails,  // the current craft cast ends without a product (its reagents went, the core refused it)
    Count
};
constexpr size_t EVENT_COUNT = static_cast<size_t>(Event::Count);

inline char const* EventWord(Event event)
{
    static char const* const words[EVENT_COUNT] = {
        "start_mail", "start_bank", "start_craft",  "update",        "combat_enter", "combat_leave",
        "die",        "revive",     "far_teleport", "near_teleport", "reset",        "map_change",
        "logout",     "login",      "reach_spot",   "lose_spot",     "cast_ends",    "cast_fails"};
    return words[static_cast<size_t>(event)];
}

// Which hooks the live code has: today's, the wiring before WB7c (no watch outside the engine, AfterReset and the
// AI's deletion drop a visit without a row, the mail command did not look at a bank visit), or the one before WBL2 (a
// near teleport left the visit running: the walk went on from wherever the bot landed, up to the 15-minute deadline,
// and held its one visit slot; WBL2 run 1, a trip visit under a test hold teleported to Orgrimmar), or the one before
// WR7's craft branch (WatchVisits, EndVisits and the mail and bank starts not knowing the craft visit: AfterReset and
// the AI's deletion drop it without a row, a near teleport leaves it running, its cast goes on after a reset). The
// kinds on the driver keep their visit in its one state (BotBankMail::visit), the others in a state of their own.
struct Wiring
{
    bool watchEveryUpdate = true;
    bool endBeforeDrop = true;
    bool oneAtATime = true;
    bool endAtNearTeleport = true;
    bool craftWired = true;
    uint8_t driverKinds = 0;  // bit per VisitKind
};
constexpr uint8_t DRIVER_BANK_MAIL =
    (1u << static_cast<uint8_t>(VisitKind::Bank)) | (1u << static_cast<uint8_t>(VisitKind::Mail));
constexpr uint8_t DRIVER_ALL = DRIVER_BANK_MAIL | (1u << static_cast<uint8_t>(VisitKind::Craft));
constexpr Wiring TODAY{true, true, true, true, true, DRIVER_ALL};
constexpr Wiring SEPARATE_STATES{true, true, true, true, true, 0};
constexpr Wiring BEFORE_WB7C{false, false, false, false, true, DRIVER_ALL};
constexpr Wiring BEFORE_WBL2{true, true, true, false, true, DRIVER_ALL};
constexpr Wiring BEFORE_WR7{true, true, true, true, false, DRIVER_BANK_MAIL};

constexpr VisitKind KINDS[] = {VisitKind::Mail, VisitKind::Bank, VisitKind::Craft};
constexpr size_t KIND_COUNT = 3;

inline uint32_t LimitOf(VisitKind kind)
{
    return kind == VisitKind::Mail ? MAIL_LIMIT_MS : kind == VisitKind::Bank ? BANK_LIMIT_MS : CRAFT_LIMIT_MS;
}

inline char const* KindWord(VisitKind kind)
{
    return kind == VisitKind::Mail ? "mail" : kind == VisitKind::Bank ? "bank" : "craft";
}

struct Slot
{
    bool runs = false;
    VisitKind kind = VisitKind::Mail;
    uint32_t id = 0;
    uint32_t startedMs = 0;
    uint32_t limitMs = 0;
    uint32_t mapId = 0;
    bool atSpot = false;
    bool spotLost = false;
    bool displaced = false;    // a teleport took the bot off the walk the visit planned from its start
    bool castPending = false;  // the craft visit waits for its cast's result
    uint32_t castStartMs = 0;
    uint32_t made = 0;  // casts that made their product
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
            case Event::StartCraft:
                Start(VisitKind::Craft);
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
                CancelCast();
                break;
            case Event::Revive:
                _alive = true;
                break;
            case Event::FarTeleport:
                if (!_online)
                    break;
                EndOrDrop(VisitEnd::Teleport);
                CancelCast();
                _mapId ^= 1;
                break;
            case Event::NearTeleport:
                if (!_online)
                    break;
                EndAtNearTeleport();
                CancelCast();
                break;
            case Event::Reset:
                if (_online)
                    EndOrDrop(VisitEnd::Reset);
                break;
            case Event::MapChange:
                if (!_online)
                    break;
                _mapId ^= 1;
                CancelCast();
                break;
            case Event::Logout:
                if (!_online)
                    break;
                EndOrDrop(VisitEnd::Logout);
                CancelCast();
                _online = false;
                _combat = false;
                break;
            case Event::Login:
                _online = true;
                break;
            case Event::ReachSpot:
                for (Slot& slot : _slots)
                    slot.atSpot = slot.runs;
                _driver.atSpot = _driver.runs;
                break;
            case Event::LoseSpot:
                for (Slot& slot : _slots)
                    slot.spotLost = slot.runs;
                _driver.spotLost = _driver.runs;
                break;
            case Event::CastEnds:
            case Event::CastFails:
                if (_castCurrent)
                {
                    _castCurrent = false;
                    _castMade = event == Event::CastEnds;
                }
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
        for (uint32_t i = 0; i * UPDATE_MS <= MAIL_LIMIT_MS + BANK_LIMIT_MS + CRAFT_LIMIT_MS && Runs(); ++i)
            Apply(Event::Update);
        if (Runs())
            Fault("a visit never ends");
        for (size_t id = 0; id < _rows.size(); ++id)
            if (_rows[id] != 1)
                Fault("visit " + std::to_string(id) + " wrote " + std::to_string(_rows[id]) + " rows");
    }

    bool Runs() const { return _slots[0].runs || _slots[1].runs || _slots[2].runs || _driver.runs; }
    std::string const& FaultText() const { return _fault; }
    char const* LastRefusal() const { return _lastRefusal; }
    char const* LastWord() const { return _lastWord; }
    uint32_t Ended(VisitKind kind, VisitEnd end) const
    {
        return _ended[static_cast<size_t>(kind)][static_cast<size_t>(end)];
    }

private:
    bool OnDriver(VisitKind kind) const { return _wiring.driverKinds & (1u << static_cast<uint8_t>(kind)); }
    Slot& SlotOf(VisitKind kind) { return OnDriver(kind) ? _driver : _slots[static_cast<size_t>(kind)]; }
    Slot const& SlotOf(VisitKind kind) const { return OnDriver(kind) ? _driver : _slots[static_cast<size_t>(kind)]; }
    // A visit of the kind runs, in its own state or in the driver's (VisitRunsOf)
    bool RunsOf(VisitKind kind) const { return SlotOf(kind).runs && SlotOf(kind).kind == kind; }
    // The live code's hooks know this kind's visit (before WR7 they knew mail and bank only)
    bool Wired(VisitKind kind) const { return kind != VisitKind::Craft || _wiring.craftWired; }

    // The GM command and Begin*Visit (MailCommands.cpp, BankCommands.cpp, CraftCommands.cpp, BeginVisit); before
    // WB7c the mail side did not look at a bank visit, before WR7 neither looked at a craft visit
    void Start(VisitKind kind)
    {
        _lastRefusal = nullptr;
        if (!_online || !_alive)
            return;
        bool const bankSeen = _wiring.oneAtATime || kind == VisitKind::Bank;
        bool const craftSeen = _wiring.craftWired || kind == VisitKind::Craft;
        // The running visit the start sees: its own kind first, then mail, bank and craft (with separate states
        // several may run)
        std::optional<VisitKind> running;
        if (RunsOf(kind))
            running = kind;
        for (VisitKind other : KINDS)
            if (!running && RunsOf(other) && (other != VisitKind::Bank || bankSeen) &&
                (other != VisitKind::Craft || craftSeen))
                running = other;
        _lastRefusal = VisitStartRefusal(kind, running);
        if (_lastRefusal)
            return;
        Slot& slot = SlotOf(kind);
        slot = Slot{};
        slot.runs = true;
        slot.kind = kind;
        slot.id = static_cast<uint32_t>(_rows.size());
        slot.startedMs = _nowMs;
        slot.limitMs = LimitOf(kind);
        slot.mapId = _mapId;
        _rows.push_back(0);
        ++_started[static_cast<size_t>(kind)];
    }

    // PlayerbotAI::UpdateAI: the watch (BankMail::WatchVisits) whatever engine runs, then the out-of-combat engine's
    // stepper (StepMailVisit, StepBankVisit, StepCraftVisit), which relies on that watch
    void Update()
    {
        if (!_online)
            return;
        _nowMs += UPDATE_MS;
        for (VisitKind kind : KINDS)
        {
            Slot& slot = SlotOf(kind);
            if (RunsOf(kind) && _wiring.watchEveryUpdate && Wired(kind))
                if (std::optional<VisitEnd> const end = WatchVisit(WatchIn(slot)))
                    End(kind, *end);
            if (!RunsOf(kind) || !_alive || _combat)
                continue;
            if (std::optional<VisitEnd> const end = StepperWatch(kind, slot))
                End(kind, *end);
            else if (kind == VisitKind::Craft)
                StepCraft(slot);
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

    // StepCraftVisit (CraftVisit.cpp): within the station's range, each queued cast is started and waited for: still
    // current, it waits up to the step bound (then the visit ends with timeout); ended, the bags tell made (the next
    // cast) from failed (the visit ends with its cause)
    void StepCraft(Slot& slot)
    {
        if (slot.spotLost)
        {
            End(VisitKind::Craft, VisitEnd::NoStation);
            return;
        }
        if (!slot.atSpot)
            return;
        if (slot.castPending)
        {
            if (_castCurrent)
            {
                if (_nowMs - slot.castStartMs >= CAST_BOUND_MS)
                    End(VisitKind::Craft, VisitEnd::Timeout);
                return;
            }
            slot.castPending = false;
            if (!_castMade)
            {
                End(VisitKind::Craft, VisitEnd::NoMaterials);
                return;
            }
            ++slot.made;
        }
        if (slot.made >= CRAFT_CASTS)
        {
            End(VisitKind::Craft, VisitEnd::Done);
            return;
        }
        slot.castPending = true;
        slot.castStartMs = _nowMs;
        _castCurrent = true;
        _castMade = false;
    }

    // The core cancels a current cast at death, a teleport, a map change and a logout
    void CancelCast()
    {
        _castCurrent = false;
        _castMade = false;
    }

    // Today the update's watch alone ends a visit. Before WB7c the stepper checked the deadline and then the map (a map
    // change counted as no spot); the craft stepper came later and watched as the update does now
    std::optional<VisitEnd> StepperWatch(VisitKind kind, Slot const& slot) const
    {
        if (_wiring.watchEveryUpdate)
            return std::nullopt;
        if (kind == VisitKind::Craft)
            return WatchVisit(WatchIn(slot));
        if (_nowMs - slot.startedMs >= slot.limitMs)
            return VisitEnd::Timeout;
        if (slot.mapId != _mapId)
            return VisitEnd::NoSpot;
        return std::nullopt;
    }

    // A far teleport, a reset or a logout: today the visit ends with its row first; before WB7c AfterReset (or the
    // AI's deletion) dropped it, and the craft visit before WR7
    void EndOrDrop(VisitEnd end)
    {
        for (VisitKind kind : KINDS)
        {
            if (!RunsOf(kind))
                continue;
            if (_wiring.endBeforeDrop && Wired(kind))
                End(kind, end);
            else
                SlotOf(kind) = Slot{};
        }
    }

    // PlayerbotAI::HandleTeleportAck's near branch: today the visit ends with its row; before WBL2 it ran on, and the
    // craft visit before WR7
    void EndAtNearTeleport()
    {
        for (VisitKind kind : KINDS)
        {
            if (!RunsOf(kind))
                continue;
            if (_wiring.endAtNearTeleport && Wired(kind))
                End(kind, VisitEnd::Teleport);
            else
                SlotOf(kind).displaced = true;
        }
    }

    // The driver's EndVisit (Visit.cpp) with the kind's end row: one row, one counter, the state cleared; the craft
    // visit cancels its cast if it is still current
    void End(VisitKind kind, VisitEnd end)
    {
        Slot& slot = SlotOf(kind);
        if (kind == VisitKind::Craft)
            CancelCast();
        ++_rows[slot.id];
        ++_ended[static_cast<size_t>(kind)][static_cast<size_t>(end)];
        _lastWord = VisitEndWord(kind, end);
        slot = Slot{};
    }

    void CheckNow()
    {
        uint32_t running = _driver.runs ? 1 : 0;
        for (Slot const& slot : _slots)
            running += slot.runs ? 1 : 0;
        if (running > 1)
            Fault("two visits run at once");
        if (_castCurrent && !(RunsOf(VisitKind::Craft) && SlotOf(VisitKind::Craft).castPending))
            Fault("a craft cast goes on without its visit");
        for (VisitKind kind : KINDS)
        {
            size_t const k = static_cast<size_t>(kind);
            Slot const& slot = SlotOf(kind);
            bool const runs = RunsOf(kind);
            if (runs && !_online)
                Fault("a visit runs while its bot is offline");
            // A visit walks only from where it started, the spot in reach (WB.12): after a teleport it has no walk
            if (runs && slot.displaced)
                Fault("a visit runs on after a teleport took its bot off the walk");
            // Time moves only in an update, and every update watches: an open visit is within its deadline
            if (runs && _nowMs - slot.startedMs >= slot.limitMs)
                Fault(std::string(KindWord(kind)) + " visit past its deadline plus an update");
            uint32_t ended = 0;
            for (uint32_t count : _ended[k])
                ended += count;
            if (_started[k] != ended + (runs ? 1 : 0))
                Fault(std::string(KindWord(kind)) + " started " + std::to_string(_started[k]) + " != ended " +
                      std::to_string(ended) + " + open " + std::to_string(runs ? 1 : 0) +
                      " (a visit ended without a row and a counter)");
        }
    }

    void Fault(std::string const& what)
    {
        if (_fault.empty())
            _fault = what;
    }

    Wiring _wiring;
    std::array<Slot, KIND_COUNT> _slots{};
    Slot _driver{};  // BotBankMail::visit, for the kinds on the driver
    bool _online = true;
    bool _alive = true;
    bool _combat = false;
    uint32_t _mapId = 0;
    uint32_t _nowMs = 0;
    bool _castCurrent = false;  // the bot's generic spell slot holds the craft visit's cast
    bool _castMade = false;     // the last cast that ended made its product
    std::array<uint32_t, KIND_COUNT> _started{};
    std::array<std::array<uint32_t, VISIT_END_COUNT>, KIND_COUNT> _ended{};
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
    for (VisitKind kind : KINDS)
    {
        Event const start = kind == VisitKind::Mail   ? Event::StartMail
                            : kind == VisitKind::Bank ? Event::StartBank
                                                      : Event::StartCraft;
        std::vector<char const*> places = {"walking", "at the spot", "spot lost"};
        if (kind == VisitKind::Craft)
            places.push_back("casting");
        for (char const* place : places)
            for (bool nearDeadline : {false, true})
                for (Condition const& condition : conditions)
                {
                    bool const casting = place[0] == 'c';
                    std::vector<Event> events = {start};
                    // Walking calm, a visit lives limit - 1 updates; a cast takes one of them to start
                    uint32_t const spent = casting ? 3 : 2;
                    for (uint32_t i = 0; nearDeadline && (i + spent) * UPDATE_MS <= LimitOf(kind); ++i)
                        events.push_back(Event::Update);
                    if (place[0] == 'a' || casting)
                        events.push_back(Event::ReachSpot);
                    else if (place[0] == 's')
                        events.push_back(Event::LoseSpot);
                    if (casting)
                        events.push_back(Event::Update);
                    events.insert(events.end(), condition.events.begin(), condition.events.end());
                    states.push_back({std::string(KindWord(kind)) + " " + place +
                                          (nearDeadline ? ", near its deadline, " : ", fresh, ") + condition.name,
                                      events});
                }
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
                      VisitStartRefusal(VisitKind::Bank, VisitKind::Mail) == std::string("a mailbox visit runs"),
                  "one visit at a time, each refusal with its reason");

    // The craft visit's own ends (professions WR1): reached by the visit, so no interruption; each with its word
    bool craftEnds = true;
    for (VisitEnd end : {VisitEnd::NoMaterials, VisitEnd::NoStation, VisitEnd::BagsFull})
        craftEnds = craftEnds && !IsInterruption(end) && EquipsAfter(end) && end < VisitEnd::Combat;
    checker.Check(craftEnds && VisitEndWord(VisitKind::Craft, VisitEnd::NoMaterials) == std::string("no_materials") &&
                      VisitEndWord(VisitKind::Craft, VisitEnd::NoStation) == std::string("no_station") &&
                      VisitEndWord(VisitKind::Craft, VisitEnd::BagsFull) == std::string("bags_full") &&
                      VisitEndWord(VisitKind::Mail, VisitEnd::NoSpot) == std::string("no_mailbox") &&
                      VisitEndWord(VisitKind::Bank, VisitEnd::NoSpot) == std::string("no_banker"),
                  "the craft ends are the visit's own, each with its word; mail and bank keep theirs");
    checker.Check(VisitStartRefusal(VisitKind::Craft, VisitKind::Mail) == std::string("a mailbox visit runs") &&
                      VisitStartRefusal(VisitKind::Craft, VisitKind::Bank) == std::string("a bank visit runs") &&
                      VisitStartRefusal(VisitKind::Craft, VisitKind::Craft) == std::string("a visit runs already") &&
                      VisitStartRefusal(VisitKind::Mail, VisitKind::Craft) == std::string("a craft visit runs") &&
                      VisitStartRefusal(VisitKind::Bank, VisitKind::Craft) == std::string("a craft visit runs") &&
                      !VisitStartRefusal(VisitKind::Craft, std::nullopt),
                  "a craft visit waits for a mailbox or bank visit, and they wait for it");

    BotModel crafting(TODAY);
    crafting.Apply(Event::StartCraft);
    std::string refusals;
    for (Event start : {Event::StartMail, Event::StartBank, Event::StartCraft})
    {
        crafting.Apply(start);
        refusals += (refusals.empty() ? "" : "; ") + std::string(crafting.LastRefusal() ? crafting.LastRefusal() : "");
    }
    checker.Note("a mail, a bank and a second craft start during a craft visit: refused, " + refusals);
    checker.Check(refusals == "a craft visit runs; a craft visit runs; a visit runs already",
                  "one visit at a time across the three kinds");

    // The craft visit's casts: each made product leads to the next, a failed cast ends with its cause, a cast that
    // never ends meets the step bound, an interruption cancels the cast
    std::vector<Event> made = {Event::StartCraft, Event::ReachSpot};
    for (uint32_t i = 0; i < CRAFT_CASTS; ++i)
        made.insert(made.end(), {Event::Update, Event::CastEnds});
    made.push_back(Event::Update);
    checker.Check(endOf(made, VisitKind::Craft, VisitEnd::Done), "every queued cast made: done");
    checker.Check(endOf({Event::StartCraft, Event::ReachSpot, Event::Update, Event::CastFails, Event::Update},
                        VisitKind::Craft, VisitEnd::NoMaterials),
                  "a cast ended without its product: no_materials");
    checker.Check(endOf({Event::StartCraft, Event::LoseSpot, Event::Update}, VisitKind::Craft, VisitEnd::NoStation),
                  "the station gone: no_station");
    std::vector<Event> hangs = {Event::StartCraft, Event::ReachSpot, Event::Update};
    for (uint32_t i = 0; i * UPDATE_MS < CAST_BOUND_MS; ++i)
        hangs.push_back(Event::Update);
    checker.Check(endOf(hangs, VisitKind::Craft, VisitEnd::Timeout), "a cast that never ends: timeout at its bound");
    std::vector<Event> combat = {Event::StartCraft, Event::ReachSpot, Event::Update, Event::CombatEnter};
    for (uint32_t i = 0; i * UPDATE_MS < CRAFT_LIMIT_MS; ++i)
        combat.push_back(Event::Update);
    checker.Check(endOf(combat, VisitKind::Craft, VisitEnd::Combat), "casting in combat past the deadline: combat");
    checker.Check(endOf({Event::StartCraft, Event::ReachSpot, Event::Update, Event::FarTeleport}, VisitKind::Craft,
                        VisitEnd::Teleport),
                  "casting at a far teleport: teleport");
    checker.Check(
        endOf({Event::StartCraft, Event::ReachSpot, Event::Update, Event::Reset}, VisitKind::Craft, VisitEnd::Reset),
        "casting at a reset: reset, the cast cancelled");

    checker.Check(WithinVisitReach(VISIT_COMMAND_REACH_YARDS - 1.0f) && WithinVisitReach(VISIT_COMMAND_REACH_YARDS) &&
                      !WithinVisitReach(VISIT_COMMAND_REACH_YARDS + 1.0f),
                  "the command's reach: within passes, beyond refuses");
    checker.Note("reach " + std::to_string(static_cast<int>(VISIT_COMMAND_REACH_YARDS)) +
                 " yd: " + std::to_string(static_cast<int>(VISIT_COMMAND_REACH_YARDS - 1.0f)) + " yd passes, " +
                 std::to_string(static_cast<int>(VISIT_COMMAND_REACH_YARDS + 1.0f)) + " yd refused, " + VISIT_TOO_FAR);
}

// The search's next sequence: 1..SEARCH_LENGTH events, updates at their share
inline std::vector<Event> RandomEvents(std::mt19937& random)
{
    std::uniform_int_distribution<uint32_t> length(1, SEARCH_LENGTH);
    std::uniform_int_distribution<uint32_t> percent(0, 99);
    std::uniform_int_distribution<uint32_t> other(0, EVENT_COUNT - 2);
    std::vector<Event> events;
    for (uint32_t i = length(random); i > 0; --i)
    {
        uint32_t const pick = other(random);
        Event const event = static_cast<Event>(pick >= static_cast<uint32_t>(Event::Update) ? pick + 1 : pick);
        events.push_back(percent(random) < UPDATE_SHARE_PERCENT ? Event::Update : event);
    }
    return events;
}

// Seeded random sequences: the first counterexample of a wiring, printed in full, or empty; counts every one
inline std::string Search(Wiring wiring, uint32_t& tried, uint32_t& counterexamples)
{
    std::string first;
    counterexamples = 0;
    std::mt19937 random(SEARCH_SEED);
    for (tried = 1; tried <= SEARCH_SEQUENCES; ++tried)
    {
        std::vector<Event> const events = RandomEvents(random);
        std::string const fault = Run(wiring, events);
        if (fault.empty())
            continue;
        ++counterexamples;
        if (first.empty())
            first = "sequence " + std::to_string(tried) + " [" + Describe(events) + "]: " + fault;
    }
    tried = SEARCH_SEQUENCES;
    return first;
}

inline void CheckSearch(Checker& checker)
{
    struct Control
    {
        char const* name;
        Wiring wiring;
    };
    uint32_t tried = 0;
    uint32_t found = 0;
    for (Control const& control :
         {Control{"before WB7c", BEFORE_WB7C}, Control{"before WBL2", BEFORE_WBL2}, Control{"before WR7", BEFORE_WR7}})
    {
        std::string const first = Search(control.wiring, tried, found);
        checker.Note("search, wiring " + std::string(control.name) + " (seed " + std::to_string(SEARCH_SEED) +
                     "): " + std::to_string(tried) + " sequences, " + std::to_string(found) + " counterexamples" +
                     (first.empty() ? "" : ", the first at " + first));
        checker.Check(found > 0, std::string("the search finds a counterexample of the wiring ") + control.name);
    }

    std::string const today = Search(TODAY, tried, found);
    checker.Note("search, today's wiring: seed " + std::to_string(SEARCH_SEED) + ", " + std::to_string(tried) +
                 " sequences of 1.." + std::to_string(SEARCH_LENGTH) + " events, " + std::to_string(found) +
                 " counterexamples" + (today.empty() ? "" : ", the first at " + today));
    checker.Check(found == 0, "no counterexample: " + today);
}
// Every end of a run, by kind, and its fault
inline std::string EndsOf(Wiring wiring, std::vector<Event> const& events)
{
    BotModel bot(wiring);
    for (Event event : events)
        bot.Apply(event);
    bot.Drain();
    std::string ends = bot.FaultText();
    for (VisitKind kind : KINDS)
        for (size_t end = 0; end < VISIT_END_COUNT; ++end)
            ends += "," + std::to_string(bot.Ended(kind, static_cast<VisitEnd>(end)));
    return ends;
}

// The driver's one state changes no end (consolidation CS28): every search sequence ends alike, faults included, with
// the kinds on the driver and with separate states
inline void CheckOneState(Checker& checker)
{
    std::mt19937 random(SEARCH_SEED);
    uint32_t differ = 0;
    std::string first;
    for (uint32_t tried = 1; tried <= SEARCH_SEQUENCES; ++tried)
    {
        std::vector<Event> const events = RandomEvents(random);
        if (EndsOf(TODAY, events) == EndsOf(SEPARATE_STATES, events))
            continue;
        if (!differ++)
            first = "sequence " + std::to_string(tried) + " [" + Describe(events) + "]";
    }
    checker.Note("one state against separate states (seed " + std::to_string(SEARCH_SEED) +
                 "): " + std::to_string(SEARCH_SEQUENCES) + " sequences, " + std::to_string(differ) +
                 " end differently" + (first.empty() ? "" : ", the first at " + first));
    checker.Check(differ == 0, "the one state ends every sequence as separate states do: " + first);
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
    CheckOneState(checker);
    lines.push_back("VISIT lifecycle selftest: " + std::to_string(result.checks) + " checks, " +
                    std::to_string(result.failed) + " failed" + (result.failed ? "" : " (PASS)"));
    return result;
}
}  // namespace BankMail

#endif
