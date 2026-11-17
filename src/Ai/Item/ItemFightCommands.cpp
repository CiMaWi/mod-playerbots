/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "ItemFightCommands.h"

#include <sstream>
#include <string>

#include "Chat.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"

namespace ItemUse
{
bool HandleUsableCommand(ChatHandler* handler, char const* args)
{
    std::istringstream in(args ? args : "");
    std::string name;
    in >> name;
    if (name.empty())
    {
        handler->SendErrorMessage("Usage: playerbots item usable <bot>");
        return false;
    }

    Player* bot = ObjectAccessor::FindPlayerByName(name, true);
    PlayerbotAI* botAI = bot ? PlayerbotsMgr::instance().GetPlayerbotAI(bot) : nullptr;
    if (!botAI)
    {
        handler->SendErrorMessage("ITEM usable: {} is not an online bot", name);
        return false;
    }

    // the print runs on the bot's map thread (ItemUse::TakeUsablePrints); the bit is the whole order
    botAI->AddWork(BotWork::ItemUsable);
    handler->PSendSysMessage("ITEM usable: {} ordered", bot->GetName());
    return true;
}
}  // namespace ItemUse
