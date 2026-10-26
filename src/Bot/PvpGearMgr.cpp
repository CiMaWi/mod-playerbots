/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "PvpGearMgr.h"

#include <algorithm>
#include <cctype>
#include <ctime>
#include <map>
#include <optional>
#include <sstream>
#include <tuple>

#include "Battleground.h"
#include "CharacterCache.h"
#include "Chat.h"
#include "DBCStores.h"
#include "DatabaseEnv.h"
#include "ItemTemplate.h"
#include "LadderMgr.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerbotAIConfig.h"
#include "PlayerbotOperation.h"
#include "PlayerbotWorldThreadProcessor.h"
#include "PlayerbotsDatabase.h"
#include "RandomPlayerbotMgr.h"
#include "StatsWeightCalculator.h"
#include "StringConvert.h"
#include "StringFormat.h"
#include "ThreadCheck.h"
#include "Timer.h"
#include "World.h"

static_assert(PvpGearMgr::SlotCount == EQUIPMENT_SLOT_END, "one loadout slot per equipment slot");

namespace
{
constexpr uint32 CcBreakSpell = 42292;  // removes movement impairing and loss of control effects: the PvP trinket
constexpr uint8 MinRequiredLevel = 71;  // Wrath gear; the Burning Crusade PvP gear needs level 70
constexpr uint8 MaxEquipAttempts = 4;
constexpr uint32 FillableSlots = PvpGearMgr::SlotCount - 2;  // no shirt, no tabard
constexpr uint32 DefaultCheckCount = 20;
constexpr uint32 MaxCheckCount = 500;

// The equipment slot groups of the inventory types
enum Group : uint8
{
    GROUP_NONE = 0,
    GROUP_HEAD,
    GROUP_NECK,
    GROUP_SHOULDERS,
    GROUP_CHEST,
    GROUP_WAIST,
    GROUP_LEGS,
    GROUP_FEET,
    GROUP_WRISTS,
    GROUP_HANDS,
    GROUP_FINGER,
    GROUP_TRINKET,
    GROUP_BACK,
    GROUP_ONE_HAND,
    GROUP_MAIN_HAND,
    GROUP_TWO_HAND,
    GROUP_OFF_HAND_WEAPON,
    GROUP_OFF_HAND_OTHER,  // shields and held in off hand
    GROUP_RANGED           // bows, guns, crossbows, wands, thrown, relics
};

uint8 GroupOf(uint32 inventoryType)
{
    switch (inventoryType)
    {
        case INVTYPE_HEAD:
            return GROUP_HEAD;
        case INVTYPE_NECK:
            return GROUP_NECK;
        case INVTYPE_SHOULDERS:
            return GROUP_SHOULDERS;
        case INVTYPE_CHEST:
        case INVTYPE_ROBE:
            return GROUP_CHEST;
        case INVTYPE_WAIST:
            return GROUP_WAIST;
        case INVTYPE_LEGS:
            return GROUP_LEGS;
        case INVTYPE_FEET:
            return GROUP_FEET;
        case INVTYPE_WRISTS:
            return GROUP_WRISTS;
        case INVTYPE_HANDS:
            return GROUP_HANDS;
        case INVTYPE_FINGER:
            return GROUP_FINGER;
        case INVTYPE_TRINKET:
            return GROUP_TRINKET;
        case INVTYPE_CLOAK:
            return GROUP_BACK;
        case INVTYPE_WEAPON:
            return GROUP_ONE_HAND;
        case INVTYPE_WEAPONMAINHAND:
            return GROUP_MAIN_HAND;
        case INVTYPE_2HWEAPON:
            return GROUP_TWO_HAND;
        case INVTYPE_WEAPONOFFHAND:
            return GROUP_OFF_HAND_WEAPON;
        case INVTYPE_SHIELD:
        case INVTYPE_HOLDABLE:
            return GROUP_OFF_HAND_OTHER;
        case INVTYPE_RANGED:
        case INVTYPE_THROWN:
        case INVTYPE_RANGEDRIGHT:
        case INVTYPE_RELIC:
            return GROUP_RANGED;
        default:
            return GROUP_NONE;
    }
}

constexpr uint32 Mask(uint8 group) { return 1u << group; }

struct SlotGroup
{
    uint8 Slot;
    uint8 Group;
};

// The slots with one group and no pairing rule
constexpr std::array<SlotGroup, 11> SimpleSlots = {{{EQUIPMENT_SLOT_HEAD, GROUP_HEAD},
                                                    {EQUIPMENT_SLOT_NECK, GROUP_NECK},
                                                    {EQUIPMENT_SLOT_SHOULDERS, GROUP_SHOULDERS},
                                                    {EQUIPMENT_SLOT_CHEST, GROUP_CHEST},
                                                    {EQUIPMENT_SLOT_WAIST, GROUP_WAIST},
                                                    {EQUIPMENT_SLOT_LEGS, GROUP_LEGS},
                                                    {EQUIPMENT_SLOT_FEET, GROUP_FEET},
                                                    {EQUIPMENT_SLOT_WRISTS, GROUP_WRISTS},
                                                    {EQUIPMENT_SLOT_HANDS, GROUP_HANDS},
                                                    {EQUIPMENT_SLOT_BACK, GROUP_BACK},
                                                    {EQUIPMENT_SLOT_RANGED, GROUP_RANGED}}};

constexpr std::array<char const*, PvpGearMgr::SlotCount> SlotNames = {
    "head",   "neck",   "shoulders", "shirt",     "chest", "waist",     "legs",     "feet",   "wrists", "hands",
    "ring 1", "ring 2", "trinket 1", "trinket 2", "back",  "main hand", "off hand", "ranged", "tabard"};

bool IsBodyArmour(uint32 inventoryType)
{
    switch (inventoryType)
    {
        case INVTYPE_HEAD:
        case INVTYPE_SHOULDERS:
        case INVTYPE_CHEST:
        case INVTYPE_ROBE:
        case INVTYPE_WAIST:
        case INVTYPE_LEGS:
        case INVTYPE_FEET:
        case INVTYPE_WRISTS:
        case INVTYPE_HANDS:
            return true;
        default:
            return false;
    }
}

// The armour type a class wears at the maximum level
uint32 BestArmourSubclass(uint8 cls)
{
    switch (cls)
    {
        case CLASS_WARRIOR:
        case CLASS_PALADIN:
        case CLASS_DEATH_KNIGHT:
            return ITEM_SUBCLASS_ARMOR_PLATE;
        case CLASS_HUNTER:
        case CLASS_SHAMAN:
            return ITEM_SUBCLASS_ARMOR_MAIL;
        case CLASS_ROGUE:
        case CLASS_DRUID:
            return ITEM_SUBCLASS_ARMOR_LEATHER;
        default:
            return ITEM_SUBCLASS_ARMOR_CLOTH;
    }
}

// A body armour piece of the cloth, leather, mail or plate kind (cloaks and misc armour are for every class)
bool IsTypedArmour(ItemTemplate const* proto)
{
    return proto->Class == ITEM_CLASS_ARMOR && IsBodyArmour(proto->InventoryType) &&
           proto->SubClass >= ITEM_SUBCLASS_ARMOR_CLOTH && proto->SubClass <= ITEM_SUBCLASS_ARMOR_PLATE;
}

char const* ClassName(uint8 cls)
{
    switch (cls)
    {
        case CLASS_WARRIOR:
            return "Warrior";
        case CLASS_PALADIN:
            return "Paladin";
        case CLASS_HUNTER:
            return "Hunter";
        case CLASS_ROGUE:
            return "Rogue";
        case CLASS_PRIEST:
            return "Priest";
        case CLASS_DEATH_KNIGHT:
            return "Death Knight";
        case CLASS_SHAMAN:
            return "Shaman";
        case CLASS_MAGE:
            return "Mage";
        case CLASS_WARLOCK:
            return "Warlock";
        case CLASS_DRUID:
            return "Druid";
        default:
            return "?";
    }
}

char const* ArmourName(uint32 subclass)
{
    switch (subclass)
    {
        case ITEM_SUBCLASS_ARMOR_CLOTH:
            return "cloth";
        case ITEM_SUBCLASS_ARMOR_LEATHER:
            return "leather";
        case ITEM_SUBCLASS_ARMOR_MAIL:
            return "mail";
        case ITEM_SUBCLASS_ARMOR_PLATE:
            return "plate";
        default:
            return "?";
    }
}

// CanEquipNewItem results that depend on the bot's state or current (PvE) gear, not on the item: the loadout is
// data, so these do not reject a candidate
bool IsStateDependent(InventoryResult result)
{
    switch (result)
    {
        case EQUIP_ERR_NOT_IN_COMBAT:
        case EQUIP_ERR_CANT_DO_RIGHT_NOW:
        case EQUIP_ERR_YOU_ARE_STUNNED:
        case EQUIP_ERR_YOU_ARE_DEAD:
        case EQUIP_ERR_NOT_DURING_ARENA_MATCH:
        case EQUIP_ERR_NOT_WHILE_DISARMED:
        case EQUIP_ERR_CANT_EQUIP_WITH_TWOHANDED:
        case EQUIP_ERR_INVENTORY_FULL:
        case EQUIP_ERR_ITEMS_CANT_BE_SWAPPED:
        case EQUIP_ERR_ITEM_UNIQUE_EQUIPABLE:
        case EQUIP_ERR_ITEM_UNIQUE_EQUIPPABLE_SOCKETED:
        case EQUIP_ERR_ITEM_MAX_LIMIT_CATEGORY_EQUIPPED_EXCEEDED:
        case EQUIP_ERR_ITEM_MAX_LIMIT_CATEGORY_SOCKETED_EXCEEDED:
            return true;
        default:
            return false;
    }
}

std::string Lower(std::string text)
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

std::vector<std::string> Split(std::string const& text, char separator)
{
    std::vector<std::string> parts;
    std::string part;
    std::istringstream in(text);
    while (std::getline(in, part, separator))
    {
        part = Trim(part);
        if (!part.empty())
            parts.push_back(part);
    }

    return parts;
}

// The tier of an item named "<prefix> Gladiator's ...", or NoTier
uint8 TierOfName(std::string const& name, std::vector<std::vector<std::string>> const& tiers)
{
    std::string const lower = Lower(name);
    size_t const pos = lower.find(" gladiator's");
    if (pos == std::string::npos)
        return PvpGearMgr::NoTier;

    std::string const prefix = lower.substr(0, pos);
    for (size_t tier = 0; tier < tiers.size(); ++tier)
        if (std::find(tiers[tier].begin(), tiers[tier].end(), prefix) != tiers[tier].end())
            return static_cast<uint8>(tier);

    return PvpGearMgr::NoTier;
}

// The lower requirement: rating first, then arena points, then honor
bool Cheaper(PvpGearMgr::Offer const& a, PvpGearMgr::Offer const& b)
{
    return std::tie(a.Rating, a.ArenaPoints, a.Honor) < std::tie(b.Rating, b.ArenaPoints, b.Honor);
}

bool IsPolearmOrStaff(uint32 entry)
{
    ItemTemplate const* proto = sObjectMgr->GetItemTemplate(entry);
    return proto && proto->Class == ITEM_CLASS_WEAPON &&
           (proto->SubClass == ITEM_SUBCLASS_WEAPON_POLEARM || proto->SubClass == ITEM_SUBCLASS_WEAPON_STAFF);
}

bool Off(ChatHandler* handler)
{
    handler->PSendSysMessage("PvP gear is off (Playerbots.PvpGear.Enable = 0).");
    return true;
}
}  // namespace

std::vector<std::vector<std::string>> PvpGearMgr::ParseTierOrder(std::string const& order)
{
    std::vector<std::vector<std::string>> tiers;
    for (std::string const& tier : Split(order, ','))
    {
        std::vector<std::string> prefixes;
        for (std::string const& prefix : Split(tier, '/'))
            prefixes.push_back(Lower(prefix));

        if (!prefixes.empty() && tiers.size() < NoTier)
            tiers.push_back(prefixes);
    }

    return tiers;
}

std::vector<PvpGearMgr::CatalogueItem> PvpGearMgr::BuildCatalogue(std::vector<Offer> const& offers,
                                                                  std::vector<std::vector<std::string>> const& tiers,
                                                                  SkipCounts& skipped)
{
    // The usable offers, the lowest requirement per item
    std::unordered_map<uint32, Offer const*> best;
    for (Offer const& offer : offers)
    {
        if (GroupOf(offer.InventoryType) == GROUP_NONE)
            ++skipped.NotEquippable;
        else if (offer.RequiredLevel < MinRequiredLevel)
            ++skipped.LowLevel;
        else if (offer.TokenCost)
            ++skipped.TokenCost;
        else if (!offer.Honor && !offer.ArenaPoints)
            ++skipped.NoPvpCurrency;
        else if (offer.ArenaSlot)
            ++skipped.ArenaSlot;
        else
        {
            auto const [it, inserted] = best.try_emplace(offer.Entry, &offer);
            if (inserted)
                continue;

            ++skipped.Duplicate;
            if (Cheaper(offer, *it->second))
                it->second = &offer;
        }
    }

    // Tiers by name; the lowest item level of each tier's named items places the unnamed ones
    std::vector<std::pair<Offer const*, uint8>> placed;
    std::vector<uint16> minLevel(tiers.size(), UINT16_MAX);
    for (auto const& [entry, offer] : best)
    {
        uint8 const tier = TierOfName(offer->Name, tiers);
        if (tier != NoTier)
            minLevel[tier] = std::min(minLevel[tier], offer->ItemLevel);

        placed.emplace_back(offer, tier);
    }

    std::vector<CatalogueItem> catalogue;
    for (auto& [offer, tier] : placed)
    {
        if (tier == NoTier)
            for (size_t candidate = 0; candidate < tiers.size(); ++candidate)
                if (minLevel[candidate] != UINT16_MAX && minLevel[candidate] <= offer->ItemLevel)
                    tier = static_cast<uint8>(candidate);

        if (tier == NoTier)
        {
            ++skipped.NoTier;
            continue;
        }

        CatalogueItem item;
        item.Entry = offer->Entry;
        item.Tier = tier;
        item.InventoryType = offer->InventoryType;
        item.ItemLevel = offer->ItemLevel;
        item.Honor = offer->Honor;
        item.ArenaPoints = offer->ArenaPoints;
        item.Rating = offer->Rating;
        item.CcBreak = offer->CcBreak;
        catalogue.push_back(item);
    }

    std::sort(catalogue.begin(), catalogue.end(), [](CatalogueItem const& a, CatalogueItem const& b)
              { return std::tie(a.Tier, a.Entry) < std::tie(b.Tier, b.Entry); });
    return catalogue;
}

void PvpGearMgr::Load()
{
    // The worn rows also with PvP gear off or without a catalogue: a bot left in PvP gear (a stop during a match, a
    // crash) is repaired at its login and its PvE items stay guarded until then
    if (!_wornLoaded)
    {
        LoadWorn();
        _wornLoaded = true;
    }

    if (!sPlayerbotAIConfig.PvpGearEnable || _loaded)
        return;

    uint32 const oldMSTime = getMSTime();
    std::vector<std::vector<std::string>> const tiers = ParseTierOrder(sPlayerbotAIConfig.PvpGearTierOrder);
    if (tiers.empty())
    {
        LOG_ERROR("playerbots", "Playerbots.PvpGear.TierOrder lists no tier: PvP gear stays off");
        return;
    }

    _tierNames.clear();
    for (std::vector<std::string> const& prefixes : tiers)
    {
        std::string name;
        for (std::string prefix : prefixes)
        {
            prefix[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(prefix[0])));
            name += (name.empty() ? "" : "/") + prefix;
        }
        _tierNames.push_back(name);
    }

    _baseTier = 0;
    std::string const base = Lower(Trim(sPlayerbotAIConfig.PvpGearBaseTier));
    if (!base.empty())
    {
        auto const found =
            std::find_if(tiers.begin(), tiers.end(), [&](std::vector<std::string> const& prefixes)
                         { return std::find(prefixes.begin(), prefixes.end(), base) != prefixes.end(); });
        if (found == tiers.end() && base != Lower(_tierNames.front()))
            LOG_ERROR("playerbots", "Playerbots.PvpGear.BaseTier '{}' is not in Playerbots.PvpGear.TierOrder: using {}",
                      sPlayerbotAIConfig.PvpGearBaseTier, _tierNames.front());
        else if (found != tiers.end())
            _baseTier = static_cast<uint8>(found - tiers.begin());
    }

    _configBaseTier = _baseTier;
    std::string unknownSlots;
    _buyOrder = ParseSlotPriority(sPlayerbotAIConfig.PvpGearBuySlotPriority, unknownSlots);
    if (!unknownSlots.empty())
        LOG_ERROR("playerbots", "Playerbots.PvpGear.Buy.SlotPriority: unknown slot names left out: {}", unknownSlots);

    _buyStart = static_cast<uint32>(time(nullptr));

    // The PvP vendors: offers whose extended cost asks for honor, arena points or a rating (DBC, no SQL table)
    std::vector<Offer> offers;
    uint32 unknown = 0;
    if (QueryResult result =
            WorldDatabase.Query("SELECT DISTINCT CAST(item AS UNSIGNED), CAST(ExtendedCost AS UNSIGNED) "
                                "FROM npc_vendor WHERE ExtendedCost > 0 AND item > 0"))
    {
        do
        {
            Field* fields = result->Fetch();
            uint32 const entry = static_cast<uint32>(fields[0].Get<uint64>());
            ItemTemplate const* proto = sObjectMgr->GetItemTemplate(entry);
            ItemExtendedCostEntry const* cost =
                sItemExtendedCostStore.LookupEntry(static_cast<uint32>(fields[1].Get<uint64>()));
            if (!proto || !cost)
            {
                ++unknown;
                continue;
            }

            Offer offer;
            offer.Entry = entry;
            offer.Name = proto->Name1;
            offer.ItemLevel = static_cast<uint16>(proto->ItemLevel);
            offer.RequiredLevel = static_cast<uint8>(std::min<uint32>(proto->RequiredLevel, UINT8_MAX));
            offer.InventoryType = static_cast<uint8>(proto->InventoryType);
            offer.Honor = cost->reqhonorpoints;
            offer.ArenaPoints = cost->reqarenapoints;
            offer.ArenaSlot = cost->reqarenaslot;
            offer.Rating = cost->reqpersonalarenarating;
            for (uint8 i = 0; i < MAX_ITEM_EXTENDED_COST_REQUIREMENTS; ++i)
                if (cost->reqitem[i])
                    offer.TokenCost = true;

            for (uint8 i = 0; i < MAX_ITEM_PROTO_SPELLS; ++i)
                if (proto->Spells[i].SpellId == static_cast<int32>(CcBreakSpell))
                    offer.CcBreak = true;

            offers.push_back(offer);
        } while (result->NextRow());
    }

    SkipCounts skipped;
    std::vector<CatalogueItem> const catalogue = BuildCatalogue(offers, tiers, skipped);
    _tiers.assign(tiers.size(), {});
    for (CatalogueItem const& item : catalogue)
        _tiers[item.Tier].push_back(item);

    _byEntry.clear();
    for (std::vector<CatalogueItem> const& tier : _tiers)
        for (CatalogueItem const& item : tier)
            _byEntry[item.Entry] = &item;

    // The prices no other undercuts in both currencies (sorted by honor: each has fewer arena points than those before)
    std::vector<std::pair<uint32, uint32>> prices;
    for (CatalogueItem const& item : catalogue)
        prices.emplace_back(item.Honor, item.ArenaPoints);

    std::sort(prices.begin(), prices.end());
    _prices.clear();
    for (std::pair<uint32, uint32> const& price : prices)
        if (_prices.empty() || price.second < _prices.back().second)
            _prices.push_back(price);

    std::string summary;
    for (size_t tier = 0; tier < _tiers.size(); ++tier)
    {
        uint16 low = UINT16_MAX;
        uint16 high = 0;
        for (CatalogueItem const& item : _tiers[tier])
        {
            low = std::min(low, item.ItemLevel);
            high = std::max(high, item.ItemLevel);
        }

        std::string const levels = _tiers[tier].empty() ? "-"
                                   : low == high        ? std::to_string(low)
                                                        : Acore::StringFormat("{}-{}", low, high);
        summary += Acore::StringFormat("{}{} {} ({} items)", summary.empty() ? "" : ", ", _tierNames[tier], levels,
                                       _tiers[tier].size());
    }

    LOG_INFO("server.loading", "PvP gear catalogue: {} tiers: {}; base tier {}", _tiers.size(), summary,
             _tierNames[_baseTier]);
    LOG_INFO("server.loading",
             "PvP gear catalogue: left out {} token costs, {} without honor or arena points, {} needing a 3v3 or 5v5 "
             "rating, {} below level {}, {} not equippable, {} below every tier, {} duplicate offers, {} unknown",
             skipped.TokenCost, skipped.NoPvpCurrency, skipped.ArenaSlot, skipped.LowLevel, MinRequiredLevel,
             skipped.NotEquippable, skipped.NoTier, skipped.Duplicate, unknown);

    // Every loadout row, once (no map runs yet)
    uint32 rows = 0;
    uint32 maxSeason = 0;
    PlayerbotsDatabasePreparedStatement* stmt = PlayerbotsDatabase.GetPreparedStatement(PLAYERBOTS_SEL_PVP_LOADOUT_ALL);
    if (PreparedQueryResult result = PlayerbotsDatabase.Query(stmt))
    {
        do
        {
            uint32 season = 0;
            if (!ReadLoadoutRow(result->Fetch(), season))
                continue;

            maxSeason = std::max(maxSeason, season);
            ++rows;
        } while (result->NextRow());
    }

    _season = std::max<uint32>(maxSeason, 1);
    _loaded = true;
    LOG_INFO("server.loading", ">> Loaded {} PvP loadouts ({} rows), season {}, in {} ms", _loadouts.size(), rows,
             _season, GetMSTimeDiffToNow(oldMSTime));
    if (sPlayerbotAIConfig.PvpGearBuyEnable)
    {
        std::string order;
        for (uint8 slot : _buyOrder)
            order += (order.empty() ? "" : ", ") + std::string(SlotNames[slot]);

        LOG_INFO("server.loading",
                 "PvP gear buying: every {} min and after rated arena matches, {} bots per tick, saving toward better "
                 "{}; slot order {}",
                 sPlayerbotAIConfig.PvpGearBuyCheckIntervalMinutes, sPlayerbotAIConfig.PvpGearBuyBatchPerTick,
                 sPlayerbotAIConfig.PvpGearBuySaveTowardBetter ? "on" : "off", order);
    }
}

bool PvpGearMgr::ReadLoadoutRow(Field* fields, uint32& season)
{
    uint8 const slot = fields[1].Get<uint8>();
    if (slot >= SlotCount)
        return false;

    LoadoutSlot& data = _loadouts[fields[0].Get<uint32>()][slot];
    data.Item = fields[2].Get<uint32>();
    // The catalogue decides the tier, so a changed tier order does not mislabel stored items
    CatalogueItem const* item = FindItem(data.Item);
    data.Tier = item ? item->Tier : fields[3].Get<uint8>();
    data.Season = fields[4].Get<uint32>();
    data.BoughtAt = fields[5].Get<uint32>();
    season = data.Season;
    return true;
}

void PvpGearMgr::ReloadLoadouts(std::vector<uint32> const& guids, QueryResult result)
{
    PLAYERBOT_CHECK_WORLD_THREAD("PvpGearMgr::ReloadLoadouts");
    if (!_loaded)
        return;

    // No rows: the login builds a base loadout, as for a bot that never had one
    for (uint32 guid : guids)
        _loadouts.erase(guid);

    uint32 rows = 0;
    uint32 maxSeason = 0;
    if (result)
    {
        do
        {
            uint32 season = 0;
            if (!ReadLoadoutRow(result->Fetch(), season))
                continue;

            maxSeason = std::max(maxSeason, season);
            ++rows;
        } while (result->NextRow());
    }

    // Like Load: the newest season among the rows, so a restart would not change it
    if (maxSeason > _season)
    {
        LOG_INFO("playerbots", "PvP gear: season {} -> {} (an arriving bot's loadout)", _season, maxSeason);
        _season = maxSeason;
    }

    LOG_DEBUG("playerbots", "PvP gear: {} loadout rows of {} arriving bots loaded", rows, guids.size());
}

bool PvpGearMgr::IsMaxLevelRandomBot(Player* bot)
{
    if (!bot || !bot->GetSession() || bot->GetLevel() < sWorld->getIntConfig(CONFIG_MAX_PLAYER_LEVEL))
        return false;

    // The account list, not the random bot manager's set: it also holds while a bot logs in
    return sPlayerbotAIConfig.IsInRandomAccountList(bot->GetSession()->GetAccountId());
}

bool PvpGearMgr::IsLadderBot(Player* bot)
{
    if (!IsMaxLevelRandomBot(bot))
        return false;

    for (uint8 slot = 0; slot < MAX_ARENA_SLOT; ++slot)
        if (bot->GetArenaTeamId(slot))
            return true;

    return false;
}

PvpGearMgr::Loadout const* PvpGearMgr::Find(uint32 guid) const
{
    auto const it = _loadouts.find(guid);
    return it == _loadouts.end() ? nullptr : &it->second;
}

PvpGearMgr::CatalogueItem const* PvpGearMgr::FindItem(uint32 entry) const
{
    auto const it = _byEntry.find(entry);
    return it == _byEntry.end() ? nullptr : it->second;
}

uint32 PvpGearMgr::MissingSlots(Loadout const& loadout)
{
    uint32 missing = 0;
    for (uint8 slot = 0; slot < SlotCount; ++slot)
    {
        if (slot == EQUIPMENT_SLOT_BODY || slot == EQUIPMENT_SLOT_TABARD || loadout[slot].Item)
            continue;

        // A two-hander without Titan's Grip leaves the off hand empty by design
        if (slot == EQUIPMENT_SLOT_OFFHAND)
            if (ItemTemplate const* main = sObjectMgr->GetItemTemplate(loadout[EQUIPMENT_SLOT_MAINHAND].Item))
                if (main->InventoryType == INVTYPE_2HWEAPON)
                    continue;

        ++missing;
    }

    return missing;
}

void PvpGearMgr::OnBotLogin(Player* bot)
{
    PLAYERBOT_CHECK_WORLD_THREAD("PvpGearMgr::OnBotLogin");
    // On an arena server a hosted ladder bot's rows are its gear, not a crash: it keeps them and wears the loadout.
    // Elsewhere rows left by a crash or a logout during a match: the PvE gear goes back on first (also for a bot that
    // is no longer a ladder bot, and with PvP gear off)
    uint32 const guid = bot->GetGUID().GetCounter();
    bool const wearsAlways = WearsAlways() && IsLadderBot(bot) && !LadderMgr::Instance().IsAway(guid);
    if (!wearsAlways && !Unwear(bot, "login repair"))
        RetryUnwear(guid);

    if (!_loaded || !IsLadderBot(bot))
        return;

    EnsureLoadout(bot);
    if (wearsAlways)
        Wear(bot, "login");
}

void PvpGearMgr::EnsureLoadout(Player* bot)
{
    uint32 const guid = bot->GetGUID().GetCounter();
    if (_loadouts.contains(guid))
        return;

    uint32 filled = 0;
    Loadout const loadout = BuildLoadout(bot, _baseTier, _season, filled);
    Store(guid, loadout);
    LOG_DEBUG("playerbots", "PvP gear: {} got a {} loadout at login ({} items, {} slots empty)", bot->GetName(),
              _tierNames[_baseTier], filled, MissingSlots(loadout));
}

bool PvpGearMgr::Usable(Player* bot, CatalogueItem const& item) const
{
    ItemTemplate const* proto = sObjectMgr->GetItemTemplate(item.Entry);
    if (!proto || bot->CanUseItem(proto) != EQUIP_ERR_OK)
        return false;

    if (IsTypedArmour(proto))
        return proto->SubClass == BestArmourSubclass(bot->getClass());

    // Weapon and shield proficiency (CanUseItem checks it only for a real item)
    uint32 const skill = proto->GetSkill();
    return !skill || bot->GetSkillValue(skill) > 0;
}

PvpGearMgr::Pick PvpGearMgr::Best(Player* bot, StatsWeightCalculator& calc, uint32 groupMask, uint8 slot, uint8 minTier,
                                  std::vector<uint32> const& exclude, int8 ccBreak, uint32 maxRating) const
{
    for (size_t tier = minTier; tier < _tiers.size(); ++tier)
    {
        Pick best;
        for (CatalogueItem const& item : _tiers[tier])
        {
            if (!(groupMask & Mask(GroupOf(item.InventoryType))))
                continue;

            if ((ccBreak > 0 && !item.CcBreak) || (ccBreak < 0 && item.CcBreak))
                continue;

            if (item.Rating > maxRating || std::find(exclude.begin(), exclude.end(), item.Entry) != exclude.end() ||
                !Usable(bot, item))
                continue;

            float const score = calc.CalculateItem(item.Entry, 0, slot);
            if (!best.Item || score > best.Score)
                best = {&item, score};
        }

        if (best.Item)
            return best;
    }

    return {};
}

PvpGearMgr::Pick PvpGearMgr::BestEquippable(Player* bot, StatsWeightCalculator& calc, uint32 groupMask, uint8 slot,
                                            uint8 minTier, std::vector<uint32> exclude, int8 ccBreak,
                                            uint32 maxRating) const
{
    for (uint8 attempt = 0; attempt < MaxEquipAttempts; ++attempt)
    {
        // An item the rating allows: from minTier up, else from the tiers below; only then any rating (the Furious
        // weapons are all item level 239 at rating 2350)
        Pick pick = Best(bot, calc, groupMask, slot, minTier, exclude, ccBreak, maxRating);
        for (uint8 tier = minTier; !pick.Item && tier > 0; --tier)
            pick = Best(bot, calc, groupMask, slot, tier - 1, exclude, ccBreak, maxRating);

        if (!pick.Item)
        {
            pick = Best(bot, calc, groupMask, slot, minTier, exclude, ccBreak);
            if (pick.Item)
                LOG_DEBUG("playerbots", "PvP gear: {} has no {} item for its rating {}: took {} (rating {})",
                          bot->GetName(), SlotNames[slot], maxRating, pick.Item->Entry, pick.Item->Rating);
        }

        if (!pick.Item)
            return pick;

        // A check only: the probe item is deleted at once
        uint16 dest = 0;
        InventoryResult const result = bot->CanEquipNewItem(slot, dest, pick.Item->Entry, true);
        if (result == EQUIP_ERR_OK || IsStateDependent(result))
            return pick;

        LOG_DEBUG("playerbots", "PvP gear: {} cannot equip {} in the {} slot (error {}), trying the next",
                  bot->GetName(), pick.Item->Entry, SlotNames[slot], static_cast<uint32>(result));
        exclude.push_back(pick.Item->Entry);
    }

    return {};
}

PvpGearMgr::Loadout PvpGearMgr::BuildLoadout(Player* bot, uint8 tier, uint32 season, uint32& filled) const
{
    Loadout loadout{};
    filled = 0;
    StatsWeightCalculator calc(bot);
    calc.SetPvpSpec(true);
    // Within a tier only what the bot's rating allows (BestEquippable)
    uint32 const rating = bot->GetMaxPersonalArenaRatingRequirement(0);
    auto const put = [&](uint8 slot, Pick const& pick)
    {
        if (!pick.Item)
            return;

        loadout[slot].Item = pick.Item->Entry;
        loadout[slot].Tier = pick.Item->Tier;
        loadout[slot].Season = season;
        ++filled;
    };

    std::vector<uint32> const none;
    for (SlotGroup const& entry : SimpleSlots)
        put(entry.Slot, BestEquippable(bot, calc, Mask(entry.Group), entry.Slot, tier, none, 0, rating));

    Pick const ring = BestEquippable(bot, calc, Mask(GROUP_FINGER), EQUIPMENT_SLOT_FINGER1, tier, none, 0, rating);
    put(EQUIPMENT_SLOT_FINGER1, ring);
    put(EQUIPMENT_SLOT_FINGER2, BestEquippable(bot, calc, Mask(GROUP_FINGER), EQUIPMENT_SLOT_FINGER2, tier,
                                               ring.Item ? std::vector<uint32>{ring.Item->Entry} : none, 0, rating));
    // Trinket 1: the PvP trinket (DT.5: every ladder bot has one, also with a racial that breaks crowd control);
    // trinket 2: another trinket
    put(EQUIPMENT_SLOT_TRINKET1,
        BestEquippable(bot, calc, Mask(GROUP_TRINKET), EQUIPMENT_SLOT_TRINKET1, tier, none, 1, rating));
    put(EQUIPMENT_SLOT_TRINKET2,
        BestEquippable(bot, calc, Mask(GROUP_TRINKET), EQUIPMENT_SLOT_TRINKET2, tier, none, -1, rating));

    // Weapons: a two-hander (with Titan's Grip plus an off-hand weapon) against a main hand plus an off hand
    std::vector<uint32> twoHandExclude;
    bool const titanGrip = bot->CanTitanGrip();
    if (titanGrip)
        for (std::vector<CatalogueItem> const& items : _tiers)
            for (CatalogueItem const& item : items)
                if (GroupOf(item.InventoryType) == GROUP_TWO_HAND && IsPolearmOrStaff(item.Entry))
                    twoHandExclude.push_back(item.Entry);

    Pick const twoHand =
        BestEquippable(bot, calc, Mask(GROUP_TWO_HAND), EQUIPMENT_SLOT_MAINHAND, tier, twoHandExclude, 0, rating);
    Pick twoHandOff;
    if (twoHand.Item && titanGrip)
    {
        twoHandExclude.push_back(twoHand.Item->Entry);
        twoHandOff =
            BestEquippable(bot, calc, Mask(GROUP_TWO_HAND) | Mask(GROUP_ONE_HAND) | Mask(GROUP_OFF_HAND_WEAPON),
                           EQUIPMENT_SLOT_OFFHAND, tier, twoHandExclude, 0, rating);
    }

    Pick const mainHand = BestEquippable(bot, calc, Mask(GROUP_ONE_HAND) | Mask(GROUP_MAIN_HAND),
                                         EQUIPMENT_SLOT_MAINHAND, tier, none, 0, rating);
    uint32 offMask = Mask(GROUP_OFF_HAND_OTHER);
    if (bot->CanDualWield())
        offMask |= Mask(GROUP_ONE_HAND) | Mask(GROUP_OFF_HAND_WEAPON);

    Pick offHand;
    if (mainHand.Item)
        offHand = BestEquippable(bot, calc, offMask, EQUIPMENT_SLOT_OFFHAND, tier,
                                 std::vector<uint32>{mainHand.Item->Entry}, 0, rating);

    float const twoHandScore = twoHand.Score + twoHandOff.Score;
    float const oneHandScore = mainHand.Score + offHand.Score;
    if (twoHand.Item && (!mainHand.Item || twoHandScore >= oneHandScore))
    {
        put(EQUIPMENT_SLOT_MAINHAND, twoHand);
        put(EQUIPMENT_SLOT_OFFHAND, twoHandOff);
    }
    else
    {
        put(EQUIPMENT_SLOT_MAINHAND, mainHand);
        put(EQUIPMENT_SLOT_OFFHAND, offHand);
    }

    return loadout;
}

namespace
{
void AppendRows(PlayerbotsDatabaseTransaction& transaction, uint32 guid, PvpGearMgr::Loadout const& loadout)
{
    for (uint8 slot = 0; slot < PvpGearMgr::SlotCount; ++slot)
    {
        PvpGearMgr::LoadoutSlot const& data = loadout[slot];
        if (!data.Item)
            continue;

        PlayerbotsDatabasePreparedStatement* stmt = PlayerbotsDatabase.GetPreparedStatement(PLAYERBOTS_REP_PVP_LOADOUT);
        stmt->SetData(0, guid);
        stmt->SetData(1, slot);
        stmt->SetData(2, data.Item);
        stmt->SetData(3, data.Tier);
        stmt->SetData(4, data.Season);
        stmt->SetData(5, data.BoughtAt);
        transaction->Append(stmt);
    }
}
}  // namespace

void PvpGearMgr::Store(uint32 guid, Loadout const& loadout)
{
    _loadouts[guid] = loadout;
    PlayerbotsDatabaseTransaction transaction = PlayerbotsDatabase.BeginTransaction();
    PlayerbotsDatabasePreparedStatement* stmt = PlayerbotsDatabase.GetPreparedStatement(PLAYERBOTS_DEL_PVP_LOADOUT);
    stmt->SetData(0, guid);
    transaction->Append(stmt);
    AppendRows(transaction, guid, loadout);
    PlayerbotsDatabase.CommitTransaction(transaction);
}

std::string PvpGearMgr::TierLabel(uint8 tier) const { return tier < _tierNames.size() ? _tierNames[tier] : "?"; }

PvpGearMgr::CatalogueItem const* PvpGearMgr::NextItem(Player* bot, uint32 guid, Loadout const& loadout,
                                                      uint8 slot) const
{
    CatalogueItem const* current = FindItem(loadout[slot].Item);
    if (!current)
        return nullptr;

    uint32 const groupMask = Mask(GroupOf(current->InventoryType));
    int8 const ccBreak = GroupOf(current->InventoryType) == GROUP_TRINKET ? (current->CcBreak ? 1 : -1) : 0;
    // An upgrade is a higher tier or, within the tier, a higher item level (as the buying counts it)
    std::vector<uint32> exclude;
    for (CatalogueItem const& item : _tiers[current->Tier])
        if (item.ItemLevel <= current->ItemLevel)
            exclude.push_back(item.Entry);

    // Rings: never the other ring's item, and ring 2 not the one ring 1 takes next
    if (slot == EQUIPMENT_SLOT_FINGER1 || slot == EQUIPMENT_SLOT_FINGER2)
        exclude.push_back(
            loadout[slot == EQUIPMENT_SLOT_FINGER1 ? EQUIPMENT_SLOT_FINGER2 : EQUIPMENT_SLOT_FINGER1].Item);

    if (slot == EQUIPMENT_SLOT_FINGER2)
        if (CatalogueItem const* first = NextItem(bot, guid, loadout, EQUIPMENT_SLOT_FINGER1))
            exclude.push_back(first->Entry);

    uint8 const minTier = current->Tier;
    if (bot)
    {
        StatsWeightCalculator calc(bot);
        calc.SetPvpSpec(true);
        // The next upgrade the bot's rating allows; with none, the next one at all
        Pick next =
            Best(bot, calc, groupMask, slot, minTier, exclude, ccBreak, bot->GetMaxPersonalArenaRatingRequirement(0));
        if (!next.Item)
            next = Best(bot, calc, groupMask, slot, minTier, exclude, ccBreak);

        return next.Item;
    }

    // Offline: the cheapest item of the same kind for the bot's class and race
    CharacterCacheEntry const* cache =
        sCharacterCache->GetCharacterCacheByGuid(ObjectGuid::Create<HighGuid::Player>(guid));
    ItemTemplate const* currentProto = sObjectMgr->GetItemTemplate(current->Entry);
    if (!cache || !currentProto)
        return nullptr;

    int32 const classMask = 1 << (cache->Class - 1);
    int32 const raceMask = 1 << (cache->Race - 1);
    CatalogueItem const* next = nullptr;
    for (size_t tier = minTier; tier < _tiers.size() && !next; ++tier)
    {
        for (CatalogueItem const& item : _tiers[tier])
        {
            ItemTemplate const* proto = sObjectMgr->GetItemTemplate(item.Entry);
            if (!proto || !(groupMask & Mask(GroupOf(item.InventoryType))) || (ccBreak > 0 && !item.CcBreak) ||
                (ccBreak < 0 && item.CcBreak) || std::find(exclude.begin(), exclude.end(), item.Entry) != exclude.end())
                continue;

            if (!(proto->AllowableClass & classMask) || !(proto->AllowableRace & raceMask) ||
                proto->Class != currentProto->Class || proto->SubClass != currentProto->SubClass)
                continue;

            if (!next || item.Honor + item.ArenaPoints < next->Honor + next->ArenaPoints)
                next = &item;
        }
    }

    return next;
}

std::string PvpGearMgr::NextUpgrade(Player* bot, uint32 guid, Loadout const& loadout, uint8 slot) const
{
    CatalogueItem const* current = FindItem(loadout[slot].Item);
    if (!current)
        return "not in the catalogue";

    CatalogueItem const* next = NextItem(bot, guid, loadout, slot);
    if (!next)
        return current->Tier + 1u >= _tiers.size() ? "none (top tier)" : "none";

    ItemTemplate const* proto = sObjectMgr->GetItemTemplate(next->Entry);
    std::string text = Acore::StringFormat("{} {} ({} {}): {} honor, {} arena points, rating {}", next->Entry,
                                           proto ? proto->Name1 : "?", TierLabel(next->Tier), next->ItemLevel,
                                           next->Honor, next->ArenaPoints, next->Rating);
    if (!bot)
        return text;

    uint32 const rating = bot->GetMaxPersonalArenaRatingRequirement(0);
    if (bot->GetHonorPoints() >= next->Honor && bot->GetArenaPoints() >= next->ArenaPoints && rating >= next->Rating)
        return text + "; affordable now";

    return text + Acore::StringFormat("; not yet (has {} honor, {} arena points, rating {})", bot->GetHonorPoints(),
                                      bot->GetArenaPoints(), rating);
}

bool PvpGearMgr::HandleStatusCommand(ChatHandler* handler, char const* /*args*/)
{
    PLAYERBOT_CHECK_WORLD_THREAD("PvpGearMgr::HandleStatusCommand");
    PvpGearMgr& mgr = Instance();
    if (!mgr._loaded)
        return Off(handler);

    std::string tiers;
    for (size_t tier = 0; tier < mgr._tiers.size(); ++tier)
        tiers +=
            Acore::StringFormat("{}{} {}", tiers.empty() ? "" : ", ", mgr._tierNames[tier], mgr._tiers[tier].size());

    uint32 online = 0;
    uint32 withLoadout = 0;
    for (auto it = sRandomPlayerbotMgr.GetPlayerBotsBegin(); it != sRandomPlayerbotMgr.GetPlayerBotsEnd(); ++it)
    {
        if (!it->second || !IsLadderBot(it->second))
            continue;

        ++online;
        if (mgr.Find(it->second->GetGUID().GetCounter()))
            ++withLoadout;
    }

    handler->PSendSysMessage("PvP gear: season {}, base tier {}; catalogue items per tier: {}", mgr._season,
                             mgr._tierNames[mgr._baseTier], tiers);
    handler->PSendSysMessage(
        "{} loadouts stored; {} ladder bots online, {} of them with a loadout (a ladder bot is a "
        "random bot at the maximum level in an arena team)",
        mgr._loadouts.size(), online, withLoadout);
    return true;
}

namespace
{
bool ResetUsage(ChatHandler* handler)
{
    handler->SendSysMessage(
        "Usage: .playerbots pvpgear reset [season, default the current + 1] [tier name or its start, default the base "
        "tier]");
    return false;
}
}  // namespace

bool PvpGearMgr::HandleResetCommand(ChatHandler* handler, char const* args)
{
    PLAYERBOT_CHECK_WORLD_THREAD("PvpGearMgr::HandleResetCommand");
    PvpGearMgr& mgr = Instance();
    if (!mgr._loaded)
        return Off(handler);

    // [season] [tier]: the season number (default the current + 1) and the tier the loadouts start at (a tier name or
    // its start, default Playerbots.PvpGear.BaseTier)
    uint32 season = mgr._season + 1;
    uint8 tier = mgr._configBaseTier;
    std::istringstream words(Trim(args ? args : ""));
    for (std::string word; words >> word;)
    {
        if (std::optional<uint32> const number = Acore::StringTo<uint32>(word))
        {
            if (!*number)
                return ResetUsage(handler);

            season = *number;
            continue;
        }

        std::string const prefix = Lower(word);
        auto const found = std::find_if(mgr._tierNames.begin(), mgr._tierNames.end(),
                                        [&prefix](std::string const& name) { return Lower(name).starts_with(prefix); });
        if (found == mgr._tierNames.end())
            return ResetUsage(handler);

        tier = static_cast<uint8>(found - mgr._tierNames.begin());
    }

    std::unordered_map<uint32, Loadout> rebuilt;
    uint32 items = 0;
    uint32 missing = 0;
    for (auto it = sRandomPlayerbotMgr.GetPlayerBotsBegin(); it != sRandomPlayerbotMgr.GetPlayerBotsEnd(); ++it)
    {
        Player* bot = it->second;
        if (!bot || !bot->IsInWorld() || !IsLadderBot(bot))
            continue;

        uint32 filled = 0;
        Loadout const loadout = mgr.BuildLoadout(bot, tier, season, filled);
        items += filled;
        missing += MissingSlots(loadout);
        rebuilt.emplace(bot->GetGUID().GetCounter(), loadout);
    }

    uint32 cleared = 0;
    for (auto const& [guid, loadout] : mgr._loadouts)
        if (!rebuilt.contains(guid))
            ++cleared;

    // One async transaction: every old row goes, the rebuilt ones are written
    PlayerbotsDatabaseTransaction transaction = PlayerbotsDatabase.BeginTransaction();
    transaction->Append(PlayerbotsDatabase.GetPreparedStatement(PLAYERBOTS_DEL_PVP_LOADOUT_ALL));
    for (auto const& [guid, loadout] : rebuilt)
        AppendRows(transaction, guid, loadout);

    PlayerbotsDatabase.CommitTransaction(transaction);
    mgr._loadouts = std::move(rebuilt);
    mgr._season = season;
    mgr._baseTier = tier;  // until the restart: the cleared loadouts are rebuilt at this tier (the next reset
                           // without a tier word starts from Playerbots.PvpGear.BaseTier again)

    // The bots that wear their loadout now (in a match, or on an arena server) swap the changed slots
    uint32 rewears = 0;
    for (auto const& [guid, loadout] : mgr._loadouts)
        if (mgr.OnLoadoutChanged(ObjectAccessor::FindPlayer(ObjectGuid::Create<HighGuid::Player>(guid))))
            ++rewears;

    std::string const message = Acore::StringFormat(
        "PvP gear season {}: {} online ladder bots set to the tier {} ({} items, {} slots empty, {} bots wearing "
        "it swapped); {} other loadouts cleared, rebuilt at the bot's next login. A ladder bot is a random bot at the "
        "maximum level in an arena team.",
        season, mgr._loadouts.size(), mgr._tierNames[tier], items, missing, rewears, cleared);
    handler->SendSysMessage(message);
    LOG_INFO("playerbots", "{}", message);
    return true;
}

bool PvpGearMgr::HandleShowCommand(ChatHandler* handler, char const* args)
{
    PLAYERBOT_CHECK_WORLD_THREAD("PvpGearMgr::HandleShowCommand");
    PvpGearMgr& mgr = Instance();
    if (!mgr._loaded)
        return Off(handler);

    std::string const target = Trim(args ? args : "");
    ObjectGuid guid = ObjectGuid::Empty;
    std::string name = target;
    if (std::optional<uint32> const number = Acore::StringTo<uint32>(target))
        guid = ObjectGuid::Create<HighGuid::Player>(*number);
    else if (normalizePlayerName(name))
        guid = sCharacterCache->GetCharacterGuidByName(name);

    if (target.empty() || guid.IsEmpty() || !sCharacterCache->GetCharacterNameByGuid(guid, name))
    {
        handler->PSendSysMessage("Usage: .playerbots pvpgear show <bot name or guid>");
        return false;
    }

    // Looked up per command; the pointer is not kept
    Player* bot = ObjectAccessor::FindConnectedPlayer(guid);
    bool const ladder = bot && IsLadderBot(bot);
    uint32 const low = guid.GetCounter();
    if (ladder)
        mgr.OnBotLogin(bot);  // a ladder bot that joined its team after its login gets its loadout now

    Loadout const* loadout = mgr.Find(low);
    Loadout preview{};
    bool const isPreview = !loadout && bot && !ladder && IsMaxLevelRandomBot(bot);
    if (isPreview)
    {
        uint32 filled = 0;
        preview = mgr.BuildLoadout(bot, mgr._baseTier, mgr._season, filled);
        loadout = &preview;
    }

    CharacterCacheEntry const* cache = sCharacterCache->GetCharacterCacheByGuid(guid);
    uint8 const cls = bot ? bot->getClass() : (cache ? cache->Class : 0);
    if (!loadout)
    {
        handler->PSendSysMessage("{} ({}, {}) has no PvP loadout: {}", name, ClassName(cls), bot ? "online" : "offline",
                                 bot ? "not a ladder bot (a random bot at the maximum level in an arena team)"
                                     : "it is rebuilt at the base tier at the bot's next login");
        return true;
    }

    uint32 items = 0;
    uint32 season = 0;
    for (LoadoutSlot const& data : *loadout)
    {
        if (data.Item)
            ++items;
        season = std::max(season, data.Season);
    }

    handler->PSendSysMessage("{} ({}, {} armour, {}): {} items, {} slots empty, season {}{}", name, ClassName(cls),
                             ArmourName(BestArmourSubclass(cls)), bot ? "online" : "offline", items,
                             MissingSlots(*loadout), season,
                             isPreview ? "; a preview at the base tier, not stored (not a ladder bot)" : "");
    for (uint8 slot = 0; slot < SlotCount; ++slot)
    {
        LoadoutSlot const& data = (*loadout)[slot];
        if (slot == EQUIPMENT_SLOT_BODY || slot == EQUIPMENT_SLOT_TABARD)
            continue;

        if (!data.Item)
        {
            handler->PSendSysMessage("  {}: empty", SlotNames[slot]);
            continue;
        }

        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(data.Item);
        std::string kind = "?";
        if (proto && IsTypedArmour(proto))
            kind = ArmourName(proto->SubClass);
        else if (proto && proto->Class == ITEM_CLASS_WEAPON)
            kind = Acore::StringFormat("weapon {}", proto->SubClass);
        else if (proto)
            kind = Acore::StringFormat("class {}.{}", proto->Class, proto->SubClass);

        handler->PSendSysMessage("  {}: {} {} ({} {}, {}); next: {}", SlotNames[slot], data.Item,
                                 proto ? proto->Name1 : "?", mgr.TierLabel(data.Tier), proto ? proto->ItemLevel : 0,
                                 kind, mgr.NextUpgrade(bot, low, *loadout, slot));
    }

    return true;
}

bool PvpGearMgr::HandleCheckCommand(ChatHandler* handler, char const* args)
{
    PLAYERBOT_CHECK_WORLD_THREAD("PvpGearMgr::HandleCheckCommand");
    PvpGearMgr& mgr = Instance();
    if (!mgr._loaded)
        return Off(handler);

    uint32 count = DefaultCheckCount;
    std::istringstream in(args ? args : "");
    std::string countText;
    std::string scope;
    in >> countText >> scope;
    bool const all = scope == "all";
    if (!countText.empty())
    {
        std::optional<uint32> const number = Acore::StringTo<uint32>(countText);
        if (!number || !*number || *number > MaxCheckCount || (!scope.empty() && !all))
        {
            handler->PSendSysMessage("Usage: .playerbots pvpgear check [count 1-{}, default {}] [all]", MaxCheckCount,
                                     DefaultCheckCount);
            return false;
        }
        count = *number;
    }

    uint32 bots = 0;
    uint32 noLoadout = 0;
    uint32 items = 0;
    uint32 ok = 0;
    uint32 stateDependent = 0;
    uint32 failed = 0;
    uint32 wrongArmour = 0;
    uint32 missing = 0;
    uint32 classMask = 0;
    uint32 previews = 0;
    std::map<uint32, uint32> stateCodes;
    for (auto it = sRandomPlayerbotMgr.GetPlayerBotsBegin(); it != sRandomPlayerbotMgr.GetPlayerBotsEnd(); ++it)
    {
        Player* bot = it->second;
        if (bots >= count)
            break;

        if (!bot || !bot->IsInWorld())
            continue;

        bool const ladder = IsLadderBot(bot);
        if (!ladder && !(all && IsMaxLevelRandomBot(bot)))
            continue;

        ++bots;
        Loadout const* loadout = mgr.Find(bot->GetGUID().GetCounter());
        Loadout preview{};
        if (!ladder)
        {
            uint32 filled = 0;
            preview = mgr.BuildLoadout(bot, mgr._baseTier, mgr._season, filled);
            loadout = &preview;
            ++previews;
        }

        if (!loadout)
        {
            ++noLoadout;
            handler->PSendSysMessage("  {} ({}): no loadout", bot->GetName(), ClassName(bot->getClass()));
            continue;
        }

        classMask |= 1u << bot->getClass();
        uint32 botItems = 0;
        uint32 botOk = 0;
        uint32 botState = 0;
        uint32 botWrong = 0;
        std::string failures;
        for (uint8 slot = 0; slot < SlotCount; ++slot)
        {
            uint32 const entry = (*loadout)[slot].Item;
            if (!entry)
                continue;

            ++botItems;
            ItemTemplate const* proto = sObjectMgr->GetItemTemplate(entry);
            if (proto && IsTypedArmour(proto) && proto->SubClass != BestArmourSubclass(bot->getClass()))
                ++botWrong;

            // A check only: the probe item is deleted at once
            uint16 dest = 0;
            InventoryResult const result = bot->CanEquipNewItem(slot, dest, entry, true);
            if (result == EQUIP_ERR_OK)
                ++botOk;
            else if (IsStateDependent(result))
            {
                ++botState;
                ++stateCodes[static_cast<uint32>(result)];
            }
            else
                failures +=
                    Acore::StringFormat(" {} {} (error {})", SlotNames[slot], entry, static_cast<uint32>(result));
        }

        uint32 const botMissing = MissingSlots(*loadout);
        uint32 const botFailed = botItems - botOk - botState;
        items += botItems;
        ok += botOk;
        stateDependent += botState;
        failed += botFailed;
        wrongArmour += botWrong;
        missing += botMissing;
        handler->PSendSysMessage(
            "  {} ({}{}): {} items, {} slots empty, CanEquipNewItem ok {}, state-dependent {}, "
            "failed {}{}, wrong armour type {}",
            bot->GetName(), ClassName(bot->getClass()), ladder ? "" : ", preview", botItems, botMissing, botOk,
            botState, botFailed, failures, botWrong);
    }

    uint32 classes = 0;
    for (uint32 mask = classMask; mask; mask &= mask - 1)
        ++classes;

    std::string codes;
    for (auto const& [code, number] : stateCodes)
        codes += Acore::StringFormat("{}error {} x{}", codes.empty() ? "" : ", ", code, number);

    std::string const summary = Acore::StringFormat(
        "PvP gear check: {} online bots ({} previews of non-ladder bots, {} classes), {} without a loadout; {} items, "
        "{} slots empty; CanEquipNewItem ok {}, state-dependent {} ({}), failed {}; wrong armour type {}",
        bots, previews, classes, noLoadout, items, missing, ok, stateDependent, codes.empty() ? "none" : codes, failed,
        wrongArmour);
    handler->SendSysMessage(summary);
    LOG_INFO("playerbots", "{}", summary);
    return true;
}

namespace
{
// The default slot priority: the weapon first, then the slots with the most stats
constexpr std::array<uint8, FillableSlots> DefaultBuyOrder = {
    EQUIPMENT_SLOT_MAINHAND, EQUIPMENT_SLOT_OFFHAND,   EQUIPMENT_SLOT_CHEST,    EQUIPMENT_SLOT_LEGS,
    EQUIPMENT_SLOT_HEAD,     EQUIPMENT_SLOT_SHOULDERS, EQUIPMENT_SLOT_HANDS,    EQUIPMENT_SLOT_FEET,
    EQUIPMENT_SLOT_WAIST,    EQUIPMENT_SLOT_WRISTS,    EQUIPMENT_SLOT_BACK,     EQUIPMENT_SLOT_NECK,
    EQUIPMENT_SLOT_FINGER1,  EQUIPMENT_SLOT_FINGER2,   EQUIPMENT_SLOT_TRINKET1, EQUIPMENT_SLOT_TRINKET2,
    EQUIPMENT_SLOT_RANGED};

struct SlotWord
{
    char const* Name;
    std::array<uint8, 2> Slots;  // NoSlot: unused
};

constexpr std::array<SlotWord, 14> SlotWords = {{
    {"weapon", {EQUIPMENT_SLOT_MAINHAND, EQUIPMENT_SLOT_OFFHAND}},
    {"ranged", {EQUIPMENT_SLOT_RANGED, PvpGearMgr::NoSlot}},
    {"head", {EQUIPMENT_SLOT_HEAD, PvpGearMgr::NoSlot}},
    {"neck", {EQUIPMENT_SLOT_NECK, PvpGearMgr::NoSlot}},
    {"shoulders", {EQUIPMENT_SLOT_SHOULDERS, PvpGearMgr::NoSlot}},
    {"chest", {EQUIPMENT_SLOT_CHEST, PvpGearMgr::NoSlot}},
    {"waist", {EQUIPMENT_SLOT_WAIST, PvpGearMgr::NoSlot}},
    {"legs", {EQUIPMENT_SLOT_LEGS, PvpGearMgr::NoSlot}},
    {"feet", {EQUIPMENT_SLOT_FEET, PvpGearMgr::NoSlot}},
    {"wrists", {EQUIPMENT_SLOT_WRISTS, PvpGearMgr::NoSlot}},
    {"hands", {EQUIPMENT_SLOT_HANDS, PvpGearMgr::NoSlot}},
    {"back", {EQUIPMENT_SLOT_BACK, PvpGearMgr::NoSlot}},
    {"rings", {EQUIPMENT_SLOT_FINGER1, EQUIPMENT_SLOT_FINGER2}},
    {"trinkets", {EQUIPMENT_SLOT_TRINKET1, EQUIPMENT_SLOT_TRINKET2}},
}};

constexpr uint32 MaxRecentPurchases = 200;
constexpr uint32 DefaultStatsCount = 20;
constexpr uint32 RatingBandWidth = 200;
constexpr uint32 RatingBands = 11;  // 0-199 ... 1800-1999, 2000+
constexpr uint32 MaxGrant = 1000000;

// World thread: the players of a rated arena match that ended get a buy check (GUIDs only; re-resolved there)
class PvpGearBuyCheckOperation : public PlayerbotOperation
{
public:
    explicit PvpGearBuyCheckOperation(std::vector<uint32> guids) : _guids(std::move(guids)) {}

    bool Execute() override
    {
        PvpGearMgr::Instance().AddBuyChecks(_guids, true);
        return true;
    }

    std::string GetName() const override { return "PvpGearBuyCheck"; }

private:
    std::vector<uint32> _guids;
};

// The bot's honor and arena points as they are in memory (capped by the core's setters)
void AppendCurrency(CharacterDatabaseTransaction& transaction, Player* bot)
{
    CharacterDatabasePreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_UDP_CHAR_HONOR_POINTS);
    stmt->SetData(0, bot->GetHonorPoints());
    stmt->SetData(1, bot->GetGUID().GetCounter());
    transaction->Append(stmt);
    stmt = CharacterDatabase.GetPreparedStatement(CHAR_UDP_CHAR_ARENA_POINTS);
    stmt->SetData(0, bot->GetArenaPoints());
    stmt->SetData(1, bot->GetGUID().GetCounter());
    transaction->Append(stmt);
}
}  // namespace

std::vector<uint8> PvpGearMgr::ParseSlotPriority(std::string const& text, std::string& unknown)
{
    std::vector<uint8> order;
    auto const add = [&order](uint8 slot)
    {
        if (slot != NoSlot && std::find(order.begin(), order.end(), slot) == order.end())
            order.push_back(slot);
    };

    unknown.clear();
    for (std::string const& part : Split(text, ','))
    {
        std::string const word = Lower(Trim(part));
        if (word.empty())
            continue;

        bool found = false;
        for (SlotWord const& entry : SlotWords)
        {
            if (word != entry.Name)
                continue;

            for (uint8 slot : entry.Slots)
                add(slot);
            found = true;
        }

        for (uint8 slot = 0; slot < SlotCount && !found; ++slot)
        {
            if (slot != EQUIPMENT_SLOT_BODY && slot != EQUIPMENT_SLOT_TABARD && word == SlotNames[slot])
            {
                add(slot);
                found = true;
            }
        }

        if (!found)
            unknown += (unknown.empty() ? "" : ", ") + word;
    }

    for (uint8 slot : DefaultBuyOrder)
        add(slot);

    return order;
}

std::vector<PvpGearMgr::Purchase> PvpGearMgr::DecideBuys(std::vector<BuySlot> const& slots, Wallet& wallet,
                                                         uint32 rating, bool saveTowardBetter)
{
    std::array<uint32, SlotCount> entries{};
    for (BuySlot const& slot : slots)
        if (slot.Slot < SlotCount)
            entries[slot.Slot] = slot.Current;

    auto const key = [](BuyCandidate const& candidate)
    { return std::make_tuple(candidate.Tier, candidate.Score, candidate.ItemLevel); };
    uint32 heldHonor = 0;  // kept for the targets of earlier slots (never more than the wallet)
    uint32 heldArenaPoints = 0;
    std::vector<Purchase> purchases;
    for (BuySlot const& slot : slots)
    {
        if (slot.Slot >= SlotCount)
            continue;

        uint32 const freeHonor = wallet.Honor - heldHonor;
        uint32 const freeArenaPoints = wallet.ArenaPoints - heldArenaPoints;
        uint32 const pairEntry = slot.Pair < SlotCount ? entries[slot.Pair] : 0;
        BuyCandidate const* target = nullptr;      // the best upgrade the rating allows
        BuyCandidate const* affordable = nullptr;  // the best one the wallet pays without a saved currency
        for (BuyCandidate const& candidate : slot.Candidates)
        {
            if (!candidate.Entry || candidate.Rating > rating || (pairEntry && candidate.Entry == pairEntry))
                continue;

            // Never down, never sideways: a higher tier, or a higher item level within the tier
            if (slot.Current &&
                std::tie(candidate.Tier, candidate.ItemLevel) <= std::tie(slot.CurrentTier, slot.CurrentLevel))
                continue;

            if (!target || key(candidate) > key(*target))
                target = &candidate;

            bool const payable = candidate.Honor <= freeHonor && candidate.ArenaPoints <= freeArenaPoints;
            if (payable && (!affordable || key(candidate) > key(*affordable)))
                affordable = &candidate;
        }

        if (!target)
            continue;

        // Saving toward the target: nothing lesser here, and the later slots spend only what is left after its price.
        // A target that needs a currency of which nothing is left is not saved toward, so honor still buys the honor
        // items while a bot has no arena points (none come in with Arena.AutoDistributePoints = 0)
        bool const reachable = (!target->Honor || freeHonor) && (!target->ArenaPoints || freeArenaPoints);
        if (saveTowardBetter && reachable && (!affordable || key(*affordable) < key(*target)))
        {
            heldHonor += std::min(target->Honor, freeHonor);
            heldArenaPoints += std::min(target->ArenaPoints, freeArenaPoints);
            continue;
        }

        if (!affordable)
            continue;

        wallet.Honor -= affordable->Honor;
        wallet.ArenaPoints -= affordable->ArenaPoints;
        entries[slot.Slot] = affordable->Entry;
        Purchase purchase;
        purchase.Slot = slot.Slot;
        purchase.Old = slot.Current;
        purchase.Entry = affordable->Entry;
        purchase.Tier = affordable->Tier;
        purchase.Honor = affordable->Honor;
        purchase.ArenaPoints = affordable->ArenaPoints;
        purchases.push_back(purchase);
    }

    return purchases;
}

void PvpGearMgr::QueueMatchEnd(Battleground* bg)
{
    if (!sPlayerbotAIConfig.PvpGearEnable || !sPlayerbotAIConfig.PvpGearBuyEnable || !Instance()._loaded || !bg ||
        !bg->isArena() || !bg->isRated())
        return;

    // Map thread: GUIDs only; the world thread re-resolves them and keeps the ladder bots
    std::vector<uint32> guids;
    for (auto const& [guid, player] : bg->GetPlayers())
        guids.push_back(guid.GetCounter());

    if (!guids.empty())
        PlayerbotWorldThreadProcessor::instance().QueueOperation(
            std::make_unique<PvpGearBuyCheckOperation>(std::move(guids)));
}

void PvpGearMgr::AddBuyChecks(std::vector<uint32> const& guids, bool matchEnd)
{
    PLAYERBOT_CHECK_WORLD_THREAD("PvpGearMgr::AddBuyChecks");
    if (matchEnd)
    {
        ++_ratedMatchEnds;
        _matchEndPlayers += guids.size();
    }

    for (uint32 guid : guids)
        if (std::find(_buyPending.begin(), _buyPending.end(), guid) == _buyPending.end())
            _buyPending.push_back(guid);
}

void PvpGearMgr::UpdateBuys(uint32 diff)
{
    if (!_loaded || !sPlayerbotAIConfig.PvpGearBuyEnable)
        return;

    PLAYERBOT_CHECK_WORLD_THREAD("PvpGearMgr::UpdateBuys");
    uint32 budget = std::max<uint32>(sPlayerbotAIConfig.PvpGearBuyBatchPerTick, 1);
    // The bots of the rated matches that ended first, then the timer's pass over the online ladder bots
    while (budget && !_buyPending.empty())
    {
        uint32 const guid = _buyPending.back();
        _buyPending.pop_back();
        CheckBuys(guid);
        ++_queuedChecks;
        --budget;
    }

    _buyTimer += diff;
    if (_buySweep.empty())
    {
        if (_buyTimer < sPlayerbotAIConfig.PvpGearBuyCheckIntervalMinutes * MINUTE * IN_MILLISECONDS)
            return;

        _buyTimer = 0;
        for (auto it = sRandomPlayerbotMgr.GetPlayerBotsBegin(); it != sRandomPlayerbotMgr.GetPlayerBotsEnd(); ++it)
            if (it->second && IsLadderBot(it->second))
                _buySweep.push_back(it->second->GetGUID().GetCounter());
    }

    while (budget && !_buySweep.empty())
    {
        uint32 const guid = _buySweep.back();
        _buySweep.pop_back();
        CheckBuys(guid);
        ++_timerChecks;
        --budget;
    }
}

void PvpGearMgr::NoteWallet(uint32 guid, Wallet const& now, bool countIncome)
{
    auto const [it, added] = _wallets.try_emplace(guid, now);
    if (added)
        return;

    if (countIncome)
    {
        if (now.Honor > it->second.Honor)
            _incomeHonor += now.Honor - it->second.Honor;

        if (now.ArenaPoints > it->second.ArenaPoints)
            _incomeArenaPoints += now.ArenaPoints - it->second.ArenaPoints;
    }

    it->second = now;
}

std::vector<PvpGearMgr::Purchase> PvpGearMgr::DecideCheckedBuys(std::vector<BuySlot>& slots, Wallet& wallet,
                                                                uint32 rating, bool saveTowardBetter,
                                                                std::function<bool(Purchase const&)> const& equippable,
                                                                std::vector<uint32>& rejected)
{
    auto const isRejected = [&rejected](uint32 entry)
    { return std::find(rejected.begin(), rejected.end(), entry) != rejected.end(); };
    Wallet const start = wallet;
    std::vector<Purchase> purchases;
    for (uint8 attempt = 0; attempt < MaxEquipAttempts; ++attempt)
    {
        wallet = start;
        purchases = DecideBuys(slots, wallet, rating, saveTowardBetter);
        size_t const before = rejected.size();
        for (Purchase const& purchase : purchases)
            if (!equippable(purchase))
                rejected.push_back(purchase.Entry);

        if (rejected.size() == before)
            return purchases;

        for (BuySlot& slot : slots)
            slot.Candidates.erase(
                std::remove_if(slot.Candidates.begin(), slot.Candidates.end(),
                               [&isRejected](BuyCandidate const& candidate) { return isRejected(candidate.Entry); }),
                slot.Candidates.end());
    }

    // Still a rejection in the last attempt: only the purchases that passed (a part of a set the wallet pays)
    wallet = start;
    std::vector<Purchase> checked;
    for (Purchase const& purchase : purchases)
    {
        if (isRejected(purchase.Entry))
            continue;

        wallet.Honor -= purchase.Honor;
        wallet.ArenaPoints -= purchase.ArenaPoints;
        checked.push_back(purchase);
    }

    return checked;
}

std::vector<PvpGearMgr::BuySlot> PvpGearMgr::BuildBuySlots(Player* bot, Loadout const& loadout, uint32 rating) const
{
    std::vector<uint8> order = _buyOrder;
    if (bot->getClass() == CLASS_HUNTER)
    {
        // A hunter's weapon is the ranged one: it takes the place before the main hand
        order.erase(std::remove(order.begin(), order.end(), static_cast<uint8>(EQUIPMENT_SLOT_RANGED)), order.end());
        order.insert(std::find(order.begin(), order.end(), static_cast<uint8>(EQUIPMENT_SLOT_MAINHAND)),
                     static_cast<uint8>(EQUIPMENT_SLOT_RANGED));
    }

    StatsWeightCalculator calc(bot);
    calc.SetPvpSpec(true);
    bool const titanGrip = bot->CanTitanGrip();
    auto const unbuyable = _unbuyable.find(bot->GetGUID().GetCounter());
    auto const leftOut = [&unbuyable, this](uint32 entry)
    {
        return unbuyable != _unbuyable.end() &&
               std::find(unbuyable->second.begin(), unbuyable->second.end(), entry) != unbuyable->second.end();
    };
    std::vector<BuySlot> slots;
    for (uint8 slot : order)
    {
        // Empty, or not in the catalogue (a changed tier order): nothing to upgrade; a reset rebuilds it
        CatalogueItem const* current = FindItem(loadout[slot].Item);
        if (!current)
            continue;

        BuySlot entry;
        entry.Slot = slot;
        entry.Current = current->Entry;
        entry.CurrentTier = current->Tier;
        entry.CurrentLevel = current->ItemLevel;
        if (slot == EQUIPMENT_SLOT_FINGER1)
            entry.Pair = EQUIPMENT_SLOT_FINGER2;
        else if (slot == EQUIPMENT_SLOT_FINGER2)
            entry.Pair = EQUIPMENT_SLOT_FINGER1;
        else if (slot == EQUIPMENT_SLOT_OFFHAND)
            entry.Pair = EQUIPMENT_SLOT_MAINHAND;

        // The same kind of item: no switch between a two-hander and one-handers; trinket 1 stays the PvP trinket
        uint8 const group = GroupOf(current->InventoryType);
        uint32 groupMask = Mask(group);
        if (slot == EQUIPMENT_SLOT_MAINHAND && (group == GROUP_ONE_HAND || group == GROUP_MAIN_HAND))
            groupMask = Mask(GROUP_ONE_HAND) | Mask(GROUP_MAIN_HAND);
        else if (slot == EQUIPMENT_SLOT_OFFHAND && (group == GROUP_ONE_HAND || group == GROUP_OFF_HAND_WEAPON))
            groupMask = Mask(GROUP_ONE_HAND) | Mask(GROUP_OFF_HAND_WEAPON);

        int8 const ccBreak = group == GROUP_TRINKET ? (current->CcBreak ? 1 : -1) : 0;
        for (size_t tier = current->Tier; tier < _tiers.size(); ++tier)
        {
            for (CatalogueItem const& item : _tiers[tier])
            {
                if (!(groupMask & Mask(GroupOf(item.InventoryType))) || item.Rating > rating ||
                    std::tie(item.Tier, item.ItemLevel) <= std::tie(current->Tier, current->ItemLevel))
                    continue;

                if ((ccBreak > 0 && !item.CcBreak) || (ccBreak < 0 && item.CcBreak) ||
                    (titanGrip && group == GROUP_TWO_HAND && IsPolearmOrStaff(item.Entry)) || !Usable(bot, item) ||
                    leftOut(item.Entry))
                    continue;

                BuyCandidate candidate;
                candidate.Entry = item.Entry;
                candidate.Tier = item.Tier;
                candidate.ItemLevel = item.ItemLevel;
                candidate.Honor = item.Honor;
                candidate.ArenaPoints = item.ArenaPoints;
                candidate.Rating = item.Rating;
                candidate.Score = calc.CalculateItem(item.Entry, 0, slot);
                entry.Candidates.push_back(candidate);
            }
        }

        // Also without candidates: the pair rule needs every slot's entry
        slots.push_back(std::move(entry));
    }

    return slots;
}

uint32 PvpGearMgr::CheckBuys(uint32 guid)
{
    Player* bot = ObjectAccessor::FindConnectedPlayer(ObjectGuid::Create<HighGuid::Player>(guid));
    if (!bot || !bot->IsInWorld() || !IsLadderBot(bot))
        return 0;

    Wallet wallet;
    wallet.Honor = bot->GetHonorPoints();
    wallet.ArenaPoints = bot->GetArenaPoints();
    NoteWallet(guid, wallet, true);
    // Not even the cheapest catalogue price: no candidates to build (every item costs honor or arena points)
    if (std::none_of(_prices.begin(), _prices.end(), [&wallet](std::pair<uint32, uint32> const& price)
                     { return price.first <= wallet.Honor && price.second <= wallet.ArenaPoints; }))
        return 0;

    EnsureLoadout(bot);
    auto const found = _loadouts.find(guid);
    if (found == _loadouts.end())
        return 0;

    // The vendor's rule: the best of min(personal rating, team rating) over the bot's arena teams
    uint32 const rating = bot->GetMaxPersonalArenaRatingRequirement(0);
    std::vector<BuySlot> slots = BuildBuySlots(bot, found->second, rating);
    auto const equippable = [bot](Purchase const& purchase)
    {
        // A check only: the probe item is deleted at once
        uint16 dest = 0;
        InventoryResult const result = bot->CanEquipNewItem(purchase.Slot, dest, purchase.Entry, true);
        if (result == EQUIP_ERR_OK || IsStateDependent(result))
            return true;

        LOG_DEBUG("playerbots", "PvP gear buy: {} cannot equip {} in the {} slot (error {}), left out for good",
                  bot->GetName(), purchase.Entry, SlotNames[purchase.Slot], static_cast<uint32>(result));
        return false;
    };
    std::vector<uint32> rejected;
    std::vector<Purchase> const purchases =
        DecideCheckedBuys(slots, wallet, rating, sPlayerbotAIConfig.PvpGearBuySaveTowardBetter, equippable, rejected);
    if (!rejected.empty())
    {
        std::vector<uint32>& unbuyable = _unbuyable[guid];
        unbuyable.insert(unbuyable.end(), rejected.begin(), rejected.end());
    }

    if (purchases.empty())
        return 0;

    // The currency in one character transaction, the rows in one playerbots transaction (both async)
    Loadout& loadout = found->second;
    CharacterDatabaseTransaction currency = CharacterDatabase.BeginTransaction();
    PlayerbotsDatabaseTransaction rows = PlayerbotsDatabase.BeginTransaction();
    uint32 const now = static_cast<uint32>(time(nullptr));
    uint32 bought = 0;
    for (Purchase const& purchase : purchases)
    {
        CatalogueItem const* item = FindItem(purchase.Entry);
        bot->ModifyHonorPoints(-static_cast<int32>(purchase.Honor));
        bot->ModifyArenaPoints(-static_cast<int32>(purchase.ArenaPoints));
        LoadoutSlot& data = loadout[purchase.Slot];
        data.Item = purchase.Entry;
        data.Tier = purchase.Tier;
        data.Season = _season;
        data.BoughtAt = now;
        PlayerbotsDatabasePreparedStatement* stmt = PlayerbotsDatabase.GetPreparedStatement(PLAYERBOTS_REP_PVP_LOADOUT);
        stmt->SetData(0, guid);
        stmt->SetData(1, purchase.Slot);
        stmt->SetData(2, data.Item);
        stmt->SetData(3, data.Tier);
        stmt->SetData(4, data.Season);
        stmt->SetData(5, data.BoughtAt);
        rows->Append(stmt);

        ++bought;
        ++_purchases;
        _spentHonor += purchase.Honor;
        _spentArenaPoints += purchase.ArenaPoints;
        LOG_INFO("playerbots",
                 "PvP gear buy: {} {} {} -> {} ({}) cost {} honor {} arena points, rating {} (needs {}), left {}/{}",
                 bot->GetName(), SlotNames[purchase.Slot], purchase.Old, purchase.Entry, TierLabel(purchase.Tier),
                 purchase.Honor, purchase.ArenaPoints, rating, item ? item->Rating : 0, bot->GetHonorPoints(),
                 bot->GetArenaPoints());

        PurchaseRecord record;
        record.Bot = bot->GetName();
        record.Slot = purchase.Slot;
        record.Old = purchase.Old;
        record.Entry = purchase.Entry;
        record.Tier = purchase.Tier;
        record.Honor = purchase.Honor;
        record.ArenaPoints = purchase.ArenaPoints;
        record.Rating = rating;
        record.At = now;
        _recent.push_back(record);
        if (_recent.size() > MaxRecentPurchases)
            _recent.pop_front();
    }

    if (!bought)
        return 0;

    AppendCurrency(currency, bot);
    CharacterDatabase.CommitTransaction(currency);
    PlayerbotsDatabase.CommitTransaction(rows);
    Wallet left;
    left.Honor = bot->GetHonorPoints();
    left.ArenaPoints = bot->GetArenaPoints();
    NoteWallet(guid, left, false);
    // A bot that wears its loadout (in a match, or on an arena server) swaps the bought slots now
    OnLoadoutChanged(bot);
    return bought;
}

bool PvpGearMgr::HandleStatsCommand(ChatHandler* handler, char const* args)
{
    PLAYERBOT_CHECK_WORLD_THREAD("PvpGearMgr::HandleStatsCommand");
    PvpGearMgr& mgr = Instance();
    if (!mgr._loaded)
        return Off(handler);

    uint32 count = DefaultStatsCount;
    std::string const text = Trim(args ? args : "");
    if (!text.empty())
    {
        std::optional<uint32> const number = Acore::StringTo<uint32>(text);
        if (!number || *number > MaxRecentPurchases)
        {
            handler->PSendSysMessage("Usage: .playerbots pvpgear stats [purchases to list 0-{}, default {}]",
                                     MaxRecentPurchases, DefaultStatsCount);
            return false;
        }
        count = *number;
    }

    struct Band
    {
        uint32 Bots = 0;
        uint64 ItemLevels = 0;
        uint32 Items = 0;
        std::vector<uint32> Tiers;
    };
    std::vector<Band> bands(RatingBands);
    for (Band& band : bands)
        band.Tiers.assign(mgr._tiers.size(), 0);

    uint32 online = 0;
    uint64 honor = 0;
    uint64 arenaPoints = 0;
    for (auto it = sRandomPlayerbotMgr.GetPlayerBotsBegin(); it != sRandomPlayerbotMgr.GetPlayerBotsEnd(); ++it)
    {
        Player* bot = it->second;
        if (!bot || !bot->IsInWorld() || !IsLadderBot(bot))
            continue;

        ++online;
        honor += bot->GetHonorPoints();
        arenaPoints += bot->GetArenaPoints();
        Loadout const* loadout = mgr.Find(bot->GetGUID().GetCounter());
        if (!loadout)
            continue;

        uint32 const rating = bot->GetMaxPersonalArenaRatingRequirement(0);
        Band& band = bands[std::min<uint32>(rating / RatingBandWidth, RatingBands - 1)];
        ++band.Bots;
        uint8 lowest = NoTier;
        for (LoadoutSlot const& data : *loadout)
        {
            CatalogueItem const* item = mgr.FindItem(data.Item);
            if (!item)
                continue;

            lowest = std::min(lowest, item->Tier);
            band.ItemLevels += item->ItemLevel;
            ++band.Items;
        }

        if (lowest != NoTier)
            ++band.Tiers[lowest];
    }

    handler->PSendSysMessage(
        "PvP gear stats: buying {} (every {} min, {} bots per tick, saving toward better {}); {} online ladder bots, "
        "mean {:.0f} honor and {:.0f} arena points",
        sPlayerbotAIConfig.PvpGearBuyEnable ? "on" : "off", sPlayerbotAIConfig.PvpGearBuyCheckIntervalMinutes,
        sPlayerbotAIConfig.PvpGearBuyBatchPerTick, sPlayerbotAIConfig.PvpGearBuySaveTowardBetter ? "on" : "off", online,
        online ? static_cast<double>(honor) / online : 0.0, online ? static_cast<double>(arenaPoints) / online : 0.0);
    handler->SendSysMessage(
        "By rating band (the best min(personal, team) rating over the bot's arena teams; tier = the lowest tier among "
        "the filled slots; ilvl = the mean item level of the loadout):");
    for (uint32 index = 0; index < RatingBands; ++index)
    {
        Band const& band = bands[index];
        if (!band.Bots)
            continue;

        std::string tiers;
        for (size_t tier = 0; tier < band.Tiers.size(); ++tier)
            if (band.Tiers[tier])
                tiers += Acore::StringFormat("{}{} {}", tiers.empty() ? "" : ", ",
                                             mgr.TierLabel(static_cast<uint8>(tier)), band.Tiers[tier]);

        std::string const range = index + 1 < RatingBands ? Acore::StringFormat("{}-{}", index * RatingBandWidth,
                                                                                (index + 1) * RatingBandWidth - 1)
                                                          : Acore::StringFormat("{}+", index * RatingBandWidth);
        handler->PSendSysMessage("  {}: {} bots, ilvl {:.1f}; {}", range, band.Bots,
                                 band.Items ? static_cast<double>(band.ItemLevels) / band.Items : 0.0,
                                 tiers.empty() ? "-" : tiers);
    }

    uint32 const now = static_cast<uint32>(time(nullptr));
    double const hours = std::max<uint32>(now - mgr._buyStart, 1) / 3600.0;
    double const days = hours / 24.0;
    double const seen = static_cast<double>(std::max<size_t>(mgr._wallets.size(), 1));
    handler->PSendSysMessage(
        "Purchases since the start {:.1f} h ago: {} ({:.1f} per hour), {} honor and {} arena points spent; bot "
        "checks: {} by the timer, {} queued by grants and rated match ends; {} rated arena matches ended ({} players "
        "queued)",
        hours, mgr._purchases, static_cast<double>(mgr._purchases) / hours, mgr._spentHonor, mgr._spentArenaPoints,
        mgr._timerChecks, mgr._queuedChecks, mgr._ratedMatchEnds, mgr._matchEndPlayers);
    handler->PSendSysMessage(
        "Income per bot per day (rises between two checks, grants and purchases left out; {} bots seen): {:.0f} honor, "
        "{:.0f} arena points",
        mgr._wallets.size(), static_cast<double>(mgr._incomeHonor) / seen / days,
        static_cast<double>(mgr._incomeArenaPoints) / seen / days);

    uint32 const shown = std::min<uint32>(count, static_cast<uint32>(mgr._recent.size()));
    if (shown)
        handler->PSendSysMessage("The last {} purchases:", shown);

    for (auto it = mgr._recent.end() - shown; it != mgr._recent.end(); ++it)
        handler->PSendSysMessage("  {} s ago: {} {} {} -> {} ({}) cost {} honor {} arena points, rating {}",
                                 now - it->At, it->Bot, SlotNames[it->Slot], it->Old, it->Entry,
                                 mgr.TierLabel(it->Tier), it->Honor, it->ArenaPoints, it->Rating);

    return true;
}

bool PvpGearMgr::HandleGrantCommand(ChatHandler* handler, char const* args)
{
    PLAYERBOT_CHECK_WORLD_THREAD("PvpGearMgr::HandleGrantCommand");
    PvpGearMgr& mgr = Instance();
    if (!mgr._loaded)
        return Off(handler);

    std::istringstream in(args ? args : "");
    std::string target;
    std::string honorText;
    std::string arenaText;
    std::string extra;
    in >> target >> honorText >> arenaText >> extra;
    std::optional<uint32> const honor = Acore::StringTo<uint32>(honorText);
    std::optional<uint32> const arenaPoints = Acore::StringTo<uint32>(arenaText);
    if (target.empty() || !honor || !arenaPoints || !extra.empty() || *honor > MaxGrant || *arenaPoints > MaxGrant)
    {
        handler->PSendSysMessage("Usage: .playerbots pvpgear grant <bot name|all> <honor 0-{}> <arena points 0-{}>",
                                 MaxGrant, MaxGrant);
        return false;
    }

    std::vector<Player*> bots;
    if (Lower(target) == "all")
    {
        for (auto it = sRandomPlayerbotMgr.GetPlayerBotsBegin(); it != sRandomPlayerbotMgr.GetPlayerBotsEnd(); ++it)
            if (it->second && it->second->IsInWorld() && IsLadderBot(it->second))
                bots.push_back(it->second);
    }
    else
    {
        std::string name = target;
        Player* bot = normalizePlayerName(name) ? ObjectAccessor::FindPlayerByName(name) : nullptr;
        if (!bot || !IsLadderBot(bot))
        {
            handler->PSendSysMessage(
                "{} is not an online ladder bot (a random bot at the maximum level in an arena team)", target);
            return false;
        }
        bots.push_back(bot);
    }

    // Through the player's own setters (capped by MaxHonorPoints and MaxArenaPoints), saved in one transaction
    CharacterDatabaseTransaction transaction = CharacterDatabase.BeginTransaction();
    std::vector<uint32> guids;
    for (Player* bot : bots)
    {
        bot->ModifyHonorPoints(static_cast<int32>(*honor));
        bot->ModifyArenaPoints(static_cast<int32>(*arenaPoints));
        AppendCurrency(transaction, bot);
        Wallet now;
        now.Honor = bot->GetHonorPoints();
        now.ArenaPoints = bot->GetArenaPoints();
        mgr.NoteWallet(bot->GetGUID().GetCounter(), now, false);  // not income
        guids.push_back(bot->GetGUID().GetCounter());
    }

    CharacterDatabase.CommitTransaction(transaction);
    if (sPlayerbotAIConfig.PvpGearBuyEnable)
        mgr.AddBuyChecks(guids, false);

    std::string const one =
        bots.size() == 1 ? Acore::StringFormat(" ({} has {} honor, {} arena points now)", bots.front()->GetName(),
                                               bots.front()->GetHonorPoints(), bots.front()->GetArenaPoints())
                         : "";
    std::string const message =
        Acore::StringFormat("PvP gear grant: {} honor and {} arena points to {} ladder bots{}{}", *honor, *arenaPoints,
                            bots.size(), one, sPlayerbotAIConfig.PvpGearBuyEnable ? "; buy checks queued" : "");
    handler->SendSysMessage(message);
    LOG_INFO("playerbots", "{}", message);
    return true;
}

void PvpGearMgr::SelftestBuy(std::function<void(bool, std::string const&)> const& check)
{
    std::string unknown;
    std::vector<uint8> order = ParseSlotPriority(
        "weapon,chest,legs,head,shoulders,hands,feet,waist,wrists,back,neck,rings,trinkets,ranged", unknown);
    check(order.size() == FillableSlots && order[0] == EQUIPMENT_SLOT_MAINHAND && order[1] == EQUIPMENT_SLOT_OFFHAND &&
              order.back() == EQUIPMENT_SLOT_RANGED && unknown.empty(),
          "buy: the default slot priority, weapon first");
    order = ParseSlotPriority(" Rings, weapon ,bogus", unknown);
    check(order.size() == FillableSlots && order[0] == EQUIPMENT_SLOT_FINGER1 && order[1] == EQUIPMENT_SLOT_FINGER2 &&
              order[2] == EQUIPMENT_SLOT_MAINHAND && order[4] == EQUIPMENT_SLOT_CHEST && unknown == "bogus",
          "buy: a partial priority, the rest in the default order, unknown names listed");

    auto const candidate =
        [](uint32 entry, uint8 tier, uint16 itemLevel, uint32 honor, uint32 arenaPoints, uint32 rating, float score)
    {
        BuyCandidate result;
        result.Entry = entry;
        result.Tier = tier;
        result.ItemLevel = itemLevel;
        result.Honor = honor;
        result.ArenaPoints = arenaPoints;
        result.Rating = rating;
        result.Score = score;
        return result;
    };
    auto const slotOf = [](uint8 slot, uint32 current, uint8 tier, uint16 itemLevel,
                           std::vector<BuyCandidate> candidates, uint8 pair = NoSlot)
    {
        BuySlot result;
        result.Slot = slot;
        result.Current = current;
        result.CurrentTier = tier;
        result.CurrentLevel = itemLevel;
        result.Pair = pair;
        result.Candidates = std::move(candidates);
        return result;
    };
    Wallet left;
    auto const decide =
        [&left](std::vector<BuySlot> const& slots, uint32 honor, uint32 arenaPoints, uint32 rating, bool save)
    {
        left.Honor = honor;
        left.ArenaPoints = arenaPoints;
        return DecideBuys(slots, left, rating, save);
    };
    auto const entries = [](std::vector<Purchase> const& purchases)
    {
        std::vector<uint32> result;
        for (Purchase const& purchase : purchases)
            result.push_back(purchase.Entry);
        return result;
    };
    using Entries = std::vector<uint32>;

    // A Deadly weapon (tier 1, 213) and a Hateful chest (tier 0, 200); arena-point and honor upgrades
    BuySlot const weapon = slotOf(EQUIPMENT_SLOT_MAINHAND, 100, 1, 213,
                                  {candidate(101, 2, 226, 0, 1300, 0, 10.0f), candidate(105, 1, 219, 0, 800, 0, 7.0f)});
    BuySlot const furiousOnly =
        slotOf(EQUIPMENT_SLOT_MAINHAND, 100, 1, 213, {candidate(101, 2, 226, 0, 1300, 0, 10.0f)});
    BuySlot const chest = slotOf(EQUIPMENT_SLOT_CHEST, 200, 0, 200, {candidate(201, 1, 213, 0, 1000, 0, 5.0f)});
    BuySlot const honorChest = slotOf(EQUIPMENT_SLOT_CHEST, 200, 0, 200, {candidate(203, 1, 213, 30000, 0, 0, 5.0f)});
    BuySlot const head = slotOf(EQUIPMENT_SLOT_HEAD, 300, 0, 200, {candidate(301, 3, 251, 40000, 0, 1600, 8.0f)});

    check(decide({weapon, chest, head}, 0, 0, 3000, true).empty(), "buy: nothing affordable, nothing bought");
    check(decide({head}, 50000, 0, 1599, false).empty(), "buy: honor enough but the rating short");
    check(entries(decide({head}, 50000, 0, 1600, false)) == Entries{301}, "buy: rating and honor enough");
    check(decide({furiousOnly}, 0, 1299, 3000, false).empty(), "buy: the rating enough but the arena points short");
    check(entries(decide({weapon, chest}, 0, 1300, 0, true)) == Entries{101},
          "buy: the weapon first (priority), the chest waits");
    check(entries(decide({weapon, chest}, 0, 1300, 0, false)) == Entries{101},
          "buy: saving off, the priority still decides who pays first");
    check(decide({weapon, chest}, 0, 1000, 0, true).empty(),
          "buy: saving toward the weapon holds the arena points back (no lesser weapon, no chest)");
    check(entries(decide({weapon, chest}, 0, 1000, 0, false)) == Entries{105},
          "buy: saving off buys the best weapon the wallet pays now");
    check(entries(decide({furiousOnly, chest}, 0, 1000, 0, false)) == Entries{201},
          "buy: saving off buys the lower-priority slot");
    check(entries(decide({weapon, honorChest}, 30000, 1000, 0, true)) == Entries{203},
          "buy: saving arena points still lets an honor piece be bought");
    check(decide({slotOf(EQUIPMENT_SLOT_HEAD, 400, 4, 270, {candidate(401, 4, 270, 0, 100, 0, 9.0f)})}, 99999, 9999,
                 3000, false)
              .empty(),
          "buy: the top tier at its highest item level, nothing bought");
    check(decide({slotOf(EQUIPMENT_SLOT_CHEST, 500, 2, 226,
                         {candidate(501, 1, 245, 100, 0, 0, 50.0f), candidate(502, 2, 226, 100, 0, 0, 60.0f)})},
                 99999, 9999, 3000, false)
              .empty(),
          "buy: never a lower tier or the same item level, whatever the score");

    std::vector<BuyCandidate> const rings = {candidate(601, 2, 226, 1000, 0, 0, 9.0f),
                                             candidate(602, 2, 226, 1000, 0, 0, 8.0f)};
    check(entries(decide({slotOf(EQUIPMENT_SLOT_FINGER1, 600, 0, 200, rings, EQUIPMENT_SLOT_FINGER2),
                          slotOf(EQUIPMENT_SLOT_FINGER2, 603, 0, 200, rings, EQUIPMENT_SLOT_FINGER1)},
                         5000, 0, 0, false)) == Entries{601, 602},
          "buy: ring 2 never takes the ring ring 1 just bought");
    check(decide({slotOf(EQUIPMENT_SLOT_FINGER1, 601, 2, 226, {}, EQUIPMENT_SLOT_FINGER2),
                  slotOf(EQUIPMENT_SLOT_FINGER2, 603, 0, 200, {candidate(601, 2, 226, 1000, 0, 0, 9.0f)},
                         EQUIPMENT_SLOT_FINGER1)},
                 5000, 0, 0, false)
              .empty(),
          "buy: ring 2 never takes ring 1's item");

    BuySlot const furious =
        slotOf(EQUIPMENT_SLOT_MAINHAND, 700, 2, 226, {candidate(701, 2, 239, 0, 1300, 2350, 12.0f)});
    check(decide({furious}, 0, 5000, 2349, false).empty(), "buy: the Furious 239 weapon not below rating 2350");
    check(entries(decide({furious}, 0, 5000, 2350, false)) == Entries{701},
          "buy: the Furious 239 weapon at rating 2350 (within the tier by item level)");

    decide({weapon}, 500, 2000, 0, true);
    check(left.Honor == 500 && left.ArenaPoints == 700, "buy: the wallet after a buy = before - the cost exactly");
    BuySlot const head2 = slotOf(EQUIPMENT_SLOT_HEAD, 800, 0, 200, {candidate(801, 1, 213, 0, 1000, 0, 4.0f)});
    check(decide({chest, head2}, 0, 1500, 0, false).size() == 1 && left.ArenaPoints == 500,
          "buy: two upgrades, the wallet pays one");
    check(decide({chest, head2}, 0, 2000, 0, false).size() == 2 && left.ArenaPoints == 0,
          "buy: two buys in one pass never exceed the wallet");

    // Organic income: honor, no arena points (Arena.AutoDistributePoints = 0); the Relentless chest needs both
    BuySlot const relentlessChest =
        slotOf(EQUIPMENT_SLOT_CHEST, 200, 0, 200, {candidate(205, 3, 245, 13200, 770, 0, 9.0f)});
    BuySlot const honorFeet = slotOf(EQUIPMENT_SLOT_FEET, 900, 0, 200, {candidate(901, 1, 213, 20000, 0, 0, 3.0f)});
    check(entries(decide({relentlessChest, honorFeet}, 30000, 0, 0, true)) == Entries{901} && left.Honor == 10000,
          "buy: no arena points, a target that needs them holds no honor back (a later honor item bought)");
    BuySlot const twoChests =
        slotOf(EQUIPMENT_SLOT_CHEST, 200, 0, 200,
               {candidate(205, 3, 245, 13200, 770, 0, 9.0f), candidate(206, 1, 213, 10000, 0, 0, 5.0f)});
    check(entries(decide({twoChests}, 30000, 0, 0, true)) == Entries{206},
          "buy: no arena points, the honor chest instead of saving toward the arena-point one");
    check(decide({relentlessChest, honorFeet}, 30000, 100, 0, true).empty(),
          "buy: saving toward the chest holds its 13200 honor back (16800 left, the feet cost 20000)");
    BuySlot const cheapFeet = slotOf(EQUIPMENT_SLOT_FEET, 900, 0, 200, {candidate(902, 1, 213, 15000, 0, 0, 3.0f)});
    check(entries(decide({relentlessChest, cheapFeet}, 30000, 100, 0, true)) == Entries{902} && left.Honor == 15000 &&
              left.ArenaPoints == 100,
          "buy: saving holds only the target's price, the rest buys a later slot");

    // A purchase the bot cannot equip for good is left out and decided again: it holds no currency back
    auto const notEntry = [](uint32 entry)
    { return [entry](Purchase const& purchase) { return purchase.Entry != entry; }; };
    std::vector<BuySlot> probed = {
        slotOf(EQUIPMENT_SLOT_MAINHAND, 100, 1, 213, {candidate(110, 2, 226, 20000, 0, 0, 10.0f)}),
        slotOf(EQUIPMENT_SLOT_FEET, 900, 0, 200, {candidate(911, 1, 213, 15000, 0, 0, 3.0f)})};
    std::vector<uint32> rejected;
    left.Honor = 30000;
    left.ArenaPoints = 0;
    check(entries(DecideCheckedBuys(probed, left, 0, true, notEntry(110), rejected)) == Entries{911} &&
              rejected == Entries{110} && left.Honor == 15000 && probed[0].Candidates.empty(),
          "buy: an unequippable weapon is left out, its honor buys the feet");
    std::vector<BuySlot> probed2 = {
        slotOf(EQUIPMENT_SLOT_MAINHAND, 100, 1, 213,
               {candidate(110, 2, 226, 20000, 0, 0, 10.0f), candidate(111, 1, 219, 10000, 0, 0, 7.0f)})};
    std::vector<BuySlot> probed3 = probed2;
    rejected.clear();
    left.Honor = 30000;
    check(entries(DecideCheckedBuys(probed2, left, 0, true, notEntry(110), rejected)) == Entries{111} &&
              left.Honor == 20000,
          "buy: an unequippable target is left out, the next weapon bought");
    rejected.clear();
    left.Honor = 30000;
    check(DecideCheckedBuys(
              probed3, left, 0, true, [](Purchase const&) { return false; }, rejected)
                  .empty() &&
              rejected.size() == 2 && left.Honor == 30000,
          "buy: nothing equippable, nothing bought and the wallet whole");
}

bool PvpGearMgr::HandleSelftestCommand(ChatHandler* handler, char const* /*args*/)
{
    uint32 checks = 0;
    uint32 failures = 0;
    auto const check = [&](bool passed, std::string const& what)
    {
        ++checks;
        if (passed)
            return;

        ++failures;
        handler->PSendSysMessage("  FAIL: {}", what);
    };

    std::vector<std::vector<std::string>> const tiers =
        ParseTierOrder(" Hateful/Savage , Deadly,Furious,,Relentless,Wrathful ");
    check(tiers.size() == 5, "five tiers parsed");
    check(!tiers.empty() && tiers[0] == std::vector<std::string>{"hateful", "savage"}, "tier 1 = hateful/savage");
    check(ParseTierOrder(" , ").empty(), "an empty order has no tier");

    auto const offer = [](uint32 entry, std::string const& name, uint16 itemLevel, uint8 requiredLevel,
                          uint8 inventoryType, uint32 honor, uint32 arenaPoints, uint32 arenaSlot = 0,
                          uint32 rating = 0, bool token = false, bool ccBreak = false)
    {
        Offer result;
        result.Entry = entry;
        result.Name = name;
        result.ItemLevel = itemLevel;
        result.RequiredLevel = requiredLevel;
        result.InventoryType = inventoryType;
        result.Honor = honor;
        result.ArenaPoints = arenaPoints;
        result.ArenaSlot = arenaSlot;
        result.Rating = rating;
        result.TokenCost = token;
        result.CcBreak = ccBreak;
        return result;
    };

    std::vector<Offer> const offers = {
        offer(1, "Hateful Gladiator's Plate Helm", 200, 80, INVTYPE_HEAD, 45200, 0),
        offer(2, "Savage Gladiator's Girdle", 200, 80, INVTYPE_WAIST, 30000, 0),
        offer(3, "Deadly Gladiator's Greatsword", 213, 80, INVTYPE_2HWEAPON, 0, 1100, 0, 1500),
        offer(3, "Deadly Gladiator's Greatsword", 213, 80, INVTYPE_2HWEAPON, 0, 1100),
        offer(4, "Medallion of the Horde", 200, 80, INVTYPE_TRINKET, 30000, 0, 0, 0, false, true),
        offer(5, "Medallion of the Horde", 226, 80, INVTYPE_TRINKET, 0, 1500, 0, 0, false, true),
        offer(6, "Furious Gladiator's Girdle", 226, 80, INVTYPE_WAIST, 40000, 0),
        offer(7, "Wrathful Gladiator's Plate Helm", 277, 80, INVTYPE_HEAD, 0, 2000, 1, 2400),
        offer(8, "Titan-Forged Rune of Cruelty", 213, 80, INVTYPE_TRINKET, 0, 0, 0, 0, true),
        offer(9, "Merciless Gladiator's Plate Helm", 136, 70, INVTYPE_HEAD, 18000, 0),
        offer(10, "Bloodstone", 200, 80, INVTYPE_NON_EQUIP, 10000, 0),
        offer(11, "Old Necklace", 190, 80, INVTYPE_NECK, 10000, 0),
        offer(13, "Relentless Gladiator's Plate Shoulders", 251, 80, INVTYPE_SHOULDERS, 0, 0, 0, 1800),
        offer(14, "Wrathful Gladiator's Plate Helm", 270, 80, INVTYPE_HEAD, 0, 1650, 0, 1800),
        offer(15, "Battlemaster's Rage", 245, 80, INVTYPE_TRINKET, 63000, 0),
        offer(16, "Relentless Gladiator's Plate Helm", 251, 80, INVTYPE_HEAD, 0, 1400, 0, 1600),
    };

    SkipCounts skipped;
    std::vector<CatalogueItem> const catalogue = BuildCatalogue(offers, tiers, skipped);
    auto const tierOf = [&](uint32 entry)
    {
        for (CatalogueItem const& item : catalogue)
            if (item.Entry == entry)
                return item.Tier;

        return NoTier;
    };
    auto const ratingOf = [&](uint32 entry)
    {
        for (CatalogueItem const& item : catalogue)
            if (item.Entry == entry)
                return item.Rating;

        return UINT32_MAX;
    };

    check(catalogue.size() == 9, Acore::StringFormat("9 catalogue items (got {})", catalogue.size()));
    check(tierOf(1) == 0, "Hateful helm in tier 1");
    check(tierOf(2) == 0, "Savage girdle in tier 1 (alias)");
    check(tierOf(3) == 1, "Deadly greatsword in tier 2");
    check(ratingOf(3) == 0, "duplicate offer: the one without a rating kept");
    check(tierOf(4) == 0, "medallion 200 placed by item level in tier 1");
    check(tierOf(5) == 2, "medallion 226 placed by item level in tier 3");
    check(tierOf(6) == 2, "Furious girdle in tier 3");
    check(tierOf(14) == 4, "Wrathful 270 helm in tier 5");
    check(tierOf(15) == 2, "Battlemaster's 245 below the first Relentless item (251) in tier 3");
    check(tierOf(16) == 3, "Relentless helm in tier 4");
    check(tierOf(7) == NoTier && skipped.ArenaSlot == 1, "3v3/5v5-only Wrathful 277 left out");
    check(tierOf(8) == NoTier && skipped.TokenCost == 1, "token cost left out");
    check(tierOf(9) == NoTier && skipped.LowLevel == 1, "level 70 gear left out");
    check(tierOf(10) == NoTier && skipped.NotEquippable == 1, "not equippable left out");
    check(tierOf(11) == NoTier && skipped.NoTier == 1, "below every tier left out");
    check(tierOf(13) == NoTier && skipped.NoPvpCurrency == 1, "rating only, no honor or arena points, left out");
    check(skipped.Duplicate == 1, "one duplicate offer");
    check(std::is_sorted(catalogue.begin(), catalogue.end(), [](CatalogueItem const& a, CatalogueItem const& b)
                         { return std::tie(a.Tier, a.Entry) < std::tie(b.Tier, b.Entry); }),
          "sorted by tier and entry");
    SelftestWear(check);
    SelftestBuy(check);

    handler->PSendSysMessage("PvP gear selftest: {} checks, {} failed: {}", checks, failures,
                             failures ? "FAIL" : "PASS");
    return true;
}
