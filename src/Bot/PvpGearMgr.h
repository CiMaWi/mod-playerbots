/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_PVPGEARMGR_H
#define _PLAYERBOT_PVPGEARMGR_H

#include <array>
#include <deque>
#include <functional>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "AsyncCallbackProcessor.h"
#include "Define.h"
#include "Transaction.h"

class Battleground;
class ChatHandler;
class Item;
class Player;
class StatsWeightCalculator;

// PvP gear of the ladder bots (Playerbots.PvpGear.Enable): a catalogue of the PvP vendor items grouped into tiers,
// built at startup, and a stored loadout per ladder bot (one item entry per equipment slot, playerbots table
// playerbots_pvp_loadout). A ladder bot is a random bot at the maximum level in an arena team. The loadout is plain
// data; wearing it (Playerbots.PvpGear.Wear, PvpGearWear.cpp) creates its items in arenas and battlegrounds while the
// PvE items wait in the bags, and no PvE item is ever destroyed. All loadouts and worn slots are loaded with one query
// each at startup; bot logins read memory only and writes are asynchronous. World thread only (startup, the login
// hook, the GM commands, the swap, the buying), except the queue calls of the battleground hooks and the upkeep
// guards. Buying (Playerbots.PvpGear.Buy.Enable) changes loadout rows and the bot's honor and arena points only.
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
    // World thread, bot login: a ladder bot without a loadout gets one at the base tier (memory, async write); on an
    // arena server (WearsAlways) a hosted ladder bot puts it on, elsewhere worn rows are a crash and are repaired
    void OnBotLogin(Player* bot);
    // A random bot at the maximum level in an arena team
    static bool IsLadderBot(Player* bot);
    // A random bot at the maximum level (show and check build a loadout preview for one that is not a ladder bot)
    static bool IsMaxLevelRandomBot(Player* bot);
    // The stored loadout, or nullptr
    Loadout const* Find(uint32 guid) const;

    // Wearing (PvpGearWear.cpp). One playerbots_pvp_worn row per swapped slot
    struct WornSlot
    {
        uint32 PvpItem = 0;  // the created loadout item (0: the slot was only emptied, the off hand of a two-hander)
        uint32 PveItem = 0;  // the PvE item moved to the bags (0: the slot was empty)
    };
    using Worn = std::array<WornSlot, SlotCount>;

    enum class WearStep : uint8
    {
        Keep,         // empty loadout slot, or the PvE item is the loadout's entry
        AlreadyWorn,  // the slot has a row
        Equip,        // empty slot: the loadout item goes on
        Displace,     // the PvE item to the bags, the loadout item on
        BagsFull,     // the slot keeps its PvE item (logged)
        CannotEquip,  // the core refuses the loadout item: the slot keeps its PvE item
    };
    struct WearInput
    {
        uint32 LoadoutItem = 0;
        uint32 EquippedItem = 0;  // entry, 0: empty slot
        bool HasRow = false;
        bool CanEquip = false;
        bool BagRoom = false;  // for the equipped item
    };
    enum class UnwearStep : uint8
    {
        NothingToRestore,  // the slot was empty before the swap
        Lost,              // the PvE item is gone (logged; the factory refills the slot)
        AlreadyEquipped,   // a crash before the swap was saved
        Restore,           // from the bags back into the slot
        SlotTaken,         // the PvE item stays in the bags (logged)
    };
    struct UnwearInput
    {
        uint32 PveItem = 0;
        bool PveFound = false;
        bool PveEquipped = false;  // in its slot already
        bool SlotFree = false;     // after the PvP items were destroyed
    };
    // Pure: the step for one slot (no game state; the selftest models the cases)
    static WearStep DecideWear(WearInput const& in);
    static UnwearStep DecideUnwear(UnwearInput const& in);

    // Map threads (the battleground hooks): queue the swap as a world-thread operation; nothing when off or for a bot
    // that is not a ladder bot
    static void QueueWear(Player* bot);
    static void QueueUnwear(Player* bot);
    // Arena server (Playerbots.Ladder.Role 2 with PvP gear and the wearing on): the ladder bots wear the loadout from
    // login on, also between matches; a depart takes it off before the members log out
    bool WearsAlways() const;
    // World thread: the loadout's items on, the displaced PvE items to the bags, one row per swapped slot. Worn slots
    // whose loadout entry changed are swapped again (the old PvP item destroyed). Outside an arena or battleground only
    // on an arena server. false: nothing ran (not now, not a ladder bot, no loadout)
    bool Wear(Player* bot, char const* reason);
    // World thread: the bot's loadout rows changed (a reset, a bought upgrade): a bot that wears its loadout swaps the
    // changed slots now; true when it did
    bool OnLoadoutChanged(Player* bot);
    // World thread: the PvP items destroyed, the PvE items equipped again, the inventory saved and the rows deleted
    // after that commit (also the crash repair at login, which runs with PvP gear off as well). false: not now (dead or
    // teleporting); the caller hands the bot to RetryUnwear
    bool Unwear(Player* bot, char const* reason);
    void RetryUnwear(uint32 guid, uint32 attempts = 0);
    // World thread, every tick: the inventory saves of the unwears, the deferred unwears every 5 s; returns at once
    // when there are none
    void Update(uint32 diff);
    // Any thread (the gear upkeep guards on map threads): the bot wears its loadout; the item is a worn PvP item or a
    // displaced PvE item of the bot. Also with PvP gear off: the rows are loaded at every start
    bool IsWorn(Player const* bot) const;
    bool IsWornItem(Player const* bot, Item const* item) const;
    static void SelftestWear(std::function<void(bool, std::string const&)> const& check);
    // Any thread holding no worn lock: the guid has worn rows on this server (also while the bot is offline)
    bool HasWornRows(uint32 guid) const;

    // Buying (Playerbots.PvpGear.Buy.*): at the vendor prices (honor, arena points) and rating requirements, after a
    // rated arena match and on a slow timer, a few bots per world tick
    static constexpr uint8 NoSlot = 0xFF;
    struct BuyCandidate
    {
        uint32 Entry = 0;
        uint8 Tier = 0;
        uint16 ItemLevel = 0;
        uint32 Honor = 0;
        uint32 ArenaPoints = 0;
        uint32 Rating = 0;
        float Score = 0.0f;  // StatsWeightCalculator, PvP mode
    };
    struct BuySlot
    {
        uint8 Slot = 0;
        uint32 Current = 0;  // the loadout's entry (0: empty, any candidate is an upgrade)
        uint8 CurrentTier = 0;
        uint16 CurrentLevel = 0;
        uint8 Pair = NoSlot;  // the other ring (and the main hand for the off hand): never the same entry
        std::vector<BuyCandidate> Candidates;
    };
    struct Wallet
    {
        uint32 Honor = 0;
        uint32 ArenaPoints = 0;
    };
    struct Purchase
    {
        uint8 Slot = 0;
        uint32 Old = 0;
        uint32 Entry = 0;
        uint8 Tier = 0;
        uint32 Honor = 0;
        uint32 ArenaPoints = 0;
    };
    // Pure: the purchases of one bot. slots in priority order; an upgrade is a higher (tier, item level) than the
    // slot's item and needs rating >= its requirement; per slot the target is the best upgrade the rating allows
    // (tier, then score). saveTowardBetter: a target the wallet cannot pay yet is not replaced by a lesser upgrade, and
    // the later slots spend none of the currencies it needs; off: per slot the best upgrade the wallet can pay. The
    // wallet is reduced by the purchases
    static std::vector<Purchase> DecideBuys(std::vector<BuySlot> const& slots, Wallet& wallet, uint32 rating,
                                            bool saveTowardBetter);
    // Pure: the slot priority option (weapon, ranged, head, neck, shoulders, chest, waist, legs, feet, wrists, hands,
    // back, rings, trinkets or a slot name such as "ring 1"); the slots it leaves out follow in the default order.
    // Unknown names are listed in unknown
    static std::vector<uint8> ParseSlotPriority(std::string const& text, std::string& unknown);
    // Map thread (OnBattlegroundEnd): the players of a rated arena match get a buy check, queued to the world thread
    static void QueueMatchEnd(Battleground* bg);
    // World thread: the bots are checked before the timer's sweep
    void AddBuyChecks(std::vector<uint32> const& guids, bool matchEnd);
    static void SelftestBuy(std::function<void(bool, std::string const&)> const& check);

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
    // Tier and item level by rating band, purchases, currency and income of the online ladder bots
    static bool HandleStatsCommand(ChatHandler* handler, char const* args);
    // Tests: adds honor and arena points to one online ladder bot or to all
    static bool HandleGrantCommand(ChatHandler* handler, char const* args);

private:
    struct Pick
    {
        CatalogueItem const* Item = nullptr;
        float Score = 0.0f;
    };
    struct PurchaseRecord
    {
        std::string Bot;
        uint8 Slot = 0;
        uint32 Old = 0;
        uint32 Entry = 0;
        uint8 Tier = 0;
        uint32 Honor = 0;
        uint32 ArenaPoints = 0;
        uint32 Rating = 0;
        uint32 At = 0;
    };

    PvpGearMgr() = default;
    void LoadWorn();
    void EnsureLoadout(Player* bot);
    // Wear's first pass: worn slots whose PvP item is gone or no longer the loadout's entry get their PvE item back
    uint32 RefreshWorn(Player* bot, Worn& worn, Loadout const& loadout) const;
    bool Usable(Player* bot, CatalogueItem const& item) const;
    // The best usable item of the groups in groupMask for the slot in the lowest tier from minTier up that has one
    // (ccBreak 1: only the PvP trinket, -1: no PvP trinket; maxRating: leaves out items that need a higher rating)
    Pick Best(Player* bot, StatsWeightCalculator& calc, uint32 groupMask, uint8 slot, uint8 minTier,
              std::vector<uint32> const& exclude, int8 ccBreak, uint32 maxRating = UINT32_MAX) const;
    // Best plus the final CanEquipNewItem check: a candidate it rejects for good is left out and the next one taken.
    // maxRating: the bot's rating; a slot nothing fits at that rating takes the pick without the limit (logged)
    Pick BestEquippable(Player* bot, StatsWeightCalculator& calc, uint32 groupMask, uint8 slot, uint8 minTier,
                        std::vector<uint32> exclude, int8 ccBreak, uint32 maxRating) const;
    Loadout BuildLoadout(Player* bot, uint8 tier, uint32 season, uint32& filled) const;
    void Store(uint32 guid, Loadout const& loadout);
    std::string TierLabel(uint8 tier) const;
    std::string NextUpgrade(Player* bot, uint32 guid, Loadout const& loadout, uint8 slot) const;
    CatalogueItem const* NextItem(Player* bot, uint32 guid, Loadout const& loadout, uint8 slot) const;
    void UpdateBuys(uint32 diff);
    std::vector<BuySlot> BuildBuySlots(Player* bot, Loadout const& loadout, uint32 rating) const;
    uint32 CheckBuys(uint32 guid);
    // The currency seen at the last check; a rise since then is income (grants and purchases move the mark)
    void NoteWallet(uint32 guid, Wallet const& now, bool countIncome);
    CatalogueItem const* FindItem(uint32 entry) const;
    static uint32 MissingSlots(Loadout const& loadout);

    bool _loaded = false;
    bool _wornLoaded = false;  // the worn rows, loaded at every start (also with Enable 0 or no catalogue)
    std::vector<std::string> _tierNames;
    uint8 _baseTier = 0;        // the login builds; a reset with a tier word changes it until the restart
    uint8 _configBaseTier = 0;  // Playerbots.PvpGear.BaseTier: the default of a reset
    uint32 _season = 1;
    std::vector<std::vector<CatalogueItem>> _tiers;
    std::unordered_map<uint32, CatalogueItem const*> _byEntry;  // into _tiers, which is not changed after Load
    std::unordered_map<uint32, Loadout> _loadouts;
    mutable std::shared_mutex _wornLock;  // the guards read on map threads, the world thread writes
    std::unordered_map<uint32, Worn> _worn;
    std::vector<std::pair<uint32, uint32>> _retry;  // guid, attempts (world thread)
    uint32 _retryTimer = 0;
    AsyncCallbackProcessor<TransactionCallback> _saveCallbacks;  // the unwears' inventory saves (world thread)
    // Buying (world thread)
    std::vector<uint8> _buyOrder;
    std::vector<uint32> _buyPending;  // the bots of rated matches that ended
    std::vector<uint32> _buySweep;    // the timer's pass over the online ladder bots
    uint32 _buyTimer = 0;
    uint32 _buyStart = 0;  // Unix time of the load
    uint64 _purchases = 0;
    uint64 _queuedChecks = 0;    // Checks queued by grants and rated match ends
    uint64 _ratedMatchEnds = 0;  // Rated arena matches that queued their players
    uint64 _matchEndPlayers = 0;
    uint64 _timerChecks = 0;
    uint64 _spentHonor = 0;
    uint64 _spentArenaPoints = 0;
    uint64 _incomeHonor = 0;
    uint64 _incomeArenaPoints = 0;
    std::unordered_map<uint32, Wallet> _wallets;
    std::deque<PurchaseRecord> _recent;
};

#endif
