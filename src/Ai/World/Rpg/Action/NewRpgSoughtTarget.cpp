/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "NewRpgSoughtTarget.h"

#include <list>
#include <variant>

#include "Creature.h"
#include "NewRpgInfo.h"
#include "Player.h"

namespace SoughtTarget
{
bool Open(Player* bot, Creature* creature)
{
    if (!creature || !creature->IsInWorld() || !creature->IsAlive())
        return false;
    if (creature->hasLootRecipient() && !creature->isTappedBy(bot))
        return false;
    return !bot->IsFriendlyTo(creature);
}

Creature* Nearest(Player* bot, std::vector<uint32> const& entries, float range,
                  std::function<bool(Creature*)> const& valid, std::size_t& candidates)
{
    std::list<Creature*> creatures;
    bot->GetCreatureListWithEntryInGrid(creatures, entries, range);
    candidates = creatures.size();
    Creature* nearest = nullptr;
    float nearestYards = range;
    for (Creature* creature : creatures)
    {
        if (!valid(creature))
            continue;
        float const yards = bot->GetDistance(creature);
        if (yards < nearestYards)
        {
            nearest = creature;
            nearestYards = yards;
        }
    }
    return nearest;
}

ObjectGuid Held(NewRpgInfo const& info)
{
    if (auto const* quest = std::get_if<NewRpgInfo::DoQuest>(&info.data))
        return quest->targetGuid;
    if (auto const* gather = std::get_if<NewRpgInfo::DoGather>(&info.data))
        return gather->targetGuid;
    return ObjectGuid::Empty;
}
}  // namespace SoughtTarget
