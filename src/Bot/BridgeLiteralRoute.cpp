/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "BridgeLiteralRoute.h"

#include "AiObjectContext.h"
#include "ChatHelper.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "SharedDefines.h"

PlayerbotBridge::LiteralClass PlayerbotBridge::ClassifyBotLiteral(PlayerbotAI* botAI, uint32_t type,
                                                                  std::string const& text)
{
    LiteralScope scope = LiteralScope::Other;
    switch (type)
    {
        case CHAT_MSG_WHISPER:
            scope = LiteralScope::Whisper;
            break;
        case CHAT_MSG_PARTY:
        case CHAT_MSG_PARTY_LEADER:
        case CHAT_MSG_RAID:
        case CHAT_MSG_RAID_LEADER:
            scope = LiteralScope::Group;
            break;
        case CHAT_MSG_SAY:
        case CHAT_MSG_YELL:
            break;
        default:
            // Raid warnings, guild and channel lines reach bots by paths this probe does not mirror.
            return LiteralClass::Uncertain;
    }

    AiObjectContext* context = botAI ? botAI->GetAiObjectContext() : nullptr;
    if (!context)
        return LiteralClass::Uncertain;

    LiteralRules const rules{sPlayerbotAIConfig.CommandSeparator, sPlayerbotAIConfig.CommandPrefix,
                             sPlayerbotAIConfig.EnableAutoTradeOnItemMention};
    return ClassifyLiteral(
        text, scope, rules, [context](std::string const& name) { return context->HasTrigger(name); },
        [](std::string const& candidate) { return ChatHelper::parseableItem(candidate); });
}
