/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "LoneKeys.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <limits>

#include "ConfigTable.h"

namespace
{
using LoneRow = ConfigRow<LoneKeys>;

// 2-9 letters (the core's name check takes letters only and a name of at most 12: room for 3 counter letters)
bool ParseTestNamePrefix(std::string const& text, LoneKeys&)
{
    return text.empty() ||
           (text.size() >= 2 && text.size() <= 9 &&
            std::all_of(text.begin(), text.end(), [](unsigned char c) { return std::isalpha(c) != 0; }));
}

// stored in lower case with a capital first letter
void NormalizeTestNamePrefix(LoneKeys& out)
{
    std::string& prefix = out.TestNamePrefix;
    if (prefix.empty())
        return;
    std::transform(prefix.begin(), prefix.end(), prefix.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    prefix[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(prefix[0])));
}

constexpr std::array<LoneRow, 1> SightRangeCapRows = {{
    {"", &LoneKeys::SightRangeCap, 0.0, std::numeric_limits<double>::infinity(), ConfigKind::TuningListed},
}};
constexpr std::array<LoneRow, 1> RatedQueueSlotsRows = {{
    {"", &LoneKeys::RatedQueueSlots, 1, std::numeric_limits<double>::infinity(), ConfigKind::TuningListed},
}};
constexpr std::array<LoneRow, 1> RatedQueueUncappedRows = {{
    {"", &LoneKeys::RatedQueueUncapped, 0, 0, ConfigKind::Test},
}};
constexpr std::array<LoneRow, 1> TestNamePrefixRows = {{
    {"", &LoneKeys::TestNamePrefix, 0, 0, ConfigKind::Test, ConfigOutOfRange::Clamp, &ParseTestNamePrefix},
}};
constexpr std::array<LoneRow, 1> DebugThreadChecksRows = {{
    {"", &LoneKeys::DebugThreadChecks, 0, 0, ConfigKind::Test},
}};
constexpr std::array<LoneRow, 1> TestMissLogRows = {{
    {"", &LoneKeys::TestMissLog, 0, 0, ConfigKind::Test},
}};

}  // namespace

// One family per key: the prefix is the whole key and the row's key is empty (like a retired switch)
ConfigFamily<LoneKeys> const SightRangeCapFamily{"Playerbots.BotSightRangeCap", SightRangeCapRows};
ConfigFamily<LoneKeys> const RatedQueueSlotsFamily{"Playerbots.RandomBotAutoJoinRatedArenaQueueSlots",
                                                   RatedQueueSlotsRows};
ConfigFamily<LoneKeys> const RatedQueueUncappedFamily{"Playerbots.RandomBotAutoJoinRatedArenaUncapped",
                                                      RatedQueueUncappedRows};
ConfigFamily<LoneKeys> const TestNamePrefixFamily{"Playerbots.RandomBotTestNamePrefix", TestNamePrefixRows,
                                                  &NormalizeTestNamePrefix};
ConfigFamily<LoneKeys> const DebugThreadChecksFamily{"Playerbots.DebugThreadChecks", DebugThreadChecksRows};
ConfigFamily<LoneKeys> const TestMissLogFamily{"Playerbots.Test.MissLog", TestMissLogRows};

LoneKeys& LoneKeysStorage()
{
    static LoneKeys values;
    return values;
}
