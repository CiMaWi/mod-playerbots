/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "KiteTacticStrategy.h"

#include <algorithm>
#include <array>
#include <cmath>

#include "ArenaCover.h"
#include "Battleground.h"
#include "Event.h"
#include "GridTerrainData.h"
#include "Group.h"
#include "Log.h"
#include "LosTactic.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "PathGenerator.h"
#include "PerfMonitor.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "PvpTestCounters.h"
#include "SpellAuraDefines.h"
#include "SplitMix64.h"
#include "Timer.h"
#include "World.h"

// G3a: the Kite tactic's game side (the policy in ArenaKite.h, the numbers in KiteConfig). A healer or caster with an
// enemy melee player on it (from "fight enemies") snares it, steps round a pillar, takes a tested jump spot (G2g-b's
// table; none yet) or steps towards a partner, else away; moves share the Los tactic's relocation guards (G2e, G2f).
// For a bot that knows the tactic the old "flee" is zeroed in scope (DT.3). The sense (the melee, the meleeDistX10
// samples, the opponent memory's LOS-break rays) runs at most once per second per ranged bot.

namespace
{
// The bot's battleground (none in the open world); another one starts a fresh state
Battleground* KiteBattleground(Player* bot, KiteBotState& state)
{
    Battleground* bg = bot->GetBattleground();
    uint32 const instance = bg ? bg->GetInstanceID() : 0;
    if (state.Instance != instance)
    {
        state.Instance = instance;
        KiteResetMatch(state);
    }

    return bg;
}

// The numbers, read again at most every KITE_PARAMS_MS
KiteParams const& BotKiteParams(PlayerbotAI* botAI, Player* bot, KiteBotState& state, uint32 nowMs)
{
    if (state.ParamsMs && nowMs - state.ParamsMs < KITE_PARAMS_MS)
        return state.Params;

    KiteGateInputs in;
    in.Skill = botAI->GetPvpSkill();
    in.FullAbility = in.Skill == 0 || botAI->HasFullSkillAbility();
    in.Knowledge = botAI->GetAttribute(BotAttribute::Knowledge);
    in.Foresight = botAI->GetAttribute(BotAttribute::Foresight);
    in.Effort = botAI->GetSkillEffort();
    in.Caution = botAI->GetAttribute(BotAttribute::Caution);
    in.Patience = botAI->GetAttribute(BotAttribute::Patience);
    state.Params = ComputeKiteParams(sPlayerbotAIConfig.TacticKite, in);
    state.Ranged = PlayerbotAI::IsRanged(bot);
    state.ParamsMs = std::max(nowMs, 1u);
    return state.Params;
}

// Before the gates and in Ring of Valor's first RvIdleMs nothing moves (the Los tactic's rule, LosIdleHere)
bool KiteIdle(Battleground* bg)
{
    if (!bg)
        return false;

    bool const ringOfValor = bg->isArena() && bg->GetMapId() == COVER_MAP_RING_OF_VALOR;
    uint32 const prepMs = sWorld->getIntConfig(CONFIG_ARENA_PREP_TIME) * IN_MILLISECONDS;
    return LosArenaIdle(bg->GetStatus() == STATUS_IN_PROGRESS, ringOfValor, LosSinceGatesMs(bg->GetStartTime(), prepMs),
                        sPlayerbotAIConfig.TacticPillar.RvIdleMs);
}

CoverVec KitePositionOf(WorldObject const* object)
{
    return {object->GetPositionX(), object->GetPositionY(), object->GetPositionZ()};
}

bool KiteControlled(Unit* unit)
{
    return unit->HasUnitState(UNIT_STATE_LOST_CONTROL) || unit->HasUnitState(UNIT_STATE_ROOT) || unit->IsRooted() ||
           unit->isFrozen();
}

LosBotState& KiteLosState(PlayerbotAI* botAI)
{
    return botAI->GetAiObjectContext()->GetValue<LosBotState&>("los state")->Get();
}

// The enemies' LOS breaks: each watched enemy (seen at the last look) gets one ray; one that moved out of sight broke
// LOS (the opponent memory notes the pillar, the side and the time after the last crowd control seen); free watches
// take the nearest enemy casters, looked at from the next sense on
void WatchLosBreaks(PlayerbotAI* botAI, Player* bot, Battleground* bg, KiteBotState& state,
                    std::array<Unit*, KITE_MEMORY_SLOTS> const& casters, uint32 nowMs)
{
    ArenaCoverTable const* table = FindArenaCoverTable(bot->GetMapId());
    uint32 const gates = table ? ArenaCoverActiveGates(bg) : 0;
    CoverVec const self = KitePositionOf(bot);
    // a bot that moved itself since the last look may have walked out of the line: no verdict on the enemy's LOS break
    bool const botMoved = state.LookSelfSet && CoverDist2d(state.LookSelf, self) > KITE_BREAK_MOVED_YARDS;
    state.LookSelf = self;
    state.LookSelfSet = true;
    for (KiteWatch& watch : state.Watch)
    {
        if (!watch.Guid)
            continue;

        Unit* unit = ObjectAccessor::GetUnit(*bot, ObjectGuid(watch.Guid));
        if (!unit || !unit->IsAlive() || !unit->IsInWorld() || unit->GetMap() != bot->GetMap())
        {
            watch = KiteWatch();
            continue;
        }

        CoverVec const at = KitePositionOf(unit);
        bool const visible = bot->IsWithinLOSInMap(unit, VMAP::ModelIgnoreFlags::M2);
        KiteLook const look = KiteJudgeLook(watch, at, visible, botMoved);
        if (look == KiteLook::Keep)
        {
            watch.At = at;
            watch.Seen = true;
            continue;
        }

        if (look == KiteLook::Break)
        {
            int32 const pillar = table ? KiteNearestPillar(*table, gates, at) : -1;
            int32 const side = pillar >= 0 ? KiteSide(self, table->Pillars[pillar], at) : 0;
            uint32 const afterCcMs = KiteAfterCcMs(state.EnemyCcMs, nowMs);
            botAI->GetOpponentMemory().NoteLosBreak(unit->GetGUID().GetCounter(), pillar, side, afterCcMs, nowMs);
            ++state.LosBreaksSeen;
            LOG_TRACE("playerbots",
                      "Kite: {} saw {} break line of sight at pillar {} (side {}), {} ms after a crowd control, map "
                      "{}, at {} ms",
                      bot->GetName(), unit->GetName(), pillar, side, afterCcMs, bot->GetMapId(), nowMs);
        }

        watch = KiteWatch();
    }

    for (Unit* unit : casters)
    {
        if (!unit)
            continue;

        uint64 const guid = unit->GetGUID().GetRawValue();
        KiteWatch* open = nullptr;
        bool watched = false;
        for (KiteWatch& watch : state.Watch)
        {
            watched = watched || watch.Guid == guid;
            if (!watch.Guid && !open)
                open = &watch;
        }

        if (watched || !open)
            continue;

        open->Guid = guid;
        open->At = KitePositionOf(unit);
        open->Seen = false;
    }
}

// Once per KITE_SENSE_MS: the nearest free melee player on the bot (the chaser), the meleeDistX10/meleeNearMs sample
// (both arms: the tactic on or off), a crowd control on an enemy, and with the memory read (arenas) the LOS breaks
void SenseKite(PlayerbotAI* botAI, Player* bot, Battleground* bg, KiteBotState& state, bool memory, uint32 nowMs)
{
    if (!KiteSenseDue(state, nowMs))
        return;

    PerfMonitorScope scope(PerfMonitor::IsEnabled() ? sPerfMonitor.acquire(PERF_MON_TOTAL, "Kite::Sense") : nullptr);
    KiteConfig const& config = sPlayerbotAIConfig.TacticKite;
    GuidVector const enemies = botAI->GetAiObjectContext()->GetValue<GuidVector>("fight enemies")->Get();
    Unit* chaser = nullptr;
    float chaserYards = KITE_NO_MELEE;
    float nearest = KITE_NO_MELEE;
    bool enemyCc = false;
    std::array<Unit*, KITE_MEMORY_SLOTS> casters{};
    std::array<float, KITE_MEMORY_SLOTS> casterYards{};
    casterYards.fill(KITE_NO_MELEE);
    bool const watchCasters = memory && bg && bg->isArena();
    for (ObjectGuid const& guid : enemies)
    {
        Unit* unit = ObjectAccessor::GetUnit(*bot, guid);
        if (!unit || !unit->IsAlive() || !unit->IsInWorld() || unit->GetMap() != bot->GetMap())
            continue;

        if (unit->HasUnitState(UNIT_STATE_CONTROLLED))
            enemyCc = true;

        Player* player = unit->ToPlayer();
        if (!player)
            continue;

        float const yards = bot->GetDistance(unit);
        KiteMeleeView view;
        view.Yards = yards;
        view.Melee = !PlayerbotAI::IsRanged(player);
        view.TargetsBot = unit->GetVictim() == bot || unit->GetTarget() == bot->GetGUID();
        view.Controlled = KiteControlled(unit);
        if (KiteMeleeSampled(view))
            nearest = std::min(nearest, yards);

        if (KiteMeleeOn(config, view) && yards < chaserYards)
        {
            chaser = unit;
            chaserYards = yards;
        }

        if (!watchCasters || view.Melee)
            continue;

        // the two nearest enemy casters, nearest first
        for (uint32 index = 0; index < KITE_MEMORY_SLOTS; ++index)
        {
            if (yards >= casterYards[index])
                continue;

            for (uint32 move = KITE_MEMORY_SLOTS - 1; move > index; --move)
            {
                casters[move] = casters[move - 1];
                casterYards[move] = casterYards[move - 1];
            }

            casters[index] = unit;
            casterYards[index] = yards;
            break;
        }
    }

    KiteSample const sample = KiteTakeSample(config, state.SenseMs, nearest, nowMs);
    if (sample.Counted)
    {
        botAI->CountTactic(TacticCounter::MeleeDistX10, sample.DistX10);
        botAI->CountTactic(TacticCounter::MeleeDistN);
        if (sample.NearMs)
            botAI->CountTactic(TacticCounter::MeleeNearMs, sample.NearMs);
    }

    state.ChaserGuid = chaser ? chaser->GetGUID().GetRawValue() : 0;
    state.ChaserYards = chaser ? chaserYards : 0.0f;
    state.NearestMeleeYards = nearest;
    if (enemyCc)
        state.EnemyCcMs = std::max(nowMs, 1u);

    if (watchCasters)
        WatchLosBreaks(botAI, bot, bg, state, casters, nowMs);

    state.SenseMs = std::max(nowMs, 1u);
}

// A healer casting while a partner is low keeps casting (the heal wins over the step)
bool KiteHealHoldNow(Player* bot)
{
    if (!PlayerbotAI::IsHeal(bot) || !bot->IsNonMeleeSpellCast(false, false, true))
        return false;

    uint32 lowest = 100;
    if (Group* group = bot->GetGroup())
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* member = ref->GetSource();
            if (!member || member == bot || !member->IsAlive() || !member->IsInWorld() ||
                member->GetMap() != bot->GetMap())
                continue;

            lowest = std::min<uint32>(lowest, static_cast<uint32>(member->GetHealthPct()));
        }

    return KiteHealHold(true, true, lowest);
}

// The nearest alive group partner at least PartnerMinYards away (within KITE_PARTNER_MAX_YARDS)
Player* KitePartnerOf(Player* bot, float minYards)
{
    Group* group = bot->GetGroup();
    if (!group)
        return nullptr;

    Player* best = nullptr;
    float bestYards = KITE_PARTNER_MAX_YARDS;
    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (!member || member == bot || !member->IsAlive() || !member->IsInWorld() || member->GetMap() != bot->GetMap())
            continue;

        float const yards = bot->GetExactDist2d(member);
        if (yards >= minYards && yards < bestYards)
        {
            bestYards = yards;
            best = member;
        }
    }

    return best;
}

// Ground at the point (within floorDz of its height), an mmap path of at most maxPathYards, never across Dalaran's
// waterfall (G2f's PillarWalkable with the kite's numbers)
bool KiteWalkable(Player* bot, CoverVec const& point, float floorDz, float maxPathYards, CoverVec& at, float& pathYards)
{
    Map* map = bot->GetMap();
    float const z =
        map->GetHeight(bot->GetPhaseMask(), point.X, point.Y, point.Z + floorDz + 1.0f, true, 2.0f * floorDz + 1.0f);
    if (z <= INVALID_HEIGHT + 1.0f || std::fabs(z - point.Z) > floorDz)
        return false;

    CoverVec const ground{point.X, point.Y, z};
    CoverVec const self = KitePositionOf(bot);
    uint32 const mapId = bot->GetMapId();
    if (PillarCrossesWaterfall(mapId, self, ground))
        return false;

    PathGenerator path(bot);
    path.CalculatePath(point.X, point.Y, z, false);
    if (!(path.GetPathType() & PATHFIND_NORMAL))
        return false;

    Movement::PointsArray const& points = path.GetPath();
    float yards = 0.0f;
    for (size_t index = 1; index < points.size(); ++index)
        yards += (points[index] - points[index - 1]).length();

    if (yards > maxPathYards || PillarWalkCrossesWaterfall(mapId, self, points))
        return false;

    at = ground;
    pathYards = yards;
    return true;
}

// G2g-b's hook: a tested escape jump spot (ArenaJump.h ChooseJumpSpot, Escape purpose) that saves JumpMinSavedYards or
// breaks LOS from the melee. The spot table has no rows until the jump spike passes (DT.13), so there is none yet.
bool KiteJumpSpot(Player* /*bot*/, Unit* /*chaser*/, CoverVec& /*from*/, CoverVec& /*land*/) { return false; }
}  // namespace

void TacticKiteStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    triggers.push_back(new TriggerNode("melee on me", {NextAction("kite step", ACTION_MOVE + 4)}));
}

void TacticKiteStrategy::InitMultipliers(std::vector<Multiplier*>& multipliers)
{
    multipliers.push_back(new TacticKiteMultiplier(botAI));
}

// DT.3: "flee" (a straight-line run from the current target, RangedCombatStrategy and the class strategies) is off for
// a bot that knows the tactic, in scope; in the open world only when it would flee from a player (PvE untouched). A bot
// below the knowledge threshold keeps today's flee (the low-skill panic run).
float TacticKiteMultiplier::GetValue(Action* action)
{
    if (!action || !botAI->TacticEnabledHere(TacticId::Kite) || !dynamic_cast<FleeAction*>(action))
        return 1.0f;

    // only a flee from a player (battlegrounds too: an NPC boss keeps the old flee)
    Unit* target = botAI->GetAiObjectContext()->GetValue<Unit*>("current target")->Get();
    if (!target || !target->ToPlayer())
        return 1.0f;

    KiteBotState& state = botAI->GetKiteState();
    KiteBattleground(bot, state);
    return BotKiteParams(botAI, bot, state, getMSTime()).Known ? 0.0f : 1.0f;
}

bool MeleeOnMeTrigger::IsActive()
{
    bool const enabled = botAI->TacticEnabledHere(TacticId::Kite);
    Battleground* bgHere = bot->GetBattleground();
    if (!enabled && !bgHere)
        return false;

    uint32 const nowMs = getMSTime();
    KiteBotState& state = botAI->GetKiteState();
    Battleground* bg = KiteBattleground(bot, state);
    KiteParams const& params = BotKiteParams(botAI, bot, state, nowMs);
    if (!state.Ranged || !bot->IsAlive() || KiteIdle(bg))
        return false;

    SenseKite(botAI, bot, bg, state, enabled && params.Memory, nowMs);
    if (!enabled)
        return false;

    bool const meleeOn = state.ChaserGuid != 0;
    if (KiteNoteMelee(state, params, meleeOn, nowMs) == KiteEpisodeEvent::GaveUp)
        LOG_TRACE("playerbots", "Kite: {} gives up kiting after {} ms (episode {}, {} steps), skill {}, at {} ms",
                  bot->GetName(), params.GiveUpMs, state.Episodes, state.Steps, botAI->GetPvpSkill(), nowMs);

    state.LastWhy = KiteTriggerVerdict(params, state, meleeOn, bot->HasUnitState(UNIT_STATE_LOST_CONTROL), nowMs);
    if (state.LastWhy != KiteWhy::Go || !botAI->TacticActive(TacticId::Kite))
        return false;

    state.LastTryMs = std::max(nowMs, 1u);
    state.Due = true;
    state.DueMs = state.LastTryMs;
    return true;
}

bool KiteStepAction::isUseful()
{
    KiteBotState const& state = botAI->GetKiteState();
    return state.Due && getMSTime() - state.DueMs < LOS_DUE_MS && bot->IsAlive();
}

// One kite step: KitePick's order, each option checked lazily (snare castable, pillar point walkable, partner step
// walkable, step away walkable); a move notes the shared relocation ring and counts
bool KiteStepAction::Execute(Event /*event*/)
{
    uint32 const nowMs = getMSTime();
    KiteBotState& state = botAI->GetKiteState();
    state.Due = false;
    Unit* chaser = ObjectAccessor::GetUnit(*bot, ObjectGuid(state.ChaserGuid));
    if (!chaser || !chaser->IsAlive() || !chaser->IsInWorld() || chaser->GetMap() != bot->GetMap())
        return false;

    PerfMonitorScope scope(PerfMonitor::IsEnabled() ? sPerfMonitor.acquire(PERF_MON_TOTAL, "Kite::Step") : nullptr);
    KiteConfig const& config = sPlayerbotAIConfig.TacticKite;
    Battleground* bg = bot->GetBattleground();
    bool const arena = bg && bg->isArena();
    CoverVec const self = KitePositionOf(bot);
    CoverVec const chaserAt = KitePositionOf(chaser);
    uint64 const hash = SplitMix64(bot->GetGUID().GetRawValue() ^ (static_cast<uint64>(nowMs) << 20) ^ state.Steps);
    LosBotState& los = KiteLosState(botAI);

    KitePickGate gate;
    gate.Known = state.Params.Known;
    gate.HealHold = KiteHealHoldNow(bot);
    gate.StepDue = KiteStepDue(config, state, nowMs);
    bool const relocGuarded =
        LosRelocGuard(los.Relocs, nowMs, PillarGuardParams(sPlayerbotAIConfig.TacticLos)) != LosGuard::Ok;
    gate.Guarded = relocGuarded || !botAI->CanMove();  // a rooted bot skips the path work (a snare cast still goes)
    gate.SnareFirst = config.SnareFirst != 0;

    int32 snareRow = -1;
    KitePillarPick pillarPick;
    KiteStepPoint stepPoint;
    CoverVec at;
    float pathYards = 0.0f;
    float const stepYards = KiteStepYards(config, hash);
    auto const can = [&](KiteMove move) -> bool
    {
        switch (move)
        {
            case KiteMove::Snare:
            {
                if (chaser->HasAuraType(SPELL_AURA_MOD_DECREASE_SPEED) || chaser->HasAuraType(SPELL_AURA_MOD_ROOT) ||
                    KiteControlled(chaser))
                    return false;

                snareRow = KitePickSnare(bot->getClass(),
                                         [&](KiteSnareRow const& row)
                                         {
                                             if (row.Facing && !bot->HasInArc(COVER_PI / 2.0f, chaser))
                                                 return false;

                                             return botAI->CanCastSpell(row.Spell, row.OnSelf ? bot : chaser);
                                         });
                return snareRow >= 0;
            }
            case KiteMove::Pillar:
            {
                ArenaCoverTable const* table = arena ? FindArenaCoverTable(bot->GetMapId()) : nullptr;
                if (!table || !table->Count)
                    return false;

                std::array<KiteHide, KITE_MAX_HIDES> hides{};
                uint32 const hideCount = state.Params.Memory
                                             ? KiteHidesFrom(botAI->GetOpponentMemory(), nowMs, config.MemoryWindowMs,
                                                             hides.data(), KITE_MAX_HIDES)
                                             : 0;
                pillarPick = PickKitePillar(*table, ArenaCoverActiveGates(bg), self, chaserAt, stepYards,
                                            static_cast<float>(config.PillarMaxYards),
                                            bot->GetMapId() == COVER_MAP_RING_OF_VALOR, hides.data(), hideCount);
                return pillarPick.Found && KiteWalkable(bot, pillarPick.Point, LOS_FLOOR_DZ,
                                                        config.PillarMaxYards * LOS_PATH_FACTOR, at, pathYards);
            }
            case KiteMove::Jump:
            {
                CoverVec from;
                CoverVec land;
                return arena && KiteJumpSpot(bot, chaser, from, land);
            }
            case KiteMove::Partner:
            {
                Player* partner = KitePartnerOf(bot, static_cast<float>(config.PartnerMinYards));
                if (!partner)
                    return false;

                stepPoint = KitePartnerPoint(self, KitePositionOf(partner), chaserAt, stepYards,
                                             static_cast<float>(config.PartnerMinYards), hash);
                return stepPoint.Found &&
                       KiteWalkable(bot, stepPoint.Point, KITE_FLOOR_DZ, stepYards * LOS_PATH_FACTOR, at, pathYards);
            }
            case KiteMove::Away:
                stepPoint = KiteAwayPoint(self, chaserAt, stepYards, hash);
                return stepPoint.Found &&
                       KiteWalkable(bot, stepPoint.Point, KITE_FLOOR_DZ, stepYards * LOS_PATH_FACTOR, at, pathYards);
            default:
                return false;
        }
    };

    KiteMove const move = KitePick(gate, can);
    if (gate.StepDue && relocGuarded && !state.Guarding)
    {
        state.Guarding = true;
        ++state.Guarded;
        botAI->CountTactic(TacticCounter::RelocGuards);
    }
    else if (!relocGuarded)
        state.Guarding = false;

    state.LastMove = move;
    state.LastMoveMs = std::max(nowMs, 1u);
    if (move == KiteMove::Stay || move == KiteMove::Panic || move == KiteMove::None)
    {
        ++state.Stays;
        return false;
    }

    if (move == KiteMove::Snare)
    {
        KiteSnareRow const& row = KITE_SNARES[snareRow];
        if (!botAI->CastSpell(row.Spell, row.OnSelf ? bot : chaser))
        {
            ++state.Refused;
            return false;
        }

        ++state.Steps;
        ++state.Snares;
        botAI->CountTactic(TacticCounter::KiteSteps);
        botAI->CountTactic(TacticCounter::KiteSnares);
        botAI->NoteTacticUse(TacticId::Kite);
        LOG_TRACE("playerbots", "Kite: {} snares {} with {} at {:.1f} yd (episode {}), skill {}, at {} ms",
                  bot->GetName(), chaser->GetName(), row.Spell, state.ChaserYards, state.Episodes, botAI->GetPvpSkill(),
                  nowMs);
        return true;
    }

    // Jump: KiteJumpSpot has no spot yet (G2g-b); a move below is a pillar, partner or away step
    if (move == KiteMove::Jump ||
        !MoveTo(bot->GetMapId(), at.X, at.Y, at.Z, false, false, false, false, MovementPriority::MOVEMENT_FORCED, true))
    {
        ++state.Refused;
        return false;
    }

    los.Relocs.Note(nowMs);
    state.LastStepMs = std::max(nowMs, 1u);
    ++state.Steps;
    botAI->CountTactic(TacticCounter::KiteSteps);
    botAI->NoteTacticUse(TacticId::Kite);
    float bendDeg = stepPoint.BendDeg;
    int32 pillar = -1;
    if (move == KiteMove::Pillar)
    {
        ++state.Pillars;
        state.LastPillar = pillarPick.Pillar;
        state.MemoryAvoided += pillarPick.Avoided ? 1 : 0;
        pillar = pillarPick.Pillar;
        bendDeg = 0.0f;
        botAI->CountTactic(TacticCounter::KitePillars);
    }
    else if (move == KiteMove::Partner)
    {
        ++state.Partner;
        botAI->CountTactic(TacticCounter::KitePartner);
    }
    else
        ++state.Away;

    LOG_TRACE("playerbots",
              "Kite: {} steps {} from ({:.1f}, {:.1f}) to ({:.1f}, {:.1f}, {:.1f}), {:.1f} yd path, bend {:.0f}, "
              "pillar {}{}, from {} at {:.1f} yd (episode {}), map {}, skill {}, at {} ms",
              bot->GetName(), KiteMoveName(move), self.X, self.Y, at.X, at.Y, at.Z, pathYards, bendDeg, pillar,
              pillarPick.Avoided && move == KiteMove::Pillar ? " (memory)" : "", chaser->GetName(), state.ChaserYards,
              state.Episodes, bot->GetMapId(), botAI->GetPvpSkill(), nowMs);
    return true;
}
