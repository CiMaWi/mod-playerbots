/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// Standalone check of the world PvP start wiring (src/Ai/World/Pvp/FightWiring.h) over synthetic snapshots: who
// starts by the rules (PvpTest's forced fighters, battlegrounds and non-persistent bots do not); each refusal word
// reached through the glue's mapping (not open world, real player, group member, in fight, graveyard of the bot and
// of the target, peaceable, gap, grey, red, resurrected, rolled recently, roll); calm and the objective stay neutral;
// a ready bot's chance starts; the chance clock and the rolled-at time change only on a chance; the first chance
// counts from login; orange by caution; the picked target's freshness; the fight's decided list and the book's
// resurrections; the nearest graveyard.
#include <cstdint>
#include <iostream>
#include <string>

#include "FightWiring.h"

using namespace WorldPvp;

namespace
{
constexpr uint32_t SECOND = 1000;
constexpr uint32_t MINUTE = 60 * SECOND;
constexpr uint32_t HOUR = 60 * MINUTE;
constexpr uint64_t ENEMY = 77;
// A server clock well past its start, so "never" and "long ago" differ from small values
constexpr uint32_t LOGIN = 10 * HOUR;

int passed = 0;
int failed = 0;

void Check(bool condition, std::string const& name)
{
    if (condition)
    {
        ++passed;
        return;
    }

    ++failed;
    std::cout << "FAIL: " << name << "\n";
}

FightRuleValues const VALUES{};

// A bot that could start: open world, its own leader, aggression 0, logged in at LOGIN, its gate read at `nowMs`
FightMemory Ready(uint32_t nowMs, GateFacts facts = GateFacts{})
{
    FightMemory memory;
    memory.persistent = true;
    memory.bornMs = LOGIN;
    RefreshGate(memory, facts, nowMs);
    memory.Remember(ENEMY, nowMs, 16);
    return memory;
}

TargetFacts Yellow()
{
    TargetFacts facts;
    facts.botLevel = 40;
    facts.targetLevel = 40;
    return facts;
}

Refusal Gate(FightMemory const& memory, uint32_t nowMs) { return GateOf(memory, nowMs, VALUES); }

Refusal Decide(FightMemory& memory, TargetFacts const& facts, uint32_t nowMs, float roll = 0.99f)
{
    StartBot const bot = MakeStartBot(memory, nowMs);
    StartTarget const target = MakeStartTarget(facts, memory, ENEMY, nowMs);
    return DecideMeeting(memory, bot, target, ENEMY, roll, nowMs, VALUES).refusal;
}

void CheckWho()
{
    BotPlace place;
    place.persistent = true;
    place.hasBook = true;
    Check(StartsByRules(place), "a persistent bot in the open world starts by the rules");
    place.forced = true;
    Check(!StartsByRules(place), "a PvpTest fighter keeps today's path");
    place.forced = false;
    place.battleground = true;
    Check(!StartsByRules(place), "a battleground or arena keeps today's path");
    place.battleground = false;
    place.hasBook = false;
    Check(!StartsByRules(place), "a dungeon keeps today's path");
    place.hasBook = true;
    place.persistent = false;
    Check(!StartsByRules(place), "a non-persistent bot keeps today's path");
}

void CheckBotRefusals()
{
    uint32_t const now = LOGIN + HOUR;
    Check(Gate(Ready(now), now) == Refusal::None, "the open gate");

    GateFacts facts;
    facts.prohibited = true;
    Check(Gate(Ready(now, facts), now) == Refusal::NotOpenWorld, "a prohibited zone: not_open_world");

    facts = GateFacts{};
    facts.realMaster = true;
    Check(Gate(Ready(now, facts), now) == Refusal::RealPlayer, "a real master: real_player");
    facts = GateFacts{};
    facts.inGroup = true;
    facts.realInGroup = true;
    facts.leader = true;
    Check(Gate(Ready(now, facts), now) == Refusal::RealPlayer, "a real player in the group: real_player");

    facts = GateFacts{};
    facts.inGroup = true;
    Check(Gate(Ready(now, facts), now) == Refusal::NotLeader, "a bot group's member: not_leader");
    facts.leader = true;
    Check(Gate(Ready(now, facts), now) == Refusal::None, "a bot group's leader decides");

    FightMemory memory = Ready(now);
    memory.fightId = 5;
    Check(Gate(memory, now) == Refusal::InFight, "in a fight: in_fight");

    facts = GateFacts{};
    facts.graveyardYards = 79.0f;
    Check(Gate(Ready(now, facts), now) == Refusal::Graveyard, "the bot within 80 yd of a graveyard: graveyard");
    facts.graveyardYards = 81.0f;
    Check(Gate(Ready(now, facts), now) == Refusal::None, "the bot beyond 80 yd of a graveyard");

    facts = GateFacts{};
    facts.aggression = -0.6f;
    Check(Gate(Ready(now, facts), now) == Refusal::Peaceable, "aggression below -0.5: peaceable");

    memory = Ready(now);
    memory.hasFought = true;
    memory.lastFightEndMs = now - 29 * MINUTE;
    Check(Gate(memory, now) == Refusal::Gap, "a fight ended 29 min ago: gap");
    memory.lastFightEndMs = now - 31 * MINUTE;
    Check(Gate(memory, now) == Refusal::None, "a fight ended 31 min ago: no gap");

    StartBot const bot = MakeStartBot(Ready(now), now);
    Check(!bot.inCalmArea && !bot.atObjective, "calm area (WP6) and objective (WP10) stay neutral");
}

void CheckTargetRefusals()
{
    uint32_t const now = LOGIN + HOUR;
    FightMemory memory = Ready(now);
    TargetFacts facts = Yellow();
    facts.enemy = false;
    Check(Decide(memory, facts, now) == Refusal::NotTarget, "today's target check failed: not_target");

    facts = Yellow();
    facts.targetLevel = 30;
    Check(Decide(memory, facts, now) == Refusal::Grey, "a grey target: grey");
    facts.targetLevel = 45;
    Check(Decide(memory, facts, now) == Refusal::Red, "a red target: red");

    facts = Yellow();
    facts.sinceResurrectionMs = 4 * MINUTE;
    Check(Decide(memory, facts, now) == Refusal::Resurrected, "resurrected 4 min ago: resurrected");
    facts.sinceResurrectionMs = NEVER_MS;
    facts.graveyardYards = 50.0f;
    Check(Decide(memory, facts, now) == Refusal::Graveyard, "the target near a graveyard: graveyard");
    Check(!memory.hasChance, "no chance after hard-rule refusals");
    Check(memory.SinceRolled(ENEMY, now) == NEVER_MS, "no roll mark after hard-rule refusals");
}

void CheckChanceClock()
{
    uint32_t const now = LOGIN + HOUR;
    FightMemory memory = Ready(now);
    StartBot bot = MakeStartBot(memory, now);
    Check(bot.sinceChanceMs == HOUR, "the first chance counts from login");
    Check(bot.sinceFightEndMs == NEVER_MS, "no fight yet: never");

    Check(Decide(memory, Yellow(), now, 0.99f) == Refusal::Roll, "a chance that loses its roll: roll");
    Check(memory.hasChance && memory.lastChanceMs == now, "a chance restarts the chance clock");
    Check(memory.SinceRolled(ENEMY, now) == 0, "a chance marks the target rolled");
    Check(memory.startGuid == 0, "a lost roll picks nobody");
    Check(MakeStartBot(memory, now + MINUTE).sinceChanceMs == MINUTE, "the chance clock runs from the chance");

    Check(Decide(memory, Yellow(), now + 5 * MINUTE, 0.0f) == Refusal::RolledRecently,
          "rolled 5 min ago: rolled_recently");
    Check(memory.lastChanceMs == now, "rolled_recently is no chance");

    uint32_t const later = now + 11 * MINUTE;
    Check(Decide(memory, Yellow(), later, 0.0f) == Refusal::None, "a roll below p starts");
    Check(memory.startGuid == ENEMY && memory.startReason == FightReason::Budget, "a start picks the target: budget");
    Check(IsPicked(memory, ENEMY, later + START_TARGET_MS - 1), "the pick is fresh within START_TARGET_MS");
    Check(!IsPicked(memory, ENEMY, later + START_TARGET_MS), "the pick expires");
    Check(!IsPicked(memory, ENEMY + 1, later), "another enemy is not picked");
}

void CheckReady()
{
    uint32_t const now = LOGIN + HOUR;
    GateFacts facts;
    facts.aggression = -0.9f;
    FightMemory memory = Ready(now, facts);
    Check(Gate(memory, now) == Refusal::Peaceable, "a peaceable bot is gated");
    memory.ready = true;
    Check(Gate(memory, now) == Refusal::None, "a ready bot is not peaceable");
    TargetFacts grey = Yellow();
    grey.targetLevel = 30;
    Check(Decide(memory, grey, now) == Refusal::Grey && memory.ready, "ready does not pass a hard rule");
    TargetFacts orange = Yellow();
    orange.targetLevel = 43;
    memory.caution = 1.0f;
    StartBot const bot = MakeStartBot(memory, now);
    StartTarget const target = MakeStartTarget(orange, memory, ENEMY, now);
    StartDecision const decision = DecideMeeting(memory, bot, target, ENEMY, 0.99f, now, VALUES);
    Check(decision.start && decision.p == 1.0f, "a ready bot's chance starts (p 1)");
    Check(!memory.ready, "the chance clears ready");
}

void CheckOrangeByCaution()
{
    uint32_t const now = LOGIN + 2 * HOUR;
    TargetFacts orange = Yellow();
    orange.targetLevel = 43;
    GateFacts facts;
    facts.caution = -1.0f;
    FightMemory daring = Ready(now, facts);
    StartDecision const bold = DecideMeeting(daring, MakeStartBot(daring, now),
                                             MakeStartTarget(orange, daring, ENEMY, now), ENEMY, 0.99f, now, VALUES);
    facts.caution = 1.0f;
    FightMemory careful = Ready(now, facts);
    StartDecision const shy = DecideMeeting(careful, MakeStartBot(careful, now),
                                            MakeStartTarget(orange, careful, ENEMY, now), ENEMY, 0.99f, now, VALUES);
    Check(bold.chance && shy.chance, "orange is a chance for both");
    Check(bold.p > 0.0f && shy.p == 0.0f, "orange: the daring bot may start, the careful one not");
}

void CheckOddsGate()
{
    uint32_t const now = LOGIN + HOUR;
    FightMemory memory = Ready(now);
    StartBot const bot = MakeStartBot(memory, now);
    StartTarget target = MakeStartTarget(Yellow(), memory, ENEMY, now);
    Check(NeedsOdds(bot, target, VALUES), "a meeting that reaches the roll needs the odds");
    target.colour = Colour::Grey;
    Check(!NeedsOdds(bot, target, VALUES), "a refused meeting needs no odds");
}

void CheckBookAdditions()
{
    Fight fight;
    Check(!fight.HasDecided(1) && fight.NoteDecided(1) && fight.HasDecided(1), "a decided bystander is kept");
    for (uint64_t guid = 2; guid <= MAX_DECIDED; ++guid)
        fight.NoteDecided(guid);
    Check(fight.decidedCount == MAX_DECIDED && !fight.NoteDecided(MAX_DECIDED + 1), "the decided list is bounded");
    Check(fight.NoteDecided(1), "an already decided bystander stays decided in a full list");

    std::atomic<uint32_t> ids{0};
    FightBook book(FightBounds::From(WorldPvpConfig{}), ids);
    uint32_t const now = LOGIN;
    Check(book.ResurrectedMsAgo(ENEMY, now) == NEVER_MS, "never resurrected: never");
    book.NoteResurrection(ENEMY, 1.0f, 2.0f, now);
    Check(book.ResurrectedMsAgo(ENEMY, now + MINUTE) == MINUTE, "resurrected a minute ago");
    Check(book.ResurrectedMsAgo(ENEMY, now + 5 * MINUTE) == NEVER_MS, "a resurrection 5 min ago is forgotten");
    book.NoteResurrection(ENEMY, 1.0f, 2.0f, now + MINUTE);
    Check(book.Resurrections().size() == 1, "a player's latest resurrection only");
    for (uint64_t guid = 1000; guid < 1000 + MAX_RESURRECTIONS + 10; ++guid)
        book.NoteResurrection(guid, 0.0f, 0.0f, now + MINUTE);
    Check(book.Resurrections().size() == MAX_RESURRECTIONS, "the resurrections are bounded");

    FightMemory memory;
    memory.Remember(ENEMY, now, 16);
    Check(memory.SinceRolled(ENEMY, now) == NEVER_MS, "a new enemy was never rolled for");
    memory.MarkRolled(ENEMY, now);
    Check(memory.SinceRolled(ENEMY, now + SECOND) == SECOND, "the rolled time");
    Check(memory.SinceRolled(ENEMY + 1, now) == NEVER_MS, "an enemy not remembered: never");

    Combatant aggressor;
    Combatant other;
    Check(StartReason(aggressor, other) == FightReason::AttackOnSight, "a bot without a pick: attack_on_sight");
    aggressor.start = FightReason::Budget;
    Check(StartReason(aggressor, other) == FightReason::Budget, "a picked start: budget");
    other.forced = true;
    Check(StartReason(aggressor, other) == FightReason::PvpTest, "a PvpTest fighter still tags the start");
}

void CheckGraveyards()
{
    std::vector<Point> const points{{0.0f, 0.0f}, {100.0f, 0.0f}};
    Check(NearestYards(points, 90.0f, 0.0f) == 10.0f, "the nearest graveyard");
    Check(NearestYards({}, 0.0f, 0.0f) == FAR_YARDS, "no graveyard: far");
    FightMemory memory;
    Check(GateDue(memory, 0), "an unread gate is due");
    RefreshGate(memory, GateFacts{}, LOGIN);
    Check(!GateDue(memory, LOGIN + GATE_REFRESH_MS - 1) && GateDue(memory, LOGIN + GATE_REFRESH_MS),
          "the gate is read again after GATE_REFRESH_MS");
}
}  // namespace

int main()
{
    CheckWho();
    CheckBotRefusals();
    CheckTargetRefusals();
    CheckChanceClock();
    CheckReady();
    CheckOrangeByCaution();
    CheckOddsGate();
    CheckBookAdditions();
    CheckGraveyards();
    std::cout << "fight-wiring-selftest: " << passed << " passed, " << failed << " failed\n";
    return failed == 0 ? 0 : 1;
}
