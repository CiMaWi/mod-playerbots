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
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "PlayerbotFactory.h"
#include "Playerbots.h"
#include "Random.h"
#include "ScriptMgr.h"
#include "StringFormat.h"
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
// `fight meet` places the enemy this many yards in front of the bot
constexpr float MEET_YARDS = 3.0f;
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
    bool ready = false;
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

// PvP combat (or a kill: `first` the killer) between two players on one open-world map; recorded when one of them is a
// persistent bot or already fights in a recorded fight. Duels and same-side combat are not fights
void NoteCombat(Player* first, Player* second, uint32 nowMs, bool kill)
{
    if (first == second || first->GetTeamId() == second->GetTeamId() || first->GetMap() != second->GetMap())
        return;
    if (first->duel && first->duel->Opponent == second)
        return;

    std::shared_ptr<BookEntry> const entry = BookOf(first->GetMap(), true);
    if (!entry)
        return;

    // The aggressor: the killer, else the one attacking the other while the other does not attack it (at the first
    // hit the target has not answered yet), else `first`
    bool const secondAggressor = !kill && Attacks(second, first) && !Attacks(first, second);
    Player* const aggressor = secondAggressor ? second : first;
    Player* const other = secondAggressor ? first : second;
    Combatant aggressorCombatant = MakeCombatant(aggressor, nowMs);
    Combatant const otherCombatant = MakeCombatant(other, nowMs);
    FightBook& book = entry->book;
    FightMemory* const aggressorMemory = MemoryOf(aggressor, nowMs);
    if (!aggressorMemory && !MemoryOf(other, nowMs) && !book.FindFightOf(aggressorCombatant.guid) &&
        !book.FindFightOf(otherCombatant.guid))
        return;
    if (aggressorMemory && IsPicked(*aggressorMemory, otherCombatant.guid, nowMs))
        aggressorCombatant.start = aggressorMemory->startReason;

    CombatNote const note = kill ? book.OnKill(aggressorCombatant, otherCombatant, nowMs)
                                 : book.OnCombat(aggressorCombatant, otherCombatant, nowMs);
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
        if (PlayerbotAI* const botAI = GET_PLAYERBOT_AI(player))
        {
            if (botAI->fightMemory && botAI->fightMemory->fightId == fight.id)
            {
                botAI->fightMemory->fightId = 0;
                botAI->fightMemory->hasFought = true;
                botAI->fightMemory->lastFightEndMs = nowMs;
            }
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
        rows.push_back(row);
    }

    entry.published = !rows.empty() || !entry.book.Ended().empty();
    std::lock_guard lock(entry.snapshotLock);
    entry.snapshot.swap(rows);
    entry.snapshotEnded = static_cast<uint32>(entry.book.Ended().size());
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
    std::vector<Fight> ended;
    entry->book.Tick(
        nowMs,
        [map](FightParticipant& participant)
        {
            Player* const player = ObjectAccessor::GetPlayer(map, ObjectGuid(participant.guid));
            if (!player || !player->IsInWorld())
                return false;
            participant.x = player->GetPositionX();
            participant.y = player->GetPositionY();
            participant.zone = player->GetZoneId();
            participant.area = player->GetAreaId();
            return player->GetCombatManager().HasPvPCombat();
        },
        ended);
    for (Fight const& fight : ended)
        EmitEnd(map, fight, nowMs);
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

void PostRequest(ObjectGuid guid, bool show, bool ready)
{
    std::lock_guard lock(requestLock);
    Request& request = requests[guid];
    request.show = request.show || show;
    request.ready = request.ready || ready;
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
    StartBot const startBot = MakeStartBot(*memory, nowMs);
    lines.back() += Acore::StringFormat(
        " gate {} aggression {:.2f} caution {:.2f} p_now {:.3f} picked {}",
        byRules ? Word(StartGate(startBot, values)) : "today", memory->aggression, memory->caution,
        StartProbability(StartRate(startBot.aggression, values), startBot.sinceChanceMs, values),
        IsPicked(*memory, memory->startGuid, nowMs) ? ObjectGuid(memory->startGuid).GetCounter() : 0);
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
    if (request.ready)
    {
        if (memory)
            memory->ready = true;
        LOG_INFO("playerbots", "[WorldPvp] FIGHT ready: {} {}", bot->GetName(),
                 memory ? "is ready (its next chance starts a fight when the hard rules pass)"
                        : "refused: not a persistent bot");
    }
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

bool TestCommandsRefused(ChatHandler* handler, char const* command)
{
    if (sPlayerbotAIConfig.WorldPvp.TestCommands)
        return false;
    handler->SendErrorMessage("FIGHT {}: refused: Playerbots.WorldPvp.TestCommands is 0 (labs only)", command);
    return true;
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
    if (GateDue(memory, nowMs))
        ReadGate(botAI, botAI->GetBot(), memory, nowMs);
    if (GateOf(memory, nowMs, FightRuleValues(sPlayerbotAIConfig.WorldPvp)) == Refusal::None)
        return false;

    PerfMonitorScope scope(PerfMonitor::IsEnabled() ? sPerfMonitor.acquire(PERF_MON_TOTAL, "WorldPvp scan gated")
                                                    : nullptr);
    return true;
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
    // A bot that cannot start a fight now meets nobody (no fight_start row)
    if (GateOf(*memory, nowMs, values) != Refusal::None)
        return false;

    PerfMonitorScope scope(PerfMonitor::IsEnabled() ? sPerfMonitor.acquire(PERF_MON_TOTAL, "WorldPvp meeting")
                                                    : nullptr);
    if (memory->SeenRecently(guid, nowMs, config.MeetingMemorySeconds * IN_MILLISECONDS) || !bot->CanSeeOrDetect(enemy))
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
    return botAI && unit && botAI->fightMemory &&
           IsPicked(*botAI->fightMemory, unit->GetGUID().GetRawValue(), getMSTime());
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
    NoteCombatRefs(bot, nowMs);
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
                "starter {} {} reason {} tag {}",
                row.id, entry->mapId, entry->instanceId, row.ageSeconds, row.x, row.y, row.radius,
                row.counts[static_cast<size_t>(Side::Alliance)], row.deaths[static_cast<size_t>(Side::Alliance)],
                row.counts[static_cast<size_t>(Side::Horde)], row.deaths[static_cast<size_t>(Side::Horde)],
                ObjectGuid(row.starter).GetCounter(), row.starterReal ? "real" : "bot", Word(row.reason),
                row.tagged ? "pvptest" : "-"));
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
    PostRequest(bot->GetGUID(), true, false);
    handler->PSendSysMessage(
        "FIGHT bot: {} asked; it answers on its next update ([WorldPvp] in the server log, and "
        "here on the next call)",
        bot->GetName());
    return true;
}

bool HandleReadyCommand(ChatHandler* handler, PlayerbotAI* botAI)
{
    if (TestCommandsRefused(handler, "ready"))
        return false;

    Player* const bot = botAI->GetBot();
    PostRequest(bot->GetGUID(), false, true);
    handler->PSendSysMessage("FIGHT ready: {} ordered; see [WorldPvp] FIGHT ready: {}", bot->GetName(), bot->GetName());
    return true;
}

bool HandleMeetCommand(ChatHandler* handler, PlayerbotAI* botAI, PlayerbotAI* enemyAI)
{
    if (TestCommandsRefused(handler, "meet"))
        return false;

    Player* const bot = botAI->GetBot();
    Player* const enemy = enemyAI->GetBot();
    char const* refusal = nullptr;
    if (bot == enemy || bot->GetTeamId() == enemy->GetTeamId())
        refusal = "the two bots are not of opposite sides";
    else if (!bot->IsAlive() || !enemy->IsAlive())
        refusal = "dead";
    else if (bot->IsInFlight() || enemy->IsInFlight())
        refusal = "in flight";
    else if (!HasBook(bot->GetMap()))
        refusal = "the bot is not in the open world";
    if (refusal)
    {
        handler->SendErrorMessage("FIGHT meet: {} and {} refused: {}", bot->GetName(), enemy->GetName(), refusal);
        return false;
    }

    // A few yards in front of the bot, facing it; no attack is ordered (the placed-enemy fixture, labs only)
    float const facing = bot->GetOrientation();
    float const x = bot->GetPositionX() + MEET_YARDS * std::cos(facing);
    float const y = bot->GetPositionY() + MEET_YARDS * std::sin(facing);
    float const z = bot->GetPositionZ();
    enemy->UpdatePvP(true, true);
    enemy->TeleportTo(bot->GetMapId(), x, y, z, Position::NormalizeOrientation(facing + static_cast<float>(M_PI)));
    handler->PSendSysMessage("FIGHT meet: {} placed beside {} (map {} {:.0f} {:.0f} {:.0f}), PvP flagged, no attack",
                             enemy->GetName(), bot->GetName(), bot->GetMapId(), x, y, z);
    return true;
}
}  // namespace WorldPvp

void AddFightBookScripts()
{
    new WorldPvp::FightBookMapScript();
    new WorldPvp::FightBookPlayerScript();
}
