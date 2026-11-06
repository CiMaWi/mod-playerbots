/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_BRIDGEINTENTEFFECT_H
#define PLAYERBOTS_BRIDGEINTENTEFFECT_H

#include <cstdint>
#include <functional>
#include <thread>

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
}  // namespace PlayerbotBridge

#endif
