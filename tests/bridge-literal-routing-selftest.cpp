/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// Standalone check of the pure literal classifier: a fake trigger table stands in for a bot's shared creators.
#include <cstdlib>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <type_traits>
#include <vector>

#include "BridgeLiteralRoute.h"

using PlayerbotBridge::ClassifyLiteral;
using PlayerbotBridge::LiteralClass;
using PlayerbotBridge::LiteralProbe;
using PlayerbotBridge::LiteralRules;
using PlayerbotBridge::LiteralScope;

// The text, scope, rules and the two probes are the only inputs: no queue, current chat or other bot state.
static_assert(
    std::is_same_v<decltype(&ClassifyLiteral), LiteralClass (*)(std::string const&, LiteralScope, LiteralRules const&,
                                                                LiteralProbe const&, LiteralProbe const&)>);

namespace
{
struct FakeTriggers
{
    std::set<std::string> creators{"follow", "attack", "stay", "co", "nc", "dungeon clear", "trade", "ll"};
    std::map<std::string, int> created;
    mutable int existsCalls = 0;
    int createCalls = 0;

    // Mirrors AiObjectContext::HasTrigger: the qualifier is cut, only the creator table is read.
    bool Has(std::string const& name) const
    {
        ++existsCalls;
        return creators.count(name.substr(0, name.find("::"))) != 0;
    }

    // Stands in for GetTrigger, which creates and caches; the classifier must never reach it.
    bool Get(std::string const& name)
    {
        ++createCalls;
        created[name] = 1;
        return Has(name);
    }
};

FakeTriggers fake;
int itemCalls = 0;
int failures = 0;
int passes = 0;

bool FakeItem(std::string const& text)
{
    ++itemCalls;
    return text.find("|Hitem:") != std::string::npos || text.find("potion") != std::string::npos;
}

LiteralProbe const triggerProbe = [](std::string const& name) { return fake.Has(name); };
LiteralProbe const itemProbe = [](std::string const& text) { return FakeItem(text); };

char const* Name(LiteralClass value)
{
    switch (value)
    {
        case LiteralClass::NotLiteral:
            return "NotLiteral";
        case LiteralClass::Literal:
            return "Literal";
        case LiteralClass::Uncertain:
            return "Uncertain";
    }

    return "?";
}

LiteralRules Rules(std::string const& prefix = "", bool autoTrade = true)
{
    return LiteralRules{"\\\\", prefix, autoTrade};
}

void Check(std::string const& label, LiteralClass expected, std::string const& text,
           LiteralScope scope = LiteralScope::Whisper, LiteralRules const& rules = Rules())
{
    LiteralClass const first = ClassifyLiteral(text, scope, rules, triggerProbe, itemProbe);
    // Hook-order independence: a different pending-command state must not change the answer.
    fake.created["pending " + label] = 1;
    LiteralClass const second = ClassifyLiteral(text, scope, rules, triggerProbe, itemProbe);
    fake.created.erase("pending " + label);
    if (first != expected || second != expected)
    {
        ++failures;
        std::cout << "FAIL " << label << ": expected " << Name(expected) << ", got " << Name(first) << "/"
                  << Name(second) << "\n";
        return;
    }

    ++passes;
}

void CheckTrue(std::string const& label, bool value)
{
    if (!value)
    {
        ++failures;
        std::cout << "FAIL " << label << "\n";
        return;
    }

    ++passes;
}
}  // namespace

int main()
{
    LiteralScope const group = LiteralScope::Group;
    LiteralScope const other = LiteralScope::Other;

    Check("follow", LiteralClass::Literal, "follow");
    Check("follow with parameter", LiteralClass::Literal, "follow me please");
    Check("stay", LiteralClass::Literal, "stay");
    Check("first-word quirk", LiteralClass::Literal, "attack the boss now");
    Check("double space head", LiteralClass::Literal, "follow  me");
    Check("trimmed", LiteralClass::Literal, " \t follow \r\n");
    Check("conversation", LiteralClass::NotLiteral, "hello there, how are you");
    Check("unknown trigger", LiteralClass::NotLiteral, "dance");
    Check("case is exact", LiteralClass::NotLiteral, "Follow me");
    Check("multi-word trigger", LiteralClass::Literal, "dungeon clear start");
    Check("alias with parameter", LiteralClass::Literal, "co +dps");
    Check("qualified name", LiteralClass::Literal, "follow::q");
    Check("unknown qualified name", LiteralClass::NotLiteral, "dance::q");

    Check("separator both", LiteralClass::Literal, "follow\\\\stay");
    Check("separator second", LiteralClass::Literal, "hello\\\\follow");
    Check("separator none", LiteralClass::NotLiteral, "hello\\\\world");
    Check("separator empty segments", LiteralClass::NotLiteral, "\\\\\\\\  \\\\");
    Check("separator mixed spacing", LiteralClass::Literal, "hello there \\\\  follow me ");
    Check("separator character set", LiteralClass::Literal, "hello\\\\world\\follow");
    Check("single backslash no split", LiteralClass::NotLiteral, "hello\\follow");
    Check("separator NUL stop", LiteralClass::NotLiteral, std::string("hello\\\\x\0\\\\follow", 17));
    Check("separator uncertain segment", LiteralClass::Uncertain, "hello\\\\@tank hi");
    Check("separator literal beats uncertain", LiteralClass::Literal, "@tank hi\\\\follow");

    Check("prefix match", LiteralClass::Literal, "!follow", LiteralScope::Whisper, Rules("!"));
    Check("prefix missing", LiteralClass::NotLiteral, "follow", LiteralScope::Whisper, Rules("!"));
    Check("prefix inside", LiteralClass::NotLiteral, "a!follow", LiteralScope::Whisper, Rules("!"));
    Check("prefix other first char", LiteralClass::NotLiteral, "xfollow", LiteralScope::Whisper, Rules("!"));
    Check("prefix per segment", LiteralClass::Literal, "hi\\\\!stay", LiteralScope::Whisper, Rules("!"));
    Check("prefix then selector", LiteralClass::Literal, "!#p stay", LiteralScope::Whisper, Rules("!"));

    Check("selector whisper", LiteralClass::Literal, "#w follow");
    Check("selector party", LiteralClass::Literal, "#p stay", group);
    Check("selector unknown", LiteralClass::NotLiteral, "#x follow");
    Check("selector alone", LiteralClass::NotLiteral, "#w ");

    Check("role filter", LiteralClass::Uncertain, "@tank follow");
    Check("role filter chat", LiteralClass::Uncertain, "@dps hi");
    Check("marker inside", LiteralClass::Uncertain, "mail me at x@y");

    Check("debug", LiteralClass::Literal, "debug x");
    Check("debug bare", LiteralClass::NotLiteral, "debug");
    Check("d action", LiteralClass::Literal, "d x");
    Check("do action", LiteralClass::Literal, "do x");
    Check("reset", LiteralClass::Literal, "reset");
    Check("logout", LiteralClass::Literal, "logout");
    Check("logout cancel", LiteralClass::Literal, "logout cancel");
    Check("queue in group", LiteralClass::Literal, "queue follow", group);
    Check("queue in whisper", LiteralClass::NotLiteral, "queue follow");
    Check("queue bare group", LiteralClass::NotLiteral, "queue", group);

    Check("item link trade", LiteralClass::Literal, "|cff1eff00|Hitem:1234:0:0:0|h[Thing]|h|r");
    Check("item link no trade", LiteralClass::NotLiteral, "|cff1eff00|Hitem:1234:0:0:0|h[Thing]|h|r",
          LiteralScope::Whisper, Rules("", false));
    Check("parseable word trade", LiteralClass::Literal, "i need a potion");
    Check("parseable word no trade", LiteralClass::NotLiteral, "i need a potion", LiteralScope::Whisper,
          Rules("", false));

    Check("say follow", LiteralClass::NotLiteral, "follow", other);
    Check("say separator", LiteralClass::NotLiteral, "follow\\\\stay", other);
    Check("say marker", LiteralClass::NotLiteral, "@tank follow", other);
    Check("party follow", LiteralClass::Literal, "follow", group);
    Check("party conversation", LiteralClass::NotLiteral, "good job everyone", group);

    std::vector<std::string> const conversation = {"hello there",
                                                   "how are you doing today?",
                                                   "nice weather, huh",
                                                   "lol",
                                                   "thanks!",
                                                   "where are we going?",
                                                   "can you help me?",
                                                   "I am going to the inn.",
                                                   "wait... what?",
                                                   "ok",
                                                   "brb",
                                                   "that was a close one",
                                                   "good night, friends",
                                                   "\xc3\xa9t\xc3\xa9 \xc3\xa0 Paris",
                                                   "\xe2\x9c\x93 done",
                                                   "are you a bot?",
                                                   "Stormwind is nice",
                                                   "(laughs)",
                                                   "1, 2, 3, go",
                                                   "we should rest"};
    for (std::string const& line : conversation)
    {
        Check("conversation whisper: " + line, LiteralClass::NotLiteral, line);
        Check("conversation party: " + line, LiteralClass::NotLiteral, line, group);
    }

    LiteralProbe const none;
    CheckTrue("null trigger probe",
              ClassifyLiteral("follow", LiteralScope::Whisper, Rules(), none, itemProbe) == LiteralClass::Uncertain);
    CheckTrue("null item probe",
              ClassifyLiteral("follow", LiteralScope::Whisper, Rules(), triggerProbe, none) == LiteralClass::Uncertain);
    CheckTrue("null probes say", ClassifyLiteral("follow", other, Rules(), none, none) == LiteralClass::NotLiteral);

    LiteralRules const noSeparator{"", "", true};
    Check("empty separator", LiteralClass::NotLiteral, "follow\\\\stay", LiteralScope::Whisper, noSeparator);

    std::string bounded = "a";
    for (uint32_t i = 1; i < PlayerbotBridge::MaxLiteralSegments; ++i)
        bounded += "\\\\a";

    Check("segments at bound", LiteralClass::NotLiteral, bounded);
    Check("segments over bound", LiteralClass::Uncertain, bounded + "\\\\follow");

    fake.existsCalls = 0;
    itemCalls = 0;
    Check("large text", LiteralClass::Uncertain, std::string(64 * 1024, 'a'));
    CheckTrue("large text probes nothing", fake.existsCalls == 0 && itemCalls == 0);

    fake.existsCalls = 0;
    ClassifyLiteral("attack the boss now", LiteralScope::Whisper, Rules(), triggerProbe, itemProbe);
    CheckTrue("probe bound one segment", fake.existsCalls > 0 && fake.existsCalls <= 4);

    fake.existsCalls = 0;
    itemCalls = 0;
    ClassifyLiteral("hello there friend\\\\how are you", LiteralScope::Whisper, Rules(), triggerProbe, itemProbe);
    CheckTrue("probe bound per segment", fake.existsCalls <= 3 + 3 && itemCalls == 2);

    CheckTrue("no trigger created", fake.createCalls == 0 && fake.created.empty());
    CheckTrue("mutating probe counts", fake.Get("follow") && fake.createCalls == 1 && fake.created.size() == 1);

    if (failures)
    {
        std::cout << "FAIL " << failures << " of " << (failures + passes) << "\n";
        return EXIT_FAILURE;
    }

    std::cout << "PASS " << passes << "\n";
    return EXIT_SUCCESS;
}
