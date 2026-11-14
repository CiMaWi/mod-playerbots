/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "EnemyPlayerValue.h"
#include "AttackersValue.h"
#include "CombatManager.h"
#include "PerfMonitor.h"
#include "Playerbots.h"
#include "PvpValues.h"
#include "ServerFacade.h"
#include "Strategy.h"
#include "TargetValue.h"
#include "TestConstraints.h"
#include "Vehicle.h"
#include "WorldPvp.h"

namespace
{
// Today's target checks' refusals (MayEnter's input); REFUSAL_FILTERED: the parent's filter
constexpr char const* REFUSAL_FILTERED = "filtered";
constexpr char const* REFUSAL_NOT_TARGET = "not_target";
constexpr char const* REFUSAL_NOT_FLAGGED = "not_flagged";
constexpr char const* REFUSAL_EXCLUSION = "exclusion";
constexpr char const* REFUSAL_NOT_VISIBLE = "not_visible";
constexpr char const* REFUSAL_MASTER = "master";

// Today's target checks on an enemy player after the parent's filter. Outside battlegrounds today's scan takes only
// enemies who can see the bot; a bot entering by enthusiasm (`byRules`) needs its own sight of the enemy, so a rogue
// that stealthed beside its pick still attacks it (WP4b)
char const* EnemyRefusal(PlayerbotAI* botAI, Player* bot, Unit* unit, bool const byRules)
{
    bool inCannon = botAI->IsInVehicle(false, true);
    Player* enemy = dynamic_cast<Player*>(unit);
    if (!enemy || !botAI->IsOpposing(enemy))
        return REFUSAL_NOT_TARGET;
    if (!enemy->IsPvP())
        return REFUSAL_NOT_FLAGGED;
    if (sPlayerbotAIConfig.IsPvpProhibited(enemy->GetZoneId(), enemy->GetAreaId()))
        return REFUSAL_EXCLUSION;
    if (enemy->HasFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_NON_ATTACKABLE | UNIT_FLAG_NON_ATTACKABLE_2) ||
        (!inCannon && enemy->HasFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_NOT_SELECTABLE)))
        return REFUSAL_NOT_TARGET;
    // Stealth and invisibility: !enemy->HasStealthAura() && !enemy->HasInvisibilityAura() is left to the sight check
    if (!((byRules || bot->InBattleground()) ? bot->CanSeeOrDetect(enemy) : enemy->CanSeeOrDetect(bot)))
        return REFUSAL_NOT_VISIBLE;
    if (enemy->HasSpiritOfRedemptionAura())
        return REFUSAL_NOT_TARGET;

    // If with master, only attack if master is PvP flagged
    Player* master = botAI->GetMaster();
    if (master && !master->IsPvP() && !master->IsFFAPvP())
        return REFUSAL_MASTER;

    return nullptr;
}
}  // namespace

bool NearestEnemyPlayersValue::AcceptUnit(Unit* unit)
{
    // World PvP (WP23): a bot that starts fights by the rules takes only an enemy its enthusiasm lets it enter on
    if (WorldPvp::StartsByRules(botAI))
        return unit->IsPlayer() && WorldPvp::MayEnter(botAI, unit, Refusal(unit, true));

    // Today's scan
    return !Refusal(unit, false);
}

char const* NearestEnemyPlayersValue::Refusal(Unit* unit, bool byRules)
{
    if (byRules && !AttackersValue::IsPossibleTarget(unit, bot, range))
        return REFUSAL_NOT_TARGET;
    // Apply parent's filtering first (includes level difference checks)
    if (!byRules && !PossibleTargetsValue::AcceptUnit(unit))
        return REFUSAL_FILTERED;
    return EnemyRefusal(botAI, bot, unit, byRules);
}

Unit* EnemyPlayerValue::Calculate()
{
    // The enemy scan (with the nearest enemy players' refresh when due); WorldPvp balance counts the local balance
    PerfMonitorScope scope(PerfMonitor::IsEnabled() ? sPerfMonitor.acquire(PERF_MON_TOTAL, "WorldPvp scan") : nullptr);
    // World PvP (WP23): the enthusiasm's engaged edge, traits and local balance before any step reads them
    bool const byRules = WorldPvp::StartsByRules(botAI);
    if (byRules)
        WorldPvp::RefreshEnthusiasm(botAI);
    bool controllingCannon = false;
    bool controllingVehicle = false;
    bool passenger = false;
    if (Vehicle* vehicle = bot->GetVehicle())
    {
        VehicleSeatEntry const* seat = vehicle->GetSeatForPassenger(bot);
        if (!seat || (!seat->CanControl() && !(seat->m_flags & VEHICLE_SEAT_FLAG_CAN_ATTACK)))
            return nullptr;  // a passenger seat that can't attack
        if (!seat->CanControl())
            passenger = true;  // fights from its seat: it can't close in
        else if (botAI->IsInVehicle(false, false, false, false, true))
            controllingCannon = true;
        else
            controllingVehicle = true;
    }

    // An enemy the team holds in a breakable CC is left alone (OwnCc, G4b; empty without it); a flag carrier is not:
    // the objective first
    GuidSet const exclusions = GatherStrategyTargetExclusions(botAI, TargetValueExclusionType::DpsTarget);

    // 1. Check units we are currently in PvP combat with.
    std::vector<Unit*> targets;
    Unit* pVictim = bot->GetVictim();
    for (auto const& [guid, combatRef] : bot->GetCombatManager().GetPvPCombatRefs())
    {
        Unit* pTarget = combatRef->GetOther(bot);
        // World PvP (WP24): not an enemy a bot that left its fight leaves alone (one that does not attack it)
        if (IsUnavailableCombatTarget(bot, pTarget) || WorldPvp::LeftAlone(botAI, pTarget))
            continue;
        if (pTarget == pVictim || !pTarget->IsPlayer() ||
            !(bot->InBattleground() ? bot->CanSeeOrDetect(pTarget) : pTarget->CanSeeOrDetect(bot)) ||
            !bot->IsWithinDist(pTarget, VISIBILITY_DISTANCE_NORMAL))
            continue;

        if ((bot->GetTeamId() == TEAM_HORDE && pTarget->HasAura(23333)) ||
            (bot->GetTeamId() == TEAM_ALLIANCE && pTarget->HasAura(23335)))
            return pTarget;

        if (exclusions.count(pTarget->GetGUID()))
            continue;

        targets.push_back(pTarget);
    }

    if (!targets.empty())
    {
        std::sort(targets.begin(), targets.end(),
                  [&](Unit const* pUnit1, Unit const* pUnit2)
                  { return bot->GetDistance(pUnit1) < bot->GetDistance(pUnit2); });

        return *targets.begin();
    }

    // A test's nofight bot starts no fight with a player either (TestConstraints::StartsFights), nor joins one; it
    // answers one already fighting it (above)
    if (!TestConstraints::StartsFights(botAI->rpgInfo.constraints))
        return nullptr;

    // 2. Find enemy player in range. A bot that starts fights by the rules skips it while its enthusiasm can enter
    // nothing; the enemy it enters on is attacked from the full distance (the thresholds weighed the balance already)
    GuidVector const players =
        byRules && WorldPvp::ScanSkipped(botAI) ? GuidVector() : AI_VALUE(GuidVector, "nearest enemy players");
    float const maxAggroDistance = GetMaxAttackDistance();
    for (auto const& gTarget : players)
    {
        Unit* pUnit = botAI->GetUnit(gTarget);
        if (!pUnit || IsUnavailableCombatTarget(bot, pUnit))
            continue;

        Player* pTarget = dynamic_cast<Player*>(pUnit);
        if (!pTarget)
            continue;

        if (pTarget == pVictim)
            continue;

        if (bot->GetTeamId() == TEAM_HORDE)
        {
            if (pTarget->HasAura(23333))
                return pTarget;
        }
        else
        {
            if (pTarget->HasAura(23335))
                return pTarget;
        }

        if (exclusions.count(pTarget->GetGUID()))
            continue;

        // Aggro weak enemies from further away.
        // If controlling mobile vehicle only agro close enemies (otherwise will never reach objective)
        uint32 const aggroDistance = passenger            ? 30.0f
                                     : controllingVehicle ? 5.0f
                                     : (controllingCannon || byRules || bot->GetHealth() > pTarget->GetHealth())
                                         ? maxAggroDistance
                                         : 20.0f;
        if (!bot->IsWithinDist(pTarget, aggroDistance))
            continue;

        // A bot entering by enthusiasm writes its entering row once the enemy is in reach (and a new pick)
        if (controllingCannon ? bot->IsWithinLOSInMap(pTarget) : WorldPvp::Reaches(bot, pTarget))
            if (!byRules || WorldPvp::NoteEntering(botAI, pTarget))
                return pTarget;
    }

    // 3. Check party attackers. A bot entering by enthusiasm joins its group's fight by step 2 (the group row of its
    // join threshold), not unasked here

    if (Group* pGroup = byRules ? nullptr : bot->GetGroup())
    {
        for (GroupReference* itr = pGroup->GetFirstMember(); itr != nullptr; itr = itr->next())
        {
            if (Unit* pMember = itr->GetSource())
            {
                if (pMember == bot)
                    continue;

                if (ServerFacade::instance().GetDistance2d(bot, pMember) > 30.0f)
                    continue;

                if (Unit* pAttacker = pMember->getAttackerForHelper())
                    if (!IsUnavailableCombatTarget(bot, pAttacker) && pAttacker->IsPlayer() &&
                        bot->IsWithinDist(pAttacker, maxAggroDistance * 2.0f) && bot->IsWithinLOSInMap(pAttacker) &&
                        pAttacker != pVictim && !exclusions.count(pAttacker->GetGUID()) &&
                        (bot->InBattleground() ? bot->CanSeeOrDetect(pAttacker) : pAttacker->CanSeeOrDetect(bot)))
                        return pAttacker;
            }
        }
    }

    return nullptr;
}

float EnemyPlayerValue::GetMaxAttackDistance()
{
    if (!bot->GetBattleground())
        return 60.0f;

    Battleground* bg = bot->GetBattleground();
    if (!bg)
        return 40.0f;

    BattlegroundTypeId bgType = bg->GetBgTypeID();
    if (bgType == BATTLEGROUND_RB)
        bgType = bg->GetBgTypeID(true);

    if (bgType == BATTLEGROUND_IC)
    {
        if (botAI->IsInVehicle(false, true))
            return 120.0f;
    }

    return 40.0f;
}
