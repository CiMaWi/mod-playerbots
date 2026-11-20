/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// Offline proof of the speak-no-reply rule (LCB5a, BridgeQuiet.h): outside an act a Tell speaks; inside an act's
// quiet scope it stays unsaid; inside an open act without the quiet scope it speaks and is counted as a line of the
// bot's own. The scopes nest and end with their owner, and each thread has its own.
// Build (Git Bash, module root, inside vcvars64): clang-cl /std:c++20 /EHsc /W4 /WX /I src/Bot
// tests/bridge-quiet-selftest.cpp
#include <cstdio>
#include <thread>

#include "BridgeQuiet.h"

using PlayerbotBridge::ActScope;
using PlayerbotBridge::ActState;
using PlayerbotBridge::FateOfTell;
using PlayerbotBridge::QuietScope;
using PlayerbotBridge::TellFate;

namespace
{
int checks = 0;
int failed = 0;

void Check(bool value, char const* label)
{
    ++checks;
    if (!value)
    {
        ++failed;
        std::printf("FAIL %s\n", label);
    }
}

TellFate Now() { return FateOfTell(ActState()); }
}  // namespace

int main()
{
    Check(Now() == TellFate::Speak, "no act: the bot speaks");
    {
        ActScope const open;
        Check(Now() == TellFate::SpeakCounted, "an act without quiet: counted");
        {
            QuietScope const quiet;
            Check(Now() == TellFate::Hold, "an act's quiet scope: unsaid");
            {
                QuietScope const inner;
                Check(Now() == TellFate::Hold, "nested quiet: unsaid");
            }
            Check(Now() == TellFate::Hold, "the outer quiet holds after the inner ends");

            TellFate other = TellFate::Hold;
            std::thread thread([&other]() { other = FateOfTell(ActState()); });
            thread.join();
            Check(other == TellFate::Speak, "another thread: its own scopes");
        }
        Check(Now() == TellFate::SpeakCounted, "quiet ended, act open: counted");
    }
    Check(Now() == TellFate::Speak, "act ended: the bot speaks");
    Check(ActState().Acts == 0 && ActState().Quiet == 0, "both scopes returned to zero");

    std::printf("%d passed, %d failed\n", checks - failed, failed);
    return failed ? 1 : 0;
}
