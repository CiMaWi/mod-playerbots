/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "NaturalMoveConfig.h"

#include <array>
#include <string>

#include "ConfigTable.h"
#include "TacticScope.h"

namespace
{
using TrackRow = ConfigRow<TrackConfig>;

// The caps of the old reads: a time in ms 0 to 60 s, a percentage 0 to 100, degrees 0 to 360, yards 0 to 500, a skill
// 0 to 100, a switch 0 to 1
constexpr uint32 MaxMs = 60000;
constexpr uint32 MaxPercent = 100;
constexpr uint32 MaxDegrees = 360;
constexpr uint32 MaxYards = 500;
constexpr uint32 MaxSkill = 100;

// "Arena,Battleground,World,Pve", All or None into the mask; the text stays in ContextsText
bool ParseTrackContexts(std::string const& text, TrackConfig& out)
{
    std::string error;
    return ParseTacticScope(text, out.Contexts, error);
}

constexpr std::array<TrackRow, 24> TrackRows = {{
    {"Enable", &TrackConfig::Enable, 0, 0, ConfigKind::Test},
    {"Contexts", &TrackConfig::ContextsText, 0, 0, ConfigKind::Tuning, ConfigOutOfRange::Clamp, &ParseTrackContexts},
    {"OutsidePvpInSight", &TrackConfig::OutsidePvpInSight},
    {"InSightYards", &TrackConfig::InSightYards, 0, MaxYards},
    {"InSightCacheMs", &TrackConfig::InSightCacheMs, 0, MaxMs},
    {"LagMsAtSkill1", &TrackConfig::LagMsAtSkill1, 0, MaxMs},
    {"LagMsAtSkill99", &TrackConfig::LagMsAtSkill99, 0, MaxMs},
    {"LagSigmaPct", &TrackConfig::LagSigmaPct, 0, MaxPercent},
    {"VelocityFromSkill", &TrackConfig::VelocityFromSkill, 0, MaxSkill},
    {"PatternFromSkill", &TrackConfig::PatternFromSkill, 0, MaxSkill},
    {"TurnRateFromSkill", &TrackConfig::TurnRateFromSkill, 0, MaxSkill},
    {"GainPctAtSkill1", &TrackConfig::GainPctAtSkill1, 0, MaxPercent},
    {"GainPctAtSkill99", &TrackConfig::GainPctAtSkill99, 0, MaxPercent},
    {"GainSigmaPct", &TrackConfig::GainSigmaPct, 0, MaxPercent},
    {"HistoryMsAtSkill1", &TrackConfig::HistoryMsAtSkill1, 0, MaxMs},
    {"HistoryMsAtSkill99", &TrackConfig::HistoryMsAtSkill99, 0, MaxMs},
    {"RingSamples", &TrackConfig::RingSamples, MinTrackRingSamples, MaxTrackRingSamples},
    {"SampleMs", &TrackConfig::SampleMs, 0, MaxMs},
    {"MaxLeadYards", &TrackConfig::MaxLeadYards, 0, MaxYards},
    {"SkipLeadRule", &TrackConfig::SkipLeadRule, 0, 1},
    {"MinMoveYards", &TrackConfig::MinMoveYards, 0, MaxYards},
    {"JukeDeg", &TrackConfig::JukeDeg, 0, MaxDegrees},
    {"StuckMs", &TrackConfig::StuckMs, 0, MaxMs},
    {"RefreshMs", &TrackConfig::RefreshMs, 0, MaxMs},
}};
}  // namespace

ConfigFamily<TrackConfig> const NaturalTrackFamily{"Playerbots.Natural.Track.", TrackRows};

TrackConfig& TrackConfigStorage()
{
    static TrackConfig values;
    return values;
}
