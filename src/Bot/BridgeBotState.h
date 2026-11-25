/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_BRIDGEBOTSTATE_H
#define PLAYERBOTS_BRIDGEBOTSTATE_H

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "BridgeAround.h"

class ObjectGuid;
class Player;

// DS.14/J5f: raw facts about a bot for one conversation-bridge request. Read on the world thread while map threads
// are idle (the chat hook, a wake submit, the post-map-join update), from stored values and Player fields; only
// `around` scans the world, through the bot's own values. The bridge module filters and bounds them before anything
// reaches a model.
namespace PlayerbotBridge
{
struct NameCandidate;  // BridgeNames.h

struct BotStateFacts
{
    float healthPct = 0.0f;
    uint8_t powerType = 0xFF;  // Powers value of the bot's power bar.
    uint32_t powerCurrent = 0;
    uint32_t powerMax = 0;
    uint32_t level = 0;
    bool alive = false;
    bool ghost = false;
    bool inCombat = false;  // In combat, or the combat engine is active.
    bool victim = false;    // The bot's own current victim; never anybody else's target.
    bool victimVisible = false;
    std::string victimName;
    uint32_t victimLevel = 0;
    float victimHealthPct = 0.0f;
    std::string victimType;  // The victim creature's type word; empty for a player or a type with no word
    bool flying = false;     // On a flight path.
    bool casting = false;
    bool looting = false;     // A loot target is set.
    bool resting = false;     // Sitting with a food or drink aura.
    bool travelling = false;  // Travel target on its way.
    bool working = false;     // Travel target at its destination.
    std::string questTitle;   // The travel destination's quest, when it has one.
    std::string zone;
    std::string area;
    uint32_t mapId = 0;
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    bool partnerOnline = false;
    bool partnerInGroup = false;
    bool partnerVisible = false;
    bool partnerSameMap = false;  // Same map, instance and phase; only then are yards and position set.
    float partnerYards = 0.0f;
    std::string partnerZone;
    float partnerX = 0.0f;
    float partnerY = 0.0f;
    float partnerZ = 0.0f;
    bool mounted = false;
    bool swimming = false;
    uint32_t groupSize = 0;  // 0 when ungrouped.
    bool leads = false;
    std::string masterName;
    bool follow = false;  // Non-combat movement strategies.
    bool stay = false;
    bool guard = false;
    bool grind = false;
    bool passive = false;
    // LCB7b (LC.36): the lasting settings in force, each only while it differs from playerbots' default: a formation
    // other than chaos, a mana save level above 1, any focus heal target.
    bool formation = false;
    bool saveMana = false;
    bool focusHeal = false;
    uint32_t freeBagSlots = 0;
    uint32_t money = 0;
    // LC8: what the bot perceives around it (SelectAround over its nearest-unit and nearest-object values; the only
    // facts here that visit the grid), the servant or pet it has out and what it rides, by name (empty: none).
    std::vector<AroundEntry> around;
    std::string servantName;
    std::string mountName;
    // LC7: the bot's recent deeds from its record, newest first, one in-world sentence each (RecordWords.h Deeds)
    std::vector<std::string> deeds;
    // LCB4: further state keys, one row each (StateKey); the bridge module forwards them with no edit. None yet.
    std::vector<StateKey> open;
};

// False (facts reset) off the world thread, or for a guid that is not an online bot in the world.
bool ReadBotState(ObjectGuid const& botGuid, ObjectGuid const& partnerGuid, BotStateFacts& facts);

// J5f-b: the topics a conversation partner can ask about, read on demand (one bridge query, same thread rule). The
// order is the wire index: append only.
enum class BotStateTopic : uint8_t
{
    Inventory,
    Quests,
    Cooldowns,
    Strategies,
    Spells,
    Plan,  // WT18a: the planner's topics (BridgeBotTopics.h formats and bounds them)
    Options,
    Route,
    QuestPlan,
    Bags,
    Places,
    History,  // LC7: the record's readers (RecordWords.h builds the entries)
    Lately,
    Known,
    Needs,  // WU23: what the bot must pay for (BridgeNeedsTopic.h words the upkeep rules' list)
    Items,  // WI10b: its usable items by kind, what is on cooldown, its last uses, what it lacks (BridgeItemsTopic.h)
    Where,  // LC18a: where a service, a person or a place is, if the bot can know it (BridgeKnowledgeRules.h)
    Who,    // LC18b: who a person is, what an item or a quest is, what a land is like, if the bot can know it
    What,   // (BridgeLookupRules.h builds the lines; the one parameter of each is a name)
    Zone,
    Mounts,  // LC18c: the bot's riding and mounts; the crafts it has and what they make (BridgeOwnRules.h)
    Recipes,
    Professions,  // WR18: the professions the bot has, how far it is, what it works towards and lacks
                  // (BridgeProfessionsTopic.h)
    Auction       // WA9: what the bot has listed and at what price, what it looks for, its open bids
                  // (BridgeAuctionTopic.h)
};

// One raw entry of a topic; the bridge module filters, cuts and names the fields. Times are seconds.
//   Inventory:  name, value = quality, extra = stack count, code 1 = equipped (0 = in a bag); equipped items first,
//               then bag items by quality, best first
//   Quests:     name = title, value = objectives done, extra = needed, code 0 active, 1 complete, 2 failed
//   Cooldowns:  name = spell name, value = seconds left (rounded up), longest first
//   Strategies: name, code 1 = combat (0 = non-combat)
//   Spells:     name, value = rank (0 = none); the known rank of each active class spell (not passive, shown in the
//               spellbook, no higher rank known), highest spell level first
//   Plan (8):   code 0 goal: name = kind word (none without a goal), value = seconds in, extra = seconds planned;
//               code 1 source: planner, bridge or group; code 2 reason: words (without a goal: not_planner,
//               paused, planning, idle or the last fallback word); code 3 a next circuit stop: name = its first open
//               quest's title, value = open tasks there, extra = yards
//   Options (5): name = kind, reason and trait words, value = value per minute, extra = seconds, code 1 = the goal
//               pursued now; empty out of the planner's scope or before a plan
//   Route (12): to the argument (a place name or "x,y,z,map"): code 0 total: name = place word, value = seconds,
//               extra = fare copper; code 1 legs in order: name = walk, portal, flight, transport or hearth, value =
//               seconds, extra = fare; code 2 failed: name = unknown_place, graph_not_ready, no_route, ...
//   QuestPlan (12): one row per circuit quest in walking order: name = title, value = quest id, code 0 take at a
//               giver, 1 task (extra = task points), 2 turn in, 3 carried to another hub, 4 deferred
//   Bags (12):  code 0 free: value = free slots, extra = all slots; code 1 the next sell stop and code 2 a bag
//               purchase from the plan: name = reason word, value = seconds, extra = vendor or item; code 3 mail
//               waiting: value = unopened mails; code 4 mail seen: value = copper worth left there; code 5 mail
//               expected: value = copper; code 6 kept item (a bot the bank rule governs, due ones first, at most four):
//               name = item name, value = the level it is due at, extra = 1 in the bank, 0 in the bags; code 7 bank
//               free: value = free bank slots, extra = all of them
//   Places (24): code 0 hearth: name = bind area, value = cooldown seconds left, extra 1 = carries a hearthstone;
//               known flight points, value = taxi node: code 1 on the bot's map, nearest first, extra = yards;
//               code 2 elsewhere by node, extra = map id
//   History (24): text = one sentence; code 0 a permanent row in time order: name = kind word, value = days ago;
//               then what the bot knows of the world, name = its name, value = events behind it: code 1 a zone,
//               code 2 a dungeon, code 3 a boss
//   Lately (12): the recent deeds, newest first: name = kind word, text = sentence, value = seconds ago
//   Known (12):  the bots of shared events: name = the bot's name, text = its people, calling and what was shared,
//               value = days since the last, extra = events shared
//   Needs (8):   text = one sentence; code 0 provided (a bot the upkeep rules do not govern: the only entry), 1 nothing
//               to pay for, 2 pending (the upkeep index is not ready); code 3 a need due, 4 a need paid when passing:
//               name = kind word, value = its price in copper, extra = the copper the bot's gold pays now; code 5 the
//               reserve: value = copper kept back, extra = copper held; code 6 saving, only with a shortfall: value =
//               the shortfall in copper. The text names items and spells and gives the money as gold, silver, copper.
//   Items (12):  text = one sentence; code 0 carried by kind (at most five): name = kind word, value = distinct items,
//               extra = units; code 1 on cooldown (at most two): name = item, value = seconds left; code 2 last uses,
//               newest first (at most three): name = item, value = seconds ago; code 3 lacks for its role (at most
//               two): name = demand slot word, value = units short; code 4 provided (a bot that keeps no stock of
//               its own: the only lacks entry). Item names come without digits.
//   Professions (8): text = one sentence; code 0 provided (a bot the rules do not govern: the only entry), 1 none (no
//               profession), 2 a profession: name = its name, text = rank word and what it works towards; code 3
//               lacks, only when the chosen stretch lacks something: text = the kinds. No number, no copper.
//   Auction (8): text = one sentence; code 0 provided (a bot the rules do not govern: the only entry), 1 none (no
//               listing, no want, no bid); code 2 listed (at most four): name = item, value = count, extra = its price
//               in copper (the buyout, else the start bid); code 3 wants (at most three): name = item, value = units,
//               extra = the most it pays per unit in copper; code 4 bids, only when one is open: value = open, extra
//               = the most it holds. Its own prices only, never a house's current offer; money in the text is worded.
// No topic carries a trait value or the hidden PvP skill: traits appear only as words. No text holds a number.
struct BotStateItem
{
    std::string name;
    uint32_t value = 0;
    uint32_t extra = 0;
    uint8_t code = 0;
    std::string text;       // The record topics' sentence; empty for the others
    std::string word;       // The row's word of [a-z_] beside its code (BridgeTexts::Lookups::ROW_*); empty for the
                            // topics that keep a code only (inventory, quests, cooldowns, strategies, spells)
    std::string side = {};  // LC18g: a place row's side word (BridgeTexts::Lookups::PLACE_SIDE_WORDS); empty for none
};

constexpr std::size_t BotStateTopicMaxItems = 64;  // The reader never returns more.

// False (items cleared) off the world thread, for an unknown topic or a guid that is not an online bot in the world.
// Only the bot's own items, quest log, cooldowns, strategies, spellbook, published plan, taxi mask, bind point and
// record; nothing here scans the world. Route runs one route search over the travel graph (a fraction of a
// millisecond).
bool ReadBotStateTopic(ObjectGuid const& botGuid, BotStateTopic topic, std::vector<BotStateItem>& items);
// The same with the topic's argument: the text of its row's one parameter (Route's place), which the topic's reader
// looks up; a topic without one ignores it.
bool ReadBotStateTopic(ObjectGuid const& botGuid, BotStateTopic topic, std::string const& argument,
                       std::vector<BotStateItem>& items);

// LC2: what an order's or a topic's item or spell name may mean, for ResolveName, when its row takes one. World
// thread, an online bot: the items it wears and carries as the inventory topic lists them (Id the item entry), and
// the spells it can cast as the spells topic lists them (Id the spell id, the highest known rank).
std::vector<NameCandidate> CarriedItemNames(Player* bot);
std::vector<NameCandidate> KnownSpellNames(Player* bot);

// LC18a: a town the bot knows (the where topic's rule), named by text, as the literal "x,y,z,map" the planner takes;
// nothing for an unknown or unmatched name, or while the planner's indexes are not built. World thread, an online bot.
std::optional<std::string> KnownTownSpot(Player* bot, std::string const& text);
}  // namespace PlayerbotBridge

#endif
