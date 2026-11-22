/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "NaturalTraceConfig.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <string>

#include "ConfigTable.h"

namespace
{
using TraceRow = ConfigRow<TraceConfig>;

// jsonl or binary in any case; another text is not valid (the default, jsonl, is used)
bool ParseTraceFormat(std::string const& text, TraceConfig& out)
{
    std::string format = text;
    std::transform(format.begin(), format.end(), format.begin(), [](unsigned char c) { return std::tolower(c); });
    out.Binary = format == "binary";
    return out.Binary || format == "jsonl";
}

constexpr std::array<TraceRow, 8> TraceRows = {{
    {"Enable", &TraceConfig::Enable, 0, 0, ConfigKind::Test},
    {"MaxUnits", &TraceConfig::MaxUnits, 1, 1000, ConfigKind::Test},
    {"SampleEvery", &TraceConfig::SampleEvery, 0, 1000000, ConfigKind::Test},
    {"Organic", &TraceConfig::Organic, 0, 0, ConfigKind::Test},
    {"Dir", &TraceConfig::Dir, 0, 0, ConfigKind::Test},
    {"IntervalMs", &TraceConfig::IntervalMs, 50, 1000, ConfigKind::Test},
    {"MaxRowsPerUnit", &TraceConfig::MaxRowsPerUnit, 100, 10000000, ConfigKind::Test},
    {"Format", &TraceConfig::FormatText, 0, 0, ConfigKind::Test, ConfigOutOfRange::Clamp, &ParseTraceFormat},
}};
}  // namespace

ConfigFamily<TraceConfig> const NaturalTraceFamily{"Playerbots.Natural.Trace.", TraceRows};

TraceConfig& TraceConfigStorage()
{
    static TraceConfig values;
    return values;
}
