/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "PvpGearMgr.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <optional>
#include <sstream>
#include <tuple>

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
            Field* fields = result->Fetch();
            uint8 const slot = fields[1].Get<uint8>();
            if (slot >= SlotCount)
                continue;

            LoadoutSlot& data = _loadouts[fields[0].Get<uint32>()][slot];
            data.Item = fields[2].Get<uint32>();
            // The catalogue decides the tier, so a changed tier order does not mislabel stored items
            CatalogueItem const* item = FindItem(data.Item);
            data.Tier = item ? item->Tier : fields[3].Get<uint8>();
            data.Season = fields[4].Get<uint32>();
            data.BoughtAt = fields[5].Get<uint32>();
            maxSeason = std::max(maxSeason, data.Season);
            ++rows;
        } while (result->NextRow());
    }

    _season = std::max<uint32>(maxSeason, 1);
    _loaded = true;
    LOG_INFO("server.loading", ">> Loaded {} PvP loadouts ({} rows), season {}, in {} ms", _loadouts.size(), rows,
             _season, GetMSTimeDiffToNow(oldMSTime));
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
                                            uint8 minTier, std::vector<uint32> exclude, int8 ccBreak) const
{
    for (uint8 attempt = 0; attempt < MaxEquipAttempts; ++attempt)
    {
        Pick const pick = Best(bot, calc, groupMask, slot, minTier, exclude, ccBreak);
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
        put(entry.Slot, BestEquippable(bot, calc, Mask(entry.Group), entry.Slot, tier, none, 0));

    Pick const ring = BestEquippable(bot, calc, Mask(GROUP_FINGER), EQUIPMENT_SLOT_FINGER1, tier, none, 0);
    put(EQUIPMENT_SLOT_FINGER1, ring);
    put(EQUIPMENT_SLOT_FINGER2, BestEquippable(bot, calc, Mask(GROUP_FINGER), EQUIPMENT_SLOT_FINGER2, tier,
                                               ring.Item ? std::vector<uint32>{ring.Item->Entry} : none, 0));
    // Trinket 1: the PvP trinket (DT.5: every ladder bot has one, also with a racial that breaks crowd control);
    // trinket 2: another trinket
    put(EQUIPMENT_SLOT_TRINKET1,
        BestEquippable(bot, calc, Mask(GROUP_TRINKET), EQUIPMENT_SLOT_TRINKET1, tier, none, 1));
    put(EQUIPMENT_SLOT_TRINKET2,
        BestEquippable(bot, calc, Mask(GROUP_TRINKET), EQUIPMENT_SLOT_TRINKET2, tier, none, -1));

    // Weapons: a two-hander (with Titan's Grip plus an off-hand weapon) against a main hand plus an off hand
    std::vector<uint32> twoHandExclude;
    bool const titanGrip = bot->CanTitanGrip();
    if (titanGrip)
        for (std::vector<CatalogueItem> const& items : _tiers)
            for (CatalogueItem const& item : items)
                if (GroupOf(item.InventoryType) == GROUP_TWO_HAND && IsPolearmOrStaff(item.Entry))
                    twoHandExclude.push_back(item.Entry);

    Pick const twoHand =
        BestEquippable(bot, calc, Mask(GROUP_TWO_HAND), EQUIPMENT_SLOT_MAINHAND, tier, twoHandExclude, 0);
    Pick twoHandOff;
    if (twoHand.Item && titanGrip)
    {
        twoHandExclude.push_back(twoHand.Item->Entry);
        twoHandOff =
            BestEquippable(bot, calc, Mask(GROUP_TWO_HAND) | Mask(GROUP_ONE_HAND) | Mask(GROUP_OFF_HAND_WEAPON),
                           EQUIPMENT_SLOT_OFFHAND, tier, twoHandExclude, 0);
    }

    Pick const mainHand =
        BestEquippable(bot, calc, Mask(GROUP_ONE_HAND) | Mask(GROUP_MAIN_HAND), EQUIPMENT_SLOT_MAINHAND, tier, none, 0);
    uint32 offMask = Mask(GROUP_OFF_HAND_OTHER);
    if (bot->CanDualWield())
        offMask |= Mask(GROUP_ONE_HAND) | Mask(GROUP_OFF_HAND_WEAPON);

    Pick offHand;
    if (mainHand.Item)
        offHand = BestEquippable(bot, calc, offMask, EQUIPMENT_SLOT_OFFHAND, tier,
                                 std::vector<uint32>{mainHand.Item->Entry}, 0);

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

std::string PvpGearMgr::NextUpgrade(Player* bot, uint32 guid, Loadout const& loadout, uint8 slot) const
{
    CatalogueItem const* current = FindItem(loadout[slot].Item);
    if (!current)
        return "not in the catalogue";

    if (current->Tier + 1u >= _tiers.size())
        return "none (top tier)";

    uint32 const groupMask = Mask(GroupOf(current->InventoryType));
    int8 const ccBreak = GroupOf(current->InventoryType) == GROUP_TRINKET ? (current->CcBreak ? 1 : -1) : 0;
    std::vector<uint32> exclude;
    if (slot == EQUIPMENT_SLOT_FINGER1 || slot == EQUIPMENT_SLOT_FINGER2)
        exclude.push_back(
            loadout[slot == EQUIPMENT_SLOT_FINGER1 ? EQUIPMENT_SLOT_FINGER2 : EQUIPMENT_SLOT_FINGER1].Item);

    uint8 const minTier = current->Tier + 1;
    Pick next;
    if (bot)
    {
        StatsWeightCalculator calc(bot);
        calc.SetPvpSpec(true);
        // The next upgrade the bot's rating allows; with none, the next one at all
        next =
            Best(bot, calc, groupMask, slot, minTier, exclude, ccBreak, bot->GetMaxPersonalArenaRatingRequirement(0));
        if (!next.Item)
            next = Best(bot, calc, groupMask, slot, minTier, exclude, ccBreak);
    }
    else
    {
        // Offline: the cheapest item of the same kind for the bot's class and race
        CharacterCacheEntry const* cache =
            sCharacterCache->GetCharacterCacheByGuid(ObjectGuid::Create<HighGuid::Player>(guid));
        ItemTemplate const* currentProto = sObjectMgr->GetItemTemplate(current->Entry);
        if (!cache || !currentProto)
            return "unknown";

        int32 const classMask = 1 << (cache->Class - 1);
        int32 const raceMask = 1 << (cache->Race - 1);
        for (size_t tier = minTier; tier < _tiers.size() && !next.Item; ++tier)
        {
            for (CatalogueItem const& item : _tiers[tier])
            {
                ItemTemplate const* proto = sObjectMgr->GetItemTemplate(item.Entry);
                if (!proto || !(groupMask & Mask(GroupOf(item.InventoryType))) || (ccBreak > 0 && !item.CcBreak) ||
                    (ccBreak < 0 && item.CcBreak) ||
                    std::find(exclude.begin(), exclude.end(), item.Entry) != exclude.end())
                    continue;

                if (!(proto->AllowableClass & classMask) || !(proto->AllowableRace & raceMask) ||
                    proto->Class != currentProto->Class || proto->SubClass != currentProto->SubClass)
                    continue;

                if (!next.Item || item.Honor + item.ArenaPoints < next.Item->Honor + next.Item->ArenaPoints)
                    next.Item = &item;
            }
        }
    }

    if (!next.Item)
        return "none";

    ItemTemplate const* proto = sObjectMgr->GetItemTemplate(next.Item->Entry);
    std::string text = Acore::StringFormat("{} {} ({} {}): {} honor, {} arena points, rating {}", next.Item->Entry,
                                           proto ? proto->Name1 : "?", TierLabel(next.Item->Tier), next.Item->ItemLevel,
                                           next.Item->Honor, next.Item->ArenaPoints, next.Item->Rating);
    if (!bot)
        return text;

    uint32 const rating = bot->GetMaxPersonalArenaRatingRequirement(0);
    if (bot->GetHonorPoints() >= next.Item->Honor && bot->GetArenaPoints() >= next.Item->ArenaPoints &&
        rating >= next.Item->Rating)
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
    uint8 tier = mgr._baseTier;
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
    mgr._baseTier = tier;  // until the restart: the cleared loadouts are rebuilt at this tier

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

    handler->PSendSysMessage("PvP gear selftest: {} checks, {} failed: {}", checks, failures,
                             failures ? "FAIL" : "PASS");
    return true;
}
