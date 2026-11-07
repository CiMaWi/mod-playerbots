/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_BRIDGEINTENTEFFECT_H
#define PLAYERBOTS_BRIDGEINTENTEFFECT_H

#include <cstdint>
#include <functional>
#include <string>
#include <thread>
#include <vector>

class OllamaBotRandomChatter;

namespace PlayerbotBridge
{
enum class Intent : uint8_t
{
    Follow,
    Stay,
    Flee,
    Regroup,
    AttackOwnTarget,
    Pull,
    Mark,
    FocusHealing,
    AssignRole,
    SaveMana,
    Formation,
    Position,
    DungeonClear
};

enum class Result : uint8_t
{
    Rejected,
    Unavailable,
    Applied,
    Failed
};

// Formation only: the wire shapes, mapped to playerbots formations (close -> near, spread -> far, line -> line).
enum class FormationShape : uint8_t
{
    None,
    Close,
    Spread,
    Line
};

struct Order
{
    Intent Operation = Intent::Stay;
    uint64_t Bot = 0;
    uint64_t Controller = 0;
    uint64_t Group = 0;
    uint64_t Target = 0;
    uint8_t Icon = 0;
    float VisibilityDistance = 0.0f;
    // Position only: the exact destination. Doubles, so a huge wire value is refused before any float conversion.
    uint32_t MapId = 0;
    double X = 0.0;
    double Y = 0.0;
    double Z = 0.0;
    // Formation only: the shape; None on every other verb.
    FormationShape Shape = FormationShape::None;
    // SaveMana only: true saves mana at the class level the "save mana *" command picks, false spends freely (1.0).
    bool Enabled = false;
};

// The lasting part of a bot a delivered order changes; None for the one-shot verbs. Follow and Stay change the same
// strategies and named positions, so they share Movement and a plan ledger keys them as one part.
enum class Part : uint8_t
{
    None,
    Movement,
    Formation,
    ManaSave,
    FocusHeal
};

constexpr Part LastingPart(Intent operation)
{
    switch (operation)
    {
        case Intent::Follow:
        case Intent::Stay:
            return Part::Movement;
        case Intent::Formation:
            return Part::Formation;
        case Intent::SaveMana:
            return Part::ManaSave;
        case Intent::FocusHealing:
            return Part::FocusHeal;
        default:
            return Part::None;
    }
}

struct NamedPosition
{
    bool Set = false;
    float X = 0.0f;
    float Y = 0.0f;
    float Z = 0.0f;
    uint32_t MapId = 0;
    bool operator==(NamedPosition const&) const = default;
};

// A copy of one lasting part of a bot, filled by Capture for the part it holds; the other fields stay default. Plain
// values only (no game type or pointer), so a plan ledger may keep it for the plan's lifetime.
struct Baseline
{
    // The bot's raw GUID; Restore refuses a baseline of another bot.
    uint64_t Bot = 0;
    Part Held = Part::None;
    // Movement: one bit per strategy the follow and stay commands change or drop as siblings (follow, stay, runaway,
    // guard, passive, grind, move from group), non-combat first, then combat.
    uint16_t Strategies = 0;
    NamedPosition Stay;
    NamedPosition Return;
    // Formation: the playerbots formation name ("formation ?").
    std::string Formation;
    // ManaSave: the "mana save level" value.
    double ManaSaveLevel = 0.0;
    // FocusHeal: the "focus heal targets" list and the combat strategy of the same name.
    std::vector<uint64_t> FocusHealTargets;
    bool FocusHealStrategy = false;

    // The one rule for "the same state of the part", for Restore and a plan ledger alike. Movement compares its
    // strategies only: the stay and return spots are where a command left the bot, and playerbots moves the stay spot
    // by itself (ReturnToStayPositionAction), so a moved spot never makes the bridge's stay a person's.
    bool operator==(Baseline const& other) const
    {
        if (Bot != other.Bot || Held != other.Held || Strategies != other.Strategies)
            return false;

        return Held == Part::Movement ||
               (Formation == other.Formation && ManaSaveLevel == other.ManaSaveLevel &&
                FocusHealTargets == other.FocusHealTargets && FocusHealStrategy == other.FocusHealStrategy);
    }
};

enum class RestoreResult : uint8_t
{
    Rejected,    // Not post-map-join, or mismatched or another bot's baselines: nothing read or changed.
    Gone,        // The bot is offline or no longer a bot: nothing changed; drop the entry.
    Superseded,  // The part no longer holds what the bridge set (a later command or the bot changed it): untouched.
    Restored,    // The part holds the baseline again (written now, or already there: a second restore writes nothing).
    Failed       // Out of the world (a teleport), or the part could not be read or written: the caller may retry.
};

// Construct only on the stack in OllamaBotRandomChatter::OnUpdate, after MapMgr::Update joins its workers.
// Friendship restricts ingress; that exact construction site remains a source-audited integration requirement.
class PostMapJoin final
{
public:
    PostMapJoin(PostMapJoin const&) = delete;
    PostMapJoin& operator=(PostMapJoin const&) = delete;
    bool Current() const;

private:
    friend class ::OllamaBotRandomChatter;
    PostMapJoin();
    std::thread::id const _thread;
};

// Synchronous only. The caller owns replay/rate/epoch/deadline/audience admission and supplies its last currentness
// check here. No order, game pointer, callback or phase scope is retained. Failed attempts must never be replayed.
Result Execute(PostMapJoin const& phase, Order const& order, std::function<bool()> const& authorize);

// Read-only, synchronous, post-map-join only: the order's position is on the bot's map, within VisibilityDistance of
// the bot and of the controller, and reached by a complete navmesh path no longer than twice that radius. One path
// query; it mutates nothing, so a caller may use it as a fact before Execute, which checks it again.
bool PositionReachable(PostMapJoin const& phase, Order const& order);

// Synchronous, post-map-join only, read-only: copies one lasting part of a connected, in-world bot into out. False (out
// cleared) off the world thread, for Part::None, or when the bot or the value is missing. No group or controller check:
// the caller captures the baseline before Execute and the applied state after an Applied result, in the same phase.
bool Capture(PostMapJoin const& phase, uint64_t bot, Part part, Baseline& out);

// Synchronous, post-map-join only. Puts the part before holds back, but only while the bot's part still equals what
// the bridge set (applied, by Baseline::operator==): a later command wins. Both baselines must be this bot's. Works
// without a group or controller, so expiry, a group change, controller loss or bridge retirement can restore; a
// logged-out bot is Gone, a bot out of the world for a teleport is Failed.
RestoreResult Restore(PostMapJoin const& phase, uint64_t bot, Baseline const& before, Baseline const& applied);
}  // namespace PlayerbotBridge

#endif
