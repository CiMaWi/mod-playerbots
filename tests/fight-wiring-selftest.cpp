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
// resurrections; the nearest graveyard. Joining (WP5): the waiting refusals (dead, in another fight, unseen, too far,
// no enemy in sight) and the kept answers (not own side, all red, roll), the always-joins (defence, group, master)
// and the followers that join only them, the chance's shape, grey joinable and red excluded, the join target's
// freshness, the book's once-per-fight answers (bounded) and the group joiner's word. Ending (WP6): a won roll is not
// lost while no enemy is in sight; the retreat's inputs and state, the chase over by time and by distance
// (aggression longer, courtesy shorter) and the leave-alone time, the calm-area cache (distance, time) and the
// ended ring's order; a joiner's reason to pick again.
#include <atomic>
#include <cstdint>
#include <deque>
#include <initializer_list>
#include <iostream>
#include <string>
#include <vector>

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
    Check(memory.ready, "ready stays (a constraint row, cleared by its order)");
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

// A bystander as the glue reads it: alive, it sees the fight 20 yd away, its side is in it
JoinInput Bystander()
{
    JoinInput input;
    input.yards = 20.0f;
    return input;
}

// Enemy participants 10, 15, 20 ... yd away with guids 100, 101, ...
std::vector<JoinEnemy> Enemies(std::initializer_list<Colour> colours)
{
    std::vector<JoinEnemy> enemies;
    for (Colour const colour : colours)
    {
        JoinEnemy enemy;
        enemy.guid = 100 + enemies.size();
        enemy.yards = 10.0f + 5.0f * static_cast<float>(enemies.size());
        enemy.colour = colour;
        enemies.push_back(enemy);
    }
    return enemies;
}

constexpr uint32_t FIGHT = 7;

void CheckJoinWaits()
{
    FightMemory const solo = Ready(LOGIN + HOUR);
    std::vector<JoinEnemy> const yellow = Enemies({Colour::Yellow});

    JoinInput dead = Bystander();
    dead.alive = false;
    JoinAnswer answer = AnswerJoin(dead, yellow, solo, FIGHT, 0.0f, VALUES);
    Check(answer.decision.refusal == Refusal::Dead && !answer.decided && !answer.decision.join,
          "a dead bystander waits");

    FightMemory busy = solo;
    busy.fightId = FIGHT + 1;
    answer = AnswerJoin(Bystander(), yellow, busy, FIGHT, 0.0f, VALUES);
    Check(answer.decision.refusal == Refusal::InFight && !answer.decided, "a bystander in another fight waits");
    busy.fightId = FIGHT;
    Check(AnswerJoin(Bystander(), yellow, busy, FIGHT, 0.0f, VALUES).decision.join, "its own fight is no other fight");

    JoinInput unseen = Bystander();
    unseen.visible = false;
    answer = AnswerJoin(unseen, yellow, solo, FIGHT, 0.0f, VALUES);
    Check(answer.decision.refusal == Refusal::Unseen && !answer.decided, "a bystander that cannot see the fight waits");

    JoinInput far = Bystander();
    far.yards = VALUES.joinYards + 1.0f;
    answer = AnswerJoin(far, yellow, solo, FIGHT, 0.0f, VALUES);
    Check(answer.decision.refusal == Refusal::TooFar && !answer.decided, "a bystander beyond JoinYards waits");

    std::vector<JoinEnemy> hidden = yellow;
    hidden[0].visible = false;
    answer = AnswerJoin(Bystander(), hidden, solo, FIGHT, 0.0f, VALUES);
    Check(answer.decision.refusal == Refusal::Unseen && !answer.decided && !answer.target,
          "a join with no enemy in sight waits as unseen");
}

void CheckJoinAnswers()
{
    FightMemory const solo = Ready(LOGIN + HOUR);

    JoinInput alone = Bystander();
    alone.ownSide = false;
    JoinAnswer answer = AnswerJoin(alone, Enemies({Colour::Yellow}), solo, FIGHT, 0.0f, VALUES);
    Check(answer.decision.refusal == Refusal::NotOwnSide && answer.decided, "a fight without its side: kept pass");

    answer = AnswerJoin(Bystander(), Enemies({Colour::Red, Colour::Red}), solo, FIGHT, 0.0f, VALUES);
    Check(answer.decision.refusal == Refusal::AllRed && answer.decided && answer.pickable == 0,
          "every enemy red: it stays out");

    answer = AnswerJoin(Bystander(), Enemies({Colour::Yellow}), solo, FIGHT, 0.999f, VALUES);
    Check(answer.decision.refusal == Refusal::Roll && answer.decided && !answer.decision.join && answer.target == 0,
          "a lost roll is a kept pass");

    answer = AnswerJoin(Bystander(), Enemies({Colour::Yellow}), solo, FIGHT, 0.0f, VALUES);
    Check(answer.decision.join && answer.decision.why == JoinWhy::Chance && answer.decided && answer.target == 100,
          "a won roll joins on the enemy");

    answer = AnswerJoin(Bystander(), Enemies({Colour::Grey}), solo, FIGHT, 0.0f, VALUES);
    Check(answer.decision.join && answer.target == 100, "a grey enemy may be joined on (no lower-level limit)");

    answer = AnswerJoin(Bystander(), Enemies({Colour::Red, Colour::Yellow}), solo, FIGHT, 0.0f, VALUES);
    Check(answer.decision.join && answer.target == 101 && answer.pickable == 1,
          "a chance join never picks the red enemy, even the nearer one");
}

void CheckJoinAlways()
{
    FightMemory const solo = Ready(LOGIN + HOUR);
    std::vector<JoinEnemy> mixed = Enemies({Colour::Red, Colour::Yellow, Colour::Green});
    mixed[1].attacking = true;

    JoinInput attacked = Bystander();
    attacked.attacked = true;
    JoinAnswer answer = AnswerJoin(attacked, mixed, solo, FIGHT, 0.999f, VALUES);
    Check(answer.decision.join && answer.decision.why == JoinWhy::Defence && answer.decision.p == 1.0f &&
              answer.target == 101,
          "an attacked bot defends itself against its attacker");

    JoinInput group = Bystander();
    group.groupFight = true;
    answer = AnswerJoin(group, Enemies({Colour::Red}), solo, FIGHT, 0.999f, VALUES);
    Check(answer.decision.join && answer.decision.why == JoinWhy::Group && answer.target == 100,
          "its group's fight: it joins, a red enemy too");

    JoinInput master = Bystander();
    master.masterFight = true;
    answer = AnswerJoin(master, Enemies({Colour::Yellow}), solo, FIGHT, 0.999f, VALUES);
    Check(answer.decision.join && answer.decision.why == JoinWhy::Master, "its real master's fight: it joins");

    GateFacts real;
    real.realMaster = true;
    FightMemory const follower = Ready(LOGIN + HOUR, real);
    answer = AnswerJoin(Bystander(), Enemies({Colour::Yellow}), follower, FIGHT, 0.0f, VALUES);
    Check(answer.decision.refusal == Refusal::RealPlayer && !answer.decision.join && !answer.decided,
          "a bot with a real master joins nothing by chance and waits for the master");
    answer = AnswerJoin(master, Enemies({Colour::Yellow}), follower, FIGHT, 0.999f, VALUES);
    Check(answer.decision.join && answer.decision.why == JoinWhy::Master, "...and joins the master's fight");

    GateFacts member;
    member.inGroup = true;
    FightMemory const bot = Ready(LOGIN + HOUR, member);
    answer = AnswerJoin(Bystander(), Enemies({Colour::Yellow}), bot, FIGHT, 0.0f, VALUES);
    Check(answer.decision.refusal == Refusal::NotLeader && !answer.decided,
          "a bot group's member waits for its leader");
    answer = AnswerJoin(group, Enemies({Colour::Yellow}), bot, FIGHT, 0.999f, VALUES);
    Check(answer.decision.join && answer.decision.why == JoinWhy::Group, "...and joins its group's fight");

    GateFacts prohibited;
    prohibited.prohibited = true;
    answer = AnswerJoin(Bystander(), Enemies({Colour::Yellow}), Ready(LOGIN + HOUR, prohibited), FIGHT, 0.0f, VALUES);
    Check(answer.decision.refusal == Refusal::NotOpenWorld && !answer.decided, "a prohibited zone: no chance join");

    JoinInput deadMember = group;
    deadMember.alive = false;
    Check(AnswerJoin(deadMember, Enemies({Colour::Yellow}), bot, FIGHT, 0.0f, VALUES).decision.refusal == Refusal::Dead,
          "a dead member does not join its group's fight");
}

void CheckJoinChance()
{
    auto const p = [](JoinInput const& input, GateFacts const& facts)
    {
        return AnswerJoin(input, Enemies({Colour::Yellow}), Ready(LOGIN + HOUR, facts), FIGHT, 0.999f, VALUES)
            .decision.p;
    };
    GateFacts daring;
    daring.aggression = 1.0f;
    GateFacts meek;
    meek.aggression = -1.0f;
    Check(p(Bystander(), daring) > p(Bystander(), GateFacts{}) && p(Bystander(), GateFacts{}) > p(Bystander(), meek),
          "the chance rises with aggression");

    JoinInput losing = Bystander();
    losing.odds = 0.3f;
    GateFacts helpful;
    helpful.helpfulness = 1.0f;
    GateFacts selfish;
    selfish.helpfulness = -1.0f;
    GateFacts brave;
    brave.caution = -1.0f;
    Check(p(losing, helpful) > p(losing, selfish), "helpfulness raises it when its side is losing");
    JoinInput targeted = Bystander();
    targeted.factionTargeted = true;
    Check(p(targeted, helpful) > p(Bystander(), helpful), "helpfulness raises it when a player of its faction is hit");
    GateFacts careful;
    careful.caution = 1.0f;
    Check(p(losing, careful) < p(losing, brave) && p(losing, GateFacts{}) < p(Bystander(), GateFacts{}),
          "bad odds and caution lower it");

    JoinInput goal = Bystander();
    goal.goalValue = 1.0f;
    Check(p(goal, GateFacts{}) == p(Bystander(), GateFacts{}), "the goal's value stays neutral (WP7b)");
}

void CheckJoinMemory()
{
    uint32_t const now = LOGIN + HOUR;
    FightMemory memory = Ready(now);
    JoinInput group = Bystander();
    group.groupFight = true;
    JoinAnswer answer = AnswerJoin(group, Enemies({Colour::Yellow}), memory, FIGHT, 0.0f, VALUES);
    NoteJoin(memory, answer, FIGHT, now);
    Check(memory.joinFightId == FIGHT && memory.joinCameIn == CameIn::Group && IsJoinTarget(memory, 100, now),
          "a group join keeps its target, fight and word");
    Check(IsJoinTarget(memory, 100, now + JOIN_TARGET_MS - 1) && !IsJoinTarget(memory, 100, now + JOIN_TARGET_MS),
          "the join target stays fresh JOIN_TARGET_MS");
    Check(!IsJoinTarget(memory, 101, now) && !IsJoinTarget(memory, 0, now), "only the join target");
    Check(!IsPicked(memory, 100, now), "a join is no start pick");

    answer = AnswerJoin(Bystander(), Enemies({Colour::Yellow}), memory, FIGHT, 0.0f, VALUES);
    NoteJoin(memory, answer, FIGHT, now);
    Check(memory.joinCameIn == CameIn::Joined, "a chance join comes in as joined");
    Check(PickFor(JoinWhy::Master) == Pick::Group && PickFor(JoinWhy::Chance) == Pick::Join &&
              CameInFor(JoinWhy::Master) == CameIn::Master && CameInFor(JoinWhy::Defence) == CameIn::Joined,
          "the pick and word per reason");
    Check(std::string(Word(JoinWhy::Group)) == "group" && std::string(Word(JoinWhy::Chance)) == "chance",
          "the join words");
}

void CheckJoinBook()
{
    std::atomic<uint32_t> ids{0};
    FightBook book(FightBounds::From(WorldPvpConfig{}), ids);
    uint32_t const now = LOGIN;
    Combatant alliance;
    alliance.guid = 1;
    Combatant horde;
    horde.guid = 2;
    horde.side = Side::Horde;
    uint32_t const fightId = book.OnCombat(alliance, horde, now).fightId;
    Check(fightId != 0 && book.NoteDecided(fightId, 50) && book.Active().front().HasDecided(50),
          "the book keeps a bystander's answer");
    Check(!book.NoteDecided(fightId + 99, 50), "no answer for a fight that is gone");
    for (uint64_t guid = 51; book.Active().front().decidedCount < MAX_DECIDED; ++guid)
        book.NoteDecided(fightId, guid);
    Check(!book.NoteDecided(fightId, 999) && book.RefusedDecisions() == 0 && book.NoteDecided(fightId, 50),
          "a full decided list takes no more answers");
    book.CountRefusedDecision(fightId);
    Check(book.RefusedDecisions() == 1 && book.Active().front().refusedDecisions == 1,
          "a bystander beyond a full list is counted for the book and the fight");

    Combatant member;
    member.guid = 3;
    member.joinedAs = CameIn::Group;
    CombatNote note = book.OnCombat(member, horde, now + SECOND);
    Check(note.joinCount == 1 && note.joins[0].cameIn == CameIn::Group &&
              book.Active().front().Find(3)->cameIn == CameIn::Group,
          "a group joiner that attacks into the fight comes in as group");
    Combatant passer;
    passer.guid = 4;
    note = book.OnCombat(horde, passer, now + SECOND);
    Check(note.joinCount == 1 && note.joins[0].cameIn == CameIn::Targeted, "a player a participant attacks: targeted");
    Combatant chance;
    chance.guid = 5;
    note = book.OnCombat(chance, horde, now + SECOND);
    Check(note.joinCount == 1 && note.joins[0].cameIn == CameIn::Joined, "a chance joiner: joined");
}

void CheckJoinSureRoll()
{
    FightMemory const solo = Ready(LOGIN + HOUR);
    std::vector<JoinEnemy> hidden = Enemies({Colour::Yellow});
    hidden[0].visible = false;
    JoinAnswer answer = AnswerJoin(Bystander(), hidden, solo, FIGHT, 0.999f, VALUES);
    Check(answer.decision.refusal == Refusal::Unseen && !answer.decided,
          "no enemy in sight: the bystander waits before rolling (a bad roll is not kept)");
    answer = AnswerJoin(Bystander(), Enemies({Colour::Yellow}), solo, FIGHT, 0.0f, VALUES);
    Check(answer.decision.join && answer.target == 100, "once an enemy is in sight the roll can still join");
    answer = AnswerJoin(Bystander(), Enemies({Colour::Yellow}), solo, FIGHT, 0.999f, VALUES);
    Check(answer.decision.refusal == Refusal::Roll && answer.decided, "with an enemy in sight a lost roll is kept");
    Check(JoinWhyOf(CameIn::Group) == JoinWhy::Group && JoinWhyOf(CameIn::Master) == JoinWhy::Master &&
              JoinWhyOf(CameIn::Joined) == JoinWhy::Chance,
          "a joiner picks again for the reason it came in");
}

void CheckRetreat()
{
    Check(ShouldRetreat(0.2f, OddsShare(1.0f, 3.0f), 0.0f, VALUES), "low health, losing: retreat");
    Check(!ShouldRetreat(0.2f, OddsShare(3.0f, 1.0f), 0.0f, VALUES), "low health, ahead: no retreat");
    Check(!ShouldRetreat(0.5f, OddsShare(1.0f, 3.0f), 0.0f, VALUES), "losing at half health: no retreat");
    Check(ShouldRetreat(0.4f, OddsShare(1.0f, 3.0f), 1.0f, VALUES) &&
              !ShouldRetreat(0.4f, OddsShare(1.0f, 3.0f), -1.0f, VALUES),
          "a careful bot retreats earlier than a daring one");

    uint32_t const now = LOGIN + HOUR;
    FightMemory memory = Ready(now);
    memory.fightId = FIGHT;
    memory.startGuid = ENEMY;
    memory.joinGuid = ENEMY;
    StartRetreat(memory, FIGHT, false, 10.0f, 20.0f, now);
    Check(Retreating(memory, now + RETREAT_MS - 1) && !Retreating(memory, now + RETREAT_MS),
          "a retreat lasts RETREAT_MS");
    Check(!memory.disengaging && memory.retreatX == 10.0f && memory.retreatY == 20.0f && !memory.startGuid &&
              !memory.joinGuid,
          "a retreat drops the picked and joined targets and keeps where the threat is");
    Check(RetreatedFromFight(memory), "the bot is in the fight it retreated from");
    memory.fightId = 0;
    Check(!RetreatedFromFight(memory), "once that fight ended it is not");
    StartRetreat(memory, FIGHT, true, 0.0f, 0.0f, now);
    Check(memory.disengaging && Retreating(memory, now), "a disengage is a retreat that notes no combat");
}

// The review fixes: who disengages, the master rule, a fight left behind on another map, a retreat given up, the
// leave-alone gate, the probe's fight lookup
void CheckEndGates()
{
    Check(Disengages(EndCause::Timeout, true, true, false) && Disengages(EndCause::Stalemate, true, true, false) &&
              Disengages(EndCause::StalemateCap, true, true, false),
          "timeout, stalemate and stalemate_cap disengage a bot by the rules in PvP combat");
    Check(!Disengages(EndCause::Wiped, true, true, false) && !Disengages(EndCause::CombatOver, true, true, false),
          "wiped and combat_over disengage nobody");
    Check(!Disengages(EndCause::Stalemate, false, true, false),
          "a non-persistent bot or PvpTest fighter does not disengage (not by the rules)");
    Check(!Disengages(EndCause::Stalemate, true, false, false), "a bot out of PvP combat has nothing to disengage");
    Check(!Disengages(EndCause::Stalemate, true, true, true), "a bot whose master fights on does not disengage");

    constexpr uint64_t MASTER = 5;
    Fight fight;
    FightParticipant master;
    master.guid = MASTER;
    master.inCombat = true;
    fight.participants.push_back(master);
    Check(FollowsMaster(fight, MASTER), "the real master fights in the fight: the bot follows it");
    fight.participants.front().inCombat = false;
    Check(!FollowsMaster(fight, MASTER), "the master out of PvP combat: the bot decides for itself");
    Check(!FollowsMaster(fight, MASTER + 1) && !FollowsMaster(fight, 0),
          "a master outside the fight, or none: the bot decides for itself");

    uint32_t const now = LOGIN + HOUR;
    FightMemory memory = Ready(now);
    memory.fightId = FIGHT;
    ForgetFightElsewhere(memory, true, now);
    Check(memory.fightId == FIGHT, "a fight the map's book holds stays the bot's");
    StartRetreat(memory, FIGHT, false, 0.0f, 0.0f, now);
    ForgetFightElsewhere(memory, false, now + MINUTE);
    Check(!memory.fightId && memory.hasFought && memory.lastFightEndMs == now + MINUTE,
          "a fight left on another map is over for the bot (the gap counts from now)");
    Check(!RetreatedFromFight(memory) && Gate(memory, now + MINUTE) != Refusal::InFight,
          "so it is neither kept out of that fight nor in_fight");

    FightMemory rooted = Ready(now);
    rooted.fightId = FIGHT;
    StartRetreat(rooted, FIGHT, false, 0.0f, 0.0f, now);
    GiveUpRetreat(rooted);
    Check(!Retreating(rooted, now + SECOND), "a retreat that cannot move is given up");
    Check(RetreatedFromFight(rooted), "after it the bot still does not rejoin that fight");
    Check(LeavesAlone(rooted, ENEMY, false, now) && !LeavesAlone(rooted, ENEMY, true, now),
          "it leaves the fight's enemies alone, except one attacking it");
    Check(!LeavesAlone(Ready(now), ENEMY, false, now), "a bot with neither state leaves nobody alone");

    std::vector<Fight> active(3);
    for (size_t index = 0; index < active.size(); ++index)
        for (uint64_t guid = 1; guid <= 3; ++guid)
        {
            FightParticipant participant;
            participant.guid = index * 10 + guid;
            active[index].participants.push_back(participant);
        }
    size_t last = 0;
    Check(FightHolding(active, active[0].participants[2], last) == &active[0] && last == 0,
          "the probe finds the first fight's participant");
    Check(FightHolding(active, active[2].participants[0], last) == &active[2] && last == 2,
          "and the next fight's, remembering it");
    Check(FightHolding(active, active[2].participants[1], last) == &active[2], "the remembered fight first");
    FightParticipant stranger;
    Check(!FightHolding(active, stranger, last), "a participant of no active fight: none");
}

void CheckChase()
{
    uint32_t const now = LOGIN + HOUR;
    ChaseLimit const limit = ChaseLimitFor(0.0f, 0.0f, VALUES);
    Check(limit.ms == 15 * SECOND && limit.yards == 40.0f, "the plain chase: 15 s, 40 yd beyond the fight");
    Check(ChaseLimitFor(1.0f, 0.0f, VALUES).ms > limit.ms && ChaseLimitFor(0.0f, 1.0f, VALUES).ms < limit.ms,
          "aggression lengthens the chase, courtesy shortens it");

    FightMemory memory = Ready(now);
    NoteChase(memory, ENEMY, 0.0f, 0.0f, 40.0f, now);
    Check(!CheckChase(memory, ENEMY + 1, 0.0f, 0.0f, now + 20 * SECOND, limit), "another target is no chase");
    NoteChase(memory, ENEMY, 50.0f, 50.0f, 40.0f, now + 5 * SECOND);
    Check(memory.chaseStartMs == now && memory.chaseX == 0.0f, "a running chase is not restarted");
    Check(!CheckChase(memory, ENEMY, 70.0f, 0.0f, now + limit.ms - 1, limit), "inside the limits the chase goes on");
    Check(CheckChase(memory, ENEMY, 70.0f, 0.0f, now + limit.ms, limit) && memory.chaseOver,
          "the chase is over by time");
    Check(ChaseLeftAlone(memory, ENEMY, now + limit.ms + LEFT_ALONE_MS - 1) &&
              !ChaseLeftAlone(memory, ENEMY, now + limit.ms + LEFT_ALONE_MS),
          "the winner leaves it alone for LEFT_ALONE_MS");
    NoteChase(memory, ENEMY, 0.0f, 0.0f, 40.0f, now + limit.ms + SECOND);
    Check(memory.chaseOver, "no new chase of it while left alone");

    FightMemory far = Ready(now);
    NoteChase(far, ENEMY, 0.0f, 0.0f, 40.0f, now);
    Check(!CheckChase(far, ENEMY, 79.0f, 0.0f, now + SECOND, limit), "39 yd beyond the fight: on");
    Check(CheckChase(far, ENEMY, 80.0f, 0.0f, now + SECOND, limit), "40 yd beyond the fight: over by distance");

    FightMemory daring = Ready(now);
    NoteChase(daring, ENEMY, 0.0f, 0.0f, 40.0f, now);
    Check(!CheckChase(daring, ENEMY, 0.0f, 0.0f, now + limit.ms, ChaseLimitFor(1.0f, 0.0f, VALUES)),
          "an aggressive winner chases longer");

    FightMemory stale = Ready(now);
    NoteChase(stale, ENEMY, 0.0f, 0.0f, 40.0f, now);
    Check(!CheckChase(stale, ENEMY, 0.0f, 0.0f, now + limit.ms + LEFT_ALONE_MS, limit) && !stale.chaseGuid,
          "a chase not looked at for its limit and the leave-alone time is forgotten");
}

void CheckCalm()
{
    uint32_t const now = LOGIN + HOUR;
    FightMemory memory = Ready(now);
    Check(CalmDue(memory, 0.0f, 0.0f, now), "an unknown calm answer is due");
    NoteCalm(memory, true, 0.0f, 0.0f, now);
    Check(!CalmDue(memory, 10.0f, 0.0f, now + GATE_REFRESH_MS - 1), "kept a while at about the same place");
    Check(CalmDue(memory, 0.0f, 0.0f, now + GATE_REFRESH_MS), "asked again after GATE_REFRESH_MS");
    Check(CalmDue(memory, CALM_MOVE_YARDS + 1.0f, 0.0f, now + SECOND), "asked again after a move");
    Check(Gate(memory, now) == Refusal::Calm, "a bot in a calm area starts nothing");
    NoteCalm(memory, false, 0.0f, 0.0f, now);
    Check(Gate(memory, now) == Refusal::None, "outside it the gate opens");

    std::deque<EndedFight> ring;
    EndedFight old;
    old.endMs = now - VALUES.calmMs - SECOND;
    old.x = 1000.0f;
    ring.push_back(old);
    EndedFight fresh;
    fresh.endMs = now - SECOND;
    ring.push_back(fresh);
    Check(InAnyCalmArea(ring, VALUES.calmYards, 0.0f, now, VALUES), "within CalmYards of a fresh end: calm");
    Check(!InAnyCalmArea(ring, VALUES.calmYards + 1.0f, 0.0f, now, VALUES), "beyond CalmYards: not calm");
    Check(!InAnyCalmArea(ring, 1000.0f, 0.0f, now, VALUES), "an end older than CalmMinutes: not calm");
}

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
    CheckJoinWaits();
    CheckJoinAnswers();
    CheckJoinAlways();
    CheckJoinChance();
    CheckJoinMemory();
    CheckJoinBook();
    CheckJoinSureRoll();
    CheckRetreat();
    CheckEndGates();
    CheckChase();
    CheckCalm();
    std::cout << "fight-wiring-selftest: " << passed << " passed, " << failed << " failed\n";
    return failed == 0 ? 0 : 1;
}
