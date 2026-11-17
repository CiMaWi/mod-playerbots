/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_ITEMUSABLE_H
#define _PLAYERBOT_ITEMUSABLE_H

#include <cstdint>
#include <span>
#include <vector>

#include "ItemSituation.h"
#include "ItemUsableRules.h"

class Player;
class PlayerbotAI;

// The bot's usable list and its situation (item use plan WI15a, INTERFACE 2.3): what the bot wears and carries that
// has a use effect, rebuilt when its bags, its gear or an enchantment change (PlayerScript hooks: a dirty mark, never
// per tick), and the small snapshot the fight's triggers take. Map thread of the bot only.
namespace ItemUse
{
// The per-bot cache (PlayerbotAI::botUsable, lazily); not saved
struct BotUsable
{
    std::vector<UsableEntry> entries;  // every use effect, worn pieces first (slot order), then the bags
    std::vector<UsableEntry> free;     // the entries with consumed false: the fight offer's list in WI15
    KindMask kinds{0};                 // MaskOf(entries)
    KindMask freeKinds{0};             // MaskOf(free)
    bool dirty{true};
    float thrift{0.0f};  // the bot's traits, read at the rebuild (BotTraitMgr's cache)
    float daring{0.0f};
    HealthRing health;
    bool lastInFight{false};
    std::uint32_t sampledMs{0};
    Situation sample;
};

// The list, rebuilt first when it is dirty; empty for a player without an AI
std::span<UsableEntry const> UsableOf(Player* bot);
// The free part of the list (consumed false) and its kinds, for the fight offer (WI15b)
std::span<UsableEntry const> FreeUsableOf(Player* bot);
KindMask UsableKindsOf(Player* bot);
// The next UsableOf rebuilds (after a use that changes a count)
void MarkUsableDirty(PlayerbotAI* botAI);
// The situation now, from values the AI has; the ready flags of the entries whose kind a wanted need reads are
// refreshed. Two calls within one AI tick share the sample
Situation SampleSituation(PlayerbotAI* botAI);

// playerbots item usable: the bot's map thread, from its AI update while BotWork::ItemUsable is set: prints the list,
// the situation and the offer to the server log (`ITEM usable:`)
void TakeUsablePrints(PlayerbotAI* botAI, Player* bot);
// The trinket spells the old trigger kept out: one spell both ON_EQUIP and ON_USE across trinket templates
bool MixedTriggerSpell(std::uint32_t spellId);
}  // namespace ItemUse

void AddItemUsableScripts();

#endif
