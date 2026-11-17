/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "NaturalPathConfig.h"

#include <algorithm>
#include <array>
#include <string>

#include "ConfigTable.h"
#include "TacticScope.h"

namespace
{
using PathRow = ConfigRow<PathConfig>;

// The caps of the old reads: a time in ms 0 to 60 s, a percentage 0 to 100, yards 0 to 500, a skill 0 to 100, an
// offset 0 to 20 yards
constexpr uint32 MaxMs = 60000;
constexpr uint32 MaxPercent = 100;
constexpr uint32 MaxYards = 500;
constexpr uint32 MaxSkill = 100;
constexpr float MaxOffsetYards = 20.0f;

// "Arena,Battleground,World,Pve", All or None into the mask; the text stays in ContextsText
bool ParsePathContexts(std::string const& text, PathConfig& out)
{
    std::string error;
    return ParseTacticScope(text, out.Contexts, error);
}

// The strafe leg's Max at least its Min (the old read raised Max silently)
void FinishPath(PathConfig& out) { out.StrafeLegMaxYards = std::max(out.StrafeLegMaxYards, out.StrafeLegMinYards); }

constexpr std::array<PathRow, 25> PathRows = {{
    {"Enable", &PathConfig::Enable, 0, 0, ConfigKind::Test},
    {"Contexts", &PathConfig::ContextsText, 0, 0, ConfigKind::Tuning, ConfigOutOfRange::Clamp, &ParsePathContexts},
    {"OutsidePvpInSight", &PathConfig::OutsidePvpInSight},
    {"InSightYards", &PathConfig::InSightYards, 0, MaxYards},
    {"InSightCacheMs", &PathConfig::InSightCacheMs, 0, MaxMs},
    {"Offsets.Enable", &PathConfig::OffsetsEnable, 0, 0, ConfigKind::NotYet},
    {"OffsetYardsAtSkill1", &PathConfig::OffsetYardsAtSkill1, 0.0f, MaxOffsetYards},
    {"OffsetYardsAtSkill99", &PathConfig::OffsetYardsAtSkill99, 0.0f, MaxOffsetYards},
    {"OffsetSigmaPct", &PathConfig::OffsetSigmaPct, 0, MaxPercent},
    {"MinLegYards", &PathConfig::MinLegYards, 0, MaxYards},
    {"Curves.Enable", &PathConfig::CurvesEnable, 0, 0, ConfigKind::Test},
    {"Strafe.Enable", &PathConfig::StrafeEnable, 0, 0, ConfigKind::NotYet},
    {"Hops.Enable", &PathConfig::HopsEnable, 0, 0, ConfigKind::Test},
    {"Walk.Enable", &PathConfig::WalkEnable, 0, 0, ConfigKind::NotYet},
    {"Strafe.FromSkill", &PathConfig::StrafeFromSkill, 0, MaxSkill},
    {"Strafe.LegMinYards", &PathConfig::StrafeLegMinYards, 0, MaxYards},
    {"Strafe.LegMaxYards", &PathConfig::StrafeLegMaxYards, 0, MaxYards},
    {"Strafe.EveryMsAtSkill50", &PathConfig::StrafeEveryMsAtSkill50, 0, MaxMs},
    {"Strafe.EveryMsAtSkill99", &PathConfig::StrafeEveryMsAtSkill99, 0, MaxMs},
    {"Hops.FromSkill", &PathConfig::HopsFromSkill, 0, MaxSkill},
    {"Hops.EveryMsAtSkill60", &PathConfig::HopsEveryMsAtSkill60, 0, MaxMs},
    {"Hops.EveryMsAtSkill99", &PathConfig::HopsEveryMsAtSkill99, 0, MaxMs},
    {"Hops.EdgeYards", &PathConfig::HopsEdgeYards, 0, MaxYards},
    {"Hops.MaxLegYards", &PathConfig::HopsMaxLegYards, 0, MaxYards},
    {"Walk.MaxLegYards", &PathConfig::WalkMaxLegYards, 0, MaxYards},
}};
}  // namespace

ConfigFamily<PathConfig> const NaturalPathFamily{"Playerbots.Natural.Path.", PathRows, &FinishPath};
