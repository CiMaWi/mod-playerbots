/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// WI10a: the use_item row's cause -> refusal map (BridgeItemRule.h) over every cause word of the item-use framework.
// Build from the module root:
//   clang-cl /std:c++20 /EHsc /W4 /WX /I src/Bot tests/bridge-item-rule-selftest.cpp

#include <cstdio>

#include "BridgeItemRule.h"

namespace
{
using PlayerbotBridge::ItemRefusal;
using PlayerbotBridge::RefusalOfCause;

struct Sample
{
    char const* Cause;
    ItemRefusal Want;
};

// INTERFACE.md 1.4, in the framework's order (ItemUseTypes.h CAUSE_WORDS, engine and handler as the row words them).
constexpr Sample Samples[] = {{"none", ItemRefusal::None},
                              {"no_effect", ItemRefusal::CannotNow},
                              {"no_item", ItemRefusal::UnknownName},
                              {"not_equipped", ItemRefusal::UnknownName},
                              {"level", ItemRefusal::CannotNow},
                              {"skill", ItemRefusal::CannotNow},
                              {"class", ItemRefusal::CannotNow},
                              {"cant_use", ItemRefusal::CannotNow},
                              {"not_ready", ItemRefusal::CannotNow},
                              {"potion", ItemRefusal::CannotNow},
                              {"no_charges", ItemRefusal::CannotNow},
                              {"busy", ItemRefusal::CannotNow},
                              {"dead", ItemRefusal::Dead},
                              {"no_control", ItemRefusal::CannotNow},
                              {"in_fight", ItemRefusal::CannotNow},
                              {"not_in_fight", ItemRefusal::CannotNow},
                              {"arena", ItemRefusal::CannotNow},
                              {"place", ItemRefusal::CannotNow},
                              {"form", ItemRefusal::CannotNow},
                              {"no_target", ItemRefusal::CannotNow},
                              {"target", ItemRefusal::CannotNow},
                              {"full", ItemRefusal::CannotNow},
                              {"range", ItemRefusal::TooFar},
                              {"too_close", ItemRefusal::TooFar},
                              {"sight", ItemRefusal::TargetNotVisible},
                              {"focus", ItemRefusal::CannotNow},
                              {"engine", ItemRefusal::CannotNow},
                              {"handler", ItemRefusal::CannotNow}};
static_assert(sizeof(Samples) / sizeof(Samples[0]) == 28, "one sample per cause word");
}  // namespace

int main()
{
    int failed = 0;
    for (Sample const& sample : Samples)
        if (RefusalOfCause(sample.Cause) != sample.Want)
        {
            std::printf("FAIL %s: got %d, want %d\n", sample.Cause, static_cast<int>(RefusalOfCause(sample.Cause)),
                        static_cast<int>(sample.Want));
            ++failed;
        }

    // An unknown word (a future cause) is never told as success or as a name problem.
    if (RefusalOfCause("future_cause") != ItemRefusal::CannotNow || RefusalOfCause("") != ItemRefusal::CannotNow)
    {
        std::printf("FAIL unknown words\n");
        ++failed;
    }

    // Only "none" is no refusal; mapped refusals other than CannotNow are the four named causes' (a swap of two
    // mappings changes one of these counts).
    int unknownName = 0, tooFar = 0, sight = 0, dead = 0, none = 0;
    for (Sample const& sample : Samples)
    {
        ItemRefusal const got = RefusalOfCause(sample.Cause);
        unknownName += got == ItemRefusal::UnknownName;
        tooFar += got == ItemRefusal::TooFar;
        sight += got == ItemRefusal::TargetNotVisible;
        dead += got == ItemRefusal::Dead;
        none += got == ItemRefusal::None;
    }
    if (unknownName != 2 || tooFar != 2 || sight != 1 || dead != 1 || none != 1)
    {
        std::printf("FAIL the map's shape\n");
        ++failed;
    }

    if (failed)
    {
        std::printf("FAIL %d checks\n", failed);
        return 1;
    }

    std::printf("PASS %d checks\n", 28 + 2 + 1);
    return 0;
}
