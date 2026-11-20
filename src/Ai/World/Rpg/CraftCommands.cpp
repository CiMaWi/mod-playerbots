/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "CraftCommands.h"

#include <algorithm>
#include <optional>
#include <sstream>
#include <string>

#include "BankMailService.h"
#include "BankMailState.h"
#include "BotKind.h"
#include "Chat.h"
#include "CraftVisit.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "StringConvert.h"

namespace BankMail
{
namespace
{
// A test command's ceiling on the casts it asks for (an errand asks for what the bot uses)
constexpr uint32 CRAFT_COMMAND_MAX_CASTS = 20;

// The named bot online with its AI, or null with the error sent
PlayerbotAI* NamedBot(ChatHandler* handler, std::string name, char const* verb)
{
    std::string const asked = name;
    Player* bot = normalizePlayerName(name) ? ObjectAccessor::FindPlayerByName(name) : nullptr;
    PlayerbotAI* botAI = bot && bot->IsInWorld() ? GET_PLAYERBOT_AI(bot) : nullptr;
    if (!botAI)
        handler->SendErrorMessage("CRAFT {}: no bot {} online", verb, asked);
    return botAI;
}

// Lab fixtures, arena bots and bots with a real master keep today's behaviour (the chat command `craft`); a bot
// runs one visit at a time. Only what the bot cannot change refuses: materials, bag space and the station in reach
// are the visit's own ends, so a started visit ends with them
char const* StartRefusal(PlayerbotAI* botAI)
{
    Player* bot = botAI->GetBot();
    if (!BotKind::KeepsPossessions(bot))
        return "the bot does not keep its possessions (not a persistent bot)";
    if (!bot->IsAlive())
        return "the bot is dead";
    if (!botAI->HasStrategy("new rpg", BOT_STATE_NON_COMBAT))
        return "the bot has no new rpg strategy";
    return VisitStartRefusal(botAI->bankMail, VisitKind::Craft);
}

// An optional number word: 0 when absent, nullopt when not a number
std::optional<uint32> OptionalNumber(std::string const& word)
{
    return word.empty() ? std::optional<uint32>(0) : Acore::StringTo<uint32>(word);
}
}  // namespace

bool HandleCraftVisitCommand(ChatHandler* handler, char const* args)
{
    std::istringstream in(args ? args : "");
    std::string name;
    std::string spellWord;
    std::string countWord;
    in >> name >> spellWord >> countWord;
    if (name.empty())
    {
        handler->PSendSysMessage("CRAFT visits: {} (usage: playerbots craft visit <bot name> <spell> [<count>])",
                                 CraftVisitCounters());
        return true;
    }
    std::optional<uint32> const spell = Acore::StringTo<uint32>(spellWord);
    std::optional<uint32> const count = OptionalNumber(countWord);
    if (!spell || !*spell || !count)
    {
        handler->SendErrorMessage("Usage: playerbots craft visit <bot name> <spell> [<count>]");
        return false;
    }
    PlayerbotAI* botAI = NamedBot(handler, name, "visit");
    if (!botAI)
        return false;
    Player* bot = botAI->GetBot();

    // World thread, between the map updates: the bot's bags, place and visit state are read and set at rest
    char const* refusal = StartRefusal(botAI);
    std::optional<CraftOrder> order = refusal ? std::nullopt : RecipeOrder(bot, *spell, *count, refusal);
    std::optional<StationSpot> spot;
    if (order && order->focus)
    {
        spot = FindStationSpot(bot, order->focus, refusal);
        // A test command (WB.12): the bot stands near a station; getting to a far one is the craft errand's job
        if (spot && !WithinVisitReach(spot->distance))
        {
            handler->SendErrorMessage(
                "CRAFT visit {}: refused, {} (the nearest station is {:.0f} yd away, reach {:.0f} yd)", bot->GetName(),
                VISIT_TOO_FAR, spot->distance, VISIT_COMMAND_REACH_YARDS);
            return false;
        }
        if (!spot)
            order.reset();
    }
    if (order)
        order->casts = std::min(order->casts, CRAFT_COMMAND_MAX_CASTS);
    if (!order || !BeginCraftVisit(botAI, *order, spot ? &*spot : nullptr))
    {
        handler->SendErrorMessage("CRAFT visit {}: refused, {}", bot->GetName(), refusal ? refusal : "no visit");
        return false;
    }

    if (spot)
        handler->PSendSysMessage(
            "CRAFT visit {}: started, spell {} x{}, station {} at ({:.0f}, {:.0f}) on map {}, {:.0f} yd away",
            bot->GetName(), order->spell, order->casts, spot->entry, spot->x, spot->y, spot->mapId, spot->distance);
    else
        handler->PSendSysMessage("CRAFT visit {}: started, spell {} x{} where the bot stands", bot->GetName(),
                                 order->spell, order->casts);
    return true;
}

bool HandleCraftProcessCommand(ChatHandler* handler, char const* args)
{
    std::istringstream in(args ? args : "");
    std::string name;
    std::string word;
    std::string entryWord;
    in >> name >> word >> entryWord;
    uint32 const spell = ProcessSpellOf(word);
    std::optional<uint32> const entry = OptionalNumber(entryWord);
    if (name.empty() || !spell || !entry)
    {
        handler->SendErrorMessage("Usage: playerbots craft process <bot name> disenchant|prospect|mill [<item entry>]");
        return false;
    }
    PlayerbotAI* botAI = NamedBot(handler, name, "process");
    if (!botAI)
        return false;
    Player* bot = botAI->GetBot();

    char const* refusal = StartRefusal(botAI);
    if (!refusal && !bot->HasSpell(spell))
        refusal = "the bot does not know the processing spell";
    // A named item is processed whatever the bags hold now (the visit ends with no_materials); without one, the first
    // bag item the spell accepts, or nothing to start
    uint32 casts = 0;
    uint32 const item = refusal ? 0 : ProcessableItem(bot, spell, *entry, casts);
    if (!refusal && !item && !*entry)
        refusal = "nothing in the bags the bot can process so";
    CraftOrder const order{spell, item ? item : *entry, 0, std::clamp<uint32>(casts, 1, CRAFT_COMMAND_MAX_CASTS)};
    if (refusal || !BeginCraftVisit(botAI, order, nullptr))
    {
        handler->SendErrorMessage("CRAFT process {}: refused, {}", bot->GetName(), refusal ? refusal : "no visit");
        return false;
    }
    handler->PSendSysMessage("CRAFT process {}: started, {} item {} x{} where the bot stands", bot->GetName(), word,
                             order.source, order.casts);
    return true;
}
}  // namespace BankMail
