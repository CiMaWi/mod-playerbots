/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "PvpTestUtil.h"

#include <algorithm>
#include <cctype>
#include <sstream>

#include "BattlegroundMgr.h"
#include "BattlegroundQueue.h"
#include "CharacterCache.h"
#include "Chat.h"
#include "Group.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerbotAIConfig.h"
#include "StringFormat.h"
#include "World.h"

namespace PvpTestUtil
{
bool CommandFail(ChatHandler* handler, std::string const& text)
{
    handler->SendSysMessage(text);
    handler->SetSentErrorMessage(true);
    return false;
}

bool CheckEnabled(ChatHandler* handler)
{
    if (sPlayerbotAIConfig.PvpTestEnable)
        return true;

    return CommandFail(handler, "The PvP test harness is disabled (Playerbots.PvpTest.Enable = 0)");
}

std::string ToLower(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

std::string Trim(std::string const& text)
{
    size_t const first = text.find_first_not_of(" \t");
    if (first == std::string::npos)
        return "";

    size_t const last = text.find_last_not_of(" \t");
    return text.substr(first, last - first + 1);
}

std::vector<std::string> Tokenize(std::string const& text)
{
    std::vector<std::string> tokens;
    std::string current;
    bool quoted = false;
    bool hasToken = false;
    for (char const c : text)
    {
        if (c == '"')
        {
            quoted = !quoted;
            hasToken = true;
            continue;
        }

        if (!quoted && std::isspace(static_cast<unsigned char>(c)))
        {
            if (hasToken)
                tokens.push_back(current);

            current.clear();
            hasToken = false;
            continue;
        }

        current += c;
        hasToken = true;
    }

    if (hasToken)
        tokens.push_back(current);

    return tokens;
}

std::vector<std::string> Split(std::string const& text, char separator)
{
    std::vector<std::string> parts;
    std::stringstream stream(text);
    std::string part;
    while (std::getline(stream, part, separator))
    {
        part = Trim(part);
        if (!part.empty())
            parts.push_back(part);
    }

    return parts;
}

std::string FormatDuration(uint32 ms)
{
    uint32 const seconds = ms / MsPerSecond;
    return Acore::StringFormat("{}:{:02}", seconds / SecondsPerMinute, seconds % SecondsPerMinute);
}

std::string DescribeMember(ObjectGuid const& guid)
{
    Player* player = ObjectAccessor::FindConnectedPlayer(guid);
    if (!player)
    {
        CharacterCacheEntry const* entry = sCharacterCache->GetCharacterCacheByGuid(guid);
        return Acore::StringFormat("  {}: offline", entry ? entry->Name : guid.ToString());
    }

    uint32 queues = 0;
    for (uint32 slot = 0; slot < PLAYER_MAX_BATTLEGROUND_QUEUES; ++slot)
    {
        if (player->GetBattlegroundQueueTypeId(slot) != BATTLEGROUND_QUEUE_NONE)
            ++queues;
    }

    Group const* group = player->GetGroup();
    return Acore::StringFormat("  {}: map {}, group {}, battleground {}, queues {}", player->GetName(),
                               player->GetMapId(), group ? std::to_string(group->GetGUID().GetCounter()) : "none",
                               player->InBattleground() ? std::to_string(player->GetBattlegroundId()) : "none", queues);
}

void LeaveQueues(Player* player)
{
    for (uint32 slot = 0; slot < PLAYER_MAX_BATTLEGROUND_QUEUES; ++slot)
    {
        BattlegroundQueueTypeId const queueTypeId = player->GetBattlegroundQueueTypeId(slot);
        if (queueTypeId == BATTLEGROUND_QUEUE_NONE)
            continue;

        sBattlegroundMgr->GetBattlegroundQueue(queueTypeId).RemovePlayer(player->GetGUID(), true);
        player->RemoveBattlegroundQueueId(queueTypeId);
    }
}

bool CheckBot(ObjectGuid const& guid, std::string& error)
{
    CharacterCacheEntry const* entry = sCharacterCache->GetCharacterCacheByGuid(guid);
    if (!entry)
    {
        error = Acore::StringFormat("Character {} is unknown", guid.ToString());
        return false;
    }

    if (!sPlayerbotAIConfig.IsInRandomAccountList(entry->AccountId))
    {
        error = Acore::StringFormat("{} is not a random bot", entry->Name);
        return false;
    }

    uint32 const requiredLevel = sWorld->getIntConfig(CONFIG_MAX_PLAYER_LEVEL);
    if (entry->Level != requiredLevel)
    {
        error = Acore::StringFormat("{} is level {}, a match needs level {}", entry->Name, entry->Level, requiredLevel);
        return false;
    }

    return true;
}
}  // namespace PvpTestUtil
