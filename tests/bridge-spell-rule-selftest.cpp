/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// LC17b: the cast row's spell rule (BridgeSpellRule.h) over a list of spells as their SpellInfo reads, the traps
// among them: a teleport, the hearthstone, a blink, a charge, a harmful spell with a helpful part, a passive, the
// profession spells. Build from the module root:
//   clang-cl /std:c++20 /EHsc /W4 /WX /I src/Bot tests/bridge-spell-rule-selftest.cpp

#include <cstdio>

#include "BridgeSpellRule.h"

namespace
{
using PlayerbotBridge::JudgeSpell;
using PlayerbotBridge::SpellFacts;
using PlayerbotBridge::SpellVerdict;
using PlayerbotBridge::TradeskillAttribute;

int passed = 0;
int failed = 0;

void Check(bool ok, char const* what)
{
    if (ok)
    {
        ++passed;
        return;
    }

    ++failed;
    std::printf("FAIL %s\n", what);
}

// SharedDefines.h SpellEffects values beside the rule's own (APPLY_AURA, HEAL, RESURRECT, CREATE_ITEM, DISPEL,
// TRANS_DOOR, ENCHANT_ITEM, TRIGGER_SPELL, DUMMY).
constexpr uint32_t Dummy = 3, ApplyAura = 6, Heal = 10, Resurrect = 18, CreateItem = 24, Dispel = 38, TransDoor = 50,
                   EnchantItem = 53, TriggerSpell = 64;

struct Sample
{
    char const* Name;
    SpellFacts Facts;
    SpellVerdict Want;
};

using namespace PlayerbotBridge::SpellEffectId;
constexpr Sample Samples[] = {
    // The helpful kinds the plan lists.
    {"Power Word: Fortitude", {false, true, false, 0, {ApplyAura, 0, 0}}, SpellVerdict::Helpful},
    {"Dampen Magic", {false, true, false, 0, {ApplyAura, ApplyAura, 0}}, SpellVerdict::Helpful},
    {"Flash Heal", {false, true, false, 0, {Heal, 0, 0}}, SpellVerdict::Helpful},
    {"Cure Disease", {false, true, false, 0, {Dispel, 0, 0}}, SpellVerdict::Helpful},
    {"Resurrection", {false, true, false, 0, {Resurrect, 0, 0}}, SpellVerdict::Helpful},
    {"Conjure Water", {false, true, false, 0, {CreateItem, 0, 0}}, SpellVerdict::Helpful},
    {"Portal: Stormwind", {false, true, false, 0, {TransDoor, 0, 0}}, SpellVerdict::Helpful},
    // Travel: the planner's own.
    {"Teleport: Stormwind", {false, true, false, 0, {TeleportUnits, 0, 0}}, SpellVerdict::MovesCaster},
    {"Hearthstone", {false, true, false, 0, {TeleportUnits, Dummy, 0}}, SpellVerdict::MovesCaster},
    {"Astral Recall", {false, true, false, 0, {0, TeleportUnitsFaceCaster, 0}}, SpellVerdict::MovesCaster},
    {"a portal's teleport", {false, true, false, 0, {PortalTeleport, 0, 0}}, SpellVerdict::MovesCaster},
    {"Blink", {false, true, false, 0, {Leap, ApplyAura, 0}}, SpellVerdict::MovesCaster},
    {"Disengage", {false, true, false, 0, {LeapBack, 0, 0}}, SpellVerdict::MovesCaster},
    {"Heroic Leap", {false, true, false, 0, {JumpDest, 0, 0}}, SpellVerdict::MovesCaster},
    {"Charge", {false, false, false, 0, {Charge, Dummy, 0}}, SpellVerdict::MovesCaster},
    {"Intervene", {false, true, false, 0, {ChargeDest, 0, 0}}, SpellVerdict::MovesCaster},
    {"Thunderstorm", {false, false, false, 0, {SchoolDamage, KnockBack, 0}}, SpellVerdict::MovesCaster},
    {"Death Grip", {false, false, false, 0, {PullTowards, Dummy, 0}}, SpellVerdict::MovesCaster},
    {"unstuck", {false, true, false, 0, {Stuck, 0, 0}}, SpellVerdict::MovesCaster},
    {"a graveyard teleport", {false, true, false, 0, {TeleportGraveyard, 0, 0}}, SpellVerdict::MovesCaster},
    // Harm: a damage spell with a helpful part (the core may count it positive: its damage still harms).
    {"Frostbolt", {false, false, false, 0, {SchoolDamage, ApplyAura, 0}}, SpellVerdict::Harmful},
    {"Holy Nova", {false, true, false, 0, {SchoolDamage, TriggerSpell, 0}}, SpellVerdict::Harmful},
    {"Drain Life", {false, false, false, 0, {HealthLeech, Heal, 0}}, SpellVerdict::Harmful},
    {"Death Coil", {false, true, false, 0, {HealthLeech, ApplyAura, 0}}, SpellVerdict::Harmful},
    {"Mana Burn", {false, true, false, 0, {PowerBurn, 0, 0}}, SpellVerdict::Harmful},
    {"Counterspell", {false, true, false, 0, {InterruptCast, 0, 0}}, SpellVerdict::Harmful},
    {"Growl", {false, true, false, 0, {AttackMe, 0, 0}}, SpellVerdict::Harmful},
    {"Fear", {false, false, false, 0, {ApplyAura, 0, 0}}, SpellVerdict::Harmful},
    {"Execute", {false, true, false, 0, {WeaponPercentDamage, 0, 0}}, SpellVerdict::Harmful},
    {"Heroic Strike", {false, true, false, 0, {WeaponDamage, 0, 0}}, SpellVerdict::Harmful},
    {"Mortal Strike", {false, true, false, 0, {NormalizedWeaponDamage, 0, 0}}, SpellVerdict::Harmful},
    {"Shoot", {false, true, false, 0, {WeaponDamageNoSchool, 0, 0}}, SpellVerdict::Harmful},
    {"Viper Sting", {false, true, false, 0, {PowerDrain, 0, 0}}, SpellVerdict::Harmful},
    {"Hellfire Effect", {false, true, false, 0, {EnvironmentalDamage, 0, 0}}, SpellVerdict::Harmful},
    {"Death Touch", {false, true, false, 0, {Instakill, 0, 0}}, SpellVerdict::Harmful},
    // Passive: nothing to cast.
    {"Arcane Mind", {true, true, false, 0, {ApplyAura, 0, 0}}, SpellVerdict::Passive},
    {"a passive teleport", {true, true, false, 0, {TeleportUnits, 0, 0}}, SpellVerdict::Passive},
    // Crafts: by the profession's skill line, the tradeskill attribute or the trade skill effect.
    {"Linen Bandage", {false, true, true, TradeskillAttribute, {CreateItem, 0, 0}}, SpellVerdict::Craft},
    {"Smelt Copper", {false, true, true, 0, {CreateItem, 0, 0}}, SpellVerdict::Craft},
    {"Enchant Bracer - Minor Health", {false, true, true, 0, {EnchantItem, 0, 0}}, SpellVerdict::Craft},
    {"a recipe without its skill line",
     {false, true, false, TradeskillAttribute, {CreateItem, 0, 0}},
     SpellVerdict::Craft},
    {"Cooking", {false, true, false, 0, {TradeSkill, 0, 0}}, SpellVerdict::Craft},
    {"a crafted teleport", {false, true, true, 0, {TeleportUnits, 0, 0}}, SpellVerdict::Craft}};
}  // namespace

int main()
{
    for (Sample const& sample : Samples)
    {
        SpellVerdict const got = JudgeSpell(sample.Facts);
        if (got != sample.Want)
            std::printf("FAIL %s: verdict %d, want %d\n", sample.Name, static_cast<int>(got),
                        static_cast<int>(sample.Want));
        Check(got == sample.Want, "a spell's verdict");
        Check(PlayerbotBridge::HelpfulSpell(sample.Facts) == (sample.Want == SpellVerdict::Helpful),
              "a helpful spell, and only one, is cast on an order");
    }

    // Each trap kind is in the list once at least (a teleport, the hearthstone, a harmful spell with a helpful part,
    // a passive, a profession spell).
    int helpful = 0, moves = 0, harmful = 0, passive = 0, craft = 0;
    for (Sample const& sample : Samples)
    {
        helpful += sample.Want == SpellVerdict::Helpful;
        moves += sample.Want == SpellVerdict::MovesCaster;
        harmful += sample.Want == SpellVerdict::Harmful;
        passive += sample.Want == SpellVerdict::Passive;
        craft += sample.Want == SpellVerdict::Craft;
    }
    Check(helpful == 7 && moves == 13 && harmful == 15 && passive == 2 && craft == 6, "the list holds every kind");

    if (failed)
    {
        std::printf("FAIL %d of %d checks\n", failed, passed + failed);
        return 1;
    }

    std::printf("PASS %d checks\n", passed);
    return 0;
}
