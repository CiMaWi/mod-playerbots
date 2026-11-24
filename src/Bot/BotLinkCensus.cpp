/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "BotLinkCensus.h"

#include "AiObjectContext.h"
#include "Group.h"
#include "LootObjectStack.h"
#include "NewRpgInfo.h"
#include "ObjectAccessor.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "ReachCounter.h"
#include "TravelMgr.h"

namespace
{
constexpr std::size_t NoFamily = ReachCounter::LinkFamilyMax;

bool registered = false;
std::size_t familyMaster = NoFamily;
std::size_t familyGroupLeader = NoFamily;
std::size_t familyCurrentTarget = NoFamily;
std::size_t familyRpgTarget = NoFamily;
std::size_t familyLootTarget = NoFamily;
std::size_t familyRpgNpc = NoFamily;

// A bot's own stored references, read only and O(1) per bot. The cached calculated unit values are not read: their
// raw pointers are refreshed only by Get(), a lazy read could hit a stale one.
void WalkBotLinks(Unit& unit, ReachCounter::LinkAction /*action*/, ReachCounter::LinkTally& tally)
{
    Player* const bot = unit.ToPlayer();
    PlayerbotAI* const ai = bot ? GET_PLAYERBOT_AI(bot) : nullptr;
    if (!ai)
        return;

    Player* const master = ai->GetMaster();
    if (master && master->IsInWorld())
        ReachCounter::CountLink(tally, familyMaster, *bot, master);

    // The leader as PlayerbotAI::GetGroupLeader finds it, without the master fallback
    if (!bot->InBattleground())
        if (Group const* group = bot->GetGroup())
            ReachCounter::CountLink(tally, familyGroupLeader, *bot, ObjectAccessor::FindPlayer(group->GetLeaderGUID()));

    AiObjectContext* const context = ai->GetAiObjectContext();
    if (!context)
        return;

    if (Value<Unit*>* const current = context->GetValue<Unit*>("current target"))
        ReachCounter::CountLink(tally, familyCurrentTarget, *bot, current->Get());

    if (Value<GuidPosition>* const rpg = context->GetValue<GuidPosition>("rpg target"))
    {
        ObjectGuid const guid = rpg->LazyGet();
        if (guid)
            ReachCounter::CountLink(tally, familyRpgTarget, *bot, ObjectAccessor::GetWorldObject(*bot, guid));
    }

    if (Value<LootObject>* const loot = context->GetValue<LootObject>("loot target"))
    {
        ObjectGuid const guid = loot->LazyGet().guid;
        if (guid)
            ReachCounter::CountLink(tally, familyLootTarget, *bot, ObjectAccessor::GetWorldObject(*bot, guid));
    }

    if (NewRpgInfo::WanderNpc const* const wander = std::get_if<NewRpgInfo::WanderNpc>(&ai->rpgInfo.data))
        if (wander->npcOrGo)
            ReachCounter::CountLink(tally, familyRpgNpc, *bot, ObjectAccessor::GetWorldObject(*bot, wander->npcOrGo));
}
}  // namespace

void RegisterBotLinkCensus()
{
    if (registered)
        return;

    registered = true;
    familyMaster = ReachCounter::RegisterLinkFamily("botlink_master");
    familyGroupLeader = ReachCounter::RegisterLinkFamily("botlink_group_leader");
    familyCurrentTarget = ReachCounter::RegisterLinkFamily("botlink_current_target");
    familyRpgTarget = ReachCounter::RegisterLinkFamily("botlink_rpg_target");
    familyLootTarget = ReachCounter::RegisterLinkFamily("botlink_loot_target");
    familyRpgNpc = ReachCounter::RegisterLinkFamily("botlink_rpg_npc");
    ReachCounter::RegisterLinkWalker(&WalkBotLinks);
}
