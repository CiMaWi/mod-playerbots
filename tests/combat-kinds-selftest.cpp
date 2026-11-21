/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// Offline proof of combat_known's kinds and the sample rows' rules (combat-foundation CFM3b; src/Bot/Engine/
// CombatKinds.h, MEASURES §1 and §6.2): one spell per kind, filled as the server fills it from the spell data, a spell
// with none, each kind's edge (a control of 1.5 s, a defensive with a 20 s cooldown, a finisher without its health
// state, a heal on the caster or an ally, a dispel on a friend or an enemy), the sample clock (one row a second, a
// late tick, a gap), the power points, the rune mask, the words and the taken difference.
// Build (Git Bash, module root):
//   clang-cl /std:c++20 /EHsc /W4 /WX /I src/Bot/Engine tests/combat-kinds-selftest.cpp

#include <cstdint>
#include <cstdio>
#include <string>

#include "CombatKinds.h"

namespace
{
using namespace CombatKinds;
using namespace CombatKinds::Data;

int passed = 0;
int failed = 0;

void Check(bool pass, std::string const& name)
{
    std::printf("%s %s\n", pass ? "PASS" : "FAIL", name.c_str());
    (pass ? passed : failed)++;
}

Effect Make(std::uint32_t effect, Aim aim, std::uint32_t aura = 0, std::int32_t value = 0)
{
    Effect e;
    e.effect = effect;
    e.aim = aim;
    e.aura = aura;
    e.value = value;
    return e;
}

constexpr std::uint32_t APPLY_AURA = 6;

SpellFacts One(Effect const& e, std::uint32_t cooldownMs = 0, std::int32_t durationMs = 0)
{
    SpellFacts spell;
    spell.effects[0] = e;
    spell.cooldownMs = cooldownMs;
    spell.durationMs = durationMs;
    return spell;
}

void Kinds()
{
    // an interrupt with a 24 s cooldown: interrupt, not a cooldown
    Check(Classify(One(Make(EFFECT_INTERRUPT_CAST, Aim::Enemy), 24000)) == INTERRUPT, "interrupt (24 s: no cooldown)");

    Effect dispel = Make(EFFECT_DISPEL, Aim::Ally);
    dispel.misc = DISPEL_TYPE_CURSE;
    Check(Classify(One(dispel)) == DISPEL_CURSE, "dispel curse on an ally");
    dispel.aim = Aim::Enemy;
    dispel.misc = DISPEL_TYPE_MAGIC;
    Check(Classify(One(dispel)) == PURGE, "dispel magic on an enemy: a purge");
    dispel.aim = Aim::Any;
    Check(Classify(One(dispel)) == (DISPEL_MAGIC | PURGE), "dispel magic on either: both");
    dispel.aim = Aim::Self;
    dispel.misc = DISPEL_TYPE_POISON;
    Check(Classify(One(dispel)) == DISPEL_POISON, "dispel poison on the caster");
    dispel.misc = DISPEL_TYPE_DISEASE;
    Check(Classify(One(dispel)) == DISPEL_DISEASE, "dispel disease");
    Check(Classify(One(Make(EFFECT_STEAL_BENEFICIAL_BUFF, Aim::Enemy))) == STEAL, "steal");

    Effect stun = Make(APPLY_AURA, Aim::Enemy, AURA_MOD_STUN);
    Check(Classify(One(stun, 0, 4000)) == CONTROL, "control: a stun of 4 s");
    Check(Classify(One(stun, 0, 1500)) == 0, "control edge: a stun of 1.5 s is none");
    Check(Classify(One(stun, 0, 2000)) == CONTROL, "control edge: 2 s is control");
    Check(Classify(One(stun, 0, -1)) == CONTROL, "control without an end");
    Effect poly = Make(APPLY_AURA, Aim::Enemy, 0);
    poly.aura = 4;  // a dummy aura: control by the effect's mechanic
    poly.mechanic = MECHANIC_POLYMORPH;
    Check(Classify(One(poly, 0, 10000)) == CONTROL, "control by an effect's mechanic");
    SpellFacts byMechanic = One(Make(APPLY_AURA, Aim::Enemy, 4), 0, 6000);
    byMechanic.mechanic = MECHANIC_FEAR;
    Check(Classify(byMechanic) == CONTROL, "control by the spell's mechanic");
    SpellFacts snare = One(Make(APPLY_AURA, Aim::Enemy, 33), 0, 8000);
    snare.mechanic = 11;
    Check(Classify(snare) == 0, "a snare is no control");
    Effect selfStun = Make(APPLY_AURA, Aim::Self, AURA_MOD_STUN);
    Check(Classify(One(selfStun, 0, 10000)) == 0, "a stun on the caster is no control");

    Check(Classify(One(Make(EFFECT_ATTACK_ME, Aim::Enemy))) == TAUNT, "taunt (attack me)");
    Check(Classify(One(Make(APPLY_AURA, Aim::Enemy, AURA_MOD_TAUNT), 0, 3000)) == TAUNT, "taunt (aura)");

    Check(Classify(One(Make(EFFECT_THREAT, Aim::Enemy, 0, -600))) == THREAT_DROP, "threat drop: a threat below zero");
    Check(Classify(One(Make(EFFECT_THREAT, Aim::Enemy, 0, 300))) == 0, "threat edge: a threat gain is none");
    Check(Classify(One(Make(APPLY_AURA, Aim::Self, AURA_MOD_TOTAL_THREAT, -500), 0, 10000)) == THREAT_DROP,
          "threat drop: total threat below zero");
    Check(Classify(One(Make(APPLY_AURA, Aim::Self, AURA_FEIGN_DEATH), 30000, 360000)) == (THREAT_DROP | COOLDOWN),
          "threat drop: a feign, with a cooldown");

    Check(Classify(One(Make(EFFECT_HEAL, Aim::Self))) == SELF_HEAL, "self-heal");
    Check(Classify(One(Make(EFFECT_HEAL, Aim::Ally))) == HEAL, "heal on an ally");
    Check(Classify(One(Make(APPLY_AURA, Aim::Ally, AURA_PERIODIC_HEAL), 0, 12000)) == HEAL, "heal over time");

    Effect shield = Make(APPLY_AURA, Aim::Self, AURA_MOD_DAMAGE_PERCENT_TAKEN, -60);
    Check(Classify(One(shield, 300000, 12000)) == (DEFENSIVE | COOLDOWN), "defensive: less damage taken, 5 min");
    Check(Classify(One(shield, 20000, 12000)) == 0, "defensive edge: a 20 s cooldown is none");
    Check(Classify(One(Make(APPLY_AURA, Aim::Self, AURA_SCHOOL_ABSORB), 0, 30000)) == 0,
          "defensive edge: an absorb without a cooldown is none");
    Check(Classify(One(Make(APPLY_AURA, Aim::Self, AURA_SCHOOL_IMMUNITY), 300000, 10000)) == (DEFENSIVE | COOLDOWN),
          "defensive: an immunity");
    Check(Classify(One(Make(APPLY_AURA, Aim::Ally, AURA_SCHOOL_ABSORB), 60000, 10000)) == COOLDOWN,
          "defensive edge: on an ally it is none");

    SpellFacts finisher = One(Make(2, Aim::Enemy));
    finisher.targetAuraState = STATE_HEALTHLESS_20;
    Check(Classify(finisher) == FINISHER, "finisher: the target below 20 %");
    finisher.targetAuraState = STATE_HEALTHLESS_35;
    Check(Classify(finisher) == FINISHER, "finisher: the target below 35 %");
    finisher.targetAuraState = 0;
    Check(Classify(finisher) == 0, "finisher edge: no health state, no finisher");

    Check(Classify(One(Make(EFFECT_CHARGE, Aim::Enemy), 15000)) == CLOSE, "close: a charge");
    Check(Classify(One(Make(EFFECT_PULL_TOWARDS, Aim::Enemy), 35000)) == (CLOSE | COOLDOWN), "close: a pull");
    Check(Classify(One(Make(EFFECT_LEAP, Aim::Other), 15000)) == ESCAPE, "escape: a blink");
    Check(Classify(One(Make(APPLY_AURA, Aim::Self, AURA_MOD_INCREASE_SPEED, 70), 300000, 15000)) == (ESCAPE | COOLDOWN),
          "escape: a speed gain");
    Check(Classify(One(Make(EFFECT_DISPEL_MECHANIC, Aim::Self), 120000)) == (ESCAPE | COOLDOWN),
          "escape: a mechanic removed from the caster");
    Effect immune = Make(APPLY_AURA, Aim::Self, AURA_MECHANIC_IMMUNITY);
    immune.misc = 5;
    Check(Classify(One(immune, 120000, 5000)) == (ESCAPE | COOLDOWN), "escape: the caster immune to fear");
    immune.misc = 11;
    Check(Classify(One(immune, 0, 5000)) == ESCAPE, "escape: the caster immune to a slow");
    immune.misc = 16;
    Check(Classify(One(immune, 0, 15000)) == 0, "escape edge: an immunity to bandages frees nothing");
    Check(Classify(One(Make(APPLY_AURA, Aim::Enemy, AURA_MOD_INCREASE_SPEED, -50), 0, 8000)) == 0,
          "escape edge: a speed loss on an enemy is none");

    Effect nova = Make(APPLY_AURA, Aim::Enemy, AURA_MOD_ROOT);
    nova.area = true;
    Check(Classify(One(nova, 25000, 8000)) == (CONTROL | AREA), "area: an area root");
    Check(Classify(One(Make(EFFECT_PERSISTENT_AREA_AURA, Aim::Other), 0, 8000)) == AREA,
          "area: a persistent area effect on the ground");

    Check(Classify(One(Make(EFFECT_ENERGIZE, Aim::Self), 60000)) == (RECOVER | COOLDOWN),
          "recover: power to the caster");
    Check(Classify(One(Make(EFFECT_ENERGIZE, Aim::Ally))) == 0, "recover edge: power to another is none");
    Check(Classify(One(Make(2, Aim::Enemy), 30000)) == COOLDOWN, "cooldown: 30 s");

    Check(Classify(One(Make(2, Aim::Enemy))) == 0, "a spell with none: plain damage");
    Check(Classify(SpellFacts{}) == 0, "a spell with no effect");

    // two effects add up: damage and an interrupt, an area
    SpellFacts both = One(Make(2, Aim::Enemy));
    both.effects[1] = Make(EFFECT_INTERRUPT_CAST, Aim::Enemy);
    both.effects[1].area = true;
    Check(Classify(both) == (INTERRUPT | AREA), "two effects: interrupt and area");
    Check(static_cast<std::uint32_t>(COOLDOWN) < (1u << 24), "the bits stay exact in a float");
}

void Samples()
{
    SampleClock clock;
    clock.Start(10000);
    Check(!clock.Due(10500), "clock: not due before a second");
    Check(clock.Due(11000), "clock: due at a second");
    Check(clock.Due(12200) && clock.lastMs == 12000, "clock: a late tick steps a second");
    Check(!clock.Due(12900) && clock.Due(13000), "clock: the next row a second after the step");
    Check(clock.Due(20000) && clock.lastMs == 20000, "clock: after a gap it starts from now");
    SampleClock fresh;
    Check(fresh.Due(5) && fresh.lastMs == 5, "clock: never started, due at once");
    // rows of a 60 s fight with ticks every 300 ms: one a second
    SampleClock fight;
    fight.Start(0);
    int rows = 0;
    for (std::uint32_t t = 300; t <= 60000; t += 300)
        rows += fight.Due(t) ? 1 : 0;
    Check(rows >= 59, "clock: a 60 s fight at 300 ms ticks has 59 rows or more: " + std::to_string(rows));
    SampleClock wrap;
    wrap.Start(0xFFFFFF00u);
    Check(wrap.Due(0x00000300u), "clock: the millisecond timer's wrap");

    Check(Points(1, 455) == 45 && Points(6, 1000) == 100, "points: rage and runic power in whole points");
    Check(Points(0, 4200) == 4200 && Points(3, 100) == 100 && Points(0, -5) == 0, "points: mana, energy, negative");
    Check(RuneMask({0, 0, 5000, 0, 1, 0}) == 0x2Bu, "rune mask: ready runes");
    Check(PowerWord(0) == "mana" && PowerWord(1) == "rage" && PowerWord(2) == "focus" && PowerWord(3) == "energy" &&
              PowerWord(6) == "runic" && PowerWord(4) == "power",
          "power words");
    Check(SHARE_WORDS[0] == "idle" && SHARE_WORDS[4] == "acting", "share words in CombatTally::Share's order");
    Check(VictimWord(Victim::Me) == "me" && VictimWord(Victim::Pet) == "pet" && VictimWord(Victim::Mate) == "mate" &&
              VictimWord(Victim::Other) == "other" && VictimWord(Victim::None) == "none",
          "victim words");
    Check(EnemyReason(true, false) == "target" && EnemyReason(true, true) == "both" &&
              EnemyReason(false, true) == "attacker",
          "enemy reasons");
    Check(Since(500, 200) == 300 && Since(100, 400) == 100, "taken since: runs on, starts again after a fight");
}
}  // namespace

int main()
{
    Kinds();
    Samples();
    std::printf("%d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
