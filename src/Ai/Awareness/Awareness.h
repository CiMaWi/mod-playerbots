/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_AWARENESS_H
#define _PLAYERBOT_AWARENESS_H

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>

#include "AwarenessRules.h"
#include "DangerRules.h"
#include "RouteTypes.h"
#include "SharedDefines.h"

class ChatHandler;
class Corpse;
class Player;
class PlayerbotAI;
class Unit;
struct GraveyardStruct;
struct Position;

// The bot's picture of the units around it, filled from one grid visit (AwarenessRules.h holds the types and rules).
// Every call runs on the bot's own map thread, or on the world thread between map updates (console commands)
namespace Awareness
{
// Corpses whose entry a test's check can count (Store::deadEntries); more are only counted
constexpr std::size_t DEAD_KEPT = 32;
// The enemies of one fight that get a FightEnemy row (AW5); more are not written
constexpr std::size_t FIGHT_ENEMIES_KEPT = 32;

// The safe-point search's walkable tests, kept while the centre stays (AW7): the centre, the radius and per candidate
// the result and the ground's height
struct WalkCache
{
    std::uint32_t mapId = 0;
    std::uint32_t instanceId = 0;
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float radius = -1.0f;
    std::array<AwarenessRules::Walk, AwarenessRules::CANDIDATES_MAX> walk{};
    std::array<float, AwarenessRules::CANDIDATES_MAX> ground{};
};

// AW8: the memo of one return from death (INTERFACE.md §3.4): the death it belongs to (AwarenessRules::DeathKey), when
// it began, the unclear clock, the healer's way once chosen and why, whether its grave had no healer (AW30b), the
// rule's last step with the facts the Rise row reads, the progress test of its walks and its healer searches (AW33b),
// the ground of its grave and the grave of its bound (AW38a). Cleared at the rise and at a new death; not a memory of
// a place (AW.1)
struct DeadMemo
{
    std::uint32_t death = 0;
    std::uint32_t startMs = 0;         // the first ask of this death: the bound's clock without corpse or death edge
    std::uint32_t unclearStartMs = 0;  // 0: no look without a clear point since the last one with
    bool healerChosen = false;
    AwarenessRules::DeadReason healerReason = AwarenessRules::DeadReason::None;
    bool graveWithoutHealer = false;  // a search at the grave found no healer (GraveFacts::searched)
    AwarenessRules::DeadStep last;
    std::uint32_t lastMs = 0;         // 0: no step yet
    std::uint32_t holdRecheckMs = 0;  // world PvP's recheck while it holds the ghost (playerHold Wait)
    AwarenessRules::DeadFacts facts;
    AwarenessRules::WalkWatch walk;  // AW33b: the walk under way and its window (WalkStalled)
    bool healerSearched = false;     // a search ran within GRAVE_SEARCH_YARDS of the grave's point
    std::int8_t healerFound = -1;    // what the last search at the grave found: -1 none yet, 0 no healer, 1 a healer
    std::uint32_t groundGrave = 0;   // AW38a: the grave whose ground graveGround holds (0: none judged yet)
    AwarenessRules::Ground graveGround = AwarenessRules::Ground::Quiet;
    bool boundGraveChosen = false;  // AW38a: the bound's grave was chosen (boundGrave; 0: the game's)
    std::uint32_t boundGrave = 0;
};

// AW38b: a leaving under way (INTERFACE.md §3.6): begun by a Leave of a bot that is not sick, ended by its Done and
// by a death. The walk's place is searched once (FindLeavePlace); the counts are the step's (LeaveStepFacts)
struct Leaving
{
    bool active = false;
    bool searched = false;
    bool placeFound = false;
    std::uint32_t placeMap = 0;
    float placeX = 0.0f;
    float placeY = 0.0f;
    float placeZ = 0.0f;
    bool walkBegun = false;
    std::uint32_t hearthTries = 0;
    std::uint32_t waitStartMs = 0;
    AwarenessRules::LeaveAct lastAct = AwarenessRules::LeaveAct::Yield;
};

// The bot's picture and its age; PlayerbotAI::awareness, created on the first Look or the record's first note of the
// bot (NoteAttack, OnFightOpen, OnRise, OnDeath)
struct Store
{
    AwarenessRules::Picture picture;
    bool built = false;
    std::uint32_t fillUs = 0;       // the last fill's duration
    std::uint16_t deadSkipped = 0;  // dead units within the range at the last fill, not in the picture
    std::array<std::uint32_t, DEAD_KEPT> deadEntries{};
    // A test's `aw check reach=<entry>`: the reach the picture named for that creature, read by a later `reached=`
    float namedReach = -1.0f;
    std::uint32_t namedReachEntry = 0;
    // AW5, the record. The bot's own attack call (NoteAttack): its target, read and cleared at the fight's edges
    std::uint64_t notedTarget = 0;
    AwarenessRules::Caller notedCaller = AwarenessRules::Caller::Other;
    // The enemies of the bot's last fight with a FightEnemy row, kept until the next fight opens (a death reads them)
    std::array<std::uint64_t, FIGHT_ENEMIES_KEPT> fightEnemies{};
    std::uint8_t fightEnemyCount = 0;
    std::uint32_t fightStartMs = 0;
    // The bot's last rise (OnRise): when and where; AW9: the deaths in a row beside it (INTERFACE.md §3.5) and the
    // last death edge's stamp (OnDeath, wall seconds; 0 after the rise), the death a return belongs to
    bool rose = false;
    std::uint32_t roseMs = 0;
    std::uint32_t roseMap = 0;
    float roseX = 0.0f;
    float roseY = 0.0f;
    std::uint32_t row = 0;
    std::uint32_t died = 0;
    // AW7: created by the first FindSafePoint
    std::unique_ptr<WalkCache> walks;
    // AW8: the return from death under way
    DeadMemo dead;
    // AW37b: the last route the trigger `way danger` judged (DangerRules::WayDue)
    DangerRules::JudgedPair judged;
    // AW38b: a rise not judged by the leaving yet (LeaveDue): set by the rise, ended by the judgement and by a death
    bool leaveDue = false;
    Leaving leaving;
    // AW33b: the self-rise rule's last ask (one per tick), the first ask of a body whose death has no edge's stamp,
    // and the self-resurrection it ordered (the Rise row's `self`); all ended by the rise and by the next death
    std::uint32_t selfRiseAskMs = 0;
    std::uint32_t selfRiseStartMs = 0;
    std::uint32_t selfRiseOrderedMs = 0;
    AwarenessRules::SelfRiseFacts selfRiseFacts;
    AwarenessRules::SelfRiseAct selfRiseAct = AwarenessRules::SelfRiseAct::Wait;
};

// What is around the bot now, both sides; rebuilt first when it is older than its age (AwarenessRules::IsFresh)
AwarenessRules::Picture const& Look(PlayerbotAI* botAI);

// Who would join if the bot attacked this creature of its picture: the neighbours the engine would call when the
// target enters the fight (AwarenessRules::WouldAssistByPlace, then the engine's CanAssistTo and the target's line
// of sight to each). Slots of the picture; empty when the target is not a creature in it. No grid visit
AwarenessRules::UnitList JoinersOf(PlayerbotAI* botAI, std::uint64_t targetGuid);

// Who reaches this point of the bot's map (AwarenessRules::JudgePoint, INTERFACE.md §3.2) on a picture at most
// FRESH_IN_FIGHT_MS old whoever asks. No row, no side effect; a bot that is not in the world answers not in view
AwarenessRules::PointAnswer JudgePoint(PlayerbotAI* botAI, Position const& position);

// `sides` (AwarenessRules::JudgeSides, INTERFACE.md §3.8) about the unit of this guid, 0 or the bot's own guid naming
// the bot: whether its side holds the fight it stands in, from the links of a picture at most FRESH_IN_FIGHT_MS old. A
// unit not in the picture, or a bot that is not in the world, holds with theirs 0. On the bot's map thread or the world
// thread between map updates; no row, no side effect
AwarenessRules::SidesAnswer JudgeSides(PlayerbotAI* botAI, std::uint64_t unitGuid);

// `target` (AwarenessRules::JudgeTarget, INTERFACE.md §3.8): whether the bot takes the unit of this guid, on a picture
// at most FRESH_IN_FIGHT_MS old with the engine's joiners on that same picture (JoinersIn: none for a player or a
// pet). `joins` are slots of that picture, valid until the next Look. A unit not in the picture, or a bot that is not
// in the world, is left as unknown. The threads as JudgeSides; no row, no side effect; safe per candidate in one tick
AwarenessRules::TargetAnswer JudgeTarget(PlayerbotAI* botAI, std::uint64_t targetGuid);

// Where within `radius` of `centre` nobody reaches, at a place the bot can walk to (AwarenessRules::FindSafePoint,
// INTERFACE.md §3.3): the candidates on their rings, the walkable test from the centre (ground within WALK_Z_YARDS of
// the centre's height, no water, a complete path at most WALK_DETOUR times the straight line plus WALK_DETOUR_YARDS),
// its results kept while the centre stays; `from` is the bot's place. No row, no side effect
AwarenessRules::SafePoint FindSafePoint(PlayerbotAI* botAI, Position const& centre, float radius);

// AW8: what this dead bot does next (AwarenessRules::NextOfDead): fills the facts once (own place and the search
// around the corpse only within GHOST_LOOK_YARDS of it), asks the rule and keeps the step in the return's memo (its
// `last`); a step of the healer's way sets healerChosen. `corpse` is the bot's corpse on its map or null. Map thread
DeadMemo const& NextOfDead(PlayerbotAI* botAI, Corpse const* corpse);
// The walk the last step named could not start: the same facts with corpseWalkFailed, asked once more
DeadMemo const& NextOfDeadWalkFailed(PlayerbotAI* botAI);
// AW33b: a tick of the walk the last step named, to the target at x, y (the walk to the corpse, a point or the leader,
// or to the grave): true when the ghost came less than WALK_PROGRESS_YARDS nearer over the last WALK_PROGRESS_SECONDS
// (AwarenessRules::WatchWalk, WalkStalled), which the caller reports as the walk's failure
bool WalkStalled(PlayerbotAI* botAI, AwarenessRules::WalkKind kind, float x, float y);
// AW33b: the self-rise rule (AwarenessRules::SelfRiseStep) for a body that holds a self-resurrection: its place on its
// own picture, the battleground, a player's bot and the seconds since the death; asked once per tick (`self resurrect`
// and `auto release` both read it), the body's seconds into bodySeconds when given. Map thread
AwarenessRules::SelfRiseAct SelfRise(PlayerbotAI* botAI, std::uint32_t* bodySeconds = nullptr);
// AW33b: the bot sends the self-resurrection the rule ordered (its Rise row's word is `self`)
void OrderSelfRise(PlayerbotAI* botAI);
// AW9: the grave's facts the spirit healer action finds for a step of the healer's way or of the bound
struct GraveFacts
{
    bool known = false;          // the game's grave for where the ghost stands
    bool atGrave = false;        // on its map within reach of it
    bool atGravePoint = false;   // AW33b: within GRAVE_SEARCH_YARDS of its point (planar), where the revive happens
    bool healerInRange = false;  // a living spirit healer found near it (searched at most every GHOST_HEALER_RETRY_MS)
    bool looked = false;         // a search for the healer ran this tick (AW33b: the healer_wait row's `found`)
    bool searched = false;       // that search ran this tick within GRAVE_SEARCH_YARDS of the grave's point (AW30b)
    bool walkFailed = false;     // the walk to it could not start
    bool seen = false;           // a real player would see the teleport to it
};
// The last step's facts with the grave's, asked once more (the healer's way and the bound name their step by them); a
// search at the grave that found no healer sets the memo's graveWithoutHealer for this death
DeadMemo const& NextOfDeadAtGrave(PlayerbotAI* botAI, GraveFacts const& grave);
// The return's memo of this bot, null before its first ask
DeadMemo const* LastOfDead(PlayerbotAI* botAI);
// AW38a: the grave of a persistent ghost's healer's way and of its bound. Before the bound the game's, the grave its
// zone links nearest to where the ghost stands (where the release sent it, WU34); past the bound, chosen once for the
// death, the nearest grave of the ghost's map its side may use (LoadGraveSides) whose ground is not deadly as the
// revive leaves the bot (GraveGround), else the game's. Null without one. Map thread, after this death's NextOfDead
GraveyardStruct const* GraveOf(PlayerbotAI* botAI);
// AW38a: the grave's ground for the bot as the spirit healer's revive leaves it (DangerRules::JudgeGround at 50 %
// health, sick where the bot pays for the revive at a level the sickness reaches), kept for the death per grave;
// quiet without a grave
AwarenessRules::Ground GraveGround(PlayerbotAI* botAI, GraveyardStruct const* grave);
// AW38a: which side may use each grave (graveyard_zone: faction 0 both, else the faction's side), one query before the
// world runs; read lock-free afterwards
void LoadGraveSides();
// A real player's order (the chat command `revive`): this death takes the healer's way
void ChooseHealer(PlayerbotAI* botAI);
// The deaths in a row of the bot's last death (INTERFACE.md §3.5), 1 at least
std::uint32_t DeathsInRow(PlayerbotAI* botAI);
// AW9, after the rise (INTERFACE.md §3.6): the facts a trigger may read in constant time (the store's rise, health,
// power, the sickness aura; `own` stays unjudged); false for a bot that has not risen since its login, is dead, in a
// battleground or led by a real player
bool RecoverFactsOf(PlayerbotAI* botAI, AwarenessRules::RecoverFacts& facts);
// `playerbots aw look <bot> [target=<entry|guid:<counter>|name>]`: the picture, one unit per line; with a target
// also the line of who would join it
bool HandleLookCommand(ChatHandler* handler, PlayerbotAI* botAI, std::string_view rest);
// `playerbots aw check <bot> [enemies=<n>] [entries=<e1,e2..> spawns=<n>] [target=.. joins=<n>] ...`: a test's
// verdict line (the words: Awareness.cpp, CheckUsage)
bool HandleCheckCommand(ChatHandler* handler, PlayerbotAI* botAI, std::string_view rest);
// `playerbots aw judge <bot> point <x> <y> <z> | from <unit> <how> [expect=<word,..>]`: the point judgement as a line;
// with expect= a test's verdict line (FIXTURE check), the words clear, reached, inview, outview, takeable, untakeable,
// supported (support above weight), unsupported (support 0). `sides <unit>` (holds, outweighed) and `target <unit>`
// (take, leave, none, strong, many, unknown, joins0) ask JudgeSides and JudgeTarget about a unit named first by name
bool HandleJudgeCommand(ChatHandler* handler, PlayerbotAI* botAI, std::string_view rest);
// `playerbots aw safe <bot> <radius> [expect=<word,..>]`: the safe-point search around the bot; the words found, none,
// clear, reached
bool HandleSafeCommand(ChatHandler* handler, PlayerbotAI* botAI, std::string_view rest);
// `playerbots aw stats`: the counters since the server started
bool HandleStatsCommand(ChatHandler* handler);
// Known danger (AW36b, INTERFACE.md §4.3): the ground at a place for a bot of this level and side, by the world data
// alone (DangerRules::JudgeGround over the layers within reach of the height). Lock-free, from any thread; a map the
// table does not cover, a cell without rows and a height no layer reaches are quiet
DangerRules::Ground GroundAt(std::uint32_t mapId, float x, float y, float z, std::uint8_t level, TeamId side,
                             DangerRules::Condition condition = DangerRules::HEALTHY);
// The facts GroundAt judges: those of the layers within reach of the height, merged
DangerRules::CellFacts GroundFactsAt(std::uint32_t mapId, float x, float y, float z, TeamId side);
// `playerbots aw danger [<player>] | <map> <x> <y> <z> <alliance|horde> <level>`: the cell, height and side, the
// layers read, the facts, both thresholds, the answer healthy and as the player is (sick, for a place), and the mean
// cost of GroundAt over DANGER_TIMED_CALLS calls. Naming a player is for game masters: it shows where that player is.
// `<bot> check [expect=<same|near|apart|close|away|path|full|dead,..>]` (scenario aw-danger) judges the place by the
// bot's picture too (DangerRules::FactsOfPicture) and, with expect=, replies a FIXTURE check line
bool HandleDangerCommand(ChatHandler* handler, std::string_view rest);
// A way's danger (AW37a, INTERFACE.md §4.5): the walk links of a route's legs (DangerRules::WalkLinksOf over the
// travel graph) judged over the published store (DangerRules::WayDanger) with the ground of its start and goal for a
// healthy bot of this side and level. Lock-free, from any thread; before the fold no link counts and only the ends do
DangerRules::DangerPart WayOf(std::span<TravelRoute::Leg const> legs, TravelRoute::Point const& start,
                              TravelRoute::Point const& goal, TeamId side, std::uint8_t level);
// AW38c (INTERFACE.md §3.6), the bot's own map thread: the way on foot that the search answers now, from the bot to the
// place, is quiet (DangerRules::QuietWay). False at the first line that fails: a store with folded links, the term on
// and the graph ready; the bot's ground quiet (one lookup, no search otherwise); one search on foot (no stone, taxi,
// transport, own flying or flight maps; the bot's record and the stuck edges); an answer of walk legs only; a quiet
// way. PerfMonitor row "awareness quiet walk", one sample per search. It knows no sick bot: the safe areas ask it
bool QuietWalk(PlayerbotAI* botAI, TravelRoute::Point const& goal);
// `playerbots aw way <bot> <map> <x> <y> <z> [expect=<clear|least|routed|safe|nosafe|quietwalk|noquietwalk,..>]`:
// the way the search answers for the bot now (its snapshot and record, as its trip would ask), WayOf and JudgeWay for
// the bot's own trip and for an order, and the answer on foot (`walk quiet|unquiet|none`, QuietWalk's; the words
// quietwalk and noquietwalk: quiet, and unquiet or none); the legs and their deadly links (yards and deadly yards) in
// Playerbots.log. With expect= a FIXTURE check line. No reply word is "refused" or "error": the scenario runner fails
// a bot on them
bool HandleWayCommand(ChatHandler* handler, PlayerbotAI* botAI, std::string_view rest);
// The trigger `way danger` (AW37b, INTERFACE.md §4.8): the bot's held trip has a route applied that was not judged
// yet (DangerRules::WayDue). Two integer compares while no new route came; it makes no store
bool WayJudgeDue(PlayerbotAI* botAI);
// Its action: keeps the pair, then (the term on) judges the way of the trip's legs for the bot's side and level. A
// refused way writes its goal to the record (DangerRules::RECORD_WORD), the `way` row and ends the status
// (abort:danger); a least deadly one writes the row. A GM's order (`route go`: its place is named) is spared
void JudgeTripWay(PlayerbotAI* botAI);
// The trigger `leaving` (AW38b, INTERFACE.md §3.6): a rise the leaving has not judged yet, or a leaving under way. Two
// flags of the store, set by OnRise and JudgeLeave, ended by the judgement, the leaving's Done and the next death
bool LeaveDue(PlayerbotAI* botAI);
// The leaving's walk is under way: its trip's goal word is `leave` (RpgStatus::TripGoal) and AW37b's refusal spares it
bool LeaveWalks(PlayerbotAI* botAI);
// The leaving's place, once per leaving: the nearest node of the travel graph (planar, within LEAVE_SEARCH_YARDS) that
// is LEAVE_AWAY_YARDS or more from the rise, is no transport stop and whose ground is not deadly for the bot healthy
void FindLeavePlace(PlayerbotAI* botAI);
// Its action, on the bot's first tick alive in the non-combat engine (the core casts the sickness after the rise
// hook): AwarenessRules::LeaveAtRise of the ground where the bot stands (judged for it healthy or sick), the row and
// the bot. A Leave does AwarenessRules::LeaveEffectsOf in order: the held trip's goal to the record
// (DangerRules::RECORD_WORD), the `leave` row, and for a bot that is not sick the end of its status with the outcome
// danger (RpgStatus::Start of `rest`; the trip's abort abort:danger). A sick bot's status is the sickness rule's
void JudgeLeave(PlayerbotAI* botAI);
// `playerbots aw ways <alliance|horde> <level> | all`: from each capital of the side to every hub of the zones a bot
// of that level plays, for a bot of the capital's race with the flight points the factory gives its level: the hubs
// with no safe way (each deadly link with its yards, deadly yards and deciding facts), the zones cut off (one of the
// level's own bracket with the level from which it opens) and the routes the term changed (a second pass under
// DangerRules::Unpriced); `all` asks both sides at the levels 5 to 80 in steps of 5. Synchronous on the world thread;
// every line in Playerbots.log
bool HandleWaysCommand(ChatHandler* handler, std::string_view rest);
// `playerbots aw term off|on`: the search's danger term off (a store without links) or on again; on at every start.
// A lab's switch: off writes the warning `danger term off` to Server.log
bool HandleTermCommand(ChatHandler* handler, std::string_view rest);
// The record (AW5): world log rows FightScene, FightEnemy and DeathScene, read off the picture; bots only, while the
// world log wants the type. The bot's own attack on a target it picked (AttackAction::Attack, the action's name)
void NoteAttack(PlayerbotAI* botAI, std::uint64_t targetGuid, std::string_view action);
// The fight tally opened a fight (the core's enter edge): FightScene and a FightEnemy for each unit in the fight
void OnFightOpen(PlayerbotAI* botAI);
// The fight tally closed it (the leave edge): the note goes
void OnFightClose(PlayerbotAI* botAI);
// At the start of an engine tick of an open fight: a FightEnemy for each unit that entered since the last tick
void OnFightTick(PlayerbotAI* botAI);
// A bot came back to life here (the Resurrect hook; `how` is the Resurrect row's word): the Rise row (AW8), the
// return's memo cleared, the store's rise (AW9: for every bot, whether or not a row is wanted)
void OnRise(Player* bot, std::string_view how);
// A bot died, at the death's edge (AW31: the world log's OnPlayerJustDied, once per death): the row of deaths and the
// death's stamp (AW9, every death), DeathScene with the killer the kill hooks named (null: the environment, none
// named, or gone by the edge) and its entry as the row's ref
void OnDeath(Player* bot, Unit* killer, std::uint32_t killerEntry);
// The meter line of `playerbots tickperf`: reads and rebuilds since the last reset; `reset` starts a new window
std::string MeterLine(bool reset);
}  // namespace Awareness

#endif
