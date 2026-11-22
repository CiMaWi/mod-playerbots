/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "NaturalTurnConfig.h"

#include <algorithm>
#include <array>
#include <string>

#include "ConfigTable.h"
#include "TacticScope.h"

namespace
{
using TurnRow = ConfigRow<TurnConfig>;

// The caps of the old reads: a time in ms 0 to 60 s, a percentage 0 to 100, degrees 0 to 360, yards 0 to 500
constexpr uint32 MaxMs = 60000;
constexpr uint32 MaxPercent = 100;
constexpr uint32 MaxDegrees = 360;
constexpr uint32 MaxYards = 500;

// "Arena,Battleground,World,Pve", All or None into the mask; the text stays in ContextsText
bool ParseTurnContexts(std::string const& text, TurnConfig& out)
{
    std::string error;
    return ParseTacticScope(text, out.Contexts, error);
}

// The correction delay's Max at least its Min (the old read raised Max silently)
void FinishTurn(TurnConfig& out)
{
    out.CorrectionDelayMaxMs = std::max(out.CorrectionDelayMaxMs, out.CorrectionDelayMinMs);
}

constexpr std::array<TurnRow, 24> TurnRows = {{
    {"Enable", &TurnConfig::Enable, 0, 0, ConfigKind::Test},
    {"Contexts", &TurnConfig::ContextsText, 0, 0, ConfigKind::Tuning, ConfigOutOfRange::Clamp, &ParseTurnContexts},
    {"OutsidePvpInSight", &TurnConfig::OutsidePvpInSight},
    {"InSightYards", &TurnConfig::InSightYards, 0, MaxYards},
    {"InSightCacheMs", &TurnConfig::InSightCacheMs, 0, MaxMs},
    {"Mode", &TurnConfig::Mode, 1, 2, ConfigKind::TuningListed},
    {"MsFor180AtSkill1", &TurnConfig::MsFor180AtSkill1, 0, MaxMs},
    {"MsFor180AtSkill99", &TurnConfig::MsFor180AtSkill99, 0, MaxMs},
    {"MinMs", &TurnConfig::MinMs, 0, MaxMs},
    {"ErrorDegAtSkill1", &TurnConfig::ErrorDegAtSkill1, 0, MaxDegrees},
    {"ErrorDegAtSkill99", &TurnConfig::ErrorDegAtSkill99, 0, MaxDegrees},
    {"ErrorSigmaPct", &TurnConfig::ErrorSigmaPct, 0, MaxPercent},
    {"CorrectionPctAtSkill1", &TurnConfig::CorrectionPctAtSkill1, 0, MaxPercent},
    {"CorrectionPctAtSkill99", &TurnConfig::CorrectionPctAtSkill99, 0, MaxPercent},
    {"CorrectionDelayMinMs", &TurnConfig::CorrectionDelayMinMs, 0, MaxMs},
    {"CorrectionDelayMaxMs", &TurnConfig::CorrectionDelayMaxMs, 0, MaxMs},
    {"CorrectionResidualPct", &TurnConfig::CorrectionResidualPct, 0, MaxPercent},
    {"KeyboardPctAtSkill1", &TurnConfig::KeyboardPctAtSkill1, 0, MaxPercent},
    {"KeyboardPctAtSkill99", &TurnConfig::KeyboardPctAtSkill99, 0, MaxPercent},
    {"KeyboardDegPerS", &TurnConfig::KeyboardDegPerS, 0, MaxMs},
    {"PacketMinDeg", &TurnConfig::PacketMinDeg, 0, MaxDegrees},
    {"PacketMinMs", &TurnConfig::PacketMinMs, 0, MaxMs},
    {"SplineMinDeg", &TurnConfig::SplineMinDeg, 0, MaxDegrees},
    {"CastRetryMs", &TurnConfig::CastRetryMs, 0, MaxMs},
}};
}  // namespace

ConfigFamily<TurnConfig> const NaturalFacingFamily{"Playerbots.Natural.Facing.", TurnRows, &FinishTurn};

TurnConfig& TurnConfigStorage()
{
    static TurnConfig values;
    return values;
}
