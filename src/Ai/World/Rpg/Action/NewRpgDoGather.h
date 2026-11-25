/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOT_NEWRPGDOGATHER_H
#define PLAYERBOT_NEWRPGDOGATHER_H

#include <optional>
#include <string_view>

#include "NewRpgBaseAction.h"

// Why a creature stop resolved (professions WR20c; the node causes are in NewRpgDoGather.cpp): a creature of the
// stop's kind died to the bot (its row's skinned alternative: 1 when its corpse was skinned); none of the kind near
// the spawn point; none killed in time after the bot came there; the walk to it stalled
namespace GatherStopWords
{
constexpr char const* KILLED = "killed";
constexpr char const* SKINNED = "skinned";
constexpr char const* SEEN_DOWN = "seen_down";
constexpr char const* TIMED_OUT = "timed_out";
constexpr char const* UNREACHED = "unreached";
}  // namespace GatherStopWords

class NewRpgDoGatherAction : public NewRpgBaseAction
{
public:
    NewRpgDoGatherAction(PlayerbotAI* botAI) : NewRpgBaseAction(botAI, "new rpg do gather") {}
    bool Execute(Event event) override;

    // The loot pipeline cast a gather spell at `node` (OpenLootAction::DoLoot): when it is the gather status's
    // target, the stop's end counts as taken. The pipeline harvests a node in loot range on its own, before this
    // action stands at it (professions WR13b, lab12 run scn-261120133124). Every bot's cast is noted by spawn for the
    // stop rows' causes (WR13d)
    static void NoteHarvestCast(PlayerbotAI* botAI, ObjectGuid node);

protected:
    // Release the loot pipeline's target if it points at the given spawn.
    // Called whenever the gather action abandons a node, so a half-done
    // harvest can't leave a stale loot target behind (which would gate off
    // loot-target replacement and wedge all subsequent harvesting).
    void ClearLootTargetForNode(ObjectGuid::LowType spawnId);

    // Drop the current node target (releasing the loot target with it) and
    // reset the per-node state; markVisited writes the spawn off for this
    // gather session.
    void AbandonNode(NewRpgInfo::DoGather& data, bool markVisited = false);

    // Max time waiting at a reached node for the loot pipeline to harvest
    // it before giving up and moving on.
    const uint32 nodeStayTime = 30 * IN_MILLISECONDS;
    // The circuit's next stop due (respawn memory), or the end of the
    // status: true when a target is set.
    bool TargetNextStop(NewRpgInfo::DoGather& data);
    // A stop resolved: taken (the bot's own harvest cast went at it and it
    // went) or not; its decision row names the cause (`cause`, the bot's
    // distance to the node), then the respawn memory, the next stop.
    void ResolveStop(NewRpgInfo::DoGather& data, bool taken, bool writeOff, std::string_view cause);
    // The walk to a stop's place, watched by Gather::WalkStalled (WR13d): nothing once within reach, else the walk's
    // answer; a stalled walk resolves the stop unreached and ends the status stuck
    std::optional<bool> WalkToStop(NewRpgInfo::DoGather& data, WorldPosition const& pos, float reach);
    // A creature stop (professions WR20c, NewRpgGatherFarm.cpp): the walk to its spawn point, a creature of its kind
    // sought there and attacked (questing's sought-target attack), its corpse left to the loot and skin pipeline, the
    // stop's row. periodic: the 2 s check is due (the search runs on it)
    bool FarmStop(NewRpgInfo::DoGather& data, bool periodic);
    // A creature stop resolved: its row (killed, with skinned; or a no_source cause), the respawn memory, the next stop
    void ResolveFarmStop(NewRpgInfo::DoGather& data, bool killed, char const* cause, bool skinned, bool writeOff);
    // A creature the stop may attack: of its kind, open (SoughtTarget::Open) and a valid attack target
    bool FarmTarget(NewRpgInfo::DoGather const& data, Creature* creature) const;

    // Cadence for the periodic target-liveness check.
    const uint32 passiveCheckInterval = 2 * IN_MILLISECONDS;
    // A status without a circuit takes the nearest live node this near.
    const float nodeSwitchDistance = 80.0f;
};

#endif
