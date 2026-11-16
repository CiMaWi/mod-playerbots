/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "WorkTally.h"

#include <array>
#include <atomic>
#include <string>

#include "Log.h"
#include "PlayerbotAI.h"
#include "StringFormat.h"

namespace
{
// BotWork's bits in their order (PlayerbotAI.h)
constexpr std::array<char const*, 10> BIT_NAMES = {"Order",    "Constraints", "Visit",    "TimingRow", "PreGateStep",
                                                   "JumpTest", "Aboard",      "Dismount", "Loot",      "MountTrace"};
static_assert(BotWork::MountTrace == 1u << (BIT_NAMES.size() - 1), "one name per BotWork bit");

constexpr uint64 TALLY_PERIOD = uint64(1) << 20;

std::atomic<uint64> updates{0};
std::atomic<uint64> withWork{0};
std::array<std::atomic<uint64>, BIT_NAMES.size()> perBit{};
}  // namespace

uint32 WorkTally::Note(uint32 work)
{
    if (work)
    {
        withWork.fetch_add(1, std::memory_order_relaxed);
        for (uint32 bit = 0; bit < BIT_NAMES.size(); ++bit)
        {
            if (work & (1u << bit))
                perBit[bit].fetch_add(1, std::memory_order_relaxed);
        }
    }

    uint64 const count = updates.fetch_add(1, std::memory_order_relaxed) + 1;
    if (count % TALLY_PERIOD)
        return work;

    std::string line =
        Acore::StringFormat("Work tally: updates {}, with work {}", count, withWork.load(std::memory_order_relaxed));
    for (uint32 bit = 0; bit < BIT_NAMES.size(); ++bit)
        line += Acore::StringFormat(", {} {}", BIT_NAMES[bit], perBit[bit].load(std::memory_order_relaxed));

    LOG_INFO("playerbots", "{}", line);
    return work;
}
