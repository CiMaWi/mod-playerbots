/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "FightBook.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "AiObjectContext.h"
#include "BotTraitMgr.h"
#include "CellImpl.h"
#include "CensusMgr.h"
#include "Chat.h"
#include "Creature.h"
#include "FightBookRules.h"
#include "FightWiring.h"
#include "Formulas.h"
#include "GameGraveyard.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Group.h"
#include "Log.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "PerfMonitor.h"
#include "Pet.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "PlayerbotFactory.h"
#include "Playerbots.h"
#include "Random.h"
#include "ScriptMgr.h"
#include "StringFormat.h"
#include "TestConstraints.h"
#include "Timer.h"
#include "WorldLog.h"
#include "WorldLogFormat.h"

namespace WorldPvp
{
namespace
{
// The book's tick and a bot's combat check run at most this often
constexpr uint32 BOOK_TICK_MS = 1 * IN_MILLISECONDS;
constexpr uint32 COMBAT_CHECK_MS = 1 * IN_MILLISECONDS;
// A bot's persistence (PlayerbotFactory::KeepsPossessions) is checked again after this long (a master can change)
constexpr uint32 PERSISTENT_RECHECK_MS = 60 * IN_MILLISECONDS;
// A PvpTest fighter stays forced this long after its last aim unless the job clears it first
constexpr uint32 FORCED_TTL_MS = 30 * MINUTE * IN_MILLISECONDS;
// A command's request waits this long for the bot's tick; an answer is shown for this long
constexpr uint32 REQUEST_TTL_MS = 60 * IN_MILLISECONDS;
constexpr uint32 ANSWER_TTL_MS = 10 * MINUTE * IN_MILLISECONDS;
// FightStart and FightEnd sides packing (world-log.md)
constexpr uint32 TARGET_SHIFT = 16;
constexpr uint32 LEVEL_SHIFT = 8;
constexpr uint32 TAG_SHIFT = 32;
constexpr uint32 ALLIANCE_DEATHS_SHIFT = 24;
constexpr uint32 HORDE_DEATHS_SHIFT = 16;
constexpr uint32 ALLIANCE_COUNT_SHIFT = 8;

// Milliseconds from oldMs to nowMs on the getMSTime clock (unsigned, so its wrap after 49 days is harmless)
uint32 ElapsedMs(uint32 oldMs, uint32 nowMs) { return nowMs - oldMs; }

// The fight ids of this server run: 1, 2, ... (a fight is (session, id))
std::atomic<uint32_t> fightIds{0};

// One active fight as the world thread's `fight list` sees it, published by the book's tick
struct FightRow
{
    uint32 id = 0;
    uint32 ageSeconds = 0;
    float x = 0.0f;
    float y = 0.0f;
    float radius = 0.0f;
    std::array<uint32, SIDE_COUNT> counts{};
    std::array<uint32, SIDE_COUNT> deaths{};
    uint64 starter = 0;
    bool starterReal = false;
    FightReason reason = FightReason::AttackOnSight;
    bool tagged = false;
    // The bystanders' kept join answers and those beyond the full list
    uint32 answers = 0;
    uint32 refused = 0;
};

// One map's book; `book` belongs to the map's update thread, the snapshot to its lock
struct BookEntry
{
    BookEntry(uint32 map, uint32 instance)
        : book(FightBounds::From(sPlayerbotAIConfig.WorldPvp), fightIds), mapId(map), instanceId(instance)
    {
    }

    FightBook book;
    uint32 sinceTickMs = 0;
    uint32 joinCheckMs = 0;
    bool published = false;
    uint32 const mapId;
    uint32 const instanceId;
    std::mutex snapshotLock;
    std::vector<FightRow> snapshot;
    uint32 snapshotEnded = 0;
};

std::shared_mutex registryLock;
std::unordered_map<Map const*, std::shared_ptr<BookEntry>> registry;

std::mutex forcedLock;
std::unordered_map<ObjectGuid, uint32> forced;
std::atomic<uint32> forcedCount{0};

struct Request
{
    bool show = false;
    uint32 postedMs = 0;
};

struct Answer
{
    std::vector<std::string> lines;
    uint32 ms = 0;
};

std::mutex requestLock;
std::unordered_map<ObjectGuid, Request> requests;
std::unordered_map<ObjectGuid, Answer> answers;
std::atomic<uint32> requestCount{0};

// Continents and the other open-world maps; battlegrounds, arenas, dungeons and raids keep no book
bool HasBook(Map const* map) { return map && !map->Instanceable(); }

std::shared_ptr<BookEntry> BookOf(Map* map, bool create)
{
    if (!HasBook(map))
        return nullptr;

    {
        std::shared_lock lock(registryLock);
        auto const found = registry.find(map);
        if (found != registry.end())
            return found->second;
    }
    if (!create)
        return nullptr;

    std::unique_lock lock(registryLock);
    std::shared_ptr<BookEntry>& slot = registry[map];
    if (!slot)
        slot = std::make_shared<BookEntry>(map->GetId(), map->GetInstanceId());
    return slot;
}

void DropBook(Map* map)
{
    std::unique_lock lock(registryLock);
    registry.erase(map);
}

bool IsForced(ObjectGuid guid, uint32 nowMs)
{
    if (!forcedCount.load(std::memory_order_relaxed))
        return false;

    std::lock_guard lock(forcedLock);
    auto const found = forced.find(guid);
    return found != forced.end() && ElapsedMs(found->second, nowMs) <= FORCED_TTL_MS;
}

// The bot's memory when it is a persistent bot (its persistence cached in the memory), else nullptr; real players
// have none
FightMemory* MemoryOf(PlayerbotAI* botAI, uint32 nowMs)
{
    if (!botAI)
        return nullptr;

    std::unique_ptr<FightMemory>& memory = botAI->fightMemory;
    if (!memory)
    {
        memory = std::make_unique<FightMemory>();
        memory->bornMs = nowMs;
    }
    if (!memory->persistentKnown || ElapsedMs(memory->persistentCheckMs, nowMs) >= PERSISTENT_RECHECK_MS)
    {
        memory->persistent = PlayerbotFactory::KeepsPossessions(botAI->GetBot());
        memory->persistentKnown = true;
        memory->persistentCheckMs = nowMs;
    }
    memory->ready = TestConstraints::FightReady(botAI->rpgInfo.constraints);
    return memory->persistent ? memory.get() : nullptr;
}

FightMemory* MemoryOf(Player* player, uint32 nowMs) { return MemoryOf(GET_PLAYERBOT_AI(player), nowMs); }

// The colour of `target` to `viewer` as the core shows it: 0 grey, 1 green, 2 yellow, 3 orange, 4 red
uint32 ColourOf(Player* viewer, Player* target)
{
    switch (Acore::XP::GetColorCode(viewer->GetLevel(), target->GetLevel()))
    {
        case XP_GRAY:
            return 0;
        case XP_GREEN:
            return 1;
        case XP_YELLOW:
            return 2;
        case XP_ORANGE:
            return 3;
        default:
            return 4;
    }
}

// The graveyards of each map (their 2D places), copied once from the graveyard store and read-only afterwards;
// spirit healers stand at them
std::once_flag graveyardsOnce;
std::unordered_map<uint32, std::vector<Point>> graveyards;

float GraveyardYards(WorldObject const* object)
{
    std::call_once(graveyardsOnce,
                   []()
                   {
                       for (auto const& [id, graveyard] : sGraveyard->GetGraveyardData())
                           graveyards[graveyard.Map].push_back({graveyard.x, graveyard.y});
                   });
    auto const found = graveyards.find(object->GetMapId());
    return found == graveyards.end() ? FAR_YARDS
                                     : NearestYards(found->second, object->GetPositionX(), object->GetPositionY());
}

// The bot-side gate values (FightWiring.h's RefreshGate, every GATE_REFRESH_MS)
void ReadGate(PlayerbotAI* botAI, Player* bot, FightMemory& memory, uint32 nowMs)
{
    if (memory.fightId)
    {
        std::shared_ptr<BookEntry> const entry = BookOf(bot->GetMap(), false);
        Fight const* const fight = entry ? entry->book.FindFight(memory.fightId) : nullptr;
        ForgetFightElsewhere(memory, fight && fight->Find(bot->GetGUID().GetRawValue()), nowMs);
    }

    GateFacts facts;
    facts.prohibited = sPlayerbotAIConfig.IsPvpProhibited(bot->GetZoneId(), bot->GetAreaId());
    Player* const master = botAI->GetMaster();
    facts.realMaster = master && master != bot && IsRealPlayer(master);
    if (Group* const group = bot->GetGroup())
    {
        facts.inGroup = true;
        facts.leader = group->GetLeaderGUID() == bot->GetGUID();
        for (GroupReference* reference = group->GetFirstMember(); reference && !facts.realInGroup;
             reference = reference->next())
        {
            Player* const member = reference->GetSource();
            facts.realInGroup = member && member != bot && IsRealPlayer(member);
        }
    }
    facts.graveyardYards = GraveyardYards(bot);
    if (std::optional<TraitRules::BotTraits> const traits = BotTraitMgr::instance().Find(bot->GetGUID().GetCounter()))
    {
        facts.aggression = TraitRules::Unit(*traits, TraitRules::Trait::Aggression);
        facts.caution = TraitRules::Unit(*traits, TraitRules::Trait::Caution);
        facts.helpfulness = TraitRules::Unit(*traits, TraitRules::Trait::Helpfulness);
        facts.courtesy = TraitRules::Unit(*traits, TraitRules::Trait::Courtesy);
        facts.patience = TraitRules::Unit(*traits, TraitRules::Trait::Patience);
    }
    RefreshGate(memory, facts, nowMs);
}

// The guid of the bot's real master, 0 for none (WP.6: the bot follows it)
uint64 RealMasterOf(PlayerbotAI* botAI, Player* bot)
{
    Player* const master = botAI->GetMaster();
    return master && master != bot && IsRealPlayer(master) ? master->GetGUID().GetRawValue() : 0;
}

// The calm-area answer at the bot's place (InAnyCalmArea over its map's ended fights), asked again when due
void ReadCalm(Player* bot, FightMemory& memory, uint32 nowMs, FightRuleValues const& values)
{
    float const x = bot->GetPositionX();
    float const y = bot->GetPositionY();
    if (!CalmDue(memory, x, y, nowMs))
        return;
    std::shared_ptr<BookEntry> const entry = BookOf(bot->GetMap(), false);
    NoteCalm(memory, entry && InAnyCalmArea(entry->book.Ended(), x, y, nowMs, values), x, y, nowMs);
}

// The bot stops attacking (a retreat, a disengage): no victim, no current target
void DropFightTarget(PlayerbotAI* botAI, Player* bot)
{
    bot->AttackStop();
    botAI->GetAiObjectContext()->GetValue<Unit*>("current target")->Set(nullptr);
}

// What a participant sees of its fight: the strength of its side's alive participants (itself included) and of the
// enemies it sees, and the centre of those enemies
struct SideView
{
    float own = 0.0f;
    float other = 0.0f;
    uint32 enemies = 0;
    float x = 0.0f;
    float y = 0.0f;
};

SideView ViewOf(Player* bot, Fight const& fight, FightRuleValues const& values)
{
    uint32 const level = bot->GetLevel();
    auto const strength = [&](Player const* player)
    { return Strength(level, player->GetLevel(), player->GetHealthPct() / 100.0f, values); };
    SideView view;
    view.own = strength(bot);
    for (FightParticipant const& participant : fight.participants)
    {
        Player* const player = ObjectAccessor::GetPlayer(bot->GetMap(), ObjectGuid(participant.guid));
        if (!player || player == bot || !player->IsInWorld() || !player->IsAlive())
            continue;
        if (player->GetTeamId() == bot->GetTeamId())
        {
            view.own += strength(player);
            continue;
        }
        if (!bot->CanSeeOrDetect(player))
            continue;
        view.other += strength(player);
        view.x += player->GetPositionX();
        view.y += player->GetPositionY();
        ++view.enemies;
    }
    if (view.enemies)
    {
        view.x /= static_cast<float>(view.enemies);
        view.y /= static_cast<float>(view.enemies);
    }
    return view;
}

// The bot's side's share of the strength it sees within ScanYards: the alive players of both sides around it, the
// bot and the target included (enemies only when the bot can see them)
float OddsAround(Player* bot, Player* enemy, FightRuleValues const& values)
{
    uint32 const level = bot->GetLevel();
    auto const strength = [&](Player const* player)
    { return Strength(level, player->GetLevel(), player->GetHealthPct() / 100.0f, values); };
    float own = strength(bot);
    float other = strength(enemy);
    std::vector<Player*> players;
    Acore::AnyPlayerInObjectRangeCheck check(bot, values.scanYards, true, true);
    Acore::PlayerListSearcher<Acore::AnyPlayerInObjectRangeCheck> searcher(bot, players, check);
    Cell::VisitObjects(bot, searcher, values.scanYards);
    for (Player* const player : players)
    {
        if (player == bot || player == enemy)
            continue;
        if (player->GetTeamId() == bot->GetTeamId())
            own += strength(player);
        else if (bot->CanSeeOrDetect(player))
            other += strength(player);
    }
    return OddsShare(own, other);
}

Combatant MakeCombatant(Player* player, uint32 nowMs)
{
    Combatant combatant;
    combatant.guid = player->GetGUID().GetRawValue();
    combatant.real = !GET_PLAYERBOT_AI(player);
    combatant.side = player->GetTeamId() == TEAM_HORDE ? Side::Horde : Side::Alliance;
    combatant.level = static_cast<uint8_t>(player->GetLevel());
    combatant.x = player->GetPositionX();
    combatant.y = player->GetPositionY();
    combatant.zone = player->GetZoneId();
    combatant.area = player->GetAreaId();
    combatant.forced = IsForced(player->GetGUID(), nowMs);
    return combatant;
}

// Whether `unit`'s victim is `player` or one of its pets
bool Attacks(Unit* unit, Player* player)
{
    Unit* const victim = unit->GetVictim();
    return victim && victim->GetCharmerOrOwnerPlayerOrPlayerItself() == player;
}

void EmitJoins(CombatNote const& note, Player* first, Player* second, uint32 nowMs)
{
    for (uint8_t index = 0; index < note.joinCount; ++index)
    {
        JoinNote const& join = note.joins[index];
        Player* const joiner = join.guid == first->GetGUID().GetRawValue() ? first : second;
        if (FightMemory* const memory = MemoryOf(joiner, nowMs))
        {
            memory->fightId = note.fightId;
            memory->startGuid = 0;
        }
        if (WorldLog::Active())
            EventLog::Emit(joiner, EVENT_FIGHT_JOIN, Word(join.cameIn), note.fightId,
                           (static_cast<int64>(joiner->GetLevel()) << LEVEL_SHIFT) | static_cast<int64>(join.side));
    }
}

// A new fight's FightStart row (when it started) and its joins
void EmitNote(CombatNote const& note, Player* aggressor, Player* other, uint32 nowMs)
{
    if (!note.fightId)
        return;

    if (note.started && WorldLog::Active())
    {
        int64 const packed = (static_cast<int64>(other->GetGUID().GetCounter()) << TARGET_SHIFT) |
                             (static_cast<int64>(other->GetLevel()) << LEVEL_SHIFT) | ColourOf(aggressor, other);
        EventLog::Emit(aggressor, EVENT_FIGHT_START, Word(note.reason), note.fightId, packed);
    }
    EmitJoins(note, aggressor, other, nowMs);
}

// The alive group leader of `member` on its map when the rules had it pick `guid` (still fresh), else nullptr: a
// member following its leader's attack may hit the picked enemy first
Player* PickingLeader(Player* member, uint64 guid, uint32 nowMs)
{
    Group* const group = member->GetGroup();
    if (!group || group->GetLeaderGUID() == member->GetGUID())
        return nullptr;

    Player* const leader = ObjectAccessor::GetPlayer(member->GetMap(), group->GetLeaderGUID());
    FightMemory* const memory = leader && leader->IsAlive() ? MemoryOf(leader, nowMs) : nullptr;
    return memory && IsPicked(*memory, guid, nowMs) ? leader : nullptr;
}

// Whether a group member of `member` is among the fight's participants: a member following its group into the fight
// comes in for the group, as a bystander deciding for its group's fight does (WP5)
bool GroupInFight(Player* member, Fight const& fight)
{
    Group* const group = member->GetGroup();
    if (!group)
        return false;

    uint64 const self = member->GetGUID().GetRawValue();
    for (FightParticipant const& participant : fight.participants)
        if (participant.guid != self && group->IsMember(ObjectGuid(participant.guid)))
            return true;
    return false;
}

// PvP combat (or a kill: `first` the killer) between two players on one open-world map; recorded when one of them is a
// persistent bot or already fights in a recorded fight. Duels, same-side combat and combat with a dead player (its pet
// fights on) are not fights
void NoteCombat(Player* first, Player* second, uint32 nowMs, bool kill)
{
    if (first == second || first->GetTeamId() == second->GetTeamId() || first->GetMap() != second->GetMap())
        return;
    if (!kill && (!first->IsAlive() || !second->IsAlive()))
        return;
    if (first->duel && first->duel->Opponent == second)
        return;

    std::shared_ptr<BookEntry> const entry = BookOf(first->GetMap(), true);
    if (!entry)
        return;

    // The aggressor (SecondIsAggressor): the killer, else the one that picked the other at a meeting, else the one
    // attacking the other while the other does not attack it, else `first`
    FightMemory* const firstMemory = MemoryOf(first, nowMs);
    FightMemory* const secondMemory = MemoryOf(second, nowMs);
    bool const firstPicked = firstMemory && IsPicked(*firstMemory, second->GetGUID().GetRawValue(), nowMs);
    bool const secondPicked = secondMemory && IsPicked(*secondMemory, first->GetGUID().GetRawValue(), nowMs);
    bool const secondAggressor =
        SecondIsAggressor(kill, firstPicked, secondPicked, Attacks(first, second), Attacks(second, first));
    Player* const aggressor = secondAggressor ? second : first;
    Player* const other = secondAggressor ? first : second;
    Combatant aggressorCombatant = MakeCombatant(aggressor, nowMs);
    Combatant const otherCombatant = MakeCombatant(other, nowMs);
    FightBook& book = entry->book;
    FightMemory* const aggressorMemory = secondAggressor ? secondMemory : firstMemory;
    if (!aggressorMemory && !(secondAggressor ? firstMemory : secondMemory) &&
        !book.FindFightOf(aggressorCombatant.guid) && !book.FindFightOf(otherCombatant.guid))
        return;
    if (aggressorMemory && IsPicked(*aggressorMemory, otherCombatant.guid, nowMs))
        aggressorCombatant.start = aggressorMemory->startReason;
    if (aggressorMemory && IsJoinTarget(*aggressorMemory, otherCombatant.guid, nowMs))
        aggressorCombatant.joinedAs = aggressorMemory->joinCameIn;

    // A member hitting first the enemy its leader picked: the leader starts the fight, the member joins it for the
    // group
    if (!kill && !aggressorCombatant.real && aggressorCombatant.start == FightReason::AttackOnSight &&
        !book.FindFightOf(aggressorCombatant.guid) && !book.FindFightOf(otherCombatant.guid))
    {
        if (Player* const leader = PickingLeader(aggressor, otherCombatant.guid, nowMs))
        {
            Combatant leaderCombatant = MakeCombatant(leader, nowMs);
            leaderCombatant.start = MemoryOf(leader, nowMs)->startReason;
            EmitNote(book.OnCombat(leaderCombatant, otherCombatant, nowMs), leader, other, nowMs);
            aggressorCombatant.joinedAs = CameIn::Group;
        }
    }
    // A member hitting an enemy of its group's fight before its join check came round (WP4b group-b15)
    if (aggressorCombatant.joinedAs == CameIn::Joined && !book.FindFightOf(aggressorCombatant.guid))
        if (Fight const* const fight = book.FindFightOf(otherCombatant.guid))
            if (GroupInFight(aggressor, *fight))
                aggressorCombatant.joinedAs = CameIn::Group;

    EmitNote(kill ? book.OnKill(aggressorCombatant, otherCombatant, nowMs)
                  : book.OnCombat(aggressorCombatant, otherCombatant, nowMs),
             aggressor, other, nowMs);
}

// Every enemy player (or its pet) `player` is in PvP combat with
void NoteCombatRefs(Player* player, uint32 nowMs)
{
    for (auto const& [guid, reference] : player->GetCombatManager().GetPvPCombatRefs())
    {
        Unit* const unit = reference->GetOther(player);
        Player* const enemy = unit ? unit->GetCharmerOrOwnerPlayerOrPlayerItself() : nullptr;
        if (enemy)
            NoteCombat(player, enemy, nowMs, false);
    }
}

void EmitEnd(Map* map, Fight const& fight, uint32 nowMs)
{
    Player* row = ObjectAccessor::GetPlayer(map, ObjectGuid(fight.starter));
    for (FightParticipant const& participant : fight.participants)
    {
        Player* const player = ObjectAccessor::GetPlayer(map, ObjectGuid(participant.guid));
        if (!player)
            continue;
        if (!row)
            row = player;
        PlayerbotAI* const botAI = GET_PLAYERBOT_AI(player);
        FightMemory* const memory = botAI ? botAI->fightMemory.get() : nullptr;
        if (!memory)
            continue;
        if (memory->fightId == fight.id)
        {
            memory->fightId = 0;
            memory->hasFought = true;
            memory->lastFightEndMs = nowMs;
        }
        if (memory->joinFightId == fight.id)
        {
            memory->joinGuid = 0;
            memory->joinFightId = 0;
        }
        // A timeout or stalemate ends a fight that is still on: its bots by the rules disengage (they step away and
        // note no PvP combat for a while, so its tail opens no new fight; an enemy that keeps hitting gets one)
        if (player->IsAlive() &&
            Disengages(fight.cause, StartsByRules(botAI), player->GetCombatManager().HasPvPCombat(),
                       FollowsMaster(fight, RealMasterOf(botAI, player))))
        {
            StartRetreat(*memory, fight.id, true, fight.centreX, fight.centreY, nowMs);
            DropFightTarget(botAI, player);
        }
    }

    uint32 const seconds = ElapsedMs(fight.startMs, fight.endMs) / IN_MILLISECONDS;
    LOG_DEBUG("playerbots", "[WorldPvp] fight {} on map {} ended ({}): {} s, {} participants", fight.id, map->GetId(),
              Word(fight.cause), seconds, fight.participants.size());
    if (!WorldLog::Active())
        return;

    auto const clamped = [](uint32 value) { return static_cast<int64>(std::min(value, COUNT_LIMIT)); };
    int64 const sides = (static_cast<int64>(fight.tagged) << TAG_SHIFT) |
                        (clamped(fight.Deaths(Side::Alliance)) << ALLIANCE_DEATHS_SHIFT) |
                        (clamped(fight.Deaths(Side::Horde)) << HORDE_DEATHS_SHIFT) |
                        (clamped(fight.Count(Side::Alliance)) << ALLIANCE_COUNT_SHIFT) |
                        clamped(fight.Count(Side::Horde));
    if (row)
    {
        EventLog::Emit(row, EVENT_FIGHT_END, Word(fight.cause), fight.id, seconds);
        EventLog::Emit(row, EVENT_FIGHT_END, SOURCE_SIDES, fight.id, sides);
        return;
    }

    // Nobody of the fight is on the map any more (PvpTest sent its brawlers back, or everyone left): the rows are the
    // starter's (else the first participant's) at the place the book last saw it there
    FightParticipant const* const anchor = fight.Anchor();
    if (!anchor)
        return;
    WorldLogFormat::Place place;
    place.map = map->GetId();
    place.zone = anchor->zone;
    place.area = anchor->area;
    place.x = anchor->x;
    place.y = anchor->y;
    uint32 const bot = ObjectGuid(anchor->guid).GetCounter();
    EventLog::EmitAt(bot, place, EVENT_FIGHT_END, Word(fight.cause), fight.id, seconds);
    EventLog::EmitAt(bot, place, EVENT_FIGHT_END, SOURCE_SIDES, fight.id, sides);
}

void Publish(BookEntry& entry, uint32 nowMs)
{
    std::vector<FightRow> rows;
    rows.reserve(entry.book.Active().size());
    for (Fight const& fight : entry.book.Active())
    {
        FightRow row;
        row.id = fight.id;
        row.ageSeconds = ElapsedMs(fight.startMs, nowMs) / IN_MILLISECONDS;
        row.x = fight.centreX;
        row.y = fight.centreY;
        row.radius = fight.radius;
        for (size_t side = 0; side < SIDE_COUNT; ++side)
        {
            row.counts[side] = fight.Count(static_cast<Side>(side));
            row.deaths[side] = fight.Deaths(static_cast<Side>(side));
        }
        row.starter = fight.starter;
        FightParticipant const* const starter = fight.Find(fight.starter);
        row.starterReal = starter && starter->real;
        row.reason = fight.reason;
        row.tagged = fight.tagged;
        row.answers = fight.decidedCount;
        row.refused = fight.refusedDecisions;
        rows.push_back(row);
    }

    entry.published = !rows.empty() || !entry.book.Ended().empty();
    std::lock_guard lock(entry.snapshotLock);
    entry.snapshot.swap(rows);
    entry.snapshotEnded = static_cast<uint32>(entry.book.Ended().size());
}

// The players within `range` of a fight's centre
struct NearFight
{
    float x = 0.0f;
    float y = 0.0f;
    float range = 0.0f;

    bool operator()(Player* player) const { return player->GetExactDist2d(x, y) <= range; }
};

// The bystander's fight_join row: the rules' reason (defence, group, master, chance) or refusal word, the
// probability, the odds, the yards to the fight and the enemies it could pick by chance
void RecordJoin(Player* bot, uint32 fightId, JoinAnswer const& answer, JoinInput const& input)
{
    if (!WorldLog::Active())
        return;

    JoinDecision const& decision = answer.decision;
    WorldLogFormat::Alternatives alternatives{WorldLogFormat::Alternative{"odds", input.odds}};
    alternatives.Add({"yards", input.yards});
    alternatives.Add({"pickable", static_cast<float>(answer.pickable)});
    WorldLog::Decision(bot, WorldLogFormat::DecisionKind::FightJoin, decision.join ? CHOICE_JOIN : CHOICE_PASS,
                       decision.join ? Word(decision.why) : Word(decision.refusal), decision.p, alternatives, fightId);
}

// A bystander of a fight whose decided list is full, counted once for it
void CountRefused(FightBook& book, Fight const& fight, FightMemory& memory)
{
    if (memory.refusedFightId == fight.id)
        return;
    memory.refusedFightId = fight.id;
    book.CountRefusedDecision(fight.id);
}

// Whether the bystander would join `fighters`' fight whatever it rolls: its master or a member of its group is in it,
// or a participant attacks it
bool AlwaysJoins(PlayerbotAI* botAI, Player* bot, std::vector<Player*> const& fighters)
{
    Player* const master = botAI->GetMaster();
    Group* const group = bot->GetGroup();
    for (Player* const fighter : fighters)
        if (fighter == master || (group && fighter->GetGroup() == group) || Attacks(fighter, bot))
            return true;
    return false;
}

// One bystander of `fight` (`fighters`: its participants on the map) answers once (WP.7): what it sees of the fight
// goes into the join rules; a waiting answer is asked again at the next check
void AskBystander(FightBook& book, Fight const& fight, std::vector<Player*> const& fighters, PlayerbotAI* botAI,
                  Player* bot, uint32 nowMs, FightRuleValues const& values)
{
    FightMemory& memory = *botAI->fightMemory;
    if (GateDue(memory, nowMs))
        ReadGate(botAI, bot, memory, nowMs);

    Player* const master = botAI->GetMaster();
    Group* const group = bot->GetGroup();
    uint32 const level = bot->GetLevel();
    auto const strength = [&](Player const* player)
    { return Strength(level, player->GetLevel(), player->GetHealthPct() / 100.0f, values); };
    JoinInput input;
    input.alive = bot->IsAlive();
    input.visible = false;
    input.ownSide = false;
    input.yards = FAR_YARDS;
    float own = strength(bot);
    float other = 0.0f;
    std::vector<JoinEnemy> enemies;
    for (Player* const fighter : fighters)
    {
        bool const alive = fighter->IsAlive();
        bool const seen = bot->CanSeeOrDetect(fighter);
        float const yards = bot->GetDistance(fighter);
        input.visible = input.visible || (alive && seen);
        input.yards = std::min(input.yards, yards);
        if (fighter->GetTeamId() == bot->GetTeamId())
        {
            input.ownSide = true;
            input.groupFight = input.groupFight || (group && fighter->GetGroup() == group);
            input.masterFight = input.masterFight || (master && fighter == master);
            if (alive)
                own += strength(fighter);
            continue;
        }
        if (!alive)
            continue;

        other += strength(fighter);
        JoinEnemy enemy;
        enemy.guid = fighter->GetGUID().GetRawValue();
        enemy.yards = yards;
        enemy.colour = WorldPvp::ColourOf(level, fighter->GetLevel());
        enemy.visible = seen;
        enemy.attacking = Attacks(fighter, bot);
        enemies.push_back(enemy);
        input.attacked = input.attacked || enemy.attacking;
        Unit* const victim = fighter->GetVictim();
        Player* const victimPlayer = victim ? victim->GetCharmerOrOwnerPlayerOrPlayerItself() : nullptr;
        input.factionTargeted =
            input.factionTargeted || (victimPlayer && victimPlayer->GetTeamId() == bot->GetTeamId());
    }
    input.odds = OddsShare(own, other);

    // A test's joinroll bot always wins the roll (the refusals before it stand)
    float const roll = TestConstraints::JoinRoll(botAI->rpgInfo.constraints) ? 0.0f : static_cast<float>(rand_norm());
    JoinAnswer const answer = AnswerJoin(input, enemies, memory, fight.id, roll, values);
    if (!answer.decided)
        return;
    // A full list keeps no more answers; a join of the bot's group, master or self-defence still counts
    bool const always = answer.decision.join && answer.decision.why != JoinWhy::Chance;
    if (!book.NoteDecided(fight.id, bot->GetGUID().GetRawValue()) && !always)
    {
        CountRefused(book, fight, memory);
        return;
    }
    if (answer.decision.join)
        NoteJoin(memory, answer, fight.id, nowMs);
    RecordJoin(bot, fight.id, answer, input);
}

// Every JOIN_CHECK_MS on the map's update thread: the persistent bots (by the rules) within JoinYards plus the radius
// of an active fight's centre that are not in it and have not answered decide whether to join it. The bots of this
// map run their AI on this thread too, so their fight memories are written here safely
void AskBystanders(Map* map, FightBook& book, uint32 nowMs)
{
    PerfMonitorScope scope(PerfMonitor::IsEnabled() ? sPerfMonitor.acquire(PERF_MON_TOTAL, "WorldPvp join") : nullptr);
    FightRuleValues const values(sPlayerbotAIConfig.WorldPvp);
    std::vector<Player*> fighters;
    std::vector<Player*> nearby;
    for (Fight const& fight : book.Active())
    {
        fighters.clear();
        for (FightParticipant const& participant : fight.participants)
        {
            Player* const player = ObjectAccessor::GetPlayer(map, ObjectGuid(participant.guid));
            if (player && player->IsInWorld())
                fighters.push_back(player);
        }
        if (fighters.empty())
            continue;

        nearby.clear();
        NearFight check{fight.centreX, fight.centreY, fight.radius + values.joinYards};
        Acore::PlayerListSearcher<NearFight> searcher(fighters.front(), nearby, check);
        Cell::VisitObjects(fight.centreX, fight.centreY, map, searcher, check.range);
        for (Player* const bot : nearby)
        {
            uint64 const guid = bot->GetGUID().GetRawValue();
            // A bot on a flight path cannot join: it is not asked
            if (bot->IsInFlight() || fight.Find(guid) || fight.HasDecided(guid))
                continue;
            PlayerbotAI* const botAI = GET_PLAYERBOT_AI(bot);
            if (!botAI || !StartsByRules(botAI))
                continue;
            // A full fight asks only those that join whatever they roll (once: a joiner walking in is not asked again)
            if (fight.decidedCount >= MAX_DECIDED)
            {
                if (botAI->fightMemory->joinFightId == fight.id)
                    continue;
                if (!AlwaysJoins(botAI, bot, fighters))
                {
                    CountRefused(book, fight, *botAI->fightMemory);
                    continue;
                }
            }
            AskBystander(book, fight, fighters, botAI, bot, nowMs, values);
        }
    }
}

// A participant in PvP combat (the book's tick): whether it is in PvP combat with an enemy participant of its fight
// (the hit clock). A persistent bot attacking a retreated enemy of its fight chases it from now (NoteChase)
bool ProbeSides(Fight const& fight, FightParticipant const& participant, Player* player, uint32 nowMs)
{
    auto const enemyOf = [&](Unit* unit) -> FightParticipant const*
    {
        Player* const enemy = unit ? unit->GetCharmerOrOwnerPlayerOrPlayerItself() : nullptr;
        FightParticipant const* const other = enemy ? fight.Find(enemy->GetGUID().GetRawValue()) : nullptr;
        return other && other->side != participant.side ? other : nullptr;
    };

    bool sides = false;
    for (auto const& [guid, reference] : player->GetCombatManager().GetPvPCombatRefs())
    {
        sides = enemyOf(reference->GetOther(player)) != nullptr;
        if (sides)
            break;
    }

    // A caster has no victim: its selected target counts (WP6 chase-b70: a warlock's chase was never noted)
    FightParticipant const* victim = enemyOf(player->GetVictim());
    if (!victim)
        victim = enemyOf(ObjectAccessor::GetUnit(*player, player->GetTarget()));
    if (victim && victim->retreated && !IsForced(player->GetGUID(), nowMs))
        if (FightMemory* const memory = MemoryOf(player, nowMs))
            NoteChase(*memory, victim->guid, fight.centreX, fight.centreY, fight.radius, victim->retreatedMs, nowMs);
    return sides;
}

// Once a second while the bot is in PvP combat: a participant that starts by the rules retreats when its health is
// low and its side is losing (ShouldRetreat over what it sees of its fight); while it retreats it moves away from the
// enemies it sees
void CheckRetreat(PlayerbotAI* botAI, Player* bot, FightMemory& memory, uint32 nowMs)
{
    if (!memory.fightId || !bot->IsAlive())
        return;
    bool const retreating = Retreating(memory, nowMs);
    if (retreating && memory.disengaging)
        return;
    bool const fromThis = memory.retreatFightId == memory.fightId;
    if (fromThis && !retreating)
        return;
    if (!StartsByRules(botAI))
        return;
    std::shared_ptr<BookEntry> const entry = BookOf(bot->GetMap(), false);
    Fight const* const fight = entry ? entry->book.FindFight(memory.fightId) : nullptr;
    if (!fight || !fight->Find(bot->GetGUID().GetRawValue()))
        return;

    FightRuleValues const values(sPlayerbotAIConfig.WorldPvp);
    SideView const view = ViewOf(bot, *fight, values);
    if (fromThis)
    {
        if (view.enemies)
        {
            memory.retreatX = view.x;
            memory.retreatY = view.y;
        }
        return;
    }

    if (GateDue(memory, nowMs))
        ReadGate(botAI, bot, memory, nowMs);
    float const health = bot->GetHealthPct() / 100.0f;
    if (!view.enemies || !ShouldRetreat(health, OddsShare(view.own, view.other), memory.caution, values) ||
        FollowsMaster(*fight, RealMasterOf(botAI, bot)))
        return;

    uint32 const fightId = entry->book.MarkRetreated(bot->GetGUID().GetRawValue(), nowMs);
    StartRetreat(memory, fightId, false, view.x, view.y, nowMs);
    DropFightTarget(botAI, bot);
    if (WorldLog::Active())
        EventLog::Emit(bot, EVENT_FIGHT_RETREAT, SOURCE_RETREAT, fightId,
                       static_cast<int64>(std::lround(health * 100.0f)));
}

void TickBook(Map* map, uint32 diff)
{
    if (!HasBook(map))
        return;
    std::shared_ptr<BookEntry> const entry = BookOf(map, false);
    if (!entry)
        return;
    entry->sinceTickMs += diff;
    if (entry->sinceTickMs < BOOK_TICK_MS)
        return;
    entry->sinceTickMs = 0;
    if (entry->book.Active().empty() && !entry->published)
        return;

    PerfMonitorScope scope(PerfMonitor::IsEnabled() ? sPerfMonitor.acquire(PERF_MON_TOTAL, "WorldPvp book") : nullptr);
    uint32 const nowMs = getMSTime();
    entry->book.SetBounds(FightBounds::From(sPlayerbotAIConfig.WorldPvp));
    FightRuleValues const values(sPlayerbotAIConfig.WorldPvp);
    FightBook const& book = entry->book;
    std::vector<Fight> ended;
    size_t holding = 0;
    entry->book.Tick(
        nowMs,
        [map, &book, &holding, nowMs](FightParticipant& participant)
        {
            Player* const player = ObjectAccessor::GetPlayer(map, ObjectGuid(participant.guid));
            participant.here = player && player->IsInWorld();
            participant.sideCombat = false;
            if (!participant.here)
                return false;
            participant.alive = player->IsAlive();
            participant.x = player->GetPositionX();
            participant.y = player->GetPositionY();
            participant.zone = player->GetZoneId();
            participant.area = player->GetAreaId();
            if (!player->GetCombatManager().HasPvPCombat())
                return false;
            Fight const* const fight = FightHolding(book.Active(), participant, holding);
            participant.sideCombat = fight && ProbeSides(*fight, participant, player, nowMs);
            return true;
        },
        [&values](FightClock const& clock) { return CheckFightEnd(clock, values); }, ended);
    for (Fight const& fight : ended)
        EmitEnd(map, fight, nowMs);
    if (!entry->book.Active().empty() && ElapsedMs(entry->joinCheckMs, nowMs) >= JOIN_CHECK_MS)
    {
        entry->joinCheckMs = nowMs;
        AskBystanders(map, entry->book, nowMs);
    }
    Publish(*entry, nowMs);
}

bool TakeRequest(ObjectGuid guid, Request& request, uint32 nowMs)
{
    std::lock_guard lock(requestLock);
    for (auto it = requests.begin(); it != requests.end();)
    {
        if (ElapsedMs(it->second.postedMs, nowMs) > REQUEST_TTL_MS)
            it = requests.erase(it);
        else
            ++it;
    }
    auto const found = requests.find(guid);
    bool const taken = found != requests.end();
    if (taken)
    {
        request = found->second;
        requests.erase(found);
    }
    requestCount.store(static_cast<uint32>(requests.size()), std::memory_order_relaxed);
    return taken;
}

void PostRequest(ObjectGuid guid, bool show)
{
    std::lock_guard lock(requestLock);
    Request& request = requests[guid];
    request.show = request.show || show;
    request.postedMs = getMSTime();
    requestCount.store(static_cast<uint32>(requests.size()), std::memory_order_relaxed);
}

std::string SecondsAgo(bool known, uint32 sinceMs, uint32 nowMs)
{
    return known ? std::to_string(ElapsedMs(sinceMs, nowMs) / IN_MILLISECONDS) : std::string("never");
}

// The meeting's fight_start row: the rules' word (today's target word when it was no target), the colour, the
// chance's probability and the odds
void RecordStart(Player* bot, Player* enemy, StartDecision const& decision, StartTarget const& target,
                 char const* refusal)
{
    if (!WorldLog::Active())
        return;

    char const* reason = decision.start ? Word(decision.reason) : Word(decision.refusal);
    if (decision.refusal == Refusal::NotTarget && refusal)
        reason = refusal;
    WorldLogFormat::Alternatives alternatives{
        WorldLogFormat::Alternative{"colour", static_cast<float>(ColourOf(bot, enemy))}};
    alternatives.Add({"p", decision.p});
    alternatives.Add({"odds", target.odds});
    float const levelDifference =
        static_cast<float>(static_cast<int32>(enemy->GetLevel()) - static_cast<int32>(bot->GetLevel()));
    WorldLog::Decision(bot, WorldLogFormat::DecisionKind::FightStart, decision.start ? CHOICE_START : CHOICE_PASS,
                       reason, levelDifference, alternatives, 0);
}

// The memory as `fight bot` shows it: one line, then one line per recent enemy
std::vector<std::string> Describe(PlayerbotAI* botAI, Player* bot, FightMemory* memory, uint32 nowMs)
{
    std::vector<std::string> lines;
    std::string const name = bot->GetName();
    if (!memory)
    {
        lines.push_back("FIGHT bot: " + name + " persistent 0 (no fight memory: not a persistent bot)");
        return lines;
    }

    uint32 const gapMs = sPlayerbotAIConfig.WorldPvp.MinGapMinutes * MINUTE * IN_MILLISECONDS;
    uint32 const sinceEndMs = ElapsedMs(memory->lastFightEndMs, nowMs);
    uint32 const gapLeft = memory->hasFought && sinceEndMs < gapMs ? (gapMs - sinceEndMs) / IN_MILLISECONDS : 0;
    lines.push_back(Acore::StringFormat(
        "FIGHT bot: {} persistent 1 fight {} last_end {} last_chance {} gap_left {} ready {} recent {} avoided 0 "
        "grudges 0",
        name, memory->fightId, SecondsAgo(memory->hasFought, memory->lastFightEndMs, nowMs),
        SecondsAgo(memory->hasChance, memory->lastChanceMs, nowMs), gapLeft, memory->ready ? 1 : 0, memory->Count()));
    // The rules' state (WP4): the gate's word ("today": PvpTest or no open world), the base probability a chance
    // would have now (the budget clock), the picked target
    FightRuleValues const values(sPlayerbotAIConfig.WorldPvp);
    bool const byRules = StartsByRules(botAI);
    if (byRules && GateDue(*memory, nowMs))
        ReadGate(botAI, bot, *memory, nowMs);
    if (byRules)
        ReadCalm(bot, *memory, nowMs, values);
    StartBot const startBot = MakeStartBot(*memory, nowMs);
    lines.back() += Acore::StringFormat(
        " gate {} aggression {:.2f} caution {:.2f} p_now {:.3f} picked {} join {}",
        byRules ? Word(StartGate(startBot, values)) : "today", memory->aggression, memory->caution,
        StartProbability(StartRate(startBot.aggression, values), startBot.sinceChanceMs, values),
        IsPicked(*memory, memory->startGuid, nowMs) ? ObjectGuid(memory->startGuid).GetCounter() : 0,
        IsJoinTarget(*memory, memory->joinGuid, nowMs) ? ObjectGuid(memory->joinGuid).GetCounter() : 0);
    // WP6: the calm-area answer, the fight it retreats (or disengages) from now, the enemy it chases
    lines.back() += Acore::StringFormat(" calm {} retreat {} chase {}{}", memory->calmKnown && memory->calm ? 1 : 0,
                                        Retreating(*memory, nowMs) ? memory->retreatFightId : 0,
                                        ObjectGuid(memory->chaseGuid).GetCounter(), memory->chaseOver ? " over" : "");
    for (uint32 index = 0; index < memory->Count(); ++index)
    {
        RecentEnemy const& enemy = (*memory)[index];
        lines.push_back(Acore::StringFormat(
            "FIGHT bot: {} enemy {} seen {} rolled {}", name, ObjectGuid(enemy.guid).GetCounter(),
            ElapsedMs(enemy.seenMs, nowMs) / IN_MILLISECONDS, SecondsAgo(enemy.rolled, enemy.rolledMs, nowMs)));
    }
    return lines;
}

void ServeRequest(PlayerbotAI* botAI, Player* bot, uint32 nowMs)
{
    Request request;
    if (!TakeRequest(bot->GetGUID(), request, nowMs))
        return;

    FightMemory* const memory = MemoryOf(botAI, nowMs);
    if (!request.show)
        return;

    std::vector<std::string> lines = Describe(botAI, bot, memory, nowMs);
    for (std::string const& line : lines)
        LOG_INFO("playerbots", "[WorldPvp] {}", line);

    std::lock_guard lock(requestLock);
    for (auto it = answers.begin(); it != answers.end();)
    {
        if (ElapsedMs(it->second.ms, nowMs) > ANSWER_TTL_MS)
            it = answers.erase(it);
        else
            ++it;
    }
    answers[bot->GetGUID()] = {std::move(lines), nowMs};
}

class FightBookMapScript : public AllMapScript
{
public:
    FightBookMapScript()
        : AllMapScript("FightBookMapScript",
                       {ALLMAPHOOK_ON_CREATE_MAP, ALLMAPHOOK_ON_DESTROY_MAP, ALLMAPHOOK_ON_MAP_UPDATE})
    {
    }

    void OnCreateMap(Map* map) override { BookOf(map, true); }
    void OnDestroyMap(Map* map) override { DropBook(map); }
    void OnMapUpdate(Map* map, uint32 diff) override { TickBook(map, diff); }
};

class FightBookPlayerScript : public PlayerScript
{
public:
    FightBookPlayerScript()
        : PlayerScript("FightBookPlayerScript", {PLAYERHOOK_ON_PVP_KILL, PLAYERHOOK_ON_PLAYER_KILLED_BY_CREATURE,
                                                 PLAYERHOOK_ON_PLAYER_ENTER_COMBAT})
    {
    }

    // The player's map thread; deaths of the fight's participants
    void OnPlayerPVPKill(Player* killer, Player* killed) override
    {
        if (!killer || !killed || !HasBook(killer->GetMap()))
            return;
        PerfMonitorScope scope(PerfMonitor::IsEnabled() ? sPerfMonitor.acquire(PERF_MON_TOTAL, "WorldPvp hooks")
                                                        : nullptr);
        NoteCombat(killer, killed, getMSTime(), true);
    }

    // The player's map thread; a killing blow by a player's pet, totem or guardian counts for its owner. The core
    // calls this hook instead of OnPlayerPVPKill when the killer is a creature, so no death is counted twice
    void OnPlayerKilledByCreature(Creature* killer, Player* killed) override
    {
        Player* const owner = killer ? killer->GetCharmerOrOwnerPlayerOrPlayerItself() : nullptr;
        if (!owner || !killed || owner == killed || !HasBook(killed->GetMap()))
            return;
        PerfMonitorScope scope(PerfMonitor::IsEnabled() ? sPerfMonitor.acquire(PERF_MON_TOTAL, "WorldPvp hooks")
                                                        : nullptr);
        NoteCombat(owner, killed, getMSTime(), true);
    }

    // The player's map thread, when it enters combat at all: the first hit of a fight, while the aggressor is
    // still the only one attacking
    void OnPlayerEnterCombat(Player* player, Unit* /*enemy*/) override
    {
        if (!player || !HasBook(player->GetMap()) || player->GetCombatManager().GetPvPCombatRefs().empty())
            return;
        PerfMonitorScope scope(PerfMonitor::IsEnabled() ? sPerfMonitor.acquire(PERF_MON_TOTAL, "WorldPvp hooks")
                                                        : nullptr);
        NoteCombatRefs(player, getMSTime());
    }
};
}  // namespace

Player* NewMeeting(PlayerbotAI* botAI, Unit* unit)
{
    if (!unit->IsPlayer())
        return nullptr;
    Player* const bot = botAI->GetBot();
    Player* const enemy = unit->ToPlayer();
    if (enemy->GetTeamId() == bot->GetTeamId() || !enemy->IsAlive() || !HasBook(bot->GetMap()))
        return nullptr;

    PerfMonitorScope scope(PerfMonitor::IsEnabled() ? sPerfMonitor.acquire(PERF_MON_TOTAL, "WorldPvp meeting")
                                                    : nullptr);
    uint32 const nowMs = getMSTime();
    FightMemory* const memory = MemoryOf(botAI, nowMs);
    if (!memory)
        return nullptr;

    WorldPvpConfig const& config = sPlayerbotAIConfig.WorldPvp;
    uint64 const guid = enemy->GetGUID().GetRawValue();
    if (memory->SeenRecently(guid, nowMs, config.MeetingMemorySeconds * IN_MILLISECONDS) || !bot->CanSeeOrDetect(enemy))
        return nullptr;

    memory->Remember(guid, nowMs, config.RecentEnemies);
    return enemy;
}

void RecordMeeting(PlayerbotAI* botAI, Player* enemy, char const* refusal)
{
    if (!WorldLog::Active())
        return;

    PerfMonitorScope scope(PerfMonitor::IsEnabled() ? sPerfMonitor.acquire(PERF_MON_TOTAL, "WorldPvp meeting")
                                                    : nullptr);
    Player* const bot = botAI->GetBot();
    uint32 const nowMs = getMSTime();
    WorldLogFormat::Alternatives alternatives{
        WorldLogFormat::Alternative{"colour", static_cast<float>(ColourOf(bot, enemy))}};
    if (IsForced(bot->GetGUID(), nowMs) || IsForced(enemy->GetGUID(), nowMs))
        alternatives.Add({"pvptest", 1.0f});
    float const levelDifference =
        static_cast<float>(static_cast<int32>(enemy->GetLevel()) - static_cast<int32>(bot->GetLevel()));
    WorldLog::Decision(bot, WorldLogFormat::DecisionKind::FightStart, refusal ? CHOICE_PASS : CHOICE_START,
                       refusal ? refusal : Word(FightReason::AttackOnSight), levelDifference, alternatives, 0);
}

bool StartsByRules(PlayerbotAI* botAI)
{
    Player* const bot = botAI ? botAI->GetBot() : nullptr;
    if (!bot)
        return false;

    BotPlace place;
    place.hasBook = HasBook(bot->GetMap());
    place.battleground = bot->InBattleground() || bot->InArena();
    if (!place.hasBook || place.battleground)
        return false;
    uint32 const nowMs = getMSTime();
    place.persistent = MemoryOf(botAI, nowMs) != nullptr;
    place.forced = place.persistent && IsForced(bot->GetGUID(), nowMs);
    return StartsByRules(place);
}

bool ScanGated(PlayerbotAI* botAI)
{
    if (!StartsByRules(botAI))
        return false;

    FightMemory& memory = *botAI->fightMemory;
    uint32 const nowMs = getMSTime();
    if (IsPicked(memory, memory.startGuid, nowMs))
        return false;
    FightRuleValues const values(sPlayerbotAIConfig.WorldPvp);
    if (GateDue(memory, nowMs))
        ReadGate(botAI, botAI->GetBot(), memory, nowMs);
    ReadCalm(botAI->GetBot(), memory, nowMs, values);
    if (GateOf(memory, nowMs, values) == Refusal::None)
        return false;

    PerfMonitorScope scope(PerfMonitor::IsEnabled() ? sPerfMonitor.acquire(PERF_MON_TOTAL, "WorldPvp scan gated")
                                                    : nullptr);
    return true;
}

bool Reaches(Unit const* bot, Unit const* enemy)
{
    return InReach(bot->GetPositionZ() - enemy->GetPositionZ(),
                   [bot, enemy]() { return bot->IsWithinLOSInMap(enemy); });
}

bool AcceptByRules(PlayerbotAI* botAI, Unit* unit, char const* refusal)
{
    Player* const bot = botAI->GetBot();
    Player* const enemy = unit->ToPlayer();
    uint32 const nowMs = getMSTime();
    FightMemory* const memory = MemoryOf(botAI, nowMs);
    if (!enemy || !memory)
        return false;

    uint64 const guid = enemy->GetGUID().GetRawValue();
    if (IsPicked(*memory, guid, nowMs))
        return !refusal;

    WorldPvpConfig const& config = sPlayerbotAIConfig.WorldPvp;
    FightRuleValues const values(config);
    if (enemy->GetTeamId() == bot->GetTeamId() || !enemy->IsAlive() || !bot->IsWithinDist(enemy, values.scanYards))
        return false;
    if (GateDue(*memory, nowMs))
        ReadGate(botAI, bot, *memory, nowMs);
    ReadCalm(bot, *memory, nowMs, values);
    // A bot that cannot start a fight now meets nobody (no fight_start row)
    if (GateOf(*memory, nowMs, values) != Refusal::None)
        return false;

    PerfMonitorScope scope(PerfMonitor::IsEnabled() ? sPerfMonitor.acquire(PERF_MON_TOTAL, "WorldPvp meeting")
                                                    : nullptr);
    // An enemy out of reach is no meeting yet: no fight_start row, no chance spent, met again once in reach
    if (memory->SeenRecently(guid, nowMs, config.MeetingMemorySeconds * IN_MILLISECONDS) ||
        !bot->CanSeeOrDetect(enemy) || !Reaches(bot, enemy))
        return false;
    memory->Remember(guid, nowMs, config.RecentEnemies);

    TargetFacts facts;
    facts.enemy = !refusal;
    facts.botLevel = bot->GetLevel();
    facts.targetLevel = enemy->GetLevel();
    facts.health = enemy->GetHealthPct() / 100.0f;
    Unit* const victim = enemy->GetVictim();
    facts.busy = victim && victim->IsCreature() && !victim->GetCharmerOrOwnerPlayerOrPlayerItself();
    facts.graveyardYards = GraveyardYards(enemy);
    if (std::shared_ptr<BookEntry> const entry = BookOf(bot->GetMap(), false))
        facts.sinceResurrectionMs = entry->book.ResurrectedMsAgo(guid, nowMs);

    StartBot const startBot = MakeStartBot(*memory, nowMs);
    StartTarget target = MakeStartTarget(facts, *memory, guid, nowMs);
    if (NeedsOdds(startBot, target, values))
        target.odds = OddsAround(bot, enemy, values);
    StartDecision const decision =
        DecideMeeting(*memory, startBot, target, guid, static_cast<float>(rand_norm()), nowMs, values);
    RecordStart(bot, enemy, decision, target, refusal);
    return decision.start;
}

bool IsPickedTarget(PlayerbotAI* botAI, Unit* unit)
{
    if (!botAI || !unit || !botAI->fightMemory)
        return false;
    uint64 const guid = unit->GetGUID().GetRawValue();
    uint32 const nowMs = getMSTime();
    return IsPicked(*botAI->fightMemory, guid, nowMs) || IsJoinTarget(*botAI->fightMemory, guid, nowMs);
}

Player* JoinTarget(PlayerbotAI* botAI)
{
    FightMemory* const memory = botAI ? botAI->fightMemory.get() : nullptr;
    if (!memory || !memory->joinFightId)
        return nullptr;
    uint32 const nowMs = getMSTime();
    bool const fresh = IsJoinTarget(*memory, memory->joinGuid, nowMs);
    if ((!fresh && memory->fightId != memory->joinFightId) || memory->retreatFightId == memory->joinFightId ||
        !StartsByRules(botAI))
        return nullptr;
    Player* const bot = botAI->GetBot();
    std::shared_ptr<BookEntry> const entry = BookOf(bot->GetMap(), false);
    Fight const* const fight = entry ? entry->book.FindFight(memory->joinFightId) : nullptr;
    if (!fight)
        return nullptr;

    ChaseLimit const limit =
        ChaseLimitFor(memory->aggression, memory->courtesy, FightRuleValues(sPlayerbotAIConfig.WorldPvp));
    auto const leftAlone = [&](FightParticipant const& participant, Player* player)
    {
        return ChaseLeftAlone(*memory, participant.guid, nowMs) ||
               (ChasePast(participant.retreatedMs, nowMs, limit) && !Attacks(player, bot));
    };
    Player* const enemy = ObjectAccessor::GetPlayer(bot->GetMap(), ObjectGuid(memory->joinGuid));
    FightParticipant const* const joined = fight->Find(memory->joinGuid);
    if (fresh && enemy && enemy->IsAlive() && joined && !leftAlone(*joined, enemy))
        return enemy;

    // Its target is dead, gone or left alone, or the bot is in the fight it joined: the nearest enemy participant it
    // sees and may pick for the reason it came in
    std::vector<JoinEnemy> enemies;
    for (FightParticipant const& participant : fight->participants)
    {
        Player* const player = ObjectAccessor::GetPlayer(bot->GetMap(), ObjectGuid(participant.guid));
        if (!player || !player->IsInWorld() || !player->IsAlive() || player->GetTeamId() == bot->GetTeamId() ||
            leftAlone(participant, player))
            continue;
        JoinEnemy candidate;
        candidate.guid = participant.guid;
        candidate.yards = bot->GetDistance(player);
        candidate.colour = WorldPvp::ColourOf(bot->GetLevel(), player->GetLevel());
        candidate.visible = bot->CanSeeOrDetect(player);
        candidate.attacking = Attacks(player, bot);
        enemies.push_back(candidate);
    }
    uint64 const target = PickJoinTarget(enemies, JoinWhyOf(memory->joinCameIn));
    if (!target)
        return nullptr;
    memory->joinGuid = target;
    memory->joinMs = nowMs;
    return ObjectAccessor::GetPlayer(bot->GetMap(), ObjectGuid(target));
}

bool ChaseEnds(PlayerbotAI* botAI, Unit* target)
{
    FightMemory* const memory = botAI && target ? botAI->fightMemory.get() : nullptr;
    if (!memory || !memory->chaseGuid || memory->chaseGuid != target->GetGUID().GetRawValue() || !StartsByRules(botAI))
        return false;
    Player* const bot = botAI->GetBot();
    // The retreated enemy hitting back does not lengthen the chase: it fights only who attacks it (LeftAlone), so
    // both stop here; kept on, the two defended against each other until the stalemate (WP6 chase-b70)
    PerfMonitorScope scope(PerfMonitor::IsEnabled() ? sPerfMonitor.acquire(PERF_MON_TOTAL, "WorldPvp chase") : nullptr);
    ChaseLimit const limit =
        ChaseLimitFor(memory->aggression, memory->courtesy, FightRuleValues(sPlayerbotAIConfig.WorldPvp));
    return CheckChase(*memory, memory->chaseGuid, bot->GetPositionX(), bot->GetPositionY(), getMSTime(), limit);
}

bool LeftAlone(PlayerbotAI* botAI, Unit* unit)
{
    FightMemory const* const memory = botAI && unit ? botAI->fightMemory.get() : nullptr;
    if (!memory)
        return false;
    // A pet is on its owner's side; a unit that belongs to no player (a creature) is never left alone
    Player* const side = unit->GetCharmerOrOwnerPlayerOrPlayerItself();
    if (!side)
        return false;
    uint64 const guid = side->GetGUID().GetRawValue();
    uint32 const nowMs = getMSTime();
    // Cheap first: most bots have neither state; a battleground, arena or PvpTest fight keeps today's targets
    if (!LeavesAlone(*memory, guid, false, nowMs) || !StartsByRules(botAI))
        return false;
    return LeavesAlone(*memory, guid, Attacks(unit, botAI->GetBot()), nowMs);
}

bool CallsPetOff(PlayerbotAI* botAI, Unit* target)
{
    Pet* const pet = botAI && target ? botAI->GetBot()->GetPet() : nullptr;
    Unit* const victim = pet ? pet->GetVictim() : nullptr;
    Player* const victimSide = victim ? victim->GetCharmerOrOwnerPlayerOrPlayerItself() : nullptr;
    Player* const leftSide = target ? target->GetCharmerOrOwnerPlayerOrPlayerItself() : nullptr;
    if (!victimSide || !leftSide)
        return false;
    // A chase over ends the pet's too, also while the enemy hits back (ChaseEnds); else the bot's leave-alone
    bool const leaves = ChaseEnds(botAI, leftSide) || LeftAlone(botAI, target);
    return PetCalledOff(leaves, victimSide->GetGUID().GetRawValue(), leftSide->GetGUID().GetRawValue());
}

bool RetreatFrom(PlayerbotAI* botAI, float& x, float& y)
{
    FightMemory const* const memory = botAI ? botAI->fightMemory.get() : nullptr;
    if (!memory || !Retreating(*memory, getMSTime()) || !StartsByRules(botAI))
        return false;
    x = memory->retreatX;
    y = memory->retreatY;
    return true;
}

void GiveUpRetreat(PlayerbotAI* botAI)
{
    if (botAI && botAI->fightMemory)
        GiveUpRetreat(*botAI->fightMemory);
}

bool RequestsWaiting() { return requestCount.load(std::memory_order_relaxed) != 0; }

void OnBotTick(PlayerbotAI* botAI, Player* bot)
{
    uint32 const nowMs = getMSTime();
    if (RequestsWaiting())
        ServeRequest(botAI, bot, nowMs);
    if (bot->GetCombatManager().GetPvPCombatRefs().empty() || !HasBook(bot->GetMap()))
        return;

    FightMemory* const memory = MemoryOf(botAI, nowMs);
    if (!memory || ElapsedMs(memory->lastCombatCheckMs, nowMs) < COMBAT_CHECK_MS)
        return;

    memory->lastCombatCheckMs = nowMs;
    PerfMonitorScope scope(PerfMonitor::IsEnabled() ? sPerfMonitor.acquire(PERF_MON_TOTAL, "WorldPvp hooks") : nullptr);
    // A disengaging bot notes no PvP combat: the ended fight's tail would open a new one at once
    if (!Retreating(*memory, nowMs) || !memory->disengaging)
        NoteCombatRefs(bot, nowMs);
    CheckRetreat(botAI, bot, *memory, nowMs);
}

void MarkForced(ObjectGuid guid)
{
    std::lock_guard lock(forcedLock);
    forced[guid] = getMSTime();
    forcedCount.store(static_cast<uint32>(forced.size()), std::memory_order_relaxed);
}

void ClearForced(ObjectGuid guid)
{
    std::lock_guard lock(forcedLock);
    forced.erase(guid);
    forcedCount.store(static_cast<uint32>(forced.size()), std::memory_order_relaxed);
}

bool HandleListCommand(ChatHandler* handler)
{
    Player* const gm = handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr;
    std::vector<std::shared_ptr<BookEntry>> entries;
    {
        std::shared_lock lock(registryLock);
        for (auto const& item : registry)
            entries.push_back(item.second);
    }

    std::vector<std::string> lines;
    uint32 ended = 0;
    uint32 maps = 0;
    for (std::shared_ptr<BookEntry> const& entry : entries)
    {
        if (gm && (entry->mapId != gm->GetMapId() || entry->instanceId != gm->GetInstanceId()))
            continue;
        ++maps;
        std::lock_guard lock(entry->snapshotLock);
        ended += entry->snapshotEnded;
        for (FightRow const& row : entry->snapshot)
            lines.push_back(Acore::StringFormat(
                "FIGHT {} map {} instance {} age {} at {:.0f} {:.0f} radius {:.0f} alliance {} dead {} horde {} dead "
                "{} "
                "starter {} {} reason {} tag {} answers {} refused {}",
                row.id, entry->mapId, entry->instanceId, row.ageSeconds, row.x, row.y, row.radius,
                row.counts[static_cast<size_t>(Side::Alliance)], row.deaths[static_cast<size_t>(Side::Alliance)],
                row.counts[static_cast<size_t>(Side::Horde)], row.deaths[static_cast<size_t>(Side::Horde)],
                ObjectGuid(row.starter).GetCounter(), row.starterReal ? "real" : "bot", Word(row.reason),
                row.tagged ? "pvptest" : "-", row.answers, row.refused));
    }

    handler->PSendSysMessage("FIGHT list: {} active, {} ended in the last {} min, {} maps", lines.size(), ended,
                             sPlayerbotAIConfig.WorldPvp.CalmMinutes, maps);
    for (std::string const& line : lines)
        handler->PSendSysMessage("{}", line);
    return true;
}

bool HandleBotCommand(ChatHandler* handler, PlayerbotAI* botAI)
{
    Player* const bot = botAI->GetBot();
    uint32 const nowMs = getMSTime();
    {
        std::lock_guard lock(requestLock);
        auto const found = answers.find(bot->GetGUID());
        if (found != answers.end() && ElapsedMs(found->second.ms, nowMs) <= ANSWER_TTL_MS)
        {
            handler->PSendSysMessage("FIGHT bot: {} answer of {} s ago:", bot->GetName(),
                                     ElapsedMs(found->second.ms, nowMs) / IN_MILLISECONDS);
            for (std::string const& line : found->second.lines)
                handler->PSendSysMessage("{}", line);
        }
    }
    PostRequest(bot->GetGUID(), true);
    handler->PSendSysMessage(
        "FIGHT bot: {} asked; it answers on its next update ([WorldPvp] in the server log, and "
        "here on the next call)",
        bot->GetName());
    return true;
}

}  // namespace WorldPvp

void AddFightBookScripts()
{
    new WorldPvp::FightBookMapScript();
    new WorldPvp::FightBookPlayerScript();
}
