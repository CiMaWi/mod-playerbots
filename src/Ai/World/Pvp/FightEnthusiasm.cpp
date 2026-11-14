/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "FightEnthusiasm.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "BotTraitMgr.h"
#include "CellImpl.h"
#include "CensusMgr.h"
#include "Chat.h"
#include "Creature.h"
#include "GameGraveyard.h"
#include "GhostWait.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Group.h"
#include "Map.h"
#include "PerfMonitor.h"
#include "Pet.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "Random.h"
#include "ScriptMgr.h"
#include "StringConvert.h"
#include "StringFormat.h"
#include "Timer.h"
#include "WorldLog.h"
#include "WorldLogFormat.h"
#include "WorldPvp.h"

namespace WorldPvp
{
namespace
{
// The one-off edits' event rows (world-log.md): source the cause, id the value before, amount the value after; a
// death's second row carries the victim's plain join threshold. The engaged row: id the value at the rising engaged
// edge, amount 1 for a PvpTest forced fighter (pvptest), else 0
constexpr char const* EVENT_ENTHUSIASM = "PvpEnthusiasm";
constexpr char const* SOURCE_DEATH_JOIN = "death_join";
constexpr char const* SOURCE_ENGAGED = "engaged";
// The entering rows' choices (fight_start, fight_join)
constexpr char const* CHOICE_START = "start";
constexpr char const* CHOICE_JOIN = "join";
// The entering rows' reasons (world-log.md)
constexpr char const* REASON_ENTHUSIASM = "enthusiasm";
constexpr char const* REASON_FACTION = "faction_attacked";
constexpr char const* REASON_GROUP = "group_attacked";
// The leaving row (world-log.md): choice leave, reason fled (an enemy player attacks the bot) or resumed (none does)
constexpr char const* CHOICE_LEAVE = "leave";
constexpr char const* REASON_FLED = "fled";
constexpr char const* REASON_RESUMED = "resumed";
// The distinct enemy players counted as attacking the bot (the leaving row's attackers)
constexpr size_t ATTACKERS_COUNTED = 8;
// A PvpTest fighter stays forced this long after its last aim unless the job clears it first
constexpr uint32 FORCED_TTL_MS = 30 * MINUTE * IN_MILLISECONDS;

// PvpTest's forced fighters (any thread): guid to the time of the last aim; the count lets the hot path skip the lock
std::mutex forcedLock;
std::unordered_map<ObjectGuid, uint32> forced;
std::atomic<uint32> forcedCount{0};

bool IsForced(ObjectGuid guid, uint32 nowMs)
{
    if (!forcedCount.load(std::memory_order_relaxed))
        return false;

    std::lock_guard lock(forcedLock);
    auto const found = forced.find(guid);
    return found != forced.end() && nowMs - found->second <= FORCED_TTL_MS;
}

// Where the bot is now (the persistence cached, GhostWait.h)
BotPlace PlaceOf(PlayerbotAI* botAI, uint32 nowMs)
{
    BotPlace place;
    Player* const bot = botAI ? botAI->GetBot() : nullptr;
    Map const* const map = bot ? bot->GetMap() : nullptr;
    if (!map)
        return place;
    place.openWorld = !map->Instanceable();
    place.battleground = bot->InBattleground() || bot->InArena();
    if (!place.openWorld || place.battleground)
        return place;
    place.persistent = IsPersistent(botAI, nowMs);
    place.forced = place.persistent && IsForced(bot->GetGUID(), nowMs);
    return place;
}

// The yards from `object` to the nearest graveyard (spirit healers stand at them) of its map
float GraveyardYards(WorldObject const* object)
{
    // The graveyards of each map (their 2D places), copied once from the graveyard store and read-only afterwards
    static std::once_flag graveyardsOnce;
    static std::unordered_map<uint32, std::vector<Point>> graveyards;
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

// The leave: the bot stops attacking, no victim, no current target, no selection
void DropFightTarget(PlayerbotAI* botAI, Player* bot)
{
    bot->AttackStop();
    // A player's selection outlives AttackStop; another bot reads it as an attack (LeftAlone)
    bot->SetSelection(ObjectGuid::Empty);
    botAI->GetAiObjectContext()->GetValue<Unit*>("current target")->Set(nullptr);
}

EnthusiasmValues Values() { return FromConfig(sPlayerbotAIConfig.WorldPvp); }

// In PvP combat or holding an alive enemy player (or its pet) as its attack target, on an open-world map
// (battlegrounds and arenas leave the value alone). The target is the victim, or what the AI holds as its current
// target: a caster has no victim, and one whose casts do not reach a runner has no PvP combat either, so it chased
// without draining (wp-leave run 4: a shaman and a priest)
bool EngagedNow(PlayerbotAI* botAI, Player* bot)
{
    Map const* const map = bot->GetMap();
    bool const openWorld = map && !map->Instanceable();
    bool const pvpCombat = openWorld && !bot->GetCombatManager().GetPvPCombatRefs().empty();
    // The target only when the references do not answer already (most ticks of an engaged bot)
    Unit* held = openWorld && !pvpCombat ? bot->GetVictim() : nullptr;
    if (openWorld && !pvpCombat && !held)
        held = botAI->GetAiObjectContext()->GetValue<Unit*>("current target")->Get();
    Player* const enemy = held && held->IsAlive() ? held->GetCharmerOrOwnerPlayerOrPlayerItself() : nullptr;
    return EngagedBy(openWorld, pvpCombat, enemy && enemy->IsAlive() && enemy->GetTeamId() != bot->GetTeamId());
}

// Whether `unit` attacks `bot`: its victim is the bot or one of its pets, or its selected target is the bot or its
// pet (a caster has no victim)
bool Threatens(Unit* unit, Player* bot)
{
    Unit* const victim = unit->GetVictim();
    if (victim && victim->GetCharmerOrOwnerPlayerOrPlayerItself() == bot)
        return true;
    ObjectGuid const target = unit->GetTarget();
    return target && (target == bot->GetGUID() || target == bot->GetPetGUID());
}

// The enemy players (alive, of the other team, on the bot's map) in PvP combat with the bot that attack it, by
// themselves or by a pet (a totem does not count); distinct players, up to ATTACKERS_COUNTED
uint32 AttackersOf(Player* bot)
{
    std::array<Player const*, ATTACKERS_COUNTED> seen{};
    size_t count = 0;
    for (auto const& [guid, reference] : bot->GetCombatManager().GetPvPCombatRefs())
    {
        Unit* const unit = reference->GetOther(bot);
        Player* const enemy = unit && !unit->IsTotem() ? unit->GetCharmerOrOwnerPlayerOrPlayerItself() : nullptr;
        if (!enemy || !enemy->IsAlive() || enemy->GetTeamId() == bot->GetTeamId() || enemy->GetMap() != bot->GetMap())
            continue;
        if (!Threatens(unit, bot) && (enemy == unit || !Threatens(enemy, bot)))
            continue;
        if (std::find(seen.begin(), seen.begin() + count, enemy) != seen.begin() + count)
            continue;
        seen[count++] = enemy;
        if (count == seen.size())
            break;
    }
    return static_cast<uint32>(count);
}

// The group row: a member other than the bot, in the world on the bot's map (this thread's), alive, within
// groupHoldYards and in PvP combat (GroupMemberHolds). Engine state only, never a member's AI
bool GroupEngagedAround(Player* bot, EnthusiasmValues const& values)
{
    Group* const group = bot->GetGroup();
    Map const* const map = bot->GetMap();
    if (!group || !map || map->Instanceable())
        return false;
    for (GroupReference* reference = group->GetFirstMember(); reference; reference = reference->next())
    {
        Player* const member = reference->GetSource();
        if (!member || member == bot || !member->IsInWorld() || member->GetMap() != map)
            continue;
        GroupMember entry;
        entry.alive = member->IsAlive();
        entry.yards = bot->GetExactDist(member);
        entry.engaged = !member->GetCombatManager().GetPvPCombatRefs().empty();
        if (GroupMemberHolds(entry, values))
            return true;
    }
    return false;
}

// The bot's enthusiasm, made and seeded at random on first use
BotEnthusiasm& Of(PlayerbotAI* botAI, uint32 nowMs, EnthusiasmValues const& values)
{
    std::unique_ptr<BotEnthusiasm>& own = botAI->enthusiasm;
    if (!own)
        own = std::make_unique<BotEnthusiasm>();
    Seed(*own, rand32(), nowMs, values);
    return *own;
}

void ReadContext(PlayerbotAI* botAI, Player* bot, BotEnthusiasm& state, uint32 nowMs, EnthusiasmValues const& values)
{
    float aggression = 0.0f;
    float caution = 0.0f;
    float helpfulness = 0.0f;
    float confidence = 0.0f;
    if (std::optional<TraitRules::BotTraits> const traits = BotTraitMgr::instance().Find(bot->GetGUID().GetCounter()))
    {
        aggression = TraitRules::Unit(*traits, TraitRules::Trait::Aggression);
        caution = TraitRules::Unit(*traits, TraitRules::Trait::Caution);
        helpfulness = TraitRules::Unit(*traits, TraitRules::Trait::Helpfulness);
        confidence = TraitRules::Unit(*traits, TraitRules::Trait::Confidence);
    }
    NewTraits(state, aggression, caution, helpfulness, confidence, nowMs, values);
    Player* const master = botAI->GetMaster();
    state.realMaster = master && master != bot && IsRealPlayer(master);
    state.prohibitedZone = sPlayerbotAIConfig.IsPvpProhibited(bot->GetZoneId(), bot->GetAreaId());
}

// LocalBalance over the living players within balanceYards: the bot, its side and the enemies it sees. The lists are
// the thread's, kept between calls
float BalanceAround(Player* bot, EnthusiasmValues const& values)
{
    PerfMonitorScope scope(PerfMonitor::IsEnabled() ? sPerfMonitor.acquire(PERF_MON_TOTAL, "WorldPvp balance")
                                                    : nullptr);
    thread_local std::vector<Player*> players;
    thread_local std::vector<BalanceEntry> entries;
    players.clear();
    entries.clear();
    Acore::AnyPlayerInObjectRangeCheck check(bot, values.balanceYards, true, true);
    Acore::PlayerListSearcher<Acore::AnyPlayerInObjectRangeCheck> searcher(bot, players, check);
    Cell::VisitObjects(bot, searcher, values.balanceYards);
    uint32 const level = bot->GetLevel();
    entries.push_back({true, Colour::Yellow, bot->GetHealthPct() / 100.0f});
    for (Player* const player : players)
    {
        if (player == bot)
            continue;
        bool const ownSide = player->GetTeamId() == bot->GetTeamId();
        if (!ownSide && !bot->CanSeeOrDetect(player))
            continue;
        entries.push_back({ownSide, ColourOf(level, player->GetLevel()), player->GetHealthPct() / 100.0f});
    }
    return LocalBalance(entries, values);
}

// A bot that left keeps its pet off whoever it leaves alone itself. The call-off at the leaving does not last: a
// defensive pet answers a hit that lands afterwards, fights on once that attacker has left too, and its hits keep
// both in PvP combat (wp-leave run 3: a hunter's pet on a mage 30 s after both had left)
void HoldPetBack(PlayerbotAI* botAI, Player* bot)
{
    Pet* const pet = bot->GetPet();
    Unit* const victim = pet ? pet->GetVictim() : nullptr;
    if (victim && LeftAlone(botAI, victim))
        botAI->PetFollow();
}

// The engaged row on the rising edge (world-log.md): the value there, and the pvptest tag
void RecordEngaged(Player* bot, BotEnthusiasm const& state, uint32 nowMs, EnthusiasmValues const& values)
{
    if (!WorldLog::Active())
        return;
    EventLog::Emit(bot, EVENT_ENTHUSIASM, SOURCE_ENGAGED,
                   static_cast<uint32>(std::lround(std::max(0.0, ValueAt(state, nowMs, values)))),
                   IsForced(bot->GetGUID(), nowMs) ? 1 : 0);
}

// The engaged edge (its row on the rising one), the context when due, the group hold when due (cheap: most bots have no
// group), and the balance and (while engaged) who attacks the bot and what its pet attacks when due while the bot is
// engaged or could enter
// (`force`: all of it now, for the command)
void Refresh(PlayerbotAI* botAI, Player* bot, BotEnthusiasm& state, uint32 nowMs, EnthusiasmValues const& values,
             bool force)
{
    if (Observe(state, EngagedNow(botAI, bot), nowMs, values) && state.engaged)
        RecordEngaged(bot, state, nowMs, values);
    if (force || ContextDue(state, nowMs))
        ReadContext(botAI, bot, state, nowMs, values);
    if (force || GroupDue(state, nowMs))
        NewGroup(state, GroupEngagedAround(bot, values), nowMs, values);
    if (force || (BalanceDue(state, nowMs) && (state.engaged || !SkipsScan(state, nowMs, values))))
    {
        NewBalance(state, BalanceAround(bot, values), GraveyardYards(bot) <= values.graveyardYards, nowMs, values);
        state.attacked = state.engaged && AttackersOf(bot) != 0;
        if (state.left)
            HoldPetBack(botAI, bot);
    }
}

struct Entering
{
    bool enters = false;
    BanReason ban = BanReason::None;
    JoinFacts facts;
    float join = 0.0f;
    double value = 0.0;
};

// The bans, the join threshold and the check for one enemy, from what the bot sees of it now: whether its victim is
// a player of the bot's faction, of its group or its real master
Entering Evaluate(PlayerbotAI* botAI, Player* bot, BotEnthusiasm const& state, Player* enemy, uint32 nowMs,
                  EnthusiasmValues const& values)
{
    Entering entering;
    Colour const colour = ColourOf(bot->GetLevel(), enemy->GetLevel());
    Unit* const victim = enemy->GetVictim();
    Player* const victimPlayer = victim ? victim->GetCharmerOrOwnerPlayerOrPlayerItself() : nullptr;
    bool const faction = victimPlayer && victimPlayer->GetTeamId() == bot->GetTeamId();
    Group* const group = bot->GetGroup();
    bool const groupAttacked = faction && group && victimPlayer != bot && victimPlayer->GetGroup() == group;

    BanFacts ban;
    ban.colour = colour;
    ban.targetAttacksFaction = faction;
    ban.prohibitedZone = state.prohibitedZone;
    ban.onTransport = bot->GetTransport() != nullptr;
    ban.inInstance = bot->GetMap()->Instanceable();
    ban.realMaster = state.realMaster;
    ban.mastersFight = state.realMaster && victimPlayer && victimPlayer == botAI->GetMaster();
    entering.ban = Banned(ban);
    if (entering.ban != BanReason::None)
        return entering;

    entering.facts = FactsFor(state, colour, faction, groupAttacked);
    entering.join = JoinThreshold(entering.facts, values);
    entering.value = ValueAt(state, nowMs, values);
    entering.enters = Enters(entering.value, entering.join, state.fleeNow, values);
    return entering;
}

// The entering row: fight_start (reason enthusiasm) or fight_join (faction_attacked, group_attacked) with the value,
// the join threshold, the colour and the terms applied
void RecordEntering(Player* bot, Entering const& entering, EnthusiasmValues const& values)
{
    if (!WorldLog::Active())
        return;

    bool const joining = FightOngoing(entering.facts);
    WorldLogFormat::Alternatives alternatives{WorldLogFormat::Alternative{"join", entering.join}};
    alternatives.Add({"colour", static_cast<float>(entering.facts.colour)});
    alternatives.Add({"terms", static_cast<float>(JoinTerms(entering.facts, values))});
    char const* const reason = !joining                             ? REASON_ENTHUSIASM
                               : entering.facts.targetAttacksGroup ? REASON_GROUP
                                                                   : REASON_FACTION;
    WorldLog::Decision(bot,
                       joining ? WorldLogFormat::DecisionKind::FightJoin : WorldLogFormat::DecisionKind::FightStart,
                       joining ? CHOICE_JOIN : CHOICE_START, reason, static_cast<float>(entering.value), alternatives,
                       0);
}

// The leaving row: fight_leave, reason fled or resumed, the value, the flee threshold used, its terms (1: the group
// hold applied) and the enemy players attacking the bot
void RecordLeave(Player* bot, BotEnthusiasm const& state, double value, uint32 attackers)
{
    if (!WorldLog::Active())
        return;
    WorldLogFormat::Alternatives alternatives{WorldLogFormat::Alternative{"flee", state.fleeNow}};
    alternatives.Add({"terms", state.groupEngaged ? 1.0f : 0.0f});
    alternatives.Add({"attackers", static_cast<float>(attackers)});
    WorldLog::Decision(bot, WorldLogFormat::DecisionKind::FightLeave, CHOICE_LEAVE,
                       attackers ? REASON_FLED : REASON_RESUMED, static_cast<float>(value), alternatives, 0);
}

// The leave check (PLAN 3.5), after the refresh: once per engaged episode an engaged bot below its flee threshold
// drops its target, calls its pet off and writes its row; the flight follows by the trigger when it is attacked
void CheckLeave(PlayerbotAI* botAI, Player* bot, BotEnthusiasm& state, uint32 nowMs, EnthusiasmValues const& values)
{
    // Cheap first: the flags, then the value, then the rules
    if (!state.engaged || state.left || state.realMaster)
        return;
    double const value = ValueAt(state, nowMs, values);
    if (!Leaves(state, value, bot->IsAlive(), StartsByRules(botAI)))
        return;
    uint32 const attackers = AttackersOf(bot);
    MarkLeft(state, attackers != 0, nowMs);
    DropFightTarget(botAI, bot);
    botAI->PetFollow();
    RecordLeave(bot, state, value, attackers);
}

// A plain threshold of the command and the death row: a yellow target, nobody of the bot's side attacked, no
// graveyard row unless asked, the cached balance
float PlainJoin(BotEnthusiasm const& state, bool faction, bool group, bool graveyard, Colour colour,
                EnthusiasmValues const& values)
{
    JoinFacts facts = FactsFor(state, colour, faction, group);
    facts.nearGraveyard = graveyard;
    return JoinThreshold(facts, values);
}

// One one-off edit on a bot that starts by the rules, with its row(s)
void EditOne(Player* player, EditCause cause, uint32 nowMs, EnthusiasmValues const& values)
{
    PlayerbotAI* const botAI = GET_PLAYERBOT_AI(player);
    if (!botAI || (!botAI->enthusiasm && !StartsByRules(botAI)))
        return;

    BotEnthusiasm& state = Of(botAI, nowMs, values);
    if (ContextDue(state, nowMs))
        ReadContext(botAI, player, state, nowMs, values);
    double const before = EditBy(state, cause, nowMs, values);
    double const after = ValueAt(state, nowMs, values);
    EventLog::Emit(player, EVENT_ENTHUSIASM, Word(cause), static_cast<uint32>(std::lround(before)),
                   static_cast<int64>(std::lround(after)));
    if (cause == EditCause::Death)
        EventLog::Emit(player, EVENT_ENTHUSIASM, SOURCE_DEATH_JOIN, 0,
                       static_cast<int64>(std::lround(
                           PlainJoin(state, false, false, state.nearGraveyard, Colour::Yellow, values))));
}
// The PvP-kill edits (the killed player's map thread): the victim's death, the killer's kill and the victim's
// group members' group death
void NoteEnthusiasmDeath(Player* killer, Player* killed)
{
    if (!killer || !killed || killer == killed)
        return;
    uint32 const nowMs = getMSTime();
    EnthusiasmValues const values = Values();
    EditOne(killed, EditCause::Death, nowMs, values);
    EditOne(killer, EditCause::Kill, nowMs, values);
    // Only members on the victim's map: it is this thread's; a member elsewhere is another map thread's
    Group* const group = killed->GetGroup();
    if (!group)
        return;
    for (GroupReference* reference = group->GetFirstMember(); reference; reference = reference->next())
    {
        Player* const member = reference->GetSource();
        if (member && member != killed && member != killer && member->IsInWorld() &&
            member->GetMap() == killed->GetMap() && member->IsWithinDist(killed, values.groupDeathYards))
            EditOne(member, EditCause::GroupDeath, nowMs, values);
    }
}

bool OpenWorld(Map const* map) { return map && !map->Instanceable(); }

class WorldPvpPlayerScript : public PlayerScript
{
public:
    WorldPvpPlayerScript()
        : PlayerScript("WorldPvpPlayerScript", {PLAYERHOOK_ON_PVP_KILL, PLAYERHOOK_ON_PLAYER_KILLED_BY_CREATURE})
    {
    }

    // The player's map thread
    void OnPlayerPVPKill(Player* killer, Player* killed) override
    {
        if (!killer || !killed || !OpenWorld(killer->GetMap()))
            return;
        PerfMonitorScope scope(PerfMonitor::IsEnabled() ? sPerfMonitor.acquire(PERF_MON_TOTAL, "WorldPvp hooks")
                                                        : nullptr);
        NoteEnthusiasmDeath(killer, killed);
    }

    // The player's map thread; a killing blow by a player's pet, totem or guardian counts for its owner. The core
    // calls this hook instead of OnPlayerPVPKill when the killer is a creature, so no death is counted twice
    void OnPlayerKilledByCreature(Creature* killer, Player* killed) override
    {
        Player* const owner = killer ? killer->GetCharmerOrOwnerPlayerOrPlayerItself() : nullptr;
        if (!owner || !killed || owner == killed || !OpenWorld(killed->GetMap()))
            return;
        PerfMonitorScope scope(PerfMonitor::IsEnabled() ? sPerfMonitor.acquire(PERF_MON_TOTAL, "WorldPvp hooks")
                                                        : nullptr);
        NoteEnthusiasmDeath(owner, killed);
    }
};
}  // namespace

bool StartsByRules(PlayerbotAI* botAI) { return StartsByRules(PlaceOf(botAI, getMSTime())); }

bool Reaches(Unit const* bot, Unit const* enemy)
{
    return InReach(bot->GetPositionZ() - enemy->GetPositionZ(),
                   [bot, enemy]() { return bot->IsWithinLOSInMap(enemy); });
}

bool CallsPetOff(PlayerbotAI* botAI, Unit* target)
{
    Pet* const pet = botAI && target ? botAI->GetBot()->GetPet() : nullptr;
    Unit* const victim = pet ? pet->GetVictim() : nullptr;
    Player* const victimSide = victim ? victim->GetCharmerOrOwnerPlayerOrPlayerItself() : nullptr;
    Player* const leftSide = target ? target->GetCharmerOrOwnerPlayerOrPlayerItself() : nullptr;
    if (!victimSide || !leftSide)
        return false;
    return PetCalledOff(LeftAlone(botAI, target), victimSide->GetGUID().GetRawValue(),
                        leftSide->GetGUID().GetRawValue());
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

void RefreshEnthusiasm(PlayerbotAI* botAI)
{
    uint32 const nowMs = getMSTime();
    EnthusiasmValues const values = Values();
    Refresh(botAI, botAI->GetBot(), Of(botAI, nowMs, values), nowMs, values, false);
}

bool ScanSkipped(PlayerbotAI* botAI)
{
    BotEnthusiasm const* const state = botAI->enthusiasm.get();
    return state && SkipsScan(*state, getMSTime(), Values());
}

bool MayEnter(PlayerbotAI* botAI, Unit* unit, char const* refusal)
{
    Player* const bot = botAI->GetBot();
    Player* const enemy = unit->ToPlayer();
    BotEnthusiasm const* const state = botAI->enthusiasm.get();
    if (!enemy || !state || refusal || !enemy->IsAlive() || enemy->GetTeamId() == bot->GetTeamId())
        return false;
    uint32 const nowMs = getMSTime();
    if (IsPick(*state, enemy->GetGUID().GetRawValue(), nowMs))
        return true;
    if (!bot->IsWithinDist(enemy, static_cast<float>(sPlayerbotAIConfig.WorldPvp.ScanYards)) ||
        LeftAlone(botAI, enemy))
        return false;
    return Evaluate(botAI, bot, *state, enemy, nowMs, Values()).enters;
}

bool NoteEntering(PlayerbotAI* botAI, Player* enemy)
{
    BotEnthusiasm* const state = botAI->enthusiasm.get();
    if (!state)
        return false;
    uint32 const nowMs = getMSTime();
    uint64 const guid = enemy->GetGUID().GetRawValue();
    if (IsPick(*state, guid, nowMs))
        return true;
    Player* const bot = botAI->GetBot();
    EnthusiasmValues const values = Values();
    Entering const entering = Evaluate(botAI, bot, *state, enemy, nowMs, values);
    if (!entering.enters)
        return false;
    SetPick(*state, guid, nowMs);
    RecordEntering(bot, entering, values);
    return true;
}

bool IsEnteredTarget(PlayerbotAI* botAI, Unit* unit)
{
    BotEnthusiasm const* const state = botAI && unit ? botAI->enthusiasm.get() : nullptr;
    return state && IsPick(*state, unit->GetGUID().GetRawValue(), getMSTime());
}

void NoteEngaged(PlayerbotAI* botAI, Player* bot, uint32 nowMs)
{
    // A forced fighter's edges are tracked too (TracksEngaged): its engaged row carries the pvptest tag
    if (!botAI->enthusiasm && (!EngagedNow(botAI, bot) || !TracksEngaged(PlaceOf(botAI, nowMs))))
        return;
    EnthusiasmValues const values = Values();
    BotEnthusiasm& state = Of(botAI, nowMs, values);
    Refresh(botAI, bot, state, nowMs, values, false);
    CheckLeave(botAI, bot, state, nowMs, values);
}

bool LeftAlone(PlayerbotAI* botAI, Unit* unit)
{
    BotEnthusiasm const* const state = botAI && unit ? botAI->enthusiasm.get() : nullptr;
    // Cheap first: most bots have not left; a battleground, arena or PvpTest fight keeps today's targets
    if (!state || !state->left)
        return false;
    // A pet is on its owner's side; a unit that belongs to no player (a creature) is never left alone
    Player* const side = unit->GetCharmerOrOwnerPlayerOrPlayerItself();
    if (!side || !StartsByRules(botAI))
        return false;
    Player* const bot = botAI->GetBot();
    return LeftAloneBy(*state, Threatens(unit, bot) || (side != unit && Threatens(side, bot)));
}

bool LeavesFlee(PlayerbotAI* botAI)
{
    BotEnthusiasm const* const state = botAI ? botAI->enthusiasm.get() : nullptr;
    return state && FleesNow(*state);
}

bool HandleEnthusiasmCommand(ChatHandler* handler, PlayerbotAI* botAI, std::string_view rest)
{
    while (!rest.empty() && rest.front() == ' ')
        rest.remove_prefix(1);
    while (!rest.empty() && rest.back() == ' ')
        rest.remove_suffix(1);
    EnthusiasmValues const values = Values();
    std::optional<double> set;
    if (!rest.empty())
    {
        std::string_view const verb = rest.substr(0, rest.find(' '));
        std::string_view number = verb.size() < rest.size() ? rest.substr(verb.size() + 1) : std::string_view();
        while (!number.empty() && number.front() == ' ')
            number.remove_prefix(1);
        set = verb == "set" ? Acore::StringTo<double>(number) : std::nullopt;
        if (!set || !std::isfinite(*set) || *set < 0.0 || *set > values.max)
        {
            handler->SendErrorMessage("Usage: .playerbots fight enthusiasm <bot> [set <0..{:.0f}>]", values.max);
            return false;
        }
    }

    Player* const bot = botAI->GetBot();
    uint32 const nowMs = getMSTime();
    BotEnthusiasm& state = Of(botAI, nowMs, values);
    Refresh(botAI, bot, state, nowMs, values, true);
    if (set)
        state.state = {*set, nowMs};

    std::string const prefix = "FIGHT enthusiasm: " + bot->GetName();
    double const value = ValueAt(state, nowMs, values);
    std::string seed;
    if (!state.seedShown)
    {
        seed = Acore::StringFormat(" seed {:.2f}", state.seed);
        state.seedShown = true;
    }
    handler->PSendSysMessage("{} value {:.2f}{}{}", prefix, value, seed, set ? " set" : "");
    handler->PSendSysMessage("{} flee {:.2f} caution {:.2f}", prefix, state.flee, state.caution);
    handler->PSendSysMessage("{} flee_group {:.2f} group {}", prefix, state.fleeLowest, state.groupEngaged ? 1 : 0);
    handler->PSendSysMessage("{} fleenow {:.2f} enter {:.2f}", prefix, state.fleeNow,
                             EnterFloor(state.fleeNow, values));
    handler->PSendSysMessage("{} join {:.2f} aggression {:.2f}", prefix,
                             PlainJoin(state, false, false, false, Colour::Yellow, values), state.aggression);
    handler->PSendSysMessage("{} join_faction {:.2f} helpfulness {:.2f}", prefix,
                             PlainJoin(state, true, false, false, Colour::Yellow, values), state.helpfulness);
    handler->PSendSysMessage("{} join_group {:.2f}", prefix,
                             PlainJoin(state, true, true, false, Colour::Yellow, values));
    handler->PSendSysMessage("{} join_graveyard {:.2f} near {}", prefix,
                             PlainJoin(state, false, false, true, Colour::Yellow, values), state.nearGraveyard ? 1 : 0);
    handler->PSendSysMessage("{} join_green {:.2f}", prefix,
                             PlainJoin(state, false, false, false, Colour::Green, values));
    handler->PSendSysMessage("{} lowest {:.2f} skips {}", prefix, state.lowestJoin,
                             SkipsScan(state, nowMs, values) ? 1 : 0);
    handler->PSendSysMessage("{} balance {:.2f} factor {:.2f} shift {:.2f}", prefix, state.balance, state.factor,
                             state.joinShift);
    handler->PSendSysMessage("{} engaged {} for {} s", prefix, state.engaged ? 1 : 0,
                             state.engaged ? (nowMs - state.engagedMs) / IN_MILLISECONDS : 0);
    bool const picked = IsPick(state, state.pickGuid, nowMs);
    handler->PSendSysMessage("{} pick {} left {} s", prefix, picked ? ObjectGuid(state.pickGuid).GetCounter() : 0,
                             picked ? (PICK_MS - (nowMs - state.pickMs)) / IN_MILLISECONDS : 0);
    handler->PSendSysMessage("{} left {} attacked {} attackers {}", prefix, state.left ? 1 : 0, state.attacked ? 1 : 0,
                             AttackersOf(bot));
    return true;
}
}  // namespace WorldPvp

void AddWorldPvpScripts() { new WorldPvp::WorldPvpPlayerScript(); }
