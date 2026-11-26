/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "WorldTravelConfig.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <sstream>
#include <vector>

#include "ConfigTable.h"

namespace
{
using TravelRow = ConfigRow<WorldTravelConfig>;

constexpr uint32_t MaxBandNumber = 1000000;

// One comma-separated value per level band (each at most MaxBandNumber); a text of another count is not valid and
// the field keeps the defaults of DEFAULT_SLOT_BANDS. An empty text is the defaults.
template <uint32_t WorldTravel::SlotBand::* Field>
bool ParseBands(std::string const& text, WorldTravelConfig& out)
{
    for (uint32_t band = 0; band < WorldTravel::LEVEL_BAND_COUNT; ++band)
        out.SlotBands[band].*Field = WorldTravel::DEFAULT_SLOT_BANDS[band].*Field;
    if (text.empty())
        return true;

    std::vector<uint32_t> values;
    std::stringstream stream(text);
    std::string token;
    while (std::getline(stream, token, ','))
    {
        char* end = nullptr;
        unsigned long const value = std::strtoul(token.c_str(), &end, 10);
        if (end == token.c_str())
            break;
        values.push_back(static_cast<uint32_t>(std::min<unsigned long>(value, MaxBandNumber)));
    }
    if (values.size() != WorldTravel::LEVEL_BAND_COUNT)
        return false;

    for (uint32_t band = 0; band < WorldTravel::LEVEL_BAND_COUNT; ++band)
        out.SlotBands[band].*Field = values[band];
    return true;
}

constexpr uint32_t DayInSeconds = 86400;

constexpr std::array<TravelRow, 20> TravelRows = {{
    {"SightYards", &WorldTravelConfig::SightYards, 0, 2000},
    {"StuckAvoidSeconds", &WorldTravelConfig::StuckAvoidSeconds, 0, DayInSeconds},
    {"InnDwellMinutes", &WorldTravelConfig::InnDwellMinutes, 0, 24 * 60, ConfigKind::TuningListed},
    {"RouteThreads", &WorldTravelConfig::RouteThreads, 1, 8},
    {"StuckPenalty", &WorldTravelConfig::StuckPenalty, 1, 100},
    {"Slots.LootPerHour", &WorldTravelConfig::SlotsLootPerHour, 0, 0, ConfigKind::Tuning, ConfigOutOfRange::Clamp,
     &ParseBands<&WorldTravel::SlotBand::LootSlotsPerHour>},
    {"Slots.TripMinutes", &WorldTravelConfig::SlotsTripMinutes, 0, 0, ConfigKind::Tuning, ConfigOutOfRange::Clamp,
     &ParseBands<&WorldTravel::SlotBand::TripMinutes>},
    {"Slots.EarnCopperPerMinute", &WorldTravelConfig::SlotsEarnCopperPerMinute, 0, 0, ConfigKind::Tuning,
     ConfigOutOfRange::Clamp, &ParseBands<&WorldTravel::SlotBand::EarnCopperPerMinute>},
    {"Bags.HorizonHours", &WorldTravelConfig::BagsHorizonHours, 0, 0, ConfigKind::Tuning, ConfigOutOfRange::Clamp,
     &ParseBands<&WorldTravel::SlotBand::BagHorizonHours>},
    {"Bags.ThriftPercent", &WorldTravelConfig::BagThriftPercent, 0, 1000},
    {"Bags.BudgetPercent", &WorldTravelConfig::BagBudgetPercent, 0, 100},
    {"Destroy.MarginPercent", &WorldTravelConfig::DestroyMarginPercent, 0, 1000},
    {"Errands.NextStopMinutes", &WorldTravelConfig::ErrandNextStopMinutes, 0, 24 * 60},
    {"Traits.AllowEdit", &WorldTravelConfig::TraitsAllowEdit, 0, 0, ConfigKind::Test},
    {"Planner.Threads", &WorldTravelConfig::PlannerThreads, 1, 8},
    {"Planner.SnapshotMinutes", &WorldTravelConfig::PlannerSnapshotMinutes, 1, 60},
    {"Planner.ResultStaleSeconds", &WorldTravelConfig::PlannerStaleSeconds, 5, 600},
    {"Planner.RecheckYards", &WorldTravelConfig::PlannerRecheckYards, 10, 2000},
    {"Planner.BlockMinutes", &WorldTravelConfig::PlannerBlockMinutes, 1, 240},
    {"Crowding.RefreshSeconds", &WorldTravelConfig::CrowdingRefreshSeconds, 1, 300},
}};
}  // namespace

ConfigFamily<WorldTravelConfig> const WorldTravelFamily{"Playerbots.WorldTravel.", TravelRows};

WorldTravelConfig& WorldTravelConfigStorage()
{
    static WorldTravelConfig values;
    return values;
}
