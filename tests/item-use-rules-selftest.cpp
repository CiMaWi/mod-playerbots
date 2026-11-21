/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// Offline proof of the use path's pure rules (src/Ai/Item/ItemUseRules.h, item-use plan WI3): a hand case per cause
// of CheapCheck in its order, a line per engine result with a cause of its own, Advance per event, and a search over
// seeded random event orders (requests, refusals, timed uses, a stun, a death, a move, a logout) that counts the
// lifecycle invariants L1 to L6 of INTERFACE.md 1.8. WI4 adds spots, objects, worn pieces (a tinker read from the
// committed dump), the one potion per fight, the bound of a timed use and the event its signs give (EventOf); the
// search then also asks spot, object and worn requests and derives events from random signs. Build (Git Bash, module
// root): clang-cl /std:c++20 /EHsc /W4 /WX /I src/Ai/Item tests/item-use-rules-selftest.cpp; run: <exe> <module root>

#include <array>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ItemUseFacts.h"
#include "ItemUseRules.h"

using namespace ItemUse;

namespace
{
int passed = 0;
int failed = 0;

void Check(bool condition, std::string const& what)
{
    if (condition)
    {
        ++passed;
        std::cout << "ok: " << what << "\n";
        return;
    }

    ++failed;
    std::cout << "FAIL: " << what << "\n";
}

// A potion on the bot: heal effect, self, consumed, shared category
Record Potion()
{
    Record record;
    record.itemEntry = 118;
    record.spellId = 439;
    record.itemClass = RULE_CLASS_CONSUMABLE;
    record.itemSubClass = 1;
    record.shape = Shape::Self;
    record.potion = true;
    record.charges = -1;
    record.consumed = true;
    record.needs.level = 1;
    record.effects[0].effect = RULE_EFFECT_HEAL;
    record.effects[0].targetA = 1;
    record.effectCount = 1;
    return record;
}

// A bandage on a friend: a channel, 15 yd
Record Bandage()
{
    Record record;
    record.itemEntry = 1251;
    record.spellId = 746;
    record.itemClass = RULE_CLASS_CONSUMABLE;
    record.shape = Shape::Friend;
    record.rangeYards = 15.0f;
    record.rangeFriendYards = 15.0f;
    record.channeled = true;
    record.durationMs = 6000;
    record.needs.skill = 129;
    record.needs.skillRank = 1;
    record.effects[0].effect = 6;
    record.effects[0].aura = 8;
    record.effectCount = 1;
    return record;
}

// A stone on the bot's own blade
Record Stone()
{
    Record record;
    record.itemEntry = 2862;
    record.spellId = 2828;
    record.itemClass = RULE_CLASS_CONSUMABLE;
    record.shape = Shape::OwnItem;
    record.targetItemClass = 2;
    record.targetItemSubClassMask = 33219;  // axe, 2h axe, polearm, sword, 2h sword, dagger
    record.targetItemMaxLevel = 165;
    record.castMs = 3000;
    record.exclude = Exclude::Coating;
    return record;
}

// A trinket against an enemy: worn only, 30 yd
Record Trinket()
{
    Record record;
    record.itemEntry = 41121;
    record.spellId = 55039;
    record.itemClass = 4;
    record.inventoryType = 12;
    record.shape = Shape::Enemy;
    record.rangeYards = 30.0f;
    record.rangeFriendYards = 30.0f;
    record.minRangeYards = 5.0f;
    return record;
}

// Ez-Thro Dynamite: a ground spot within 30 yd, a 1 s cast (the dump)
Record Dynamite()
{
    Record record;
    record.itemEntry = 6714;
    record.spellId = 8331;
    record.itemClass = RULE_CLASS_CONSUMABLE;
    record.shape = Shape::Ground;
    record.rangeYards = 30.0f;
    record.rangeFriendYards = 30.0f;
    record.radiusYards = 11.0f;
    record.castMs = 1000;
    record.charges = -1;
    record.consumed = true;
    return record;
}

// Kolkar Booty Key: a game object within 5 yd, a 5 s cast (the dump)
Record Key()
{
    Record record;
    record.itemEntry = 5020;
    record.spellId = 5107;
    record.shape = Shape::Object;
    record.rangeYards = 5.0f;
    record.rangeFriendYards = 5.0f;
    record.castMs = 5000;
    record.moveInterrupts = true;
    record.charges = -1;
    record.consumed = true;
    return record;
}

// Goblin Rocket Boots: worn only, on the bot, instant
Record Boots()
{
    Record record;
    record.itemEntry = 7189;
    record.spellId = 8892;
    record.itemClass = 4;
    record.inventoryType = 8;
    record.shape = Shape::Self;
    record.cooldownMs = 300000;
    return record;
}

BotView ReadyBot()
{
    BotView bot;
    bot.level = 20;
    bot.classBit = 1;
    bot.raceBit = 1;
    bot.skillValue = 75;
    bot.held = 2;
    bot.chargesLeft = -1;
    return bot;
}

Request Ask(Record const& record, TargetKind kind)
{
    Request request;
    request.item.entry = record.itemEntry;
    request.target.kind = kind;
    return request;
}

TargetView Self()
{
    TargetView target;
    target.kind = TargetKind::Self;
    target.found = true;
    target.isBot = true;
    target.friendly = true;
    return target;
}

TargetView Mate(float yards)
{
    TargetView target;
    target.kind = TargetKind::Unit;
    target.found = true;
    target.friendly = true;
    target.yards = yards;
    return target;
}

TargetView Foe(float yards)
{
    TargetView target;
    target.kind = TargetKind::Unit;
    target.found = true;
    target.hostile = true;
    target.yards = yards;
    return target;
}

TargetView Blade(std::uint32_t subClass)
{
    TargetView target;
    target.kind = TargetKind::OwnItem;
    target.found = true;
    target.itemClass = 2;
    target.itemSubClass = subClass;
    target.itemInvType = 13;
    target.itemLevel = 2;
    return target;
}

TargetView Spot(float yards, bool inSight = true)
{
    TargetView target;
    target.kind = TargetKind::Spot;
    target.found = true;
    target.yards = yards;
    target.inSight = inSight;
    return target;
}

TargetView Chest(float yards, bool inSight = true)
{
    TargetView target;
    target.kind = TargetKind::Object;
    target.found = true;
    target.yards = yards;
    target.inSight = inSight;
    return target;
}

void Expect(Cause got, Cause want, std::string const& what)
{
    Check(got == want, what + " -> " + std::string(CauseWord(got)) + " (want " + std::string(CauseWord(want)) + ")");
}

void HandCases()
{
    Record const potion = Potion();
    Request const selfAsk = Ask(potion, TargetKind::Self);
    TargetView hurt = Self();
    BotView const ready = ReadyBot();
    Expect(CheapCheck(potion, selfAsk, ready, hurt), Cause::None, "a potion at low health is used");

    // 1..7 holds and may use
    Record none = potion;
    none.spellId = 0;
    Expect(CheapCheck(none, selfAsk, ready, hurt), Cause::NoEffect, "1 no use effect");
    Request other = selfAsk;
    other.spellId = 12345;
    Expect(CheapCheck(potion, other, ready, hurt), Cause::NoEffect, "1 not the spell named");
    Check(SelectRecord(std::span<Record const>(), 0) == nullptr, "1 no record: SelectRecord answers null");
    std::array<Record, 2> two{potion, Bandage()};
    Check(SelectRecord(two, 0) == &two[0], "SelectRecord: spell 0 is the first use effect");
    Check(SelectRecord(two, 746) == &two[1], "SelectRecord: the spell named");
    Check(SelectRecord(two, 1) == nullptr, "SelectRecord: a spell the item does not have");
    BotView bot = ready;
    bot.held = 0;
    Expect(CheapCheck(potion, selfAsk, bot, hurt), Cause::NoItem, "2 not held");
    Record const trinket = Trinket();
    Request const foeAsk = Ask(trinket, TargetKind::Unit);
    Expect(CheapCheck(trinket, foeAsk, ready, Foe(20.0f)), Cause::NotEquipped, "3 a trinket in the bags");
    BotView worn = ready;
    worn.equipped = true;
    Expect(CheapCheck(trinket, foeAsk, worn, Foe(20.0f)), Cause::None, "3 the worn trinket on a foe is used");
    Record high = potion;
    high.needs.level = 70;
    Expect(CheapCheck(high, selfAsk, ready, hurt), Cause::Level, "4 above the bot's level");
    Record const bandage = Bandage();
    Request const mateAsk = Ask(bandage, TargetKind::Unit);
    bot = ready;
    bot.skillValue = 0;
    Expect(CheapCheck(bandage, mateAsk, bot, Mate(5.0f)), Cause::Skill, "5 without the skill");
    Record ranked = bandage;
    ranked.needs.skillRank = 225;
    Expect(CheapCheck(ranked, mateAsk, ready, Mate(5.0f)), Cause::Skill, "5 below the skill's rank");
    bot = ready;
    bot.hasRequiredSpell = false;
    Expect(CheapCheck(potion, selfAsk, bot, hurt), Cause::Skill, "5 without the required spell");
    Record classed = potion;
    classed.needs.classMask = 8;
    Expect(CheapCheck(classed, selfAsk, ready, hurt), Cause::Class, "6 another class");
    Record raced = potion;
    raced.needs.raceMask = 2;
    Expect(CheapCheck(raced, selfAsk, ready, hurt), Cause::Class, "6 another race");
    bot = ready;
    bot.canUse = false;
    Expect(CheapCheck(potion, selfAsk, bot, hurt), Cause::CantUse, "7 CanUseItem refuses otherwise");
    bot.canUseCause = Cause::Dead;
    Expect(CheapCheck(potion, selfAsk, bot, hurt), Cause::Dead, "7 CanUseItem's refusal keeps its own cause");

    // 8..11 ready
    bot = ready;
    bot.cooldownLeftMs = 500;
    Expect(CheapCheck(potion, selfAsk, bot, hurt), Cause::NotReady, "8 the cooldown runs");
    bot = ready;
    bot.potionDrunk = true;
    Expect(CheapCheck(potion, selfAsk, bot, hurt), Cause::Potion, "9 the fight's potion is drunk");
    Record notPotion = potion;
    notPotion.potion = false;
    Expect(CheapCheck(notPotion, selfAsk, bot, hurt), Cause::None, "9 the potion mark holds back only a potion");
    bot = ready;
    bot.chargesLeft = 0;
    Expect(CheapCheck(potion, selfAsk, bot, hurt), Cause::NoCharges, "10 no charge left");
    Record endless = potion;
    endless.charges = 0;
    Expect(CheapCheck(endless, selfAsk, bot, hurt), Cause::None, "10 an item without charges needs none");
    bot = ready;
    bot.busy = true;
    Expect(CheapCheck(potion, selfAsk, bot, hurt), Cause::Busy, "11 the bot casts");

    // 12..18 state
    bot = ready;
    bot.alive = false;
    Expect(CheapCheck(potion, selfAsk, bot, hurt), Cause::Dead, "12 the bot is dead");
    bot = ready;
    bot.lostControl = LostControl::Stunned;
    Expect(CheapCheck(potion, selfAsk, bot, hurt), Cause::NoControl, "13 stunned");
    bot.lostControl = LostControl::Fleeing;
    Expect(CheapCheck(potion, selfAsk, bot, hurt), Cause::NoControl, "13 feared");
    bot.lostControl = LostControl::Confused;
    Expect(CheapCheck(potion, selfAsk, bot, hurt), Cause::NoControl, "13 confused");
    bot.lostControl = LostControl::Other;
    Expect(CheapCheck(potion, selfAsk, bot, hurt), Cause::NoControl, "13 jumping or charging");
    // a spell allowed while controlled (the PvP trinket's attributes): each state needs its own allowance
    Record breaker = potion;
    breaker.attributes[5] =
        RULE_ATTR5_ALLOW_WHILE_STUNNED | RULE_ATTR5_ALLOW_WHILE_FLEEING | RULE_ATTR5_ALLOW_WHILE_CONFUSED;
    bot.lostControl = LostControl::Stunned;
    Expect(CheapCheck(breaker, selfAsk, bot, hurt), Cause::None, "13 allowed while stunned");
    bot.lostControl = LostControl::Fleeing;
    Expect(CheapCheck(breaker, selfAsk, bot, hurt), Cause::None, "13 allowed while feared");
    bot.lostControl = LostControl::Confused;
    Expect(CheapCheck(breaker, selfAsk, bot, hurt), Cause::None, "13 allowed while confused");
    bot.lostControl = LostControl::Stunned | LostControl::Fleeing | LostControl::Confused;
    Expect(CheapCheck(breaker, selfAsk, bot, hurt), Cause::None, "13 allowed under all three");
    bot.lostControl = LostControl::Stunned | LostControl::Other;
    Expect(CheapCheck(breaker, selfAsk, bot, hurt), Cause::NoControl, "13 allowed while stunned, not while charging");
    Record stunOnly = potion;
    stunOnly.attributes[5] = RULE_ATTR5_ALLOW_WHILE_STUNNED;
    bot.lostControl = LostControl::Stunned;
    Expect(CheapCheck(stunOnly, selfAsk, bot, hurt), Cause::None, "13 stun allowance, stunned");
    bot.lostControl = LostControl::Fleeing;
    Expect(CheapCheck(stunOnly, selfAsk, bot, hurt), Cause::NoControl, "13 stun allowance, feared");
    bot.lostControl = LostControl::Stunned | LostControl::Confused;
    Expect(CheapCheck(stunOnly, selfAsk, bot, hurt), Cause::NoControl, "13 stun allowance, stunned and confused");
    bot = ready;
    bot.inFlight = true;
    Expect(CheapCheck(potion, selfAsk, bot, hurt), Cause::Flight, "13b on a flight path");
    {
        BotView noGuard = bot;  // the mutant "no guard": the flight is not read
        noGuard.inFlight = false;
        Check(CheapCheck(potion, selfAsk, noGuard, hurt) != Cause::Flight, "mutant killed: no flight guard");
    }
    Record peace = potion;
    peace.needs.fight = FightNeed::Peace;
    bot = ready;
    bot.inFight = true;
    Expect(CheapCheck(peace, selfAsk, bot, hurt), Cause::InFight, "14 needs peace, in a fight");
    Record fight = potion;
    fight.needs.fight = FightNeed::Fight;
    Expect(CheapCheck(fight, selfAsk, ready, hurt), Cause::NotInFight, "15 needs a fight, at peace");
    Record arena = potion;
    arena.needs.notInArena = true;
    bot = ready;
    bot.inArena = true;
    Expect(CheapCheck(arena, selfAsk, bot, hurt), Cause::Arena, "16 refused in an arena");
    bot = ready;
    bot.placeFits = false;
    Expect(CheapCheck(potion, selfAsk, bot, hurt), Cause::Place, "17 the wrong map or area");
    Record outdoors = potion;
    outdoors.needs.outdoors = true;
    bot = ready;
    bot.outdoors = false;
    Expect(CheapCheck(outdoors, selfAsk, bot, hurt), Cause::Place, "17 needs the outdoors, indoors");
    Record indoors = potion;
    indoors.needs.indoors = true;
    Expect(CheapCheck(indoors, selfAsk, ready, hurt), Cause::Place, "17 needs a roof, outdoors");
    Record form = potion;
    form.needs.notShapeshifted = true;
    bot = ready;
    bot.shapeshifted = true;
    Expect(CheapCheck(form, selfAsk, bot, hurt), Cause::Form, "18 shapeshifted");

    // 19..21 target fit
    Expect(CheapCheck(bandage, Ask(bandage, TargetKind::None), ready, TargetView()), Cause::NoTarget,
           "19 a friend's bandage without a target");
    TargetView gone = Mate(5.0f);
    gone.found = false;
    Expect(CheapCheck(bandage, mateAsk, ready, gone), Cause::NoTarget, "19 the unit is gone");
    Record const stone = Stone();
    Expect(CheapCheck(stone, Ask(stone, TargetKind::Unit), ready, Mate(1.0f)), Cause::NoTarget, "19 a stone on a unit");
    Expect(CheapCheck(bandage, Ask(bandage, TargetKind::Self), ready, Self()), Cause::None,
           "a bandage on the bot itself");
    Expect(CheapCheck(bandage, mateAsk, ready, Foe(5.0f)), Cause::BadTarget, "20 a friend's item on an enemy");
    worn = ready;
    worn.equipped = true;
    Expect(CheapCheck(trinket, foeAsk, worn, Mate(20.0f)), Cause::BadTarget, "20 an enemy's item on a friend");
    Expect(CheapCheck(trinket, Ask(trinket, TargetKind::Self), worn, Self()), Cause::BadTarget,
           "20 an enemy's item on the bot");
    TargetView deadMate = Mate(5.0f);
    deadMate.alive = false;
    Expect(CheapCheck(bandage, mateAsk, ready, deadMate), Cause::BadTarget, "20 a dead friend for the living");
    Record corpse = bandage;
    corpse.life = Life::Dead;
    Expect(CheapCheck(corpse, mateAsk, ready, Mate(5.0f)), Cause::BadTarget, "20 a living friend for the dead");
    Expect(CheapCheck(corpse, mateAsk, ready, deadMate), Cause::None, "a dead friend for the dead");
    Request deadAsk = mateAsk;
    deadAsk.target.dead = true;
    Expect(CheapCheck(bandage, deadAsk, ready, deadMate), Cause::None, "the caller's dead word fits a dead unit");
    Request otherUnit = selfAsk;
    otherUnit.target.kind = TargetKind::Unit;
    Expect(CheapCheck(potion, otherUnit, ready, Mate(3.0f)), Cause::BadTarget, "20 a self item on another unit");
    Request const bladeAsk = Ask(stone, TargetKind::OwnItem);
    Expect(CheapCheck(stone, bladeAsk, ready, Blade(7)), Cause::None, "a stone on a sword");
    Expect(CheapCheck(stone, bladeAsk, ready, Blade(4)), Cause::BadTarget, "20 a stone on a mace");
    TargetView armour = Blade(7);
    armour.itemClass = 4;
    Expect(CheapCheck(stone, bladeAsk, ready, armour), Cause::BadTarget, "20 a stone on armour");
    TargetView coated = Blade(7);
    coated.itemHasCoating = true;
    Expect(CheapCheck(stone, bladeAsk, ready, coated), Cause::BadTarget, "20 a coating on a coated blade");
    TargetView old = Blade(7);
    old.itemLevel = 200;
    Expect(CheapCheck(stone, bladeAsk, ready, old), Cause::BadTarget, "20 a blade above the stone's level");
    // WI8b: the engine's floor reads the weapon's RequiredLevel (ItemLevel only when that is 0): a RequiredLevel 30,
    // ItemLevel 60 blade under the stone of floor 45 is refused (the old rule, ItemLevel only, let it through)
    Record adamant = stone;
    adamant.targetItemMinLevel = 45;
    TargetView lowReq = Blade(7);
    lowReq.itemLevel = 60;
    lowReq.requiredLevel = 30;
    Expect(CheapCheck(adamant, bladeAsk, ready, lowReq), Cause::BadTarget,
           "20 a RequiredLevel 30 blade under floor 45");
    lowReq.requiredLevel = 0;
    Expect(CheapCheck(adamant, bladeAsk, ready, lowReq), Cause::None, "RequiredLevel 0 falls back to ItemLevel 60");
    lowReq.requiredLevel = 50;
    Expect(CheapCheck(adamant, bladeAsk, ready, lowReq), Cause::None, "a RequiredLevel 50 blade under floor 45");
    TargetView full = Self();
    full.healthFull = true;
    Expect(CheapCheck(potion, selfAsk, ready, full), Cause::Full, "21 a heal on full health");
    Record mana = potion;
    mana.effects[0].effect = RULE_EFFECT_ENERGIZE;
    Expect(CheapCheck(mana, selfAsk, ready, Self()), Cause::None, "a mana potion on low mana");
    TargetView fullMana = Self();
    fullMana.powerFull = true;
    Expect(CheapCheck(mana, selfAsk, ready, fullMana), Cause::Full, "21 mana on full mana");
    Record both = potion;
    both.effects[1].effect = RULE_EFFECT_ENERGIZE;
    both.effectCount = 2;
    Expect(CheapCheck(both, selfAsk, ready, full), Cause::None, "a heal and mana item, mana not full");
    Expect(CheapCheck(bandage, mateAsk, ready,
                      []
                      {
                          TargetView t = Mate(5.0f);
                          t.healthFull = true;
                          return t;
                      }()),
           Cause::None, "a bandage's aura is not refused at full health (the engine's rule)");

    // 22..24 range and sight
    Expect(CheapCheck(bandage, mateAsk, ready, Mate(60.0f)), Cause::Range, "22 a friend out of range");
    Expect(CheapCheck(trinket, foeAsk, worn, Foe(31.0f)), Cause::Range, "22 an enemy out of range");
    Expect(CheapCheck(trinket, foeAsk, worn, Foe(2.0f)), Cause::TooClose, "23 nearer than the minimum");
    TargetView wall = Mate(5.0f);
    wall.inSight = false;
    Expect(CheapCheck(bandage, mateAsk, ready, wall), Cause::Sight, "24 behind a wall");
    TargetView selfBlind = Self();
    selfBlind.inSight = false;
    Expect(CheapCheck(bandage, Ask(bandage, TargetKind::Self), ready, selfBlind), Cause::None,
           "the bot itself needs no range or sight");

    // order: the first cause that holds wins
    bot = ready;
    bot.held = 0;
    bot.cooldownLeftMs = 100;
    bot.alive = false;
    Expect(CheapCheck(potion, selfAsk, bot, full), Cause::NoItem, "order: no_item before not_ready and dead");
    bot = ready;
    bot.busy = true;
    bot.lostControl = LostControl::Stunned;
    Expect(CheapCheck(bandage, mateAsk, bot, Mate(60.0f)), Cause::Busy, "order: busy before no_control and range");
    Expect(CheapCheck(bandage, mateAsk, ready,
                      []
                      {
                          TargetView t = Foe(60.0f);
                          t.inSight = false;
                          return t;
                      }()),
           Cause::BadTarget, "order: target before range and sight");

    // 25..27 the engine's results and the handler
    Check(CauseOfEngine(CAST_OK) == Cause::None, "engine: SPELL_CAST_OK -> none");
    Check(CauseOfEngine(CAST_UNIT_NOT_INFRONT) == Cause::None, "engine: UNIT_NOT_INFRONT -> none (Use turns)");
    Check(CauseOfEngine(CAST_NOT_READY) == Cause::NotReady, "engine: NOT_READY -> not_ready");
    Check(CauseOfEngine(CAST_NO_CHARGES) == Cause::NoCharges, "engine: NO_CHARGES_REMAIN -> no_charges");
    Check(CauseOfEngine(CAST_NOT_IN_ARENA) == Cause::Arena, "engine: NOT_IN_ARENA -> arena");
    Check(CauseOfEngine(CAST_BAD_TARGETS) == Cause::BadTarget, "engine: BAD_TARGETS -> target");
    Check(CauseOfEngine(CAST_FULL_HEALTH) == Cause::Full, "engine: ALREADY_AT_FULL_HEALTH -> full");
    Check(CauseOfEngine(CAST_FULL_POWER) == Cause::Full, "engine: ALREADY_AT_FULL_POWER -> full");
    Check(CauseOfEngine(CAST_OUT_OF_RANGE) == Cause::Range, "engine: OUT_OF_RANGE -> range");
    Check(CauseOfEngine(CAST_TOO_CLOSE) == Cause::TooClose, "engine: TOO_CLOSE -> too_close");
    Check(CauseOfEngine(CAST_LINE_OF_SIGHT) == Cause::Sight, "engine: LINE_OF_SIGHT -> sight");
    Check(CauseOfEngine(CAST_SPELL_FOCUS) == Cause::Focus, "25 engine: REQUIRES_SPELL_FOCUS -> focus");
    Check(CauseOfEngine(29) == Cause::Engine, "26 engine: another result (EQUIPPED_ITEM_CLASS) -> engine");
    Check(CauseWord(Cause::Engine) == "engine" && CauseWord(Cause::Handler) == "handler",
          "26, 27 the words engine and handler");
    Check(CauseWord(Cause::BadTarget) == "target" && CauseWord(Cause::NotEquipped) == "not_equipped",
          "the row's words of 20 and 3");
}

// WI4: spots, objects, the one potion per fight, worn pieces and their tinkers
void SpotObjectPotionCases()
{
    BotView const bot = ReadyBot();
    Record const dynamite = Dynamite();
    Expect(CheapCheck(dynamite, Ask(dynamite, TargetKind::Spot), bot, Spot(20.0f)), Cause::None, "spot: in range");
    Expect(CheapCheck(dynamite, Ask(dynamite, TargetKind::Spot), bot, Spot(80.0f)), Cause::Range, "spot: 80 yd of 30");
    Expect(CheapCheck(dynamite, Ask(dynamite, TargetKind::Spot), bot, Spot(7.0f, false)), Cause::Sight,
           "spot: behind a wall");
    Expect(CheapCheck(dynamite, Ask(dynamite, TargetKind::Unit), bot, Foe(10.0f)), Cause::NoTarget,
           "spot: a unit does not fit a ground spell");
    TargetView missing = Spot(10.0f);
    missing.found = false;
    Expect(CheapCheck(dynamite, Ask(dynamite, TargetKind::Spot), bot, missing), Cause::NoTarget,
           "spot: no creature stood there (at creature)");
    Record close = dynamite;
    close.minRangeYards = 8.0f;
    Expect(CheapCheck(close, Ask(close, TargetKind::Spot), bot, Spot(5.0f)), Cause::TooClose, "spot: too close");

    Record const key = Key();
    Expect(CheapCheck(key, Ask(key, TargetKind::Object), bot, Chest(3.0f)), Cause::None, "object: at 3 yd");
    Expect(CheapCheck(key, Ask(key, TargetKind::Object), bot, Chest(9.0f)), Cause::Range, "object: 9 yd of 5");
    Expect(CheapCheck(key, Ask(key, TargetKind::Object), bot, Chest(3.0f, false)), Cause::Sight,
           "object: out of sight");
    Expect(CheapCheck(key, Ask(key, TargetKind::Spot), bot, Spot(3.0f)), Cause::NoTarget,
           "object: a spot does not fit an object spell");
    Record nearKey = key;
    nearKey.minRangeYards = 2.0f;
    Expect(CheapCheck(nearKey, Ask(nearKey, TargetKind::Object), bot, Chest(1.0f)), Cause::TooClose,
           "object: too close");

    Record const potion = Potion();
    BotView drunk = ReadyBot();
    drunk.potionDrunk = true;
    drunk.inFight = true;
    Expect(CheapCheck(potion, Ask(potion, TargetKind::Self), drunk, Self()), Cause::Potion,
           "potion: a second one in the fight");
    Record mana = potion;
    mana.itemEntry = 2455;
    mana.spellId = 437;
    mana.effects[0].effect = RULE_EFFECT_ENERGIZE;
    Expect(CheapCheck(mana, Ask(mana, TargetKind::Self), drunk, Self()), Cause::Potion,
           "potion: a mana potion after a healing one");
    Record const stone = Stone();
    Expect(CheapCheck(stone, Ask(stone, TargetKind::OwnItem), drunk, Blade(7)), Cause::None,
           "potion: the mark does not refuse a stone");
    BotView fresh = drunk;
    fresh.potionDrunk = false;
    Expect(CheapCheck(potion, Ask(potion, TargetKind::Self), fresh, Self()), Cause::None,
           "potion: the mark cleared (the fight ended) allows one");

    Record const boots = Boots();
    BotView worn = ReadyBot();
    worn.skillValue = 225;
    worn.held = 1;
    Expect(CheapCheck(boots, Ask(boots, TargetKind::None), worn, Self()), Cause::NotEquipped,
           "worn: boots in the bags");
    worn.equipped = true;
    Expect(CheapCheck(boots, Ask(boots, TargetKind::None), worn, Self()), Cause::None, "worn: boots worn");
}

// A worn piece's tinker from the committed dump: the enchantment's record as the piece's (PieceRecord)
// The PvP trinket of the dump (Insignia of the Horde 18834, spell 42292) is used under the states it breaks
void ControlCases(std::string const& root)
{
    std::ifstream in(root + "/tests/fixtures/item-use-dump.txt", std::ios::binary);
    std::optional<Record> insignia;
    std::string line;
    while (!insignia && std::getline(in, line))
        if (std::optional<DumpRow> const row = ParseDumpLine(line))
            if (row->record.itemEntry == 18834 && row->record.spellId == 42292)
                insignia = row->record;
    Check(insignia.has_value(), "control: the dump has Insignia of the Horde 18834, spell 42292");
    if (!insignia)
        return;

    BotView bot = ReadyBot();
    bot.level = 80;
    bot.held = 1;
    bot.equipped = true;
    bot.inFight = true;
    Request const ask = Ask(*insignia, TargetKind::None);
    Expect(CheapCheck(*insignia, ask, bot, Self()), Cause::None, "control: the insignia worn, in control");
    bot.lostControl = LostControl::Stunned;
    Expect(CheapCheck(*insignia, ask, bot, Self()), Cause::None, "control: the insignia while stunned");
    bot.lostControl = LostControl::Fleeing;
    Expect(CheapCheck(*insignia, ask, bot, Self()), Cause::None, "control: the insignia while feared");
    bot.lostControl = LostControl::Confused;
    Expect(CheapCheck(*insignia, ask, bot, Self()), Cause::None, "control: the insignia while confused");
    bot.lostControl = LostControl::Other;
    Expect(CheapCheck(*insignia, ask, bot, Self()), Cause::NoControl, "control: the insignia while charging");
}

void WornCases(std::string const& root)
{
    std::ifstream in(root + "/tests/fixtures/item-use-dump.txt", std::ios::binary);
    Check(static_cast<bool>(in), "worn: the dump opens (" + root + ")");
    std::optional<Record> accelerators;
    std::optional<Record> rocket;
    std::string line;
    while (std::getline(in, line))
        if (std::optional<DumpRow> const row = ParseDumpLine(line))
            if (row->record.source == Source::EnchantSpell)
            {
                if (row->record.enchantId == 3604)
                    accelerators = row->record;
                else if (row->record.enchantId == 3603)
                    rocket = row->record;
            }
    Check(accelerators && accelerators->spellId == 54758 && accelerators->itemEntry == 0,
          "worn: enchant 3604 is spell 54758, no item");
    Check(rocket && rocket->spellId == 54757, "worn: enchant 3603 is spell 54757");
    ControlCases(root);
    if (!accelerators || !rocket)
        return;

    constexpr std::uint32_t GLOVES = 3308;
    constexpr std::uint8_t HANDS = 10;  // INVTYPE_HANDS
    Record const piece = PieceRecord(*accelerators, GLOVES, HANDS);
    Check(piece.itemEntry == GLOVES && piece.inventoryType == HANDS && piece.spellId == 54758 &&
              piece.source == Source::EnchantSpell && piece.enchantId == 3604,
          "worn: the tinker's record names the gloves, keeps its spell");
    std::array<Record, 2> const set = {PieceRecord(*accelerators, GLOVES, HANDS), PieceRecord(*rocket, GLOVES, HANDS)};
    Record const* first = SelectRecord(set, 0);
    Record const* meant = SelectRecord(set, 54757);
    Check(first && first->spellId == 54758 && meant && meant->spellId == 54757,
          "worn: the set's first use, else the spell meant");
    BotView bot = ReadyBot();
    bot.level = 80;
    bot.held = 1;
    bot.skillValue = 450;
    Request ask = Ask(piece, TargetKind::None);
    Expect(CheapCheck(piece, ask, bot, Self()), Cause::NotEquipped, "worn: tinkered gloves in the bags");
    bot.equipped = true;
    Expect(CheapCheck(piece, ask, bot, Self()), Cause::None, "worn: tinkered gloves worn");
    bot.skillValue = 350;
    Expect(CheapCheck(piece, ask, bot, Self()), Cause::Skill, "worn: engineering 350 of 400");
}

// Two requests of WI5's old callers on records of the committed dump: a trinket whose spell takes a friend, which
// the trinket action uses on the bot, and an item with several use spells, whose request names the spell its target
// takes (the first one is checked otherwise)
void OldCallerCases(std::string const& root)
{
    constexpr std::uint32_t RECOMBOBULATOR = 4381;  // a trinket, spell 18805 on a friend
    constexpr std::uint32_t CORE = 31088;           // spells 38134 (a friend), 3366 (an object), 38132 (the bot)
    constexpr std::uint32_t OPENING = 3366;
    std::ifstream in(root + "/tests/fixtures/item-use-dump.txt", std::ios::binary);
    std::optional<Record> trinket;
    std::vector<Record> core;
    std::string line;
    while (std::getline(in, line))
        if (std::optional<DumpRow> const row = ParseDumpLine(line))
            if (row->record.source == Source::ItemSpell)
            {
                if (row->record.itemEntry == RECOMBOBULATOR)
                    trinket = row->record;
                else if (row->record.itemEntry == CORE)
                    core.push_back(row->record);
            }
    Check(trinket && trinket->shape == Shape::Friend && trinket->inventoryType,
          "old callers: trinket 4381 takes a friend");
    Check(core.size() == 3 && core[0].shape == Shape::Friend && core[1].shape == Shape::Object &&
              core[1].spellId == OPENING,
          "old callers: item 31088 has a friend's spell first, then the object's");
    if (!trinket || core.size() != 3)
        return;

    BotView bot = ReadyBot();
    bot.level = 80;
    bot.skillValue = 450;
    bot.equipped = true;
    Expect(CheapCheck(*trinket, Ask(*trinket, TargetKind::None), bot, TargetView{}), Cause::NoTarget,
           "old callers: a friend's trinket with no target named");
    Expect(CheapCheck(*trinket, Ask(*trinket, TargetKind::Self), bot, Self()), Cause::None,
           "old callers: a friend's trinket on the bot, as the trinket action asks");

    Record const* first = SelectRecord(core, 0);
    Record const* opening = SelectRecord(core, OPENING);
    Check(first && first->spellId == core[0].spellId && opening && opening->spellId == OPENING,
          "old callers: no spell named means the first use spell");
    if (!first || !opening)
        return;
    Request onObject = Ask(*first, TargetKind::Object);
    Expect(CheapCheck(*first, onObject, bot, Chest(3.0f)), Cause::NoTarget,
           "old callers: the first use spell (a friend's) against an object");
    onObject.spellId = OPENING;
    Expect(CheapCheck(*opening, onObject, bot, Chest(3.0f)), Cause::None,
           "old callers: the object's own use spell named");
}

// The bound of a timed use and the event its signs give at an update
void SignCases()
{
    Check(BoundOf(Dynamite(), 1500) == 2500, "bound: a cast time plus the grace");
    Check(BoundOf(Bandage(), 1500) == 7500, "bound: a channel's duration plus the grace");
    Check(BoundOf(Boots(), 1500) == 1500, "bound: an aura's duration does not count");

    PendingSigns cast;
    cast.runs = true;
    Check(EventOf(cast) == UseEvent::Tick, "signs: a cast that runs is a tick");
    cast.castSeen = true;
    Check(EventOf(cast) == UseEvent::CastDone, "signs: the engine cast it at the cast time's end");
    cast.alive = false;
    Check(EventOf(cast) == UseEvent::CastDone, "signs: a death after the cast keeps the use");
    PendingSigns moved;
    moved.runs = true;
    moved.cancelSeen = true;
    moved.movedAtCancel = true;
    Check(EventOf(moved) == UseEvent::Moved, "signs: cancelled while moving");
    moved.movedAtCancel = false;
    moved.controlLostAtCancel = true;
    Check(EventOf(moved) == UseEvent::LostControl, "signs: cancelled while stunned");
    moved.controlLostAtCancel = false;
    Check(EventOf(moved) == UseEvent::CastFailed, "signs: cancelled otherwise (damage, a reset)");
    PendingSigns dead;
    dead.alive = false;
    dead.cancelSeen = true;
    Check(EventOf(dead) == UseEvent::Died, "signs: a death");
    PendingSigns gone;
    Check(EventOf(gone) == UseEvent::CastFailed, "signs: a cast gone without its cast failed");
    PendingSigns channel;
    channel.channeled = true;
    channel.castSeen = true;
    channel.runs = true;
    Check(EventOf(channel) == UseEvent::Tick, "signs: a channel that runs is a tick");
    channel.runs = false;
    Check(EventOf(channel) == UseEvent::CastDone, "signs: a channel that ran out is used");
    channel.castSeen = false;
    Check(EventOf(channel) == UseEvent::CastFailed, "signs: a channel that never started failed");
    channel.castSeen = true;
    channel.runs = true;
    channel.cancelSeen = true;
    channel.movedAtCancel = true;
    Check(EventOf(channel) == UseEvent::Moved, "signs: a channel cancelled by a move");
}

void AdvanceCases()
{
    PendingUse pending;
    pending.spellId = 746;
    pending.startedMs = 1000;
    pending.boundMs = 7000;
    auto ends = [&](UseEvent event, std::uint32_t now, Outcome outcome, Interrupt interrupt, std::string const& what)
    {
        std::optional<UseResult> const result = Advance(pending, event, now);
        Check(result && result->outcome == outcome && result->interrupt == interrupt && result->spellId == 746, what);
    };
    ends(UseEvent::CastDone, 7000, Outcome::Used, Interrupt::None, "Advance: the cast's end is used");
    ends(UseEvent::CastFailed, 2000, Outcome::Interrupted, Interrupt::Other, "Advance: a failed cast is other");
    ends(UseEvent::Moved, 2000, Outcome::Interrupted, Interrupt::Moved, "Advance: a move");
    ends(UseEvent::LostControl, 2000, Outcome::Interrupted, Interrupt::Control, "Advance: a stun");
    ends(UseEvent::Died, 2000, Outcome::Interrupted, Interrupt::Death, "Advance: a death");
    ends(UseEvent::LoggedOut, 2000, Outcome::Interrupted, Interrupt::Logout, "Advance: a logout");
    ends(UseEvent::Reset, 2000, Outcome::Interrupted, Interrupt::Reset, "Advance: a reset");
    ends(UseEvent::Tick, 8001, Outcome::Interrupted, Interrupt::Bound, "Advance: a tick past the bound");
    Check(!Advance(pending, UseEvent::Tick, 8000), "Advance: a tick at the bound leaves it open");
    Check(!Advance(pending, UseEvent::Tick, 1500), "Advance: a tick inside the bound leaves it open");
    pending.startedMs = 0xFFFFF000u;
    Check(!Advance(pending, UseEvent::Tick, 0x00000100u), "Advance: the clock's wrap inside the bound");
}

// The search: a model bot takes requests and events in a seeded random order. Use refuses (row at once), uses an
// instant (row at once) or starts a timed use (row at its end, from Advance); a request while one is pending is
// refused busy. Each request id must end with exactly one row
struct Lcg
{
    std::uint64_t state;
    std::uint32_t Next()
    {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        return static_cast<std::uint32_t>(state >> 33);
    }
    std::uint32_t Below(std::uint32_t n) { return Next() % n; }
};

constexpr std::uint32_t UPDATE_MS = 100;  // the model's AI update
constexpr std::uint32_t GRACE_MS = 1000;
constexpr std::size_t EVENT_KINDS = 8;

struct Counters
{
    std::uint32_t requests{0};
    std::uint32_t refused{0};
    std::uint32_t instants{0};
    std::uint32_t timed{0};
    std::uint32_t fromSigns{0};
    std::array<std::uint32_t, 6> kinds{};  // requests per TargetKind
    std::array<std::uint32_t, EVENT_KINDS> events{};
    std::uint32_t noRow{0};
    std::uint32_t twoRows{0};
    std::uint32_t lateEnd{0};
    std::uint32_t packetOnRefusal{0};
    std::uint32_t twoPending{0};
};

struct Run
{
    std::vector<std::uint32_t> rows;  // per request id
    std::vector<bool> refusedIds;
    std::vector<bool> packets;
    std::uint64_t trace{1469598103934665603ULL};
};

void Note(Run& run, std::uint32_t id, std::uint32_t word)
{
    run.rows[id] += 1;
    run.trace = (run.trace ^ (static_cast<std::uint64_t>(id) << 8 | word)) * 1099511628211ULL;
}

Run Simulate(std::uint64_t seed, Counters& counters)
{
    Lcg random{seed};
    Run run;
    std::optional<PendingUse> pending;
    std::uint32_t pendingId = 0;
    std::uint32_t pendingCount = 0;
    std::uint32_t now = 1000;
    std::uint32_t const steps = 20 + random.Below(60);
    for (std::uint32_t step = 0; step < steps; ++step)
    {
        if (random.Below(3) == 0)
        {
            std::uint32_t const id = static_cast<std::uint32_t>(run.rows.size());
            run.rows.push_back(0);
            run.refusedIds.push_back(false);
            run.packets.push_back(false);
            ++counters.requests;
            BotView bot = ReadyBot();
            bot.busy = pending.has_value();
            bot.cooldownLeftMs = random.Below(4) == 0 ? 500 : 0;
            bot.equipped = random.Below(2) == 0;
            bot.skillValue = 225;
            std::array<Record, 5> const records = {Potion(), Bandage(), Dynamite(), Key(), Boots()};
            Record const record = records[random.Below(static_cast<std::uint32_t>(records.size()))];
            float const yards = static_cast<float>(random.Below(40));
            bool const inSight = random.Below(5) != 0;
            TargetView target;
            if (record.shape == Shape::Self)
                target = Self();
            else if (record.shape == Shape::Ground)
                target = Spot(yards, inSight);
            else if (record.shape == Shape::Object)
                target = Chest(yards / 4.0f, inSight);
            else
                target = Mate(yards);
            ++counters.kinds[static_cast<std::size_t>(target.kind)];
            Cause const cause = CheapCheck(record, Ask(record, target.kind), bot, target);
            if (cause != Cause::None)
            {
                ++counters.refused;
                run.refusedIds[id] = true;
                Note(run, id, 100 + static_cast<std::uint32_t>(cause));
                continue;
            }
            run.packets[id] = true;  // the one builder
            if (!record.channeled && !record.castMs)
            {
                ++counters.instants;
                Note(run, id, 1);
                continue;
            }
            ++counters.timed;
            if (pending)
                ++counters.twoPending;
            pending = PendingUse{record.itemEntry, record.spellId, Caller::Test, now, BoundOf(record, GRACE_MS)};
            pendingId = id;
            ++pendingCount;
            continue;
        }

        // an event drawn, or the one random signs give (the engine's hooks and the bot's state)
        UseEvent event = static_cast<UseEvent>(random.Below(EVENT_KINDS));
        if (random.Below(2) == 0)
        {
            PendingSigns signs;
            signs.channeled = random.Below(2) == 0;
            signs.castSeen = random.Below(2) == 0;
            signs.cancelSeen = random.Below(4) == 0;
            signs.movedAtCancel = random.Below(2) == 0;
            signs.controlLostAtCancel = random.Below(2) == 0;
            signs.alive = random.Below(8) != 0;
            signs.runs = random.Below(3) != 0;
            event = EventOf(signs);
            ++counters.fromSigns;
        }
        ++counters.events[static_cast<std::size_t>(event)];
        if (event == UseEvent::Tick)
        {
            // a stretch of AI updates, each a Tick
            for (std::uint32_t updates = 1 + random.Below(40); updates; --updates)
            {
                now += UPDATE_MS;
                if (!pending)
                    continue;
                std::optional<UseResult> const end = Advance(*pending, UseEvent::Tick, now);
                if (!end)
                    continue;
                if (now > pending->startedMs + pending->boundMs + UPDATE_MS)
                    ++counters.lateEnd;
                Note(run, pendingId, 10 + static_cast<std::uint32_t>(end->interrupt));
                pending.reset();
                --pendingCount;
            }
            continue;
        }
        if (!pending)
            continue;
        if (std::optional<UseResult> const end = Advance(*pending, event, now))
        {
            Note(run, pendingId, 10 + static_cast<std::uint32_t>(end->interrupt));
            pending.reset();
            --pendingCount;
        }
        if (pendingCount > 1)
            ++counters.twoPending;
    }

    // after the last event the bot keeps updating: a pending use must end within its bound plus one update
    if (pending)
    {
        std::uint32_t const limit = pending->startedMs + pending->boundMs + UPDATE_MS;
        while (pending && now <= limit + UPDATE_MS)
        {
            now += UPDATE_MS;
            if (std::optional<UseResult> const end = Advance(*pending, UseEvent::Tick, now))
            {
                if (now > limit)
                    ++counters.lateEnd;
                Note(run, pendingId, 10 + static_cast<std::uint32_t>(end->interrupt));
                pending.reset();
            }
        }
        if (pending)
            ++counters.lateEnd;
    }

    for (std::size_t id = 0; id < run.rows.size(); ++id)
    {
        if (run.rows[id] == 0)
            ++counters.noRow;
        if (run.rows[id] > 1)
            ++counters.twoRows;
        if (run.refusedIds[id] && run.packets[id])
            ++counters.packetOnRefusal;
    }
    return run;
}

void Search()
{
    constexpr std::uint32_t SEEDS = 2000;
    Counters counters;
    std::uint32_t notSame = 0;
    for (std::uint32_t seed = 1; seed <= SEEDS; ++seed)
    {
        Run const first = Simulate(seed, counters);
        Counters again;
        Run const second = Simulate(seed, again);
        if (first.trace != second.trace || first.rows != second.rows)
            ++notSame;
    }
    std::cout << "search: " << SEEDS << " seeds, " << counters.requests << " requests (" << counters.refused
              << " refused, " << counters.instants << " instant, " << counters.timed << " timed); events";
    for (std::uint32_t count : counters.events)
        std::cout << " " << count;
    std::cout << " (" << counters.fromSigns << " from signs); kinds";
    for (std::uint32_t count : counters.kinds)
        std::cout << " " << count;
    std::cout << "\n";
    std::cout << "L1 no_row " << counters.noRow << ", L2 two_rows " << counters.twoRows << ", L3 late_end "
              << counters.lateEnd << ", L4 packet_on_refusal " << counters.packetOnRefusal << ", L5 two_pending "
              << counters.twoPending << ", L6 not_same " << notSame << "\n";
    bool everyEvent = true;
    for (std::uint32_t count : counters.events)
        everyEvent = everyEvent && count > 0;
    Check(counters.requests > 0 && counters.refused > 0 && counters.instants > 0 && counters.timed > 0,
          "search: requests of every end were made");
    Check(everyEvent, "search: every kind of event happened (stun, death, move, logout, ...)");
    Check(counters.fromSigns > 0 && counters.kinds[static_cast<std::size_t>(TargetKind::Spot)] > 0 &&
              counters.kinds[static_cast<std::size_t>(TargetKind::Object)] > 0 &&
              counters.kinds[static_cast<std::size_t>(TargetKind::Self)] > 0,
          "search: spot, object and self requests, events from signs");
    Check(counters.noRow == 0, "L1 no_row is 0");
    Check(counters.twoRows == 0, "L2 two_rows is 0");
    Check(counters.lateEnd == 0, "L3 late_end is 0");
    Check(counters.packetOnRefusal == 0, "L4 packet_on_refusal is 0");
    Check(counters.twoPending == 0, "L5 two_pending is 0");
    Check(notSame == 0, "L6 not_same is 0");
}
}  // namespace

int main(int argc, char** argv)
{
    std::string const root = argc > 1 ? argv[1] : ".";
    HandCases();
    SpotObjectPotionCases();
    WornCases(root);
    OldCallerCases(root);
    SignCases();
    AdvanceCases();
    Search();
    std::cout << passed << " passed, " << failed << " failed\n";
    return failed == 0 ? 0 : 1;
}
