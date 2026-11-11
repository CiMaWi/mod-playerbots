/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_FIGHTWIRING_H
#define _PLAYERBOT_FIGHTWIRING_H

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

#include "FightBookRules.h"
#include "FightRules.h"

// How the glue (FightBook.cpp) feeds the fight rules (FightRules.h) from what it reads off the players, and what a
// decision (a start at a meeting, WP4; a bystander's join, WP5; a retreat, a chase, a calm area, WP6) changes in the
// bot's fight memory. Player-free:
// tests/fight-wiring-selftest.cpp proves the mapping.
namespace WorldPvp
{
// The target the rules picked stays the scan's target this long (the bot closes in and attacks within it)
constexpr uint32_t START_TARGET_MS = 20 * MS_PER_SECOND;
// The bot-side gate values are read again after this long
constexpr uint32_t GATE_REFRESH_MS = 5 * MS_PER_SECOND;
// The fight book asks the bystanders of its active fights this often
constexpr uint32_t JOIN_CHECK_MS = 3 * MS_PER_SECOND;
// A joiner's target stays its target this long (it closes in and attacks within it)
constexpr uint32_t JOIN_TARGET_MS = 20 * MS_PER_SECOND;

// Where a bot is, for the choice between the rules and today's attack on sight
struct BotPlace
{
    bool persistent = false;
    // An open-world map (it has a fight book: not instanceable)
    bool hasBook = false;
    bool battleground = false;
    // A PvpTest fighter (WP.6)
    bool forced = false;
};

// A persistent bot on an open-world map outside battlegrounds and arenas, unless PvpTest forces it, starts fights by
// the rules; every other bot keeps today's attack on sight
constexpr bool StartsByRules(BotPlace const& place)
{
    return place.persistent && place.hasBook && !place.battleground && !place.forced;
}

// The bot-side values the gate caches, as the glue reads them
struct GateFacts
{
    bool prohibited = false;
    bool realMaster = false;
    bool realInGroup = false;
    bool inGroup = false;
    bool leader = false;
    float graveyardYards = FAR_YARDS;
    float aggression = 0.0f;
    float caution = 0.0f;
    float helpfulness = 0.0f;
    float courtesy = 0.0f;
    float patience = 0.0f;
};

inline bool GateDue(FightMemory const& memory, uint32_t nowMs)
{
    return !memory.gateKnown || nowMs - memory.gateMs >= GATE_REFRESH_MS;
}

inline void RefreshGate(FightMemory& memory, GateFacts const& facts, uint32_t nowMs)
{
    memory.gateKnown = true;
    memory.gateMs = nowMs;
    memory.openWorld = !facts.prohibited;
    memory.realPlayer = facts.realMaster || facts.realInGroup;
    memory.groupMember = facts.inGroup && !facts.leader;
    memory.graveyardYards = facts.graveyardYards;
    memory.aggression = facts.aggression;
    memory.caution = facts.caution;
    memory.helpfulness = facts.helpfulness;
    memory.courtesy = facts.courtesy;
    memory.patience = facts.patience;
}

// The bot's side of a meeting: the cached gate values and the memory's clocks
inline StartBot MakeStartBot(FightMemory const& memory, uint32_t nowMs)
{
    StartBot bot;
    bot.openWorld = memory.openWorld;
    bot.realPlayer = memory.realPlayer;
    bot.groupMember = memory.groupMember;
    bot.inFight = memory.fightId != 0;
    bot.atObjective = false;  // WP10: capture points
    bot.inCalmArea = memory.calmKnown && memory.calm;
    bot.sinceFightEndMs = memory.hasFought ? nowMs - memory.lastFightEndMs : NEVER_MS;
    bot.sinceChanceMs = nowMs - (memory.hasChance ? memory.lastChanceMs : memory.bornMs);
    bot.graveyardYards = memory.graveyardYards;
    // A ready bot (the lab's `fight ready`) is never peaceable: its next chance starts when the hard rules pass
    bot.aggression = memory.ready ? std::max(memory.aggression, 0.0f) : memory.aggression;
    bot.caution = memory.caution;
    bot.courtesy = memory.courtesy;
    return bot;
}

// The scan gate's word: Refusal::None when the bot could start a fight now
inline Refusal GateOf(FightMemory const& memory, uint32_t nowMs, FightRuleValues const& values)
{
    return StartGate(MakeStartBot(memory, nowMs), values);
}

// The enemy met, as the glue reads it
struct TargetFacts
{
    // Today's target checks passed: attackable, PvP flagged, visible, outside the exclusions
    bool enemy = true;
    uint32_t botLevel = 1;
    uint32_t targetLevel = 1;
    float health = 1.0f;
    // Its victim is a monster
    bool busy = false;
    float graveyardYards = FAR_YARDS;
    uint32_t sinceResurrectionMs = NEVER_MS;
};

inline StartTarget MakeStartTarget(TargetFacts const& facts, FightMemory const& memory, uint64_t guid, uint32_t nowMs)
{
    StartTarget target;
    target.enemy = facts.enemy;
    target.colour = ColourOf(facts.botLevel, facts.targetLevel);
    target.sinceResurrectionMs = facts.sinceResurrectionMs;
    target.graveyardYards = facts.graveyardYards;
    target.contestsObjective = false;  // WP10: capture points
    target.sinceRolledMs = memory.SinceRolled(guid, nowMs);
    target.busy = facts.busy;
    target.health = facts.health;
    target.grudge = 0.0f;  // WP9: grudges
    return target;
}

// Whether the meeting reaches the roll, so the glue counts the odds (a search of the players around) only then
inline bool NeedsOdds(StartBot const& bot, StartTarget const& target, FightRuleValues const& values)
{
    return DecideStart(bot, target, 1.0f, values).chance;
}

// The meeting's decision and what it changes in the memory: a chance restarts the chance clock and marks the target
// as rolled for, start or not; a ready bot's chance starts (p 1) and clears ready; a start picks the target
inline StartDecision DecideMeeting(FightMemory& memory, StartBot const& bot, StartTarget const& target, uint64_t guid,
                                   float roll, uint32_t nowMs, FightRuleValues const& values)
{
    StartDecision decision = DecideStart(bot, target, roll, values);
    if (decision.chance)
    {
        if (memory.ready)
        {
            decision.start = true;
            decision.p = 1.0f;
            decision.refusal = Refusal::None;
            memory.ready = false;
        }
        memory.hasChance = true;
        memory.lastChanceMs = nowMs;
        memory.MarkRolled(guid, nowMs);
    }
    if (decision.start)
    {
        memory.startGuid = guid;
        memory.startMs = nowMs;
        memory.startReason = decision.reason;
    }
    return decision;
}

// Whether `guid` is the target the rules picked for this bot, still fresh
inline bool IsPicked(FightMemory const& memory, uint64_t guid, uint32_t nowMs)
{
    return guid && memory.startGuid == guid && nowMs - memory.startMs < START_TARGET_MS;
}

// --- Joining (WP5) ---

// An enemy participant of the fight as the bystander sees it
struct JoinEnemy
{
    uint64_t guid = 0;
    float yards = FAR_YARDS;
    Colour colour = Colour::Yellow;
    bool visible = true;
    // It attacks the bystander
    bool attacking = false;
};

// What a bystander may pick for its reason: by chance not red, in self-defence and for its group or master anything
constexpr Pick PickFor(JoinWhy why)
{
    if (why == JoinWhy::Defence)
        return Pick::Defence;
    if (why == JoinWhy::Group || why == JoinWhy::Master)
        return Pick::Group;
    return Pick::Join;
}

// How a joiner comes into the fight when it attacks its target (the FightJoin word)
constexpr CameIn CameInFor(JoinWhy why)
{
    if (why == JoinWhy::Group)
        return CameIn::Group;
    if (why == JoinWhy::Master)
        return CameIn::Master;
    return CameIn::Joined;
}

// The enemies a bystander may pick by chance: not red (grey ones too: no lower-level limit when joining); whether it
// sees one is the target's matter
inline uint32_t Pickable(std::vector<JoinEnemy> const& enemies)
{
    return static_cast<uint32_t>(std::count_if(
        enemies.begin(), enemies.end(), [](JoinEnemy const& enemy) { return MayPick(enemy.colour, Pick::Join); }));
}

// The joiner's target: the nearest enemy it sees and may pick for its reason, in self-defence one attacking it first;
// 0 for none
inline uint64_t PickJoinTarget(std::vector<JoinEnemy> const& enemies, JoinWhy why)
{
    Pick const pick = PickFor(why);
    uint64_t best = 0;
    float bestYards = FAR_YARDS;
    bool bestAttacking = false;
    for (JoinEnemy const& enemy : enemies)
    {
        if (!enemy.visible || !MayPick(enemy.colour, pick))
            continue;
        bool const attacking = why == JoinWhy::Defence && enemy.attacking;
        if (best && (attacking < bestAttacking || (attacking == bestAttacking && enemy.yards >= bestYards)))
            continue;
        best = enemy.guid;
        bestYards = enemy.yards;
        bestAttacking = attacking;
    }
    return best;
}

// A bot that could not start a fight by its place or company joins none by chance: a prohibited zone, a real player as
// its master or in its group, a bot group it does not lead (WP.6: they join their group's or master's fight only)
inline Refusal FollowerRefusal(FightMemory const& memory)
{
    if (!memory.openWorld)
        return Refusal::NotOpenWorld;
    if (memory.realPlayer)
        return Refusal::RealPlayer;
    if (memory.groupMember)
        return Refusal::NotLeader;
    return Refusal::None;
}

// A refusal that may pass later (dead, in another fight, out of sight or reach, following someone who has not joined):
// no answer yet, the next check asks again. Every other answer is the bot's once for the fight (WP.7)
constexpr bool Waits(Refusal refusal)
{
    switch (refusal)
    {
        case Refusal::Dead:
        case Refusal::InFight:
        case Refusal::Unseen:
        case Refusal::TooFar:
        case Refusal::NotOpenWorld:
        case Refusal::RealPlayer:
        case Refusal::NotLeader:
            return true;
        default:
            return false;
    }
}

struct JoinAnswer
{
    JoinDecision decision;
    // The joiner's target (0 when it does not join)
    uint64_t target = 0;
    uint32_t pickable = 0;
    // An answer the fight keeps; false while the bot waits
    bool decided = false;
};

inline JoinAnswer AnswerFor(JoinInput const& input, std::vector<JoinEnemy> const& enemies, FightMemory const& memory,
                            float roll, FightRuleValues const& values)
{
    JoinAnswer answer;
    answer.pickable = input.pickable;
    answer.decision = DecideJoin(input, roll, values);
    bool const always = answer.decision.join && answer.decision.why != JoinWhy::Chance;
    Refusal const follower = FollowerRefusal(memory);
    if (!always && follower != Refusal::None && !Waits(answer.decision.refusal))
        answer.decision = JoinDecision{follower};
    if (answer.decision.join)
    {
        answer.target = PickJoinTarget(enemies, answer.decision.why);
        if (!answer.target)
            answer.decision = JoinDecision{Refusal::Unseen};
    }
    answer.decided = !Waits(answer.decision.refusal);
    return answer;
}

// A bystander's answer for fight `fightId`: the input the glue read off the players, completed from the memory (the
// fight it is in, the traits of the gate's cache) and the enemies; a join without a target it sees waits as unseen.
// A bot that would join at the best roll but sees no enemy it may pick waits before rolling, so no roll is spent
// while the enemies are out of sight
inline JoinAnswer AnswerJoin(JoinInput input, std::vector<JoinEnemy> const& enemies, FightMemory const& memory,
                             uint32_t fightId, float roll, FightRuleValues const& values)
{
    input.inOtherFight = memory.fightId != 0 && memory.fightId != fightId;
    input.pickable = Pickable(enemies);
    input.goalValue = 0.0f;  // WP7b: the goal's value
    input.aggression = memory.aggression;
    input.helpfulness = memory.helpfulness;
    input.caution = memory.caution;

    JoinAnswer const sure = AnswerFor(input, enemies, memory, 0.0f, values);
    if (sure.decision.refusal == Refusal::Unseen)
        return sure;
    return AnswerFor(input, enemies, memory, roll, values);
}

// A join gives the bot its target in the fight and how it comes in
inline void NoteJoin(FightMemory& memory, JoinAnswer const& answer, uint32_t fightId, uint32_t nowMs)
{
    memory.joinGuid = answer.target;
    memory.joinFightId = fightId;
    memory.joinMs = nowMs;
    memory.joinCameIn = CameInFor(answer.decision.why);
}

// Whether `guid` is the target of the fight the bot decided to join, still fresh
inline bool IsJoinTarget(FightMemory const& memory, uint64_t guid, uint32_t nowMs)
{
    return guid && memory.joinGuid == guid && nowMs - memory.joinMs < JOIN_TARGET_MS;
}

// The reason a joiner picks a new target in its fight by (its first one's dead or gone): as it came in
constexpr JoinWhy JoinWhyOf(CameIn cameIn)
{
    if (cameIn == CameIn::Group)
        return JoinWhy::Group;
    if (cameIn == CameIn::Master)
        return JoinWhy::Master;
    return JoinWhy::Chance;
}

// --- Ending a fight (WP6) ---

// A retreat (and a timeout's or stalemate's disengage) moves the bot away this long
constexpr uint32_t RETREAT_MS = 15 * MS_PER_SECOND;
// Once a chase is over the winner leaves the retreated enemy alone this long (unless it attacks the winner)
constexpr uint32_t LEFT_ALONE_MS = 30 * MS_PER_SECOND;
// The calm-area answer is asked again after GATE_REFRESH_MS or once the bot moved this far
constexpr float CALM_MOVE_YARDS = 20.0f;

inline bool CalmDue(FightMemory const& memory, float x, float y, uint32_t nowMs)
{
    return !memory.calmKnown || nowMs - memory.calmCheckMs >= GATE_REFRESH_MS ||
           std::hypot(x - memory.calmX, y - memory.calmY) > CALM_MOVE_YARDS;
}

inline void NoteCalm(FightMemory& memory, bool calm, float x, float y, uint32_t nowMs)
{
    memory.calmKnown = true;
    memory.calm = calm;
    memory.calmCheckMs = nowMs;
    memory.calmX = x;
    memory.calmY = y;
}

// The bot moves away from (x, y) for RETREAT_MS: a retreat by the rules from fight `fightId`, or a disengage at the
// end of a fight. Either way it drops its fight's targets
inline void StartRetreat(FightMemory& memory, uint32_t fightId, bool disengage, float x, float y, uint32_t nowMs)
{
    memory.retreating = true;
    memory.disengaging = disengage;
    memory.retreatFightId = fightId;
    memory.retreatStartMs = nowMs;
    memory.retreatUntilMs = nowMs + RETREAT_MS;
    memory.retreatX = x;
    memory.retreatY = y;
    memory.startGuid = 0;
    memory.joinGuid = 0;
}

inline bool Retreating(FightMemory const& memory, uint32_t nowMs)
{
    return memory.retreating && nowMs - memory.retreatStartMs < memory.retreatUntilMs - memory.retreatStartMs;
}

// The bot is in the fight it retreated from: it does not rejoin it
inline bool RetreatedFromFight(FightMemory const& memory)
{
    return memory.retreatFightId != 0 && memory.fightId == memory.retreatFightId;
}

// The book saw this bot attack `guid`, a retreated enemy of its fight (centre and radius then): a chase starts,
// unless it is the chase already running or one just over
inline void NoteChase(FightMemory& memory, uint64_t guid, float x, float y, float radius, uint32_t nowMs)
{
    if (memory.chaseGuid == guid && (!memory.chaseOver || nowMs - memory.chaseOverMs < LEFT_ALONE_MS))
        return;
    memory.chaseGuid = guid;
    memory.chaseStartMs = nowMs;
    memory.chaseX = x;
    memory.chaseY = y;
    memory.chaseRadius = radius;
    memory.chaseOver = false;
}

// Whether the bot (at x, y) must stop chasing its target `guid` now: the chase's time or its yards beyond the fight
// passed the bot's limit (ChaseLimitFor). A chase not looked at for longer than its limit and the leave-alone time
// is forgotten
inline bool CheckChase(FightMemory& memory, uint64_t guid, float x, float y, uint32_t nowMs, ChaseLimit const& limit)
{
    if (!guid || memory.chaseGuid != guid)
        return false;
    if (memory.chaseOver)
        return nowMs - memory.chaseOverMs < LEFT_ALONE_MS;
    uint32_t const chaseMs = nowMs - memory.chaseStartMs;
    if (chaseMs >= limit.ms + LEFT_ALONE_MS)
    {
        memory.chaseGuid = 0;
        return false;
    }
    float const beyond = std::hypot(x - memory.chaseX, y - memory.chaseY) - memory.chaseRadius;
    if (!ChaseOver(chaseMs, beyond, limit))
        return false;
    memory.chaseOver = true;
    memory.chaseOverMs = nowMs;
    return true;
}

// A chase over: the winner leaves `guid` alone for a while
inline bool ChaseLeftAlone(FightMemory const& memory, uint64_t guid, uint32_t nowMs)
{
    return guid && memory.chaseGuid == guid && memory.chaseOver && nowMs - memory.chaseOverMs < LEFT_ALONE_MS;
}

// A bot follows its real master (WP.6): while the master (`masterGuid`, 0 for none) fights in `fight`, the bot
// neither retreats nor disengages from it
inline bool FollowsMaster(Fight const& fight, uint64_t masterGuid)
{
    FightParticipant const* const master = masterGuid ? fight.Find(masterGuid) : nullptr;
    return master && master->inCombat;
}

// A timeout, stalemate or stalemate_cap disengages a participant bot that starts by the rules (persistent, not
// PvpTest's) and is still in PvP combat, unless it follows its master; wiped and combat_over leave nothing to undo
inline bool Disengages(EndCause cause, bool byRules, bool inPvpCombat, bool followsMaster)
{
    bool const stillOn = cause == EndCause::Timeout || cause == EndCause::Stalemate || cause == EndCause::StalemateCap;
    return stillOn && byRules && inPvpCombat && !followsMaster;
}

// The bot's fight is not one its map's book holds now (`held`): it left the map while the fight went on, or the
// fight ended without it there. The fight is over for the bot, so it neither stays out of it nor counts as in a fight
inline void ForgetFightElsewhere(FightMemory& memory, bool held, uint32_t nowMs)
{
    if (!memory.fightId || held)
        return;
    memory.fightId = 0;
    memory.hasFought = true;
    memory.lastFightEndMs = nowMs;
}

// The retreat cannot move (rooted, or every direction blocked): the bot gives it up and fights where it stands. After
// a retreat by the rules it still does not rejoin that fight's other enemies
inline void GiveUpRetreat(FightMemory& memory) { memory.retreating = false; }

// Whether the bot leaves `guid` alone: it retreated from the fight it is in, or its chase of `guid` just ended; never
// an enemy attacking it (self-defence)
inline bool LeavesAlone(FightMemory const& memory, uint64_t guid, bool attacksBot, uint32_t nowMs)
{
    return !attacksBot && (RetreatedFromFight(memory) || ChaseLeftAlone(memory, guid, nowMs));
}

// The active fight whose participant list holds `participant` (the book's probe visits each fight's participants in
// turn, so `last`, the index found before, nearly always is it): O(1) instead of a search through every participant
inline Fight const* FightHolding(std::vector<Fight> const& active, FightParticipant const& participant, size_t& last)
{
    auto const holds = [&participant](Fight const& fight)
    {
        FightParticipant const* const first = fight.participants.data();
        return std::less_equal<>()(first, &participant) &&
               std::less<>()(&participant, first + fight.participants.size());
    };
    if (last < active.size() && holds(active[last]))
        return &active[last];
    for (size_t index = 0; index < active.size(); ++index)
        if (holds(active[index]))
        {
            last = index;
            return &active[index];
        }
    return nullptr;
}

struct Point
{
    float x = 0.0f;
    float y = 0.0f;
};

// The yards to the nearest of `points` (FAR_YARDS for none)
inline float NearestYards(std::vector<Point> const& points, float x, float y)
{
    float nearest = FAR_YARDS;
    for (Point const& point : points)
        nearest = std::min(nearest, std::hypot(point.x - x, point.y - y));
    return nearest;
}
}  // namespace WorldPvp

#endif
