/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "LosTacticStrategy.h"

#include <algorithm>
#include <vector>

#include "BattleGroundTactics.h"
#include "Battleground.h"
#include "GridTerrainData.h"
#include "Group.h"
#include "LastMovementValue.h"
#include "Log.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "PathGenerator.h"
#include "PerfMonitor.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "Spell.h"
#include "Timer.h"
#include "World.h"

// G2e: the Los tactic's game side (the policy in LosTactic.h, the cover in ArenaCover.h). G2f fills the pillar parts.

namespace
{
LosBotState& LosState(PlayerbotAI* botAI)
{
    return botAI->GetAiObjectContext()->GetValue<LosBotState&>("los state")->Get();
}

LosParams BotLosParams(PlayerbotAI* botAI)
{
    LosGateInputs in;
    in.Skill = botAI->GetPvpSkill();
    in.FullAbility = in.Skill == 0 || botAI->HasFullSkillAbility();
    in.Knowledge = botAI->GetAttribute(BotAttribute::Knowledge);
    in.Speed = botAI->GetAttribute(BotAttribute::Speed);
    in.Wisdom = botAI->GetAttribute(BotAttribute::Wisdom);
    in.Effort = botAI->GetSkillEffort();
    in.Exponent = sPlayerbotAIConfig.SkillGate.CurveExponent;
    return ComputeLosParams(sPlayerbotAIConfig.TacticLos, in);
}

// The spell check's line of sight (Spell::CheckCast: M2 models ignored)
bool SeesUnit(Player* bot, Unit* unit) { return bot->IsWithinLOSInMap(unit, VMAP::ModelIgnoreFlags::M2); }

// Before the gates open and in Ring of Valor's first RvIdleMs after them (the battleground's clock: the triggers run
// in combat only, so the bot's first look comes too late) nothing moves for LOS; outside battlegrounds never idle.
// Another battleground (or none) starts a fresh episode.
bool LosIdleHere(Player* bot, LosBotState& state)
{
    Battleground* bg = bot->GetBattleground();
    uint32 const instance = bg ? bg->GetInstanceID() : 0;
    if (state.ArenaInstance != instance)
    {
        state.ArenaInstance = instance;
        LosResetEpisode(state);
    }

    if (!bg)
        return false;

    bool const ringOfValor = bg->isArena() && bg->GetMapId() == COVER_MAP_RING_OF_VALOR;
    uint32 const prepMs = sWorld->getIntConfig(CONFIG_ARENA_PREP_TIME) * IN_MILLISECONDS;
    return LosArenaIdle(bg->GetStatus() == STATUS_IN_PROGRESS, ringOfValor, LosSinceGatesMs(bg->GetStartTime(), prepMs),
                        sPlayerbotAIConfig.TacticPillar.RvIdleMs);
}

Battleground* ArenaOf(Player* bot)
{
    Map* map = bot->GetMap();
    BattlegroundMap* bgMap = map && map->IsBattleArena() ? map->ToBattlegroundMap() : nullptr;
    return bgMap ? bgMap->GetBG() : nullptr;
}

CoverVec PositionOf(WorldObject const* object)
{
    return {object->GetPositionX(), object->GetPositionY(), object->GetPositionZ()};
}

// The healer's partner out of sight: the group's alive partners within LOS_PARTNER_YARDS, the episode's partner
// (keepGuid) first, then the most hurt, one ray each while the budget lasts. Returns the first unseen one's raw guid,
// 0 when it sees all it tested.
uint64 LostPartner(Player* bot, uint64 keepGuid, uint32 budget, uint32& rays)
{
    Group* group = bot->GetGroup();
    if (!group)
        return 0;

    std::vector<LosPartner> partners;
    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (!member || member == bot || !member->IsInWorld() || member->GetMap() != bot->GetMap())
            continue;

        partners.push_back(
            {member->GetGUID().GetRawValue(), member->IsAlive(), bot->GetDistance(member), member->GetHealthPct()});
    }

    LosPartnerOrder(partners, keepGuid);
    for (LosPartner const& partner : partners)
    {
        if (rays >= budget)
            break;

        Player* member = ObjectAccessor::GetPlayer(*bot, ObjectGuid(partner.Guid));
        if (!member)
            continue;

        ++rays;
        if (!SeesUnit(bot, member))
            return partner.Guid;
    }

    return 0;
}

// Does the held point still do its job? Reached and still blind: no. A path point holds until it is reached; a cover
// point is re-tested by CoverIsStillValid (its pillar still up, at most two rays, only when the budget has them).
bool PlanHolds(Player* bot, CoverPlan const& plan, uint32 budget, uint32& rays)
{
    if (!plan.Valid || plan.MapId != bot->GetMapId())
        return false;

    if (bot->GetExactDist2d(plan.Point.X, plan.Point.Y) < LOS_AT_POINT_YARDS)
        return false;

    Battleground* bg = ArenaOf(bot);
    ArenaCoverTable const* table = bg ? FindArenaCoverTable(bot->GetMapId()) : nullptr;
    if (plan.Pillar < 0 || !table)
        return true;

    Unit* see = plan.WantSee ? ObjectAccessor::GetUnit(*bot, ObjectGuid(plan.SeeGuid)) : nullptr;
    Unit* enemy = plan.WantHidden ? ObjectAccessor::GetUnit(*bot, ObjectGuid(plan.EnemyGuid)) : nullptr;
    CoverPlan test = plan;
    test.WantHidden = enemy && enemy->IsAlive();
    uint32 const need = (test.WantHidden ? 1 : 0) + (test.WantSee ? 1 : 0);
    if (rays + need > budget)
        return true;

    Map* map = bot->GetMap();
    uint32 const phaseMask = bot->GetPhaseMask();
    auto const los = [map, phaseMask](CoverVec const& a, CoverVec const& b)
    { return ArenaCoverLos(map, phaseMask, a, b); };
    CoverVec const enemyAt = enemy ? PositionOf(enemy) : CoverVec();
    CoverVec const seeAt = see ? PositionOf(see) : CoverVec();
    return CoverIsStillValid(test, *table, ArenaCoverActiveGates(bg), enemyAt, see ? &seeAt : nullptr, los,
                             COVER_EYE_YARDS, &rays);
}

// The unit a regain walks for died or left: the regain ends (aborted, not regained) and the bot stops if it still
// walks to the point issued for it (the last movement goes there)
void AbortRegain(PlayerbotAI* botAI, Player* bot, LosBotState& state, uint32 nowMs)
{
    Value<CoverPlan>* planValue = botAI->GetAiObjectContext()->GetValue<CoverPlan>("cover plan");
    CoverPlan plan = planValue->Get();
    if (plan.Valid && plan.WantSee && plan.SeeGuid == state.SeeGuid)
    {
        LastMovement const& last = botAI->GetAiObjectContext()->GetValue<LastMovement&>("last movement")->Get();
        if (bot->isMoving() && std::fabs(last.lastMoveToX - plan.Point.X) < LOS_AT_POINT_YARDS &&
            std::fabs(last.lastMoveToY - plan.Point.Y) < LOS_AT_POINT_YARDS)
            bot->StopMoving();

        plan.Valid = false;
        planValue->Set(plan);
    }

    LOG_TRACE("playerbots", "Los: {} stops regaining sight of its {} (gone or dead after {} ms), skill {}, at {} ms",
              bot->GetName(), LosWantName(state.Want), nowMs - state.RegainStartMs, botAI->GetPvpSkill(), nowMs);
    LosAbortRegain(state);
}

// The sight refresh: what the bot cannot see now, at most once per LOS_REFRESH_MS and at most RaysPerSecond rays.
// partnerLosLostMs counts for every healer while the harness counts (also where the tactic is off: A against B).
void RefreshLos(PlayerbotAI* botAI, Player* bot, LosBotState& state, bool enabled, bool healer, uint32 nowMs)
{
    if (state.RefreshMs && nowMs - state.RefreshMs < LOS_REFRESH_MS)
        return;

    PerfMonitorScope scope(PerfMonitor::IsEnabled() ? sPerfMonitor.acquire(PERF_MON_TOTAL, "Los::Sight") : nullptr);
    uint32 const lostMs = LosLostGapMs(state.PartnerLost, state.RefreshMs, nowMs);
    if (lostMs)
    {
        state.PartnerLostMs += lostMs;
        botAI->CountTactic(TacticCounter::PartnerLosLostMs, lostMs);
    }

    state.Params = BotLosParams(botAI);
    uint32 const budget = state.Params.RaysPerRefresh;
    uint32 rays = 0;
    LosWant want = LosWant::None;
    uint64 seeGuid = 0;
    if (healer)
    {
        seeGuid = LostPartner(bot, state.Want == LosWant::Partner ? state.SeeGuid : 0, budget, rays);
        want = seeGuid ? LosWant::Partner : LosWant::None;
    }
    else if (enabled)
    {
        Unit* target = botAI->GetAiObjectContext()->GetValue<Unit*>("current target")->Get();
        if (target && target->IsAlive() && target->IsInWorld() && target->GetMap() == bot->GetMap())
        {
            ++rays;
            if (!SeesUnit(bot, target))
            {
                want = LosWant::Target;
                seeGuid = target->GetGUID().GetRawValue();
            }
        }
    }

    state.PartnerLost = want == LosWant::Partner;
    if (!enabled)
    {
        state.RefreshMs = std::max(nowMs, 1u);
        return;
    }

    if (state.RegainStartMs && (want != state.Want || seeGuid != state.SeeGuid))
    {
        Unit* seen = ObjectAccessor::GetUnit(*bot, ObjectGuid(state.SeeGuid));
        if (!seen || !seen->IsAlive() || !seen->IsInWorld() || seen->GetMap() != bot->GetMap())
            AbortRegain(botAI, bot, state, nowMs);
    }

    state.PlanHolds = false;
    if (want != LosWant::None && state.RegainStartMs && want == state.Want && seeGuid == state.SeeGuid)
        state.PlanHolds =
            PlanHolds(bot, botAI->GetAiObjectContext()->GetValue<CoverPlan>("cover plan")->Get(), budget, rays);

    uint32 const regainMs = state.RegainStartMs ? nowMs - state.RegainStartMs : 0;
    LosEvent const event = LosNoteSight(state, want, seeGuid, bot->GetGUID().GetRawValue(), nowMs);
    state.RefreshMs = std::max(nowMs, 1u);
    if (event == LosEvent::Timeout)
    {
        botAI->CountTactic(TacticCounter::LosTimeouts);
        LOG_TRACE("playerbots", "Los: {} gave up regaining sight of its {} after {} ms, skill {}, at {} ms",
                  bot->GetName(), LosWantName(state.Want), regainMs, botAI->GetPvpSkill(), nowMs);
    }
    else if (event == LosEvent::Regained)
        LOG_TRACE("playerbots", "Los: {} sees again after {} ms of regaining, skill {}, at {} ms", bot->GetName(),
                  regainMs, botAI->GetPvpSkill(), nowMs);
}

// Both triggers: refresh, then is the bot's regain for this want due now (and the tactic used in this window)?
bool LosTriggerDue(PlayerbotAI* botAI, Player* bot, LosWant want, bool enabled)
{
    uint32 const nowMs = getMSTime();
    LosBotState& state = LosState(botAI);
    if (LosIdleHere(bot, state) || !bot->IsAlive())
    {
        LosResetEpisode(state);
        return false;
    }

    RefreshLos(botAI, bot, state, enabled, want == LosWant::Partner, nowMs);
    if (!enabled || state.Want != want)
        return false;

    bool atPoint = false;
    if (state.RegainStartMs)
    {
        CoverPlan const& plan = botAI->GetAiObjectContext()->GetValue<CoverPlan>("cover plan")->Get();
        atPoint = plan.Valid && bot->GetExactDist2d(plan.Point.X, plan.Point.Y) < LOS_AT_POINT_YARDS;
    }

    LosDue const due = LosRegainDue(state, atPoint, bot->isMoving(), botAI->GetCoverState().LastSearchMs, nowMs);
    if (due == LosDue::Guarded)
    {
        if (!state.Guarding)
        {
            state.Guarding = true;
            ++state.Guarded;
            botAI->CountTactic(TacticCounter::RelocGuards);
        }

        return false;
    }

    state.Guarding = false;
    if (due == LosDue::No || !botAI->TacticActive(TacticId::Los))
        return false;

    state.Due = static_cast<uint8>(due);
    state.DueMs = std::max(nowMs, 1u);
    return true;
}

// The nearest enemy caster in the arena other than the unit to see (class and position only: what a player sees)
Unit* NearestEnemyCaster(Player* bot, Battleground* bg, Unit* see)
{
    if (!bg)
        return nullptr;

    Player* best = nullptr;
    float bestYards = LOS_PARTNER_YARDS;
    for (auto const& [guid, listed] : bg->GetPlayers())
    {
        Player* enemy = ObjectAccessor::GetPlayer(*bot, guid);
        if (!enemy || enemy == see || !enemy->IsAlive() || enemy->GetBgTeamId() == bot->GetBgTeamId() ||
            !PlayerbotAI::IsRanged(enemy))
            continue;

        float const yards = bot->GetDistance(enemy);
        if (yards < bestYards)
        {
            bestYards = yards;
            best = enemy;
        }
    }

    return best;
}

float PathYards(Movement::PointsArray const& points)
{
    float yards = 0.0f;
    for (size_t index = 1; index < points.size(); ++index)
        yards += (points[index] - points[index - 1]).length();

    return yards;
}

// An arena with pillars: a ring point that sees the unit (FindCoverPoint; from HidesFromCaster also hidden from the
// nearest enemy caster), its ground at the pillar's floor and an mmap path to it of at most LOS_PATH_FACTOR x the walk
// (the table's Z is the pillar's floor: Dalaran's pipes and Blade's Edge's bridge are a second level); never within
// LOS_MELEE_YARDS of an enemy target
bool FindCoverRegain(Player* bot, Unit* see, bool enemyTarget, LosParams const& params, CoverPlan& plan, uint32& rays)
{
    Battleground* bg = ArenaOf(bot);
    ArenaCoverTable const* table = bg ? FindArenaCoverTable(bot->GetMapId()) : nullptr;
    if (!table || !table->Count)
        return false;

    Map* map = bot->GetMap();
    uint32 const phaseMask = bot->GetPhaseMask();
    auto const los = [map, phaseMask](CoverVec const& a, CoverVec const& b)
    { return ArenaCoverLos(map, phaseMask, a, b); };
    Unit* caster = params.HidesFromCaster ? NearestEnemyCaster(bot, bg, see) : nullptr;
    CoverVec const seeAt = PositionOf(see);
    CoverSearchParams search;
    search.RequireHidden = false;
    search.PreferHidden = caster != nullptr;
    search.MaxRays = params.MaxRays;
    CoverSearchResult const found =
        FindCoverPoint(*table, ArenaCoverActiveGates(bg), PositionOf(bot), caster ? PositionOf(caster) : seeAt, &seeAt,
                       LOS_MAX_MOVE_YARDS, los, search);
    rays += found.Rays;
    if (!found.Found || LosPointInMelee(found.Point, seeAt, enemyTarget))
        return false;

    float const z = map->GetHeight(phaseMask, found.Point.X, found.Point.Y, found.Point.Z + LOS_FLOOR_DZ + 1.0f, true,
                                   2.0f * LOS_FLOOR_DZ + 1.0f);
    if (z <= INVALID_HEIGHT + 1.0f || std::fabs(z - found.Point.Z) > LOS_FLOOR_DZ)
        return false;

    PathGenerator path(bot);
    path.CalculatePath(found.Point.X, found.Point.Y, z, false);
    if (!(path.GetPathType() & PATHFIND_NORMAL) || PathYards(path.GetPath()) > LOS_MAX_MOVE_YARDS * LOS_PATH_FACTOR)
        return false;

    plan.Pillar = found.Pillar;
    plan.Point = {found.Point.X, found.Point.Y, z};
    plan.WantHidden = caster && found.Hidden;
    plan.EnemyGuid = plan.WantHidden ? caster->GetGUID().GetRawValue() : 0;
    return true;
}

// Anywhere (and an episode into the open): the first point of the mmap path to the unit that sees it (as MoveToLOS,
// at most the search's rays), never within LOS_MELEE_YARDS of an enemy target
bool FindPathRegain(Player* bot, Unit* see, bool enemyTarget, uint32 maxRays, CoverPlan& plan, uint32& rays)
{
    PathGenerator path(bot);
    path.CalculatePath(see->GetPositionX(), see->GetPositionY(), see->GetPositionZ(), false);
    if (path.GetPathType() & PATHFIND_NOPATH)
        return false;

    Movement::PointsArray const& points = path.GetPath();
    for (size_t index = 1; index < points.size() && rays < maxRays; ++index)
    {
        G3D::Vector3 const& point = points[index];
        if (enemyTarget && see->GetDistance(point.x, point.y, point.z) < LOS_MELEE_YARDS)
            return false;

        ++rays;
        if (!see->IsWithinLOS(point.x, point.y, point.z + bot->GetCollisionHeight()))
            continue;

        plan.Pillar = -1;
        plan.Point = {point.x, point.y, point.z};
        plan.WantHidden = false;
        plan.EnemyGuid = 0;
        return true;
    }

    return false;
}
}  // namespace

void TacticLosStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    // Above the plain movement, below the combat actions: a regain must not outrank a heal or an interrupt
    triggers.push_back(new TriggerNode("target out of los", {NextAction("regain los", ACTION_MOVE + 2)}));
    triggers.push_back(new TriggerNode("partner out of los", {NextAction("regain los", ACTION_MOVE + 2)}));
}

void TacticLosStrategy::InitMultipliers(std::vector<Multiplier*>& multipliers)
{
    multipliers.push_back(new TacticLosMultiplier(botAI));
}

void TacticPillarStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    // Below the interrupts (ACTION_INTERRUPT), above the movement
    triggers.push_back(new TriggerNode("cast at me", {NextAction("break los", ACTION_INTERRUPT - 1)}));
    triggers.push_back(new TriggerNode("pillar step", {NextAction("pillar step", ACTION_MOVE + 3)}));
}

// ArenaTactics walks a ranged bot into its target on lost LOS (BattleGroundTactics.cpp); a bot whose regain runs or is
// due moves to a point that sees instead. Cheap exits first: outside arenas every action ends at one test.
float TacticLosMultiplier::GetValue(Action* action)
{
    if (!action || !bot->InArena() || !dynamic_cast<ArenaTactics*>(action) || !botAI->TacticEnabledHere(TacticId::Los))
        return 1.0f;

    // idle (gates, Ring of Valor's elevators), waiting out the lateness or given up: ArenaTactics' walk as before
    LosBotState& state = LosState(botAI);
    if (LosIdleHere(bot, state))
        return 1.0f;

    Unit* victim = bot->GetVictim();
    bool const parks = LosParksArenaTactics(PlayerbotAI::IsRanged(bot), bot->IsInCombat(), victim != nullptr,
                                            LosRegainActive(state, getMSTime()),
                                            victim ? bot->GetPositionZ() - victim->GetPositionZ() : 0.0f);
    return parks ? 0.0f : 1.0f;
}

bool TargetOutOfLosTrigger::IsActive()
{
    if (!botAI->TacticEnabledHere(TacticId::Los) || PlayerbotAI::IsHeal(bot) || !PlayerbotAI::IsRanged(bot))
        return false;

    return LosTriggerDue(botAI, bot, LosWant::Target, true);
}

bool PartnerOutOfLosTrigger::IsActive()
{
    // O(1) when the tactic is off and the harness does not count; with counting every healer measures
    // partnerLosLostMs (so a side without the tactic is the baseline)
    bool const enabled = botAI->TacticEnabledHere(TacticId::Los);
    if ((!enabled && !botAI->IsPvpTestCounting()) || !PlayerbotAI::IsHeal(bot))
        return false;

    return LosTriggerDue(botAI, bot, LosWant::Partner, enabled);
}

bool CastAtMeTrigger::IsActive() { return false; }

bool PillarStepTrigger::IsActive() { return false; }

bool RegainLosAction::isUseful()
{
    LosBotState const& state = LosState(botAI);
    if (static_cast<LosDue>(state.Due) == LosDue::No || getMSTime() - state.DueMs >= LOS_DUE_MS || !bot->IsAlive() ||
        bot->GetCurrentSpell(CURRENT_CHANNELED_SPELL))
        return false;

    // a healer's cast on someone it sees finishes first
    return state.Want != LosWant::Partner || !bot->IsNonMeleeSpellCast(false);
}

bool RegainLosAction::Execute(Event /*event*/)
{
    uint32 const nowMs = getMSTime();
    LosBotState& state = LosState(botAI);
    LosDue const due = static_cast<LosDue>(state.Due);
    state.Due = static_cast<uint8>(LosDue::No);
    Unit* see = ObjectAccessor::GetUnit(*bot, ObjectGuid(state.SeeGuid));
    if (!see || !see->IsAlive() || !see->IsInWorld() || see->GetMap() != bot->GetMap())
        return false;

    CoverPlan plan = AI_VALUE(CoverPlan, "cover plan");
    if (due == LosDue::Resume && plan.Valid)
    {
        state.ResumeMs = std::max(nowMs, 1u);
        return MoveTo(plan.MapId, plan.Point.X, plan.Point.Y, plan.Point.Z, false, false, false, false,
                      MovementPriority::MOVEMENT_COMBAT, true);
    }

    botAI->GetCoverState().LastSearchMs = std::max(nowMs, 1u);
    ++state.Searches;
    uint32 rays = 0;
    bool cover = false;
    bool found = false;
    {
        PerfMonitorScope scope(PerfMonitor::IsEnabled() ? sPerfMonitor.acquire(PERF_MON_TOTAL, "Cover::Search")
                                                        : nullptr);
        if (!state.EpisodeIntoOpen)
            cover = found = FindCoverRegain(bot, see, state.Want == LosWant::Target, state.Params, plan, rays);

        if (!found)
            found = FindPathRegain(bot, see, state.Want == LosWant::Target, state.Params.MaxRays, plan, rays);
    }

    state.LastSearchRays = rays;
    state.SearchRays += rays;
    if (!found)
    {
        // nothing sees it from here: given up until the bot sees it again (no search every interval for nothing)
        ++state.NoPoint;
        state.GaveUp = true;
        LOG_TRACE("playerbots", "Los: {} found no point that sees its {} ({} rays), skill {}, at {} ms", bot->GetName(),
                  LosWantName(state.Want), rays, botAI->GetPvpSkill(), nowMs);
        return false;
    }

    // a cast at the unseen target fails at its end anyway (Spell::CheckCast's LOS); other casts go on
    Spell* spell = state.Want == LosWant::Target ? bot->GetCurrentSpell(CURRENT_GENERIC_SPELL) : nullptr;
    if (spell && spell->m_targets.GetUnitTargetGUID() == see->GetGUID())
        bot->InterruptSpell(CURRENT_GENERIC_SPELL, false);

    if (!MoveTo(bot->GetMapId(), plan.Point.X, plan.Point.Y, plan.Point.Z, false, false, false, false,
                MovementPriority::MOVEMENT_COMBAT, true))
        return false;

    plan.UpdatedMs = std::max(nowMs, 1u);
    plan.Valid = true;
    plan.MapId = bot->GetMapId();
    plan.WantSee = true;
    plan.SeeGuid = state.SeeGuid;
    SET_AI_VALUE(CoverPlan, "cover plan", plan);
    state.Relocs.Note(nowMs);
    ++(cover ? state.CoverPoints : state.PathPoints);
    if (!state.RegainStartMs)
    {
        state.RegainStartMs = std::max(nowMs, 1u);
        ++state.Regains;
        botAI->CountTactic(TacticCounter::LosRegains);
        botAI->NoteTacticUse(TacticId::Los);
    }

    LOG_TRACE("playerbots",
              "Los: {} regains sight of its {} {} at ({:.1f}, {:.1f}, {:.1f}), {} {}, hidden {}, {} rays, lost {} ms, "
              "skill {}, at {} ms",
              bot->GetName(), LosWantName(state.Want), see->GetName(), plan.Point.X, plan.Point.Y, plan.Point.Z,
              cover ? "pillar" : "path point", plan.Pillar, plan.WantHidden ? "yes" : "no", rays,
              nowMs - state.LostSinceMs, botAI->GetPvpSkill(), nowMs);
    return true;
}

bool BreakLosAction::isUseful() { return false; }

bool BreakLosAction::Execute(Event /*event*/) { return false; }

bool PillarStepAction::isUseful() { return false; }

bool PillarStepAction::Execute(Event /*event*/) { return false; }
