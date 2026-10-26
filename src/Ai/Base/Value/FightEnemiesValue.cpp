/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "FightEnemiesValue.h"

#include <algorithm>

#include "Battleground.h"
#include "CombatManager.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "Tactics.h"

namespace
{
// A seated player sees only what is visible to him: stealthed enemies stay out of the list (DT.12)
bool IsFightEnemy(Player* bot, Unit* unit)
{
    return unit && unit != bot && unit->IsInWorld() && unit->GetMap() == bot->GetMap() && unit->IsAlive() &&
           bot->IsValidAttackTarget(unit) && bot->CanSeeOrDetect(unit);
}
}  // namespace

GuidVector FightEnemiesValue::Calculate()
{
    std::vector<Unit*> candidates;
    if (botAI->AllowActivity(ALL_ACTIVITY))
    {
        switch (PlayerbotAI::ProbeTacticContext(bot))
        {
            case TacticContext::Arena:
                AddArenaEnemies(candidates);
                break;
            case TacticContext::Battleground:
                AddAttackers(candidates, true);
                break;
            case TacticContext::OpenWorldPvp:
                AddOpenWorldEnemies(candidates);
                break;
            default:
                AddAttackers(candidates, false);
                break;
        }
    }

    std::sort(candidates.begin(), candidates.end());
    candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());

    std::vector<std::pair<float, Unit*>> byDistance;
    byDistance.reserve(candidates.size());
    for (Unit* unit : candidates)
        byDistance.emplace_back(bot->GetExactDistSq(unit), unit);

    std::sort(byDistance.begin(), byDistance.end(),
              [](auto const& left, auto const& right)
              {
                  if (left.first != right.first)
                      return left.first < right.first;

                  return left.second->GetGUID() < right.second->GetGUID();
              });

    GuidVector result;
    for (auto const& [distance, unit] : byDistance)
    {
        if (result.size() >= TACTIC_FIGHT_ENEMIES_MAX)
            break;

        result.push_back(unit->GetGUID());
    }

    botAI->SetFightEnemiesSnapshot(result);
    return result;
}

void FightEnemiesValue::AddArenaEnemies(std::vector<Unit*>& candidates)
{
    Battleground* arena = bot->GetBattleground();
    if (!arena)
        return;

    TeamId const team = bot->GetBgTeamId();
    for (auto const& entry : arena->GetPlayers())
    {
        Player* player = entry.second;
        if (player && player->GetBgTeamId() != team && IsFightEnemy(bot, player))
            candidates.push_back(player);
    }
}

void FightEnemiesValue::AddAttackers(std::vector<Unit*>& candidates, bool playersOnly)
{
    for (ObjectGuid const guid : AI_VALUE(GuidVector, "attackers"))
    {
        Unit* unit = botAI->GetUnit(guid);
        if (unit && unit->IsInWorld() && (!playersOnly || unit->IsPlayer()))
            candidates.push_back(unit);
    }
}

void FightEnemiesValue::AddOpenWorldEnemies(std::vector<Unit*>& candidates)
{
    std::vector<Unit*> sources;
    if (Unit* attacker = bot->getAttackerForHelper())
        sources.push_back(attacker->GetCharmerOrOwnerPlayerOrPlayerItself());

    for (auto const& entry : bot->GetCombatManager().GetPvPCombatRefs())
    {
        if (entry.second)
            sources.push_back(entry.second->GetOther(bot));
    }

    if (bot->duel && bot->duel->Opponent)
        sources.push_back(bot->duel->Opponent);

    sources.push_back(AI_VALUE(Unit*, "enemy player target"));
    for (Unit* unit : sources)
    {
        if (IsFightEnemy(bot, unit))
            candidates.push_back(unit);
    }
}
