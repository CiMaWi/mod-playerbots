/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "MatchLogConfig.h"

#include <array>

#include "ConfigTable.h"

namespace
{
using MatchLogRow = ConfigRow<MatchLogConfig>;

constexpr std::array<MatchLogRow, 3> MatchLogRows = {{
    {"TraceEvery", &MatchLogConfig::TraceEvery, 0, 4294967295.0, ConfigKind::Test},
    {"TraceMaxEvents", &MatchLogConfig::TraceMaxEvents, 16, 1000000, ConfigKind::Test},
    {"TraceMaxBytes", &MatchLogConfig::TraceMaxBytes, 4096, 67108864, ConfigKind::Test},
}};

}  // namespace

ConfigFamily<MatchLogConfig> const MatchLogFamily{"Playerbots.MatchLog.", MatchLogRows};
