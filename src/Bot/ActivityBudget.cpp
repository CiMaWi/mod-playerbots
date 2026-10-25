/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "ActivityBudget.h"

#include <sstream>
#include <string>
#include <vector>

#include "Chat.h"
#include "InstanceScript.h"
#include "Map.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "RandomPlayerbotMgr.h"
#include "ThreadCheck.h"
#include "Timer.h"
#include "UpdateTime.h"

namespace
{
uint32 SituationFloor(int32 situation)
{
    uint32 const global = sPlayerbotAIConfig.BotActiveAloneSmartScaleDiffLimitFloor;
    if (situation < 0 || situation >= static_cast<int32>(BotSituation::Count))
        return global;

    uint32 const value = sPlayerbotAIConfig.SituationDiffLimitFloor[situation];
    return value ? value : global;
}

uint32 SituationCeiling(int32 situation)
{
    uint32 const global = sPlayerbotAIConfig.BotActiveAloneSmartScaleDiffLimitCeiling;
    if (situation < 0 || situation >= static_cast<int32>(BotSituation::Count))
        return global;

    uint32 const value = sPlayerbotAIConfig.SituationDiffLimitCeiling[situation];
    return value ? value : global;
}

// Smart scale's factor (percent of BotActiveAlone kept), the same maths as PlayerbotAI::AutoScaleActivity
uint32 SmartScalePercent(uint32 metric, uint32 floor, uint32 ceiling)
{
    if (ceiling <= floor)
        return metric > ceiling ? 0 : 100;

    if (metric > ceiling)
        return 0;

    if (metric <= floor)
        return 100;

    double const lagProgress = (metric - floor) / static_cast<double>(ceiling - floor);
    return static_cast<uint32>(100 * (1 - lagProgress));
}

char const* SituationLabel(int32 situation)
{
    if (situation < 0 || situation >= static_cast<int32>(BotSituation::Count))
        return "none";

    return ActivityBudget::SituationName(static_cast<BotSituation>(situation));
}
}  // namespace

void ActivityBudget::Update()
{
    if (sPlayerbotAIConfig.SituationEnable && sPlayerbotAIConfig.BotActiveAloneSmartScale)
    {
        uint32 const now = getMSTime();
        if (!_classified || getMSTimeDiff(_lastClassifyMs, now) >= sPlayerbotAIConfig.SituationIntervalMs)
        {
            _lastClassifyMs = now;
            ClassifyPlayers();
        }
    }

    uint32 const percentile = sPlayerbotAIConfig.BotActiveAloneSmartScalePercentile;
    if (!percentile || !sPlayerbotAIConfig.BotActiveAloneSmartScale)
        return;

    // GetPercentile reads the size minus one: an empty table would wrap
    if (!sWorldUpdateTime.GetDatasetSize())
        return;

    _tickMs.store(sWorldUpdateTime.GetPercentile(static_cast<uint8>(percentile)), std::memory_order_relaxed);
}

uint32 ActivityBudget::GetTickMs() const
{
    if (!sPlayerbotAIConfig.BotActiveAloneSmartScalePercentile)
        return sWorldUpdateTime.GetMaxUpdateTimeOfCurrentTable();

    return _tickMs.load(std::memory_order_relaxed);
}

uint32 ActivityBudget::GetDiffLimitFloor() const
{
    if (sPlayerbotAIConfig.SituationEnable)
    {
        // 0 until the first classification
        uint32 const floor = _floorMs.load(std::memory_order_relaxed);
        if (floor)
            return floor;
    }

    return sPlayerbotAIConfig.BotActiveAloneSmartScaleDiffLimitFloor;
}

uint32 ActivityBudget::GetDiffLimitCeiling() const
{
    if (sPlayerbotAIConfig.SituationEnable)
    {
        uint32 const ceiling = _ceilingMs.load(std::memory_order_relaxed);
        if (ceiling)
            return ceiling;
    }

    return sPlayerbotAIConfig.BotActiveAloneSmartScaleDiffLimitCeiling;
}

BotSituation ActivityBudget::Classify(Player* player)
{
    if (PlayerbotAI::IsPlayerInPvpFight(player))
        return BotSituation::Pvp;

    Map* map = player->GetMap();
    if (map && (map->IsDungeon() || map->IsRaid()))
    {
        InstanceScript* instance = player->GetInstanceScript();
        if (instance && instance->IsEncounterInProgress())
            return BotSituation::Boss;

        return BotSituation::Dungeon;
    }

    return BotSituation::OpenWorld;
}

char const* ActivityBudget::SituationName(BotSituation situation)
{
    switch (situation)
    {
        case BotSituation::Pvp:
            return "pvp";
        case BotSituation::Boss:
            return "boss";
        case BotSituation::Dungeon:
            return "dungeon";
        case BotSituation::OpenWorld:
            return "open";
        default:
            return "none";
    }
}

int32 ActivityBudget::GetForce() const
{
    return _commandForce != CommandForceUnset ? _commandForce : sPlayerbotAIConfig.SituationForce;
}

void ActivityBudget::ClassifyPlayers()
{
    PLAYERBOT_CHECK_WORLD_THREAD("ActivityBudget::ClassifyPlayers");
    _classified = true;

    // The real players online: every non-random-bot player, the headless test player included (a few entries)
    int32 strictest = NoSituation;
    uint32 count = 0;
    for (Player* player : sRandomPlayerbotMgr.GetPlayers())
    {
        if (!player || !player->IsInWorld())
            continue;

        ++count;
        int32 const situation = static_cast<int32>(Classify(player));
        if (situation > strictest)
            strictest = situation;
    }

    _playerCount = count;

    int32 const force = GetForce();
    int32 const situation = force != NoSituation ? force : strictest;
    _situation.store(situation, std::memory_order_relaxed);
    _floorMs.store(SituationFloor(situation), std::memory_order_relaxed);
    _ceilingMs.store(SituationCeiling(situation), std::memory_order_relaxed);
}

bool ActivityBudget::HandleStatusCommand(ChatHandler* handler, char const* /*args*/)
{
    PLAYERBOT_CHECK_WORLD_THREAD("ActivityBudget::HandleStatusCommand");
    ActivityBudget& budget = Instance();
    PlayerbotAIConfig const& config = sPlayerbotAIConfig;

    uint32 const metric = budget.GetTickMs();
    uint32 const floor = budget.GetDiffLimitFloor();
    uint32 const ceiling = budget.GetDiffLimitCeiling();
    uint32 const percent = SmartScalePercent(metric, floor, ceiling);

    if (!config.SituationEnable)
    {
        handler->PSendSysMessage(
            "Situation budgets are off (Playerbots.Situation.Enable = 0): the global floor and "
            "ceiling apply.");
    }
    else
    {
        int32 const force = budget.GetForce();
        std::string forceText = "none (detect)";
        if (force != NoSituation)
            forceText = std::string(SituationLabel(force)) +
                        (budget._commandForce != CommandForceUnset ? " (command)" : " (Playerbots.Situation.Force)");

        handler->PSendSysMessage(
            "Situation budgets: on, every {} ms{}; force {}; situation {} ({} real players)",
            config.SituationIntervalMs, config.BotActiveAloneSmartScale ? "" : " (paused: smart scale is off)",
            forceText, SituationLabel(budget._situation.load(std::memory_order_relaxed)), budget._playerCount);

        std::ostringstream budgets;
        for (int32 i = static_cast<int32>(BotSituation::Count) - 1; i >= 0; --i)
            budgets << (i + 1 < static_cast<int32>(BotSituation::Count) ? ", " : "") << SituationLabel(i) << " "
                    << SituationFloor(i) << "/" << SituationCeiling(i);

        handler->PSendSysMessage("Budgets (floor/ceiling ms): {}; global {}/{}", budgets.str(),
                                 config.BotActiveAloneSmartScaleDiffLimitFloor,
                                 config.BotActiveAloneSmartScaleDiffLimitCeiling);
    }

    std::string const metricName = config.BotActiveAloneSmartScalePercentile
                                       ? "p" + std::to_string(config.BotActiveAloneSmartScalePercentile)
                                       : std::string("max");
    handler->PSendSysMessage(
        "Tick metric {} ms ({}), effective floor {} ms, ceiling {} ms; smart scale keeps {}% of "
        "BotActiveAlone {} = {}% rotation share{} (forced bots ignore it; no forced/rotation "
        "counts are kept)",
        metric, metricName, floor, ceiling, percent, config.BotActiveAlone, config.BotActiveAlone * percent / 100,
        config.BotActiveAloneSmartScale ? "" : " (smart scale off: not applied)");

    if (!config.SituationEnable)
        return true;

    for (Player* player : sRandomPlayerbotMgr.GetPlayers())
    {
        if (!player || !player->IsInWorld())
            continue;

        handler->PSendSysMessage("  {}: {} (map {}, zone {})", player->GetName(), SituationName(Classify(player)),
                                 player->GetMapId(), player->GetZoneId());
    }

    return true;
}

bool ActivityBudget::HandleForceCommand(ChatHandler* handler, char const* args)
{
    if (!sPlayerbotAIConfig.SituationEnable)
    {
        handler->PSendSysMessage("Situation budgets are off (Playerbots.Situation.Enable = 0).");
        return true;
    }

    PLAYERBOT_CHECK_WORLD_THREAD("ActivityBudget::HandleForceCommand");
    std::istringstream in(args ? args : "");
    std::string name;
    in >> name;

    int32 force = CommandForceUnset;
    if (name == "off")
        force = NoSituation;
    else
    {
        for (int32 i = 0; i < static_cast<int32>(BotSituation::Count); ++i)
        {
            if (name == SituationLabel(i))
                force = i;
        }
    }

    if (force == CommandForceUnset)
    {
        handler->PSendSysMessage("Usage: .playerbots situation force <pvp|boss|dungeon|open|off>");
        handler->SetSentErrorMessage(true);
        return false;
    }

    // Applies at once (world thread), not at the next interval
    ActivityBudget& budget = Instance();
    budget._commandForce = force;
    budget.ClassifyPlayers();

    int32 const situation = budget._situation.load(std::memory_order_relaxed);
    handler->PSendSysMessage("Situation force {}: situation {}, floor {} ms, ceiling {} ms",
                             force == NoSituation ? "off (detect)" : SituationLabel(force), SituationLabel(situation),
                             budget.GetDiffLimitFloor(), budget.GetDiffLimitCeiling());
    return true;
}
