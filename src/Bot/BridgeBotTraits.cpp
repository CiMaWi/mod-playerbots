/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "BridgeBotTraits.h"

#include <optional>

#include "BotTraitMgr.h"
#include "ObjectGuid.h"

namespace PlayerbotBridge
{
bool ReadBotPersonality(ObjectGuid const& botGuid, BotPersonality& out)
{
    out = BotPersonality{};
    if (!botGuid.IsPlayer())
        return false;

    std::optional<TraitRules::BotTraits> const traits = BotTraitMgr::instance().Find(botGuid.GetCounter());
    if (!traits)
        return false;

    out.words = TraitRules::Describe(*traits);
    out.areas = TraitRules::DescribeAreas(*traits);
    out.interests = TraitRules::InterestNames(*traits);
    return true;
}
}  // namespace PlayerbotBridge
