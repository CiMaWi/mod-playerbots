/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "ItemUseCommands.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <map>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "Chat.h"
#include "Config.h"
#include "GitRevision.h"
#include "ItemBuffRules.h"
#include "ItemDemand.h"
#include "ItemUsageValue.h"
#include "ItemUse.h"
#include "ItemUseFacts.h"
#include "ItemUseIndex.h"
#include "ItemUseKinds.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "ThreadCheck.h"

namespace ItemUse
{
namespace
{
// A console reply prints at most this many lines of a list; the rest is counted
constexpr std::size_t LINES_SHOWN = 200;

std::string Trim(std::string text)
{
    std::size_t const first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
        return {};
    return text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
}

// The dump goes into the server's logs folder under a bare file name: players may run the command, so the argument
// never names a path elsewhere (an empty result: refused)
std::string DumpPath(std::string const& argument)
{
    std::string const name = argument.empty() ? "item-use-dump.txt" : argument;
    if (name.find_first_of("/\\:") != std::string::npos || name.find("..") != std::string::npos)
        return {};
    std::string dir = sConfigMgr->GetOption<std::string>("LogsDir", "", false);
    if (!dir.empty() && dir.back() != '/' && dir.back() != '\\')
        dir += '/';
    return dir + name;
}

// The worn slots' words of `item use` (INTERFACE.md 3.2), or the game's slot number
std::optional<std::uint8_t> SlotOfWord(std::string const& word)
{
    static std::array<std::pair<char const*, std::uint8_t>, 17> const words = {{{"head", 0},
                                                                                {"neck", 1},
                                                                                {"shoulders", 2},
                                                                                {"chest", 4},
                                                                                {"waist", 5},
                                                                                {"legs", 6},
                                                                                {"feet", 7},
                                                                                {"wrists", 8},
                                                                                {"hands", 9},
                                                                                {"finger1", 10},
                                                                                {"finger2", 11},
                                                                                {"trinket1", 12},
                                                                                {"trinket2", 13},
                                                                                {"back", 14},
                                                                                {"mainhand", 15},
                                                                                {"offhand", 16},
                                                                                {"ranged", 17}}};
    for (auto const& [name, slot] : words)
        if (word == name)
            return slot;
    if (!word.empty() && word.find_first_not_of("0123456789") == std::string::npos && word.size() <= 2 &&
        std::stoul(word) <= words.back().second)
        return static_cast<std::uint8_t>(std::stoul(word));
    return std::nullopt;
}

// A coordinate word: a finite number and nothing else
bool ReadCoordinate(std::istringstream& in, float& value)
{
    std::string word;
    if (!(in >> word))
        return false;
    char* end = nullptr;
    value = std::strtof(word.c_str(), &end);
    return end && *end == 0 && end != word.c_str() && std::isfinite(value);
}

// The words of `item use` after the bot's name into an order; false on a word it does not take
bool ParseOrder(std::istringstream& in, Order& order)
{
    std::string word;
    if (!(in >> word))
        return false;
    if (word == "slot")
    {
        std::string slotWord;
        in >> slotWord;
        std::optional<std::uint8_t> const slot = SlotOfWord(slotWord);
        if (!slot)
            return false;
        order.slot = *slot;
    }
    else
    {
        if (word.find_first_not_of("0123456789") != std::string::npos || word.size() > 9)
            return false;
        order.entry = static_cast<std::uint32_t>(std::stoul(word));
        if (!order.entry)
            return false;
    }

    while (in >> word)
    {
        if (word == "dead")
            order.dead = true;
        else if (word == "self")
            order.target = OrderTarget::Self;
        else if (word == "unit" && (in >> order.unitName))
            order.target = OrderTarget::Unit;
        else if (word == "creature" && (in >> order.creatureEntry) && order.creatureEntry)
            order.target = OrderTarget::Creature;
        else if (word == "at")
        {
            std::istringstream::pos_type const mark = in.tellg();
            std::string next;
            if (in >> next && next == "creature")
            {
                if (!(in >> order.creatureEntry) || !order.creatureEntry)
                    return false;
                order.target = OrderTarget::SpotCreature;
                continue;
            }
            in.clear();
            in.seekg(mark);
            if (!ReadCoordinate(in, order.x) || !ReadCoordinate(in, order.y) || !ReadCoordinate(in, order.z))
                return false;
            order.target = OrderTarget::Spot;
        }
        else if (word == "object" && (in >> order.objectEntry) && order.objectEntry)
            order.target = OrderTarget::Object;
        else if (word == "item")
        {
            std::string slotWord;
            in >> slotWord;
            std::optional<std::uint8_t> const slot = SlotOfWord(slotWord);
            if (!slot)
                return false;
            order.target = OrderTarget::Item;
            order.itemSlot = *slot;
        }
        else
            return false;
    }
    return true;
}
}  // namespace

bool HandleCensusCommand(ChatHandler* handler, char const* args)
{
    ItemUseIndex const& index = ItemUseIndex::Get();
    ItemUseIndex::Stats const& stats = index.GetStats();
    bool const listUnclassified = Trim(args ? args : "").find("unclassified") != std::string::npos;

    std::map<std::pair<int, int>, std::uint32_t> classes;
    std::array<std::uint32_t, static_cast<std::size_t>(Shape::Count)> shapes{};
    std::array<std::uint32_t, static_cast<std::size_t>(Kind::Count)> kinds{};
    std::uint32_t cut = 0;
    std::vector<std::pair<std::uint32_t, Classified>> unclassified;
    std::span<Record const> const all = index.All();
    for (std::uint32_t id = 0; id < all.size(); ++id)
    {
        Record const& record = all[id];
        if (record.source == Source::ItemSpell)
            ++classes[{record.itemClass, record.itemSubClass}];
        ++shapes[static_cast<std::size_t>(record.shape)];
        Classified const result = Classify(record);
        ++kinds[static_cast<std::size_t>(result.kind)];
        cut += record.effectsCut ? 1 : 0;
        if (result.kind == Kind::Unclassified)
            unclassified.emplace_back(id, result);
    }

    if (listUnclassified)
    {
        handler->PSendSysMessage("ITEM census unclassified: {} records", unclassified.size());
        for (std::size_t i = 0; i < unclassified.size() && i < LINES_SHOWN; ++i)
        {
            Record const& record = all[unclassified[i].first];
            bool const item = record.source == Source::ItemSpell;
            handler->PSendSysMessage("ITEM unclassified {} {} {} {}: {}", item ? record.itemEntry : record.enchantId,
                                     item ? "entry" : "enchant", record.spellId, index.NameOf(unclassified[i].first),
                                     unclassified[i].second.why);
        }
        if (unclassified.size() > LINES_SHOWN)
            handler->PSendSysMessage("ITEM unclassified: {} more (see the dump)", unclassified.size() - LINES_SHOWN);
        return true;
    }

    handler->PSendSysMessage("ITEM census: records {} items {} enchants {} cut {} no_spell {} build_ms {}", all.size(),
                             stats.itemRecords, stats.enchantRecords, cut, stats.missingSpells, stats.buildMs);
    std::size_t lines = 0;
    for (auto const& [key, count] : classes)
    {
        if (++lines > LINES_SHOWN)
            break;
        handler->PSendSysMessage("ITEM census class {} sub {}: {}", key.first, key.second, count);
    }
    for (std::size_t shape = 0; shape < shapes.size(); ++shape)
        handler->PSendSysMessage("ITEM census shape {}: {}", ShapeWord(static_cast<Shape>(shape)), shapes[shape]);
    for (std::size_t kind = 0; kind < kinds.size(); ++kind)
        handler->PSendSysMessage("ITEM census kind {}: {}", KindWord(static_cast<Kind>(kind)), kinds[kind]);
    return true;
}

bool HandleInfoCommand(ChatHandler* handler, char const* args)
{
    std::istringstream in(args ? args : "");
    std::uint32_t entry = 0;
    std::string word;
    in >> entry >> word;
    if (!entry)
    {
        handler->SendErrorMessage("Usage: playerbots item info <entry> [enchant]");
        return false;
    }
    bool const enchant = word == "enchant";
    ItemUseIndex const& index = ItemUseIndex::Get();
    std::span<Record const> const records = enchant ? index.OfEnchant(entry) : index.OfItem(entry);
    if (records.empty())
    {
        handler->PSendSysMessage("ITEM info: {} has no use effect", entry);
        return true;
    }
    for (Record const& record : records)
    {
        Classified const result = Classify(record);
        handler->PSendSysMessage("ITEM info {} {}: kind {} {}", entry, enchant ? "enchant" : "entry",
                                 KindWord(result.kind), result.why);
        handler->PSendSysMessage("{}", FormatDumpLine(record, index.NameOf(index.IdOf(record))));
    }
    return true;
}

bool HandleDumpCommand(ChatHandler* handler, char const* args)
{
    ItemUseIndex const& index = ItemUseIndex::Get();
    std::string const path = DumpPath(Trim(args ? args : ""));
    if (path.empty())
    {
        handler->SendErrorMessage("ITEM dump: give a bare file name; the dump goes into the logs folder");
        return false;
    }

    std::string text = DumpHeader("unknown", GitRevision::GetHash(), index.All().size());
    text += '\n';
    text += DumpFieldNames();
    text += '\n';
    for (std::uint32_t id = 0; id < index.All().size(); ++id)
    {
        text += FormatDumpLine(index.All()[id], index.NameOf(id));
        text += '\n';
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    out.flush();
    if (!out)
    {
        handler->SendErrorMessage("ITEM dump: cannot write {}", path);
        return false;
    }
    handler->PSendSysMessage("ITEM dump: {} records to {}", index.All().size(), path);
    return true;
}

bool HandleUseCommand(ChatHandler* handler, char const* args)
{
    std::istringstream in(args ? args : "");
    std::string name;
    in >> name;
    Order order;
    if (name.empty() || !ParseOrder(in, order))
    {
        handler->SendErrorMessage(
            "Usage: playerbots item use <bot> <entry>|slot <slot> [self | unit <name> | creature "
            "<entry> | item <slot> | at <x> <y> <z> | at creature <entry> | object <entry>] [dead]");
        return false;
    }

    Player* bot = ObjectAccessor::FindPlayerByName(name, true);
    PlayerbotAI* botAI = bot ? PlayerbotsMgr::instance().GetPlayerbotAI(bot) : nullptr;
    if (!botAI)
    {
        handler->SendErrorMessage("ITEM use: {} is not an online bot", name);
        return false;
    }

    std::uint32_t const entry = order.entry;
    std::uint8_t const slot = order.slot;
    PostOrder(botAI, bot->GetGUID().GetRawValue(), std::move(order));
    if (slot != SLOT_BAGS)
        handler->PSendSysMessage("ITEM use: {} slot {} ordered", bot->GetName(), slot);
    else
        handler->PSendSysMessage("ITEM use: {} {} ordered", bot->GetName(), entry);
    return true;
}

bool HandleAheadCommand(ChatHandler* handler, char const* args)
{
    std::istringstream in(args ? args : "");
    std::string name;
    std::string word;
    in >> name >> word;
    Ahead ahead = Ahead::None;
    bool const automatic = word == "auto";
    if (name.empty() || (!automatic && !AheadOfWord(word, ahead)))
    {
        handler->SendErrorMessage("Usage: playerbots item ahead <bot> dungeon|elite|group|bg|none|auto");
        return false;
    }

    Player* bot = ObjectAccessor::FindPlayerByName(name, true);
    PlayerbotAI* botAI = bot ? PlayerbotsMgr::instance().GetPlayerbotAI(bot) : nullptr;
    if (!botAI)
    {
        handler->SendErrorMessage("ITEM ahead: {} is not an online bot", name);
        return false;
    }

    Order order;
    order.setsAhead = true;
    order.ahead = automatic ? AHEAD_AUTO : static_cast<std::uint8_t>(ahead);
    PostOrder(botAI, bot->GetGUID().GetRawValue(), std::move(order));
    handler->PSendSysMessage("ITEM ahead: {} {} ordered", bot->GetName(), automatic ? "auto" : AheadWord(ahead));
    return true;
}

namespace
{
constexpr std::array<std::string_view, 5> ROLE_WORDS = {"melee", "ranged", "tank", "healer", "caster"};

std::string_view RoleWordOf(Role role)
{
    switch (role)
    {
        case Role::Melee:
            return ROLE_WORDS[0];
        case Role::Ranged:
            return ROLE_WORDS[1];
        case Role::Tank:
            return ROLE_WORDS[2];
        case Role::Healer:
            return ROLE_WORDS[3];
        case Role::Caster:
            return ROLE_WORDS[4];
        default:
            return "unknown";
    }
}

std::string_view UsageWord(ItemUsage usage)
{
    static constexpr std::array<std::string_view, 15> WORDS = {
        "none",       "equip",      "replace", "bad_equip", "broken_equip", "quest", "skill", "use",
        "guild_task", "disenchant", "ah",      "keep",      "vendor",       "ammo",  "bank"};
    return usage < WORDS.size() ? WORDS[usage] : std::string_view("unknown");
}

std::string NameOfItem(std::uint32_t entry)
{
    ItemTemplate const* proto = entry ? sObjectMgr->GetItemTemplate(entry) : nullptr;
    return proto ? proto->Name1 : std::string("-");
}
}  // namespace

bool HandleDemandCommand(ChatHandler* handler, char const* args)
{
    std::istringstream in(args ? args : "");
    std::string name;
    in >> name;
    if (name.empty())
    {
        handler->SendErrorMessage("Usage: playerbots item demand <bot>");
        return false;
    }

    Player* bot = ObjectAccessor::FindPlayerByName(name, true);
    PlayerbotAI* botAI = bot ? PlayerbotsMgr::instance().GetPlayerbotAI(bot) : nullptr;
    if (!botAI)
    {
        handler->SendErrorMessage("ITEM demand: {} is not an online bot", name);
        return false;
    }

    // Commands run on the world thread inside World::Update, outside MapMgr::Update (which waits for its map
    // threads): no map thread touches the bot while the answer reads its bags, as `planner needs` does
    PLAYERBOT_CHECK_WORLD_THREAD("item demand");
    DemandBuilt const built = BuildDemand(botAI, bot);
    DemandAnswer const& answer = built.answer;
    BotItems const* items = botAI->botItems.get();
    std::string age = "none";
    if (items && items->demand && items->demand->valid)
        age = std::to_string(getMSTimeDiff(items->demand->builtMs, getMSTime()));
    handler->PSendSysMessage(
        "ITEM demand {}: persistent {}, control {}, class {}, level {}, role {}, earn {} c/min, cache age {} ms",
        bot->GetName(), answer.persistent ? "yes" : "no", answer.control ? "yes" : "no", bot->getClass(),
        bot->GetLevel(), RoleWordOf(built.role), built.earnCopperPerMinute, age);
    if (!answer.persistent)
    {
        handler->PSendSysMessage("ITEM demand: no demand (not persistent)");
        return true;
    }

    for (DemandLine const& line : answer.lines)
    {
        if (!line.wantedStock && !line.held && !line.bestEntry)
            continue;
        handler->PSendSysMessage("ITEM demand {} {}/{}: want {}, hold {}, use {:.2f}/h {}, worth {} c each, best {} {}",
                                 DemandSlotWord(line.slot), KindWord(line.kind), SubWord(line.sub), line.wantedStock,
                                 line.held, line.usePerHour, line.measured ? "measured" : "default",
                                 line.worthCopperEach, line.bestEntry, NameOfItem(line.bestEntry));
        if (line.buyUnits)
            handler->PSendSysMessage("ITEM demand {}: buy {} {} x{}", DemandSlotWord(line.slot), line.buyEntry,
                                     NameOfItem(line.buyEntry), line.buyUnits);
    }

    AiObjectContext* context = botAI->GetAiObjectContext();
    for (DemandHeld const& held : built.held)
    {
        DemandSlot slot = DemandSlot::Heal;
        DemandVerdict const verdict = VerdictOf(answer, held.entry, &slot);
        std::uint32_t keep = 0;
        for (DemandKeep const& line : answer.keeps)
            if (line.entry == held.entry)
                keep = std::max(keep, line.keepUnits);
        ItemUsage const usage = context->GetValue<ItemUsage>("item usage", held.entry)->Get();
        handler->PSendSysMessage("ITEM demand item {} {} x{}: {}{}{} keep {}, usage {}", held.entry,
                                 NameOfItem(held.entry), held.count, DemandVerdictWord(verdict),
                                 verdict == DemandVerdict::None ? "" : " ",
                                 verdict == DemandVerdict::None ? "" : DemandSlotWord(slot), keep, UsageWord(usage));
    }
    return true;
}
}  // namespace ItemUse
