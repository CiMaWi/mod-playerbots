/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "Fixture.h"

#include <algorithm>
#include <cstdint>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "AiFactory.h"
#include "Chat.h"
#include "ClassQuestCompletion.h"
#include "DBCStores.h"
#include "Item.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "PlayerbotFactory.h"
#include "Playerbots.h"
#include "PvpGearMgr.h"
#include "SpellMgr.h"

namespace Fixture
{
namespace
{
// A premade spec's talents as the factory's template walks them: its points per tab over every level's link, and each
// talent as one key (tab page, row, column)
struct SpecShape
{
    uint32 points[3] = {0, 0, 0};
    std::set<uint32> keys;
    uint32 tab = 0;
    // the deepest talent (highest row) the template takes in its own tab: the talent that defines the spec
    uint32 definingKey = 0;
    bool hasDefining = false;
};

uint32 KeyOf(uint32 tab, uint32 row, uint32 col) { return (tab << 16) | (row << 8) | col; }

SpecShape ShapeOf(uint32 cls, uint32 specNo)
{
    SpecShape shape;
    for (uint32 level = 0; level < MAX_LEVEL; ++level)
        for (std::vector<uint32> const& p : sPlayerbotAIConfig.ParsedSpecLinkOrder[cls][specNo][level])
            if (p.size() >= 4 && p[0] < 3)
            {
                shape.points[p[0]] += p[3];
                shape.keys.insert(KeyOf(p[0], p[1], p[2]));
            }
    for (uint32 i = 1; i < 3; ++i)
        if (shape.points[i] > shape.points[shape.tab])
            shape.tab = i;
    uint32 deepest = 0;
    for (uint32 level = 0; level < MAX_LEVEL; ++level)
        for (std::vector<uint32> const& p : sPlayerbotAIConfig.ParsedSpecLinkOrder[cls][specNo][level])
            if (p.size() >= 4 && p[0] == shape.tab && (!shape.hasDefining || p[1] > deepest))
            {
                deepest = p[1];
                shape.definingKey = KeyOf(p[0], p[1], p[2]);
                shape.hasDefining = true;
            }
    return shape;
}

// every talent the bot owns in its active spec, as keys, and the ranks per tab (what AiFactory sums)
std::set<uint32> OwnedKeys(Player* bot)
{
    std::set<uint32> owned;
    for (auto const& [spellId, talent] : bot->GetTalentMap())
    {
        if ((bot->GetActiveSpecMask() & talent->specMask) == 0)
            continue;
        TalentSpellPos const* pos = GetTalentSpellPos(spellId);
        TalentEntry const* info = pos ? sTalentStore.LookupEntry(pos->talent_id) : nullptr;
        TalentTabEntry const* tab = info ? sTalentTabStore.LookupEntry(info->TalentTab) : nullptr;
        if (tab)
            owned.insert(KeyOf(tab->tabpage, info->Row, info->Col));
    }
    return owned;
}

std::string SlotList(std::vector<uint32> const& slots)
{
    if (slots.empty())
        return "-";
    std::ostringstream out;
    for (size_t i = 0; i < slots.size(); ++i)
        out << (i ? "," : "") << slots[i];
    return out.str();
}

struct Slots
{
    std::vector<uint32> filled;
    std::vector<uint32> empty;
    // empty slots a bot of this level can fill: the factory leaves neck and head empty below 30, rings below 20 and
    // trinkets below 50, and the off hand and the ranged slot depend on the weapon and the class
    std::vector<uint32> emptyHard;
};

Slots SlotsOf(Player* bot)
{
    Slots slots;
    uint32 const level = bot->GetLevel();
    for (uint32 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
    {
        if (slot == EQUIPMENT_SLOT_BODY || slot == EQUIPMENT_SLOT_TABARD)
            continue;
        if (bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
        {
            slots.filled.push_back(slot);
            continue;
        }
        slots.empty.push_back(slot);
        bool const soft = slot == EQUIPMENT_SLOT_OFFHAND || slot == EQUIPMENT_SLOT_RANGED ||
                          ((slot == EQUIPMENT_SLOT_NECK || slot == EQUIPMENT_SLOT_HEAD) && level < 30) ||
                          ((slot == EQUIPMENT_SLOT_FINGER1 || slot == EQUIPMENT_SLOT_FINGER2) && level < 20) ||
                          ((slot == EQUIPMENT_SLOT_TRINKET1 || slot == EQUIPMENT_SLOT_TRINKET2) && level < 50);
        if (!soft)
            slots.emptyHard.push_back(slot);
    }
    return slots;
}

uint32 SpecCount(uint32 cls)
{
    uint32 count = 0;
    while (count < MAX_SPECNO && !sPlayerbotAIConfig.PremadeSpecName[cls][count].empty())
        ++count;
    return count;
}

bool EndsWith(std::string const& text, std::string const& tail)
{
    return text.size() >= tail.size() && text.compare(text.size() - tail.size(), tail.size(), tail) == 0;
}

// the full name, else `<name> pve`, else the first pve row that starts with the name; -1 when none
int SpecNoOf(uint32 cls, std::string const& name)
{
    uint32 const count = SpecCount(cls);
    for (uint32 i = 0; i < count; ++i)
        if (sPlayerbotAIConfig.PremadeSpecName[cls][i] == name)
            return int(i);
    for (uint32 i = 0; i < count; ++i)
        if (sPlayerbotAIConfig.PremadeSpecName[cls][i] == name + " pve")
            return int(i);
    for (uint32 i = 0; i < count; ++i)
    {
        std::string const& row = sPlayerbotAIConfig.PremadeSpecName[cls][i];
        if (row.compare(0, name.size(), name) == 0 && EndsWith(row, "pve"))
            return int(i);
    }
    return -1;
}

std::string NamesOf(uint32 cls)
{
    std::ostringstream out;
    for (uint32 i = 0; i < SpecCount(cls); ++i)
        out << (i ? ", " : "") << sPlayerbotAIConfig.PremadeSpecName[cls][i];
    return out.str();
}

uint32 SpentPoints(Player* bot)
{
    std::map<uint8, uint32> const tabs = AiFactory::GetPlayerSpecTabs(bot);
    uint32 spent = 0;
    for (auto const& [tab, ranks] : tabs)
        spent += ranks;
    return spent;
}

void Spec(ChatHandler* handler, Player* bot, std::string const& name)
{
    uint32 const cls = bot->getClass();
    if (name == "list")
    {
        for (uint32 i = 0; i < SpecCount(cls); ++i)
            handler->PSendSysMessage("FIXTURE spec {} list: {} {} tab={}", bot->GetName(), i,
                                     sPlayerbotAIConfig.PremadeSpecName[cls][i], ShapeOf(cls, i).tab);
        return;
    }
    int const specNo = SpecNoOf(cls, name);
    if (specNo < 0)
    {
        handler->SendErrorMessage("FIXTURE spec {}: error, no spec '{}' for class {}: {}", bot->GetName(), name, cls,
                                  NamesOf(cls));
        return;
    }
    PlayerbotFactory::InitTalentsBySpecNo(bot, specNo, true);
    PlayerbotFactory factory(bot, bot->GetLevel());
    factory.InitGlyphs(false);
    handler->PSendSysMessage("FIXTURE spec {}: ok, {} specno {} spent {} free {}", bot->GetName(),
                             sPlayerbotAIConfig.PremadeSpecName[cls][specNo], specNo, SpentPoints(bot),
                             bot->GetFreeTalentPoints());
}

void Spells(ChatHandler* handler, Player* bot)
{
    size_t const before = bot->GetSpellMap().size();
    PlayerbotFactory factory(bot, bot->GetLevel());
    factory.InitClassSpells();
    factory.InitAvailableSpells();
    // the class quests a character of this level did, as at its creation (upkeep WU40); queued until the quest index
    // is ready
    Planner::ClassQuestCompletion::Request(bot, Planner::ClassQuests::When::Creation);
    handler->PSendSysMessage("FIXTURE spells {}: ok, taught {} (spells {} -> {})", bot->GetName(),
                             bot->GetSpellMap().size() - before, before, bot->GetSpellMap().size());
}

void Gear(ChatHandler* handler, Player* bot)
{
    if (PvpGearMgr::Instance().IsWorn(bot))
    {
        handler->PSendSysMessage("FIXTURE gear {}: ok, a PvP loadout is worn, left as it is", bot->GetName());
        return;
    }
    // quality 0: the factory takes Playerbots.RandomGearQualityLimit and RandomGearScoreLimit (the random bots'
    // limits, not the autogear command's)
    PlayerbotFactory factory(bot, bot->GetLevel());
    factory.InitEquipment(false);
    factory.InitAmmo();
    Slots const slots = SlotsOf(bot);
    handler->PSendSysMessage("FIXTURE gear {}: ok, filled {} empty {} (quality limit {})", bot->GetName(),
                             slots.filled.size(), SlotList(slots.empty), sPlayerbotAIConfig.RandomGearQualityLimit);
}

void Show(ChatHandler* handler, Player* bot)
{
    uint32 const cls = bot->getClass();
    std::map<uint8, uint32> tabs = AiFactory::GetPlayerSpecTabs(bot);
    std::set<uint32> const owned = OwnedKeys(bot);
    int best = -1;
    uint32 bestScore = 0;
    for (uint32 i = 0; i < SpecCount(cls); ++i)
    {
        uint32 score = 0;
        for (uint32 key : ShapeOf(cls, i).keys)
            score += owned.count(key);
        if (score > bestScore)
        {
            bestScore = score;
            best = int(i);
        }
    }
    Slots const slots = SlotsOf(bot);
    handler->PSendSysMessage(
        "FIXTURE show {}: class={} level={} tab={} name={} tabs={}/{}/{} free={} spent={} "
        "specno={} filled={} empty={}",
        bot->GetName(), cls, bot->GetLevel(), uint32(AiFactory::GetPlayerSpecTab(bot)),
        AiFactory::GetPlayerSpecName(bot), tabs[0], tabs[1], tabs[2], bot->GetFreeTalentPoints(), SpentPoints(bot),
        best, SlotList(slots.filled), SlotList(slots.empty));
}

// The verdict a scenario reads from the console reply: error (a plain failure), gap (the bot lacks what defines the
// spec at its level or the expansion limit holds points back: input for class combat, listed, not failed), else ok
void Check(ChatHandler* handler, Player* bot, std::string const& name, bool gear)
{
    uint32 const cls = bot->getClass();
    uint32 const level = bot->GetLevel();
    int const specNo = SpecNoOf(cls, name);
    std::vector<std::string> errors;
    std::vector<std::string> gaps;
    std::ostringstream facts;
    if (specNo < 0)
        errors.push_back("no spec '" + name + "' for the class");
    else if (level < 10)
        errors.push_back("level below 10 has no spec");
    else
    {
        SpecShape const shape = ShapeOf(cls, specNo);
        uint32 const tab = AiFactory::GetPlayerSpecTab(bot);
        uint32 const spent = SpentPoints(bot);
        uint32 const free = bot->GetFreeTalentPoints();
        uint32 const budget = level - 9;
        facts << "spec " << sPlayerbotAIConfig.PremadeSpecName[cls][specNo] << " tab " << tab << " want " << shape.tab
              << " spent " << spent << " free " << free << " budget " << budget;
        if (tab != shape.tab)
            errors.push_back("printed tab " + std::to_string(tab) + " is not the spec's " + std::to_string(shape.tab));
        if (spent + free != budget)
            errors.push_back("spent " + std::to_string(spent) + " + free " + std::to_string(free) +
                             " is not the budget " + std::to_string(budget));
        else if (free > 0)
        {
            std::string const why = "free points " + std::to_string(free) + " unspent";
            if (sPlayerbotAIConfig.LimitTalentsExpansion && level < 80)
                gaps.push_back(why + " (expansion limit)");
            else
                errors.push_back(why);
        }
        if (shape.hasDefining && !OwnedKeys(bot).count(shape.definingKey))
            gaps.push_back("spec-gap: the defining talent (row " + std::to_string((shape.definingKey >> 8) & 0xFF) +
                           " of tab " + std::to_string(shape.tab) + ") is not owned at level " + std::to_string(level));
    }
    if (gear)
    {
        Slots const slots = SlotsOf(bot);
        facts << " filled " << slots.filled.size() << " empty " << SlotList(slots.empty);
        if (!slots.emptyHard.empty())
            errors.push_back("empty slots a bot of the level can fill: " + SlotList(slots.emptyHard));
    }
    std::string const word = !errors.empty() ? "error" : (!gaps.empty() ? "gap" : "ok");
    std::ostringstream out;
    out << "FIXTURE check " << bot->GetName() << ": " << word << ", level " << level << " " << facts.str();
    for (std::string const& item : errors)
        out << "; " << item;
    for (std::string const& item : gaps)
        out << "; " << item;
    handler->SendSysMessage(out.str());
}
}  // namespace

bool HandleCommand(ChatHandler* handler, char const* args)
{
    std::istringstream in(args ? args : "");
    std::string verb;
    std::string botName;
    in >> verb >> botName;
    std::string rest;
    std::getline(in, rest);
    rest.erase(0, rest.find_first_not_of(' '));
    size_t const last = rest.find_last_not_of(" \r\n");
    rest.erase(last == std::string::npos ? 0 : last + 1);

    if (verb.empty() || botName.empty())
    {
        handler->SendErrorMessage(
            "Usage: playerbots fixture spec <bot> <name>|list | spells <bot> | gear <bot> | "
            "show <bot> | check <bot> <name> [gear]");
        return false;
    }
    Player* bot = ObjectAccessor::FindPlayerByName(botName, true);
    if (!bot || !PlayerbotsMgr::instance().GetPlayerbotAI(bot))
    {
        handler->SendErrorMessage("FIXTURE {}: error, {} is not an online bot", verb, botName);
        return false;
    }

    if (verb == "spec" && !rest.empty())
        Spec(handler, bot, rest);
    else if (verb == "spells")
        Spells(handler, bot);
    else if (verb == "gear")
        Gear(handler, bot);
    else if (verb == "show")
        Show(handler, bot);
    else if (verb == "check" && !rest.empty())
    {
        bool gear = false;
        if (EndsWith(rest, " gear"))
        {
            gear = true;
            rest.erase(rest.size() - 5);
        }
        Check(handler, bot, rest, gear);
    }
    else
    {
        handler->SendErrorMessage(
            "Usage: playerbots fixture spec <bot> <name>|list | spells <bot> | gear <bot> | "
            "show <bot> | check <bot> <name> [gear]");
        return false;
    }
    return true;
}
}  // namespace Fixture
