/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_PVPGEARMGR_H
#define _PLAYERBOT_PVPGEARMGR_H

#include <array>
#include <string>
#include <unordered_map>
#include <vector>

#include "Define.h"

class ChatHandler;
class Player;
class StatsWeightCalculator;

// PvP gear of the ladder bots (Playerbots.PvpGear.Enable): a catalogue of the PvP vendor items grouped into tiers,
// built at startup, and a stored loadout per ladder bot (one item entry per equipment slot, playerbots table
// playerbots_pvp_loadout). A ladder bot is a random bot at the maximum level in an arena team. The loadout is plain
// data: nothing is created, worn or bought here, and the PvE gear is never touched. All loadouts are loaded with one
// query at startup; bot logins read memory only and writes are asynchronous. World thread only (startup, the login
// hook, the GM commands).
class PvpGearMgr
{
public:
    static constexpr uint8 SlotCount = 19;  // EQUIPMENT_SLOT_END
    static constexpr uint8 NoTier = 0xFF;

    // A catalogue entry: the lowest-requirement vendor offer of the item
    struct CatalogueItem
    {
        uint32 Entry = 0;
        uint8 Tier = 0;
        uint8 InventoryType = 0;
        uint16 ItemLevel = 0;
        uint32 Honor = 0;
        uint32 ArenaPoints = 0;
        uint32 Rating = 0;  // personal and team rating in any arena team (the 2v2 counts)
        bool CcBreak = false;
    };

    // One vendor offer (an npc_vendor row with its ExtendedCost), the catalogue's input
    struct Offer
    {
        uint32 Entry = 0;
        std::string Name;
        uint16 ItemLevel = 0;
        uint8 RequiredLevel = 0;
        uint8 InventoryType = 0;
        uint32 Honor = 0;
        uint32 ArenaPoints = 0;
        uint32 ArenaSlot = 0;  // > 0: needs a 3v3 or 5v5 rating
        uint32 Rating = 0;
        bool TokenCost = false;  // asks for items (emblems, marks, tokens)
        bool CcBreak = false;
    };

    struct SkipCounts
    {
        uint32 TokenCost = 0;
        uint32 NoPvpCurrency = 0;
        uint32 ArenaSlot = 0;
        uint32 LowLevel = 0;
        uint32 NotEquippable = 0;
        uint32 NoTier = 0;
        uint32 Duplicate = 0;
    };

    struct LoadoutSlot
    {
        uint32 Item = 0;
        uint8 Tier = 0;
        uint32 Season = 0;
        uint32 BoughtAt = 0;  // Unix time; 0: given (season start, first login)
    };
    using Loadout = std::array<LoadoutSlot, SlotCount>;

    static PvpGearMgr& Instance()
    {
        static PvpGearMgr instance;
        return instance;
    }

    // Startup (world DB and DBC stores loaded, before any map runs): catalogue and every loadout row. Nothing when off
    void Load();
    bool IsLoaded() const { return _loaded; }
    // World thread, bot login: a ladder bot without a loadout gets one at the base tier (memory, async write)
    void OnBotLogin(Player* bot);
    // A random bot at the maximum level in an arena team
    static bool IsLadderBot(Player* bot);
    // A random bot at the maximum level (show and check build a loadout preview for one that is not a ladder bot)
    static bool IsMaxLevelRandomBot(Player* bot);
    // The stored loadout, or nullptr
    Loadout const* Find(uint32 guid) const;

    // Pure: the tier order option, one entry per tier, each a list of lower-case name prefixes ("hateful/savage")
    static std::vector<std::vector<std::string>> ParseTierOrder(std::string const& order);
    // Pure: the offers grouped into tiers (sorted by tier, then entry); left-out offers are counted in skipped
    static std::vector<CatalogueItem> BuildCatalogue(std::vector<Offer> const& offers,
                                                     std::vector<std::vector<std::string>> const& tiers,
                                                     SkipCounts& skipped);

    static bool HandleStatusCommand(ChatHandler* handler, char const* args);
    static bool HandleResetCommand(ChatHandler* handler, char const* args);
    static bool HandleShowCommand(ChatHandler* handler, char const* args);
    // CanEquipNewItem on the loadouts of up to N online ladder bots (a check only; nothing is equipped); with "all"
    // also on a preview loadout (not stored) of the other random bots at the maximum level
    static bool HandleCheckCommand(ChatHandler* handler, char const* args);
    // Checks ParseTierOrder and BuildCatalogue on made-up offers (no game state)
    static bool HandleSelftestCommand(ChatHandler* handler, char const* args);

private:
    struct Pick
    {
        CatalogueItem const* Item = nullptr;
        float Score = 0.0f;
    };

    PvpGearMgr() = default;
    bool Usable(Player* bot, CatalogueItem const& item) const;
    // The best usable item of the groups in groupMask for the slot in the lowest tier from minTier up that has one
    // (ccBreak 1: only the PvP trinket, -1: no PvP trinket; maxRating: leaves out items that need a higher rating)
    Pick Best(Player* bot, StatsWeightCalculator& calc, uint32 groupMask, uint8 slot, uint8 minTier,
              std::vector<uint32> const& exclude, int8 ccBreak, uint32 maxRating = UINT32_MAX) const;
    // Best plus the final CanEquipNewItem check: a candidate it rejects for good is left out and the next one taken
    Pick BestEquippable(Player* bot, StatsWeightCalculator& calc, uint32 groupMask, uint8 slot, uint8 minTier,
                        std::vector<uint32> exclude, int8 ccBreak) const;
    Loadout BuildLoadout(Player* bot, uint8 tier, uint32 season, uint32& filled) const;
    void Store(uint32 guid, Loadout const& loadout);
    std::string TierLabel(uint8 tier) const;
    std::string NextUpgrade(Player* bot, uint32 guid, Loadout const& loadout, uint8 slot) const;
    CatalogueItem const* FindItem(uint32 entry) const;
    static uint32 MissingSlots(Loadout const& loadout);

    bool _loaded = false;
    std::vector<std::string> _tierNames;
    uint8 _baseTier = 0;
    uint32 _season = 1;
    std::vector<std::vector<CatalogueItem>> _tiers;
    std::unordered_map<uint32, CatalogueItem const*> _byEntry;  // into _tiers, which is not changed after Load
    std::unordered_map<uint32, Loadout> _loadouts;
};

#endif
