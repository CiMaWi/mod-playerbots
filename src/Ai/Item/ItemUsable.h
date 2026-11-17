/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_ITEMUSABLE_H
#define _PLAYERBOT_ITEMUSABLE_H

#include <array>
#include <cstdint>
#include <span>
#include <vector>

#include "ItemOfferRules.h"
#include "ItemSituation.h"
#include "ItemUsableRules.h"

class Player;
class PlayerbotAI;
class Unit;

// The bot's usable list and its situation (item use plan WI15a, INTERFACE 2.3): what the bot wears and carries that
// has a use effect, rebuilt when its bags, its gear or an enchantment change (PlayerScript hooks: a dirty mark, never
// per tick), and the small snapshot the fight's triggers take. Map thread of the bot only.
namespace ItemUse
{
// The per-bot cache (PlayerbotAI::botUsable, lazily); not saved
struct BotUsable
{
    std::vector<UsableEntry> entries;  // every use effect, worn pieces first (slot order), then the bags
    std::vector<UsableEntry>
        fight;               // the fight offer's list (FightListed: the free ones, the consumed of WI16's kinds)
    KindMask kinds{0};       // MaskOf(entries)
    KindMask fightKinds{0};  // MaskOf(fight)
    bool dirty{true};
    float thrift{0.0f};  // the bot's traits, read at the rebuild (BotTraitMgr's cache)
    float daring{0.0f};
    std::uint32_t earnCopperPerMinute{0};  // the level band's earning rate, read at the rebuild (a level change marks)
    HealthRing health;
    bool lastInFight{false};
    std::uint32_t sampledMs{0};
    Situation sample;
    Offer offer;  // the fight's offer over the fight list, made again after OFFER_MS, a use or a rebuild
    std::uint32_t offerMs{0};
    struct Blocked
    {
        std::uint32_t record{0};
        std::uint32_t entry{0};
        std::uint32_t untilMs{0};
        Cause cause{Cause::None};
    };
    std::vector<Blocked> blocked;  // offered entries Check refused lately: the offer skips them for BLOCK_MS
    // WI16: the item_spend rows: the fight (counted when one begins) and per need the verdict last written in it
    std::uint32_t fightSerial{0};
    std::uint32_t spendFight{0};
    std::array<std::uint8_t, NEED_COUNT> spendNoted{};  // 0: none yet, else spend bit 0x80 | reason + 1
};

// The list, rebuilt first when it is dirty; empty for a player without an AI
std::span<UsableEntry const> UsableOf(Player* bot);
// The fight offer's part of the list (FightListed) and its kinds, the triggers' O(1) gate
std::span<UsableEntry const> FightUsableOf(Player* bot);
KindMask UsableKindsOf(Player* bot);
KindMask FightKindsOf(PlayerbotAI* botAI);
// WI18: the fight list holds a repair gadget (the out-of-combat repair trigger's gate; a scan of the short list)
bool FightHoldsRepair(PlayerbotAI* botAI);
// The next UsableOf rebuilds and the next FightOffer is made again (after a use that changes a count or a cooldown)
void MarkUsableDirty(PlayerbotAI* botAI);
// GenericBoostTrigger's rule, its one definition (the trigger and the sampler call it): against a player the
// Defensive tactic's hold decides (holds: the hold is on), else the fight's balance at or below the bar
bool BurstsNow(PlayerbotAI* botAI, Unit* target, float balance, bool* holds = nullptr);
// The fight's offer (MakeOffer over the fight list and the sampled situation), shared by the triggers and the action
// of the "item fight" strategy; made at most once per OFFER_MS (ItemUsable.cpp), at once after a use or a rebuild.
// A consumed candidate's verdict is an item_spend decision row, once per fight and need, again when it changes
Offer const& FightOffer(PlayerbotAI* botAI);
// The fight list's entry the offer names (free or consumed), nullptr when it names none
UsableEntry const* OfferedEntry(PlayerbotAI* botAI, Offer const& offer);
// The fight's request for the offer's entry: a worn piece by its slot, a carried one by its entry; the offer's target
Request FightRequest(Player* bot, Offer const& offer, UsableEntry const& entry);
// Check refused the offered entry: the offer skips it for a while (a refusal that holds, as out of sight, must not
// keep the entries of the later needs from their turn) and is made again now
void BlockOffered(PlayerbotAI* botAI, UsableEntry const& entry, Cause cause);
// The situation now, from values the AI has; the ready flags of the entries whose kind a wanted need reads are
// refreshed. Two calls within one AI tick share the sample
Situation SampleSituation(PlayerbotAI* botAI);

// playerbots item usable / item spend: the bot's map thread, from its AI update while BotWork::ItemUsable is set:
// prints the list, the situation and the offer (`ITEM usable:`), then each weighed consumed candidate with its spend
// case and verdict (`ITEM spend:`) to the server log
void TakeUsablePrints(PlayerbotAI* botAI, Player* bot);
// The trinket spells the old trigger kept out: one spell both ON_EQUIP and ON_USE across trinket templates
bool MixedTriggerSpell(std::uint32_t spellId);
}  // namespace ItemUse

void AddItemUsableScripts();

#endif
