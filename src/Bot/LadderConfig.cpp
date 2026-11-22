/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "LadderConfig.h"

#include <algorithm>
#include <array>

#include "ConfigTable.h"

namespace
{
// The longest delay is never below the shortest (the old read clamped it into DelayMinSeconds to 600)
void FinishMatchmaker(MatchmakerConfig& out)
{
    out.DelayMaxSeconds = std::max(out.DelayMaxSeconds, out.DelayMinSeconds);
}

using LadderRow = ConfigRow<LadderConfig>;

constexpr std::array<LadderRow, 5> LadderRows = {{
    {"Role", &LadderConfig::Role, 0, 2, ConfigKind::Role},
    {"ServerId", &LadderConfig::ServerId, 0, 0, ConfigKind::Role},
    {"DepartTimeoutSeconds", &LadderConfig::DepartTimeoutSeconds, 5, 86400, ConfigKind::TuningListed},
    {"ContactMinutes", &LadderConfig::ContactMinutes, 0, 1440, ConfigKind::TuningListed},
    {"PoolTeams", &LadderConfig::PoolTeams, 0, 5000, ConfigKind::TuningListed},
}};

using MatchmakerRow = ConfigRow<MatchmakerConfig>;

constexpr std::array<MatchmakerRow, 10> MatchmakerRows = {{
    {"DelayMinSeconds", &MatchmakerConfig::DelayMinSeconds, 0, 600, ConfigKind::TuningListed},
    {"DelayMaxSeconds", &MatchmakerConfig::DelayMaxSeconds, 0, 600, ConfigKind::TuningListed},
    {"MmrWidth", &MatchmakerConfig::MmrWidth, 1, 5000},
    {"Window", &MatchmakerConfig::Window, 1, 5000},
    {"WindowGrowth", &MatchmakerConfig::WindowGrowth, 0, 1000},
    {"RecentOpponents", &MatchmakerConfig::RecentOpponents, 0, 20},
    {"TimeoutSeconds", &MatchmakerConfig::TimeoutSeconds, 5, 3600, ConfigKind::TuningListed},
    {"MaxPicks", &MatchmakerConfig::MaxPicks, 1, 100},
    {"Skirmish", &MatchmakerConfig::Skirmish, 0, 0},
    {"PoolOnly", &MatchmakerConfig::PoolOnly, 0, 0},
}};

}  // namespace

ConfigFamily<LadderConfig> const LadderFamily{"Playerbots.Ladder.", LadderRows};
ConfigFamily<MatchmakerConfig> const MatchmakerFamily{"Playerbots.Ladder.Matchmaker.", MatchmakerRows,
                                                      &FinishMatchmaker};

LadderConfig& LadderConfigStorage()
{
    static LadderConfig values;
    return values;
}
