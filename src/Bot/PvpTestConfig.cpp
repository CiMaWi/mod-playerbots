/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "PvpTestConfig.h"

#include <array>
#include <cstdint>
#include <sstream>
#include <string>

#include "ConfigTable.h"

namespace
{
// "<map> <x> <y> <z> <o>" fills the five fight members; an empty text is the default spot
bool ParseFightLocation(std::string const& text, PvpTestConfig& out)
{
    PvpTestConfig const defaults;
    out.FightMap = defaults.FightMap;
    out.FightX = defaults.FightX;
    out.FightY = defaults.FightY;
    out.FightZ = defaults.FightZ;
    out.FightO = defaults.FightO;
    if (text.empty())
        return true;
    uint32_t map = 0;
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float o = 0.0f;
    std::istringstream stream(text);
    if (!(stream >> map >> x >> y >> z >> o))
        return false;
    out.FightMap = map;
    out.FightX = x;
    out.FightY = y;
    out.FightZ = z;
    out.FightO = o;
    return true;
}

using PvpTestRow = ConfigRow<PvpTestConfig>;

constexpr std::array<PvpTestRow, 3> PvpTestRows = {{
    {"Enable", &PvpTestConfig::Enable, 0, 0, ConfigKind::Test},
    {"TimeoutSeconds", &PvpTestConfig::TimeoutSeconds, 60, 3600, ConfigKind::Test},
    {"FightLocation", &PvpTestConfig::FightLocation, 0, 0, ConfigKind::Test, ConfigOutOfRange::Clamp,
     &ParseFightLocation},
}};

}  // namespace

ConfigFamily<PvpTestConfig> const PvpTestFamily{"Playerbots.PvpTest.", PvpTestRows};
