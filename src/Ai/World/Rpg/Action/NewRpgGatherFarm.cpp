/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// The gather status's creature stops (professions WR20c, PLAN 3.9, DECISIONS WR.24): going out for cloth, leather and
// meat is a gather circuit whose stops are creatures' spawn points. At a stop the bot looks for a creature of the
// stop's kind near the spawn point, attacks it with questing's sought-target attack (SoughtTarget, family rule 16),
// leaves the corpse to the ordinary loot pipeline (it skins by the pass-by gate, GatherPassBy, WR13c), then the stop's
// row, the respawn memory and the next stop

#include <algorithm>

#include "Creature.h"
#include "GatherMemoryState.h"
#include "NewRpgDoGather.h"
#include "NewRpgInfo.h"
#include "NewRpgSoughtTarget.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "ProfessionConfig.h"
#include "Timer.h"
#include "WorldLog.h"

namespace
{
constexpr float FARM_SEEK_START_YARDS = 60.0f;  // within this of the spawn point the bot looks for the stop's kind
constexpr float FARM_SEEK_YARDS = 40.0f;        // around itself: a creature of the kind this near is the stop's
constexpr float FARM_REACH_YARDS = 10.0f;       // at the spawn point with none of the kind near: seen down
constexpr float FARM_ENGAGE_YARDS = 30.0f;      // within this and in sight: the attack (questing's engage range)
constexpr uint32 FARM_STOP_MS = 90 * IN_MILLISECONDS;      // from the first sight of a target: no kill, timed out
constexpr uint32 FARM_LOOT_MS = 30 * IN_MILLISECONDS;      // after the kill: its loot and skin take at most this
constexpr uint32 FARM_SKIN_WAIT_MS = 8 * IN_MILLISECONDS;  // a looted corpse still skinnable this long: not skinned

// A corpse of the stop's kind the bot killed that the respawn memory does not hold yet: the grind and attacked
// strategies often engage the stop's creature on the walk up, before the stop picks a target, so their kill is the
// stop's (the corpse's spawn goes into the memory with the stop's row, so one kill counts once)
Creature* OwnCorpse(Player* bot, PlayerbotAI* botAI, uint32 entry)
{
    uint32 const now = getMSTime();
    std::size_t candidates = 0;
    return SoughtTarget::Nearest(
        bot, {entry}, FARM_SEEK_YARDS,
        [bot, botAI, now](Creature* creature)
        {
            return !creature->IsAlive() && creature->isTappedBy(bot) &&
                   !botAI->rpgInfo.gatherMemory.Away(creature->GetSpawnId() | Gather::CREATURE_STOP, now);
        },
        candidates);
}
}  // namespace

bool NewRpgDoGatherAction::FarmTarget(NewRpgInfo::DoGather const& data, Creature* creature) const
{
    return creature && creature->GetEntry() == data.creatureEntry && SoughtTarget::Open(bot, creature) &&
           bot->IsValidAttackTarget(creature);
}

bool NewRpgDoGatherAction::FarmStop(NewRpgInfo::DoGather& data, bool periodic)
{
    // The fight is the combat strategies'; the stop's clock goes on (a fight within FARM_STOP_MS is a kill's)
    if (bot->IsInCombat())
        return false;
    uint32 const now = std::max<uint32>(getMSTime(), 1);
    Creature* target = !data.targetGuid.IsEmpty() ? botAI->GetCreature(data.targetGuid) : nullptr;
    if (data.killedMs)
    {
        // The corpse: the loot pipeline loots it and, when the gate lets it, skins it (the skinnable flag comes once
        // it is looted and goes with the skin). Done when it holds nothing more for the bot, or the time is over
        bool const lootable = target && target->HasFlag(UNIT_DYNAMIC_FLAGS, UNIT_DYNFLAG_LOOTABLE);
        bool const skinnable = target && target->HasFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_SKINNABLE);
        if (skinnable && !data.skinSeenMs)
            data.skinSeenMs = now;
        bool const left = !lootable && skinnable && GetMSTimeDiffToNow(data.skinSeenMs) > FARM_SKIN_WAIT_MS;
        if (target && (lootable || skinnable) && !left && GetMSTimeDiffToNow(data.killedMs) <= FARM_LOOT_MS)
            return false;
        ResolveFarmStop(data, true, GatherStopWords::KILLED, data.harvestCast || (data.skinSeenMs && !skinnable),
                        false);
        return true;
    }
    if (!data.targetGuid.IsEmpty() && (!target || !target->IsAlive()))
    {
        if (target && target->isTappedBy(bot))
        {
            data.killedMs = now;  // its corpse is the loot pipeline's now
            return false;
        }
        data.targetGuid.Clear();  // gone or killed by another
        target = nullptr;
    }
    if (target && !FarmTarget(data, target))
    {
        data.targetGuid.Clear();
        target = nullptr;
    }
    if (data.lastReach && GetMSTimeDiffToNow(data.lastReach) > FARM_STOP_MS)
    {
        ResolveFarmStop(data, false, GatherStopWords::TIMED_OUT, false, true);
        return true;
    }
    if (!target)
    {
        float const yards = bot->GetExactDist(data.nodePos);
        if (periodic && yards <= FARM_SEEK_START_YARDS + FARM_SEEK_YARDS)
            if (Creature* corpse = OwnCorpse(bot, botAI, data.creatureEntry))
            {
                data.targetGuid = corpse->GetGUID();  // the stop's kill, made in the bot's own fighting
                data.killedMs = now;
                return false;
            }
        if (periodic && yards <= FARM_SEEK_START_YARDS)
        {
            std::size_t candidates = 0;
            target = SoughtTarget::Nearest(
                bot, {data.creatureEntry}, FARM_SEEK_YARDS,
                [this, &data](Creature* creature) { return FarmTarget(data, creature); }, candidates);
            if (target)
            {
                data.targetGuid = target->GetGUID();
                if (!data.lastReach)
                    data.lastReach = now;
            }
        }
        if (!target)
        {
            if (yards > FARM_REACH_YARDS)
                return WalkToStop(data, data.nodePos, FARM_REACH_YARDS).value_or(false);
            if (!periodic)
                return false;
            ResolveFarmStop(data, false, GatherStopWords::SEEN_DOWN, false, false);
            return true;
        }
    }
    float const yards = bot->GetDistance(target);
    if (yards > FARM_ENGAGE_YARDS || !bot->IsWithinLOSInMap(target))
    {
        if (bot->isMoving())
            return false;
        return MoveTo(target->GetMapId(), target->GetPositionX(), target->GetPositionY(), target->GetPositionZ(), false,
                      false, false, false);
    }
    return botAI->DoSpecificAction("new rpg attack quest target", Event(), true);
}

void NewRpgDoGatherAction::ResolveFarmStop(NewRpgInfo::DoGather& data, bool killed, char const* cause, bool skinned,
                                           bool writeOff)
{
    using namespace Professions;
    uint32 const spawnId = data.nodeSpawnId & ~Gather::CREATURE_STOP;
    bool const planned = data.next < data.circuit.size() && data.circuit[data.next] == data.nodeSpawnId;
    // The stop's row (three alternatives): its number in walk order, the lap and the cause with the bot's distance
    // to the spawn point; a kill's third says whether its corpse was skinned, another cause's second the kind
    // (creature)
    WorldLogFormat::Alternatives alternatives{{"lap", static_cast<float>(data.lap + 1)}};
    if (!killed)
        alternatives.Add({"creature", static_cast<float>(data.creatureEntry)});
    alternatives.Add({cause, bot->GetExactDist(data.nodePos)});
    if (killed)
        alternatives.Add({GatherStopWords::SKINNED, skinned ? 1.0f : 0.0f});
    WorldLog::Decision(bot, WorldLogFormat::DecisionKind::Profession, ChoiceWord(Choice::Gather),
                       ReasonWord(killed ? Reason::Supply : Reason::NoSource),
                       planned ? static_cast<float>(data.next + 1) : 0.0f, alternatives, spawnId);
    ++data.span.resolved;  // the observed rate's span counts the rows part 4 counts (WPC13b)
    if (killed)
        ++data.span.takes;
    // The respawn memory: killed, seen down or written off, the bot comes back after the respawn at the earliest
    CreatureData const* spawn = sObjectMgr->GetCreatureData(spawnId);
    botAI->rpgInfo.gatherMemory.Note(data.nodeSpawnId, getMSTime(), spawn ? spawn->spawntimesecs * IN_MILLISECONDS : 0);
    // The kill of another spawn of the kind: that spawn too is down until its respawn
    if (Creature* kill = killed && !data.targetGuid.IsEmpty() ? botAI->GetCreature(data.targetGuid) : nullptr)
        if (uint32 const own = kill->GetSpawnId(); own && own != spawnId)
            botAI->rpgInfo.gatherMemory.Note(own | Gather::CREATURE_STOP, getMSTime(),
                                             kill->GetRespawnDelay() * IN_MILLISECONDS);
    if (planned)
        ++data.next;
    AbandonNode(data, writeOff || data.circuit.empty());
    data.creatureEntry = 0;
    data.targetGuid.Clear();
    data.killedMs = 0;
    data.skinSeenMs = 0;
    data.harvestCast = false;
}
