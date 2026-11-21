/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// Offline proof of the name check and the multiplier rule (combat-foundation CF4):
//  1. src/Bot/Engine/CombatNames.h: the base-name split the engine does, the known-file parse (comments, blanks, a
//     malformed line) and the diff; a known line removed from the file makes the diff fail (shown once, below).
//     Also the action-node alias resolution, the intersection behind the shared section, the referrer-suffix strip and
//     a shared line (CF.16).
//  2. tests/combat-names-known.txt is byte-identical to the copy the server embeds
//     (src/Bot/Engine/CombatNamesKnown.inc), no file outside src/Ai/Raid and src/Ai/Dungeon refers to a shared
//     entry (its name as the first argument of NextAction(, ActionNode(, TriggerNode( or supported.push_back(), and it
//     lists no "caster aoe", "boost" or "cast time" strategy.
//  3. Every strategy name AiFactory.cpp adds (addStrategy, addStrategiesNoInit, addStrategies) is a `creators["..."]`
//     key somewhere under src. A coarse proxy: the union over all classes and all kinds; the server's check
//     (playerbots combat names) and the unknown-name log in Engine::addStrategy decide per class.
//  4. R4 (Multiplier.h): no multiplier GetValue returns a numeric literal other than 0 or 1 (a bare literal, a ternary
//     branch, an operand of '*'); the scanner has its own cases. Returns that are computed (a call, a member) cannot
//     be checked lexically; their count is printed as information (see the per-site notes at the end).
// Computed returns, per site (R4's note; the value is 0 or 1 at every one):
//  - FocusStrategy.cpp TacticHoldMultiplier::GetValue returns Answer(...), which ends in `return 0.0f` (the hold
//    refuses) or `return 1.0f`; nothing else leaves it.
//  - PullStrategy.cpp delegates to PassiveMultiplier::GetValue, whose returns are 1.0f (an allowed action) and 0.
//  - The raid multipliers' GetValue (Gruul, Hyjal, Mag, SSC, TK, ZA, ZG) is `in encounter ? GetValueInEncounter(a) :
//    1.0f`; the encounter overrides are scanned too (the scan covers GetValue*), and each returns a literal 0 or 1 or
//    the same kind of delegation.
//  - GetDpsHoldValue(bot, action) (Mag, SSC, TK; src/Util/EncounterHelpers.cpp) returns 0.0f or `cast ? 1.0f : 0.0f`.
//  - ICCMultipliers.cpp, MCMultipliers.cpp, RSMultipliers.cpp: returns of lambdas inside a GetValue (a stack count, a
//    comparator, a bool predicate), not returns of the multiplier.
//  - A string literal holding the word "return" is blanked before the scan.
// Reads files relative to the current directory (the repo root, as the selftests runner sets it).
// Build (Git Bash, module root):
//   clang-cl /std:c++20 /EHsc /W4 /WX /I src/Bot/Engine tests/combat-names-selftest.cpp

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "CombatNames.h"

namespace
{
namespace fs = std::filesystem;
using namespace CombatNames;

int passed = 0;
int failed = 0;

void Check(bool pass, std::string const& name)
{
    std::printf("%s %s\n", pass ? "PASS" : "FAIL", name.c_str());
    (pass ? passed : failed)++;
}

std::string ReadFile(std::string const& path)
{
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// Comments out, newlines kept (so a line number still counts); string and character literals are left as they are
std::string StripComments(std::string const& s)
{
    std::string out;
    out.reserve(s.size());
    size_t i = 0;
    while (i < s.size())
    {
        char const c = s[i];
        if (c == '/' && i + 1 < s.size() && s[i + 1] == '/')
        {
            while (i < s.size() && s[i] != '\n')
                ++i;
        }
        else if (c == '/' && i + 1 < s.size() && s[i + 1] == '*')
        {
            i += 2;
            while (i + 1 < s.size() && !(s[i] == '*' && s[i + 1] == '/'))
            {
                if (s[i] == '\n')
                    out += '\n';
                ++i;
            }
            i += 2;
            out += ' ';
        }
        else if (c == '"' || (c == '\'' && !(i > 0 && std::isalnum(static_cast<unsigned char>(s[i - 1])))))
        {
            out += c;
            ++i;
            while (i < s.size() && s[i] != c && s[i] != '\n')
            {
                if (s[i] == '\\' && i + 1 < s.size())
                    out += s[i++];
                out += s[i++];
            }
            if (i < s.size())
                out += s[i++];
        }
        else
        {
            out += c;
            ++i;
        }
    }
    return out;
}

std::vector<std::string> SourceFiles(std::string const& dir)
{
    std::vector<std::string> files;
    if (!fs::exists(dir))
        return files;
    for (auto const& entry : fs::recursive_directory_iterator(dir))
        if (entry.is_regular_file())
        {
            std::string const ext = entry.path().extension().string();
            if (ext == ".cpp" || ext == ".h")
                files.push_back(entry.path().generic_string());
        }
    return files;
}

// Every creators["name"] key in the files of dir
std::set<std::string> RegisteredNames(std::string const& dir)
{
    std::set<std::string> names;
    std::regex const re(R"re(creators\["([^"]+)"\])re");
    for (std::string const& file : SourceFiles(dir))
    {
        std::string const text = StripComments(ReadFile(file));
        for (std::sregex_iterator it(text.begin(), text.end(), re), end; it != end; ++it)
            names.insert((*it)[1].str());
    }
    return names;
}

// ---- part 3: the strategy names the factory adds ----

std::vector<std::string> FactoryStrategyLiterals(std::string const& source)
{
    std::string const text = StripComments(source);
    std::vector<std::string> names;
    std::regex const call(R"(\b(addStrategy|addStrategiesNoInit|addStrategies)\s*\()");
    for (std::sregex_iterator it(text.begin(), text.end(), call), end; it != end; ++it)
    {
        bool const single = (*it)[1].str() == "addStrategy";
        size_t i = it->position() + it->length();
        int depth = 1;
        while (i < text.size() && depth > 0)
        {
            char const c = text[i];
            if (c == '(')
                ++depth;
            else if (c == ')')
                --depth;
            else if (c == '"')
            {
                size_t const close = text.find('"', i + 1);
                names.push_back(BaseName(text.substr(i + 1, close - i - 1)));
                i = close;
                if (single)
                    break;
            }
            ++i;
        }
    }
    return names;
}

// ---- part 4: the multiplier scan ----

std::string Trim(std::string s)
{
    size_t const a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos)
        return "";
    size_t const b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

// The index of the bracket that closes the one at open, or npos
size_t MatchingClose(std::string const& s, size_t open)
{
    int depth = 0;
    for (size_t i = open; i < s.size(); ++i)
    {
        char const c = s[i];
        if (c == '"' || c == '\'')
        {
            size_t j = i + 1;
            while (j < s.size() && s[j] != c)
                j += s[j] == '\\' ? 2 : 1;
            i = j;
        }
        else if (c == '(' || c == '{' || c == '[')
            ++depth;
        else if (c == ')' || c == '}' || c == ']')
        {
            if (--depth == 0)
                return i;
        }
    }
    return std::string::npos;
}

// Positions of c at bracket depth 0 (and outside literals)
std::vector<size_t> TopLevel(std::string const& s, char target)
{
    std::vector<size_t> found;
    int depth = 0;
    for (size_t i = 0; i < s.size(); ++i)
    {
        char const c = s[i];
        if (c == '"' || c == '\'')
        {
            size_t j = i + 1;
            while (j < s.size() && s[j] != c)
                j += s[j] == '\\' ? 2 : 1;
            i = j;
        }
        else if (c == '(' || c == '{' || c == '[')
            ++depth;
        else if (c == ')' || c == '}' || c == ']')
            --depth;
        else if (depth == 0 && c == target)
        {
            if (target == ':' && ((i + 1 < s.size() && s[i + 1] == ':') || (i > 0 && s[i - 1] == ':')))
                continue;
            found.push_back(i);
        }
    }
    return found;
}

bool IsNumber(std::string const& s, double& value)
{
    static std::regex const re(R"(^[+-]?(\d+\.?\d*|\.\d+)([eE][+-]?\d+)?[fF]?$)");
    if (!std::regex_match(s, re))
        return false;
    value = std::strtod(s.c_str(), nullptr);
    return true;
}

struct Scan
{
    std::vector<std::string> flags;
    int computed = 0;
    std::vector<std::string> sites;  // where and what the computed returns are
};

void Analyze(std::string e, Scan& scan, std::string const& where);

// A literal in value position is flagged unless it is 0 or 1; anything else counts as computed
void AnalyzeOperand(std::string e, Scan& scan, std::string const& where)
{
    e = Trim(e);
    double value = 0;
    if (IsNumber(e, value))
    {
        if (value != 0.0 && value != 1.0)
            scan.flags.push_back(where + " " + e);
    }
    else
    {
        ++scan.computed;
        scan.sites.push_back(where + " return " + e.substr(0, 60));
    }
}

void Analyze(std::string e, Scan& scan, std::string const& where)
{
    e = Trim(e);
    while (e.size() >= 2 && e.front() == '(' && MatchingClose(e, 0) == e.size() - 1)
        e = Trim(e.substr(1, e.size() - 2));

    std::vector<size_t> const question = TopLevel(e, '?');
    if (!question.empty())
    {
        // cond ? a : b: the condition is no value; the first top-level ':' after the first '?' that is not nested
        size_t const q = question[0];
        std::vector<size_t> const colons = TopLevel(e, ':');
        size_t const nested = question.size() - 1;
        size_t colon = std::string::npos;
        size_t seen = 0;
        for (size_t c : colons)
            if (c > q && seen++ == nested)
                colon = c;
        if (colon == std::string::npos)
        {
            ++scan.computed;
            return;
        }
        Analyze(e.substr(q + 1, colon - q - 1), scan, where);
        Analyze(e.substr(colon + 1), scan, where);
        return;
    }

    std::vector<size_t> const stars = TopLevel(e, '*');
    if (stars.empty())
    {
        AnalyzeOperand(e, scan, where);
        return;
    }
    size_t from = 0;
    for (size_t star : stars)
    {
        AnalyzeOperand(e.substr(from, star - from), scan, where);
        from = star + 1;
    }
    AnalyzeOperand(e.substr(from), scan, where);
}

int LineOf(std::string const& text, size_t pos)
{
    return 1 + static_cast<int>(std::count(text.begin(), text.begin() + static_cast<std::ptrdiff_t>(pos), '\n'));
}

// The text inside string literals blanked (a "return" in a message is no return); lengths and newlines stay
std::string BlankStrings(std::string s)
{
    for (size_t i = 0; i < s.size(); ++i)
        if (s[i] == '"')
        {
            size_t j = i + 1;
            while (j < s.size() && s[j] != '"' && s[j] != '\n')
            {
                if (s[j] == '\\' && j + 1 < s.size())
                    s[j++] = ' ';
                s[j++] = ' ';
            }
            i = j;
        }
    return s;
}

// Every `float [X::]GetValue[InEncounter](Action* ...) [override] {` body in the source: its returns, analyzed
Scan ScanMultipliers(std::string const& source, std::string const& file, int* definitions = nullptr)
{
    Scan scan;
    std::string const text = BlankStrings(StripComments(source));
    static std::regex const sig(
        R"(\bfloat\s+(?:\w+::)?GetValue\w*\s*\(\s*(?:\[\[maybe_unused\]\]\s*)?Action\s*\*\s*\w*\s*\))");
    std::regex const ret(R"(\breturn\b)");
    for (std::sregex_iterator it(text.begin(), text.end(), sig), end; it != end; ++it)
    {
        size_t i = it->position() + it->length();
        while (i < text.size() &&
               (std::isspace(static_cast<unsigned char>(text[i])) || std::isalpha(static_cast<unsigned char>(text[i]))))
            ++i;  // override, const, final
        if (i >= text.size() || text[i] != '{')
            continue;  // a declaration
        size_t const close = MatchingClose(text, i);
        if (close == std::string::npos)
            continue;
        if (definitions)
            ++*definitions;
        std::string const body = text.substr(i + 1, close - i - 1);
        for (std::sregex_iterator r(body.begin(), body.end(), ret), rend; r != rend; ++r)
        {
            size_t start = r->position() + r->length();
            size_t stop = start;
            int depth = 0;
            while (stop < body.size() && !(body[stop] == ';' && depth == 0))
            {
                if (body[stop] == '(' || body[stop] == '{' || body[stop] == '[')
                    ++depth;
                else if (body[stop] == ')' || body[stop] == '}' || body[stop] == ']')
                    --depth;
                ++stop;
            }
            std::string const where = file + ":" + std::to_string(LineOf(text, i + 1 + r->position()));
            Analyze(body.substr(start, stop - start), scan, where);
        }
    }
    return scan;
}

void TestScanner()
{
    auto flags = [](std::string const& src) { return ScanMultipliers(src, "x").flags.size(); };
    Check(flags("float A::GetValue(Action* action) { return 0.1f; }") == 1, "scanner flags a bare 0.1f");
    Check(flags("float A::GetValue(Action* a) { return x ? 1.0f : 0.0f; }") == 0, "scanner: 1.0f : 0.0f passes");
    Check(flags("float A::GetValue(Action* a) { return x ? 0.5f : 1.0f; }") == 1, "scanner flags a ternary branch");
    Check(flags("float A::GetValue(Action* a) { return hp > 50.0f ? 0.0f : 1.0f; }") == 0,
          "scanner skips a literal in the condition");
    Check(flags("float A::GetValue(Action* a) { return a->b(30) ? 1 : 0; }") == 0,
          "scanner: a call's argument is no value");
    Check(flags("float A::GetValue(Action* a) { // return 0.5f;\n /* return 0.25f; */ return 1.0f; }") == 0,
          "scanner skips commented literals");
    Check(flags("float A::GetValue(Action* a) { return base * 0.5f; }") == 1, "scanner flags an operand of '*'");
    Check(flags("float A::GetValue(Action* a) { return base * 1.0f; }") == 0, "scanner: * 1.0f passes");
    Check(flags("float A::GetValue(Action* a) { return (c ? 0.0f : 0.1f); }") == 1, "scanner sees through parentheses");
    Check(flags("float A::GetValue(Action* a) { return c ? (d ? 1.0f : 0.0f) : 0.3f; }") == 1,
          "scanner: nested ternary");
    Check(flags("float GetValue(Action* action) override { if (x) return 1.0f; return 0.9f; }") == 1,
          "scanner reads an inline header definition");
    Check(flags("float GetValue(Action* action) override;\nfloat Other() { return 0.2f; }") == 0,
          "scanner skips a declaration and other functions");
    Check(flags("float A::GetValue(Action* a) { auto s = \"a//b\"; return 0.0f; }") == 0,
          "scanner: // inside a string");
    Scan const computed = ScanMultipliers("float A::GetValue(Action* a) { return Answer(a); }", "x");
    Check(computed.flags.empty() && computed.computed == 1, "scanner counts a computed return, flags nothing");
}

void TestPureNames()
{
    Check(BaseName("cast::x") == "cast" && BaseName("plain") == "plain", "base name drops the qualifier");
    Check(Entry("action", "chaos bolt::x") == "action:chaos bolt", "entry is kind:name on the base name");
    Check(std::string(ClassName(6)) == "deathknight" && std::string(ClassName(10)).empty(), "class names");

    std::string const text =
        "# comment\n\nwarlock action:conflagrate\nwarlock action:chaos bolt\r\nhunter trigger:x\n  # indented\nbad "
        "line\n"
        "druid weird:name\n";
    Known const known = ParseKnown(text);
    Check(known.byClass.size() == 2 && known.byClass.at("warlock").size() == 2,
          "parse: entries per class, spaces in names");
    Check(known.byClass.at("warlock").count("action:chaos bolt") == 1, "parse: CRLF line");
    Check(known.malformed.size() == 2, "parse: a line without an entry and one with an unknown kind are malformed");

    std::set<std::string> const found = {"action:a", "trigger:b"};
    Diff same = Compare(found, found);
    Check(same.unexpected.empty() && same.stale.empty(), "diff: equal sets pass");
    Diff unexpected = Compare({"action:a", "trigger:b", "action:new"}, found);
    Check(unexpected.unexpected.size() == 1 && unexpected.stale.empty(), "diff: a new name fails");

    // alias resolution: a node key resolves through its node's own name (key -> node name -> creator)
    NodeKeys nodeKeys;
    nodeKeys["alias"].insert("real action");
    nodeKeys["dead alias"].insert("no such action");
    auto const hasAction = [](std::string const& action) { return action == "real action" || action == "plain"; };
    Check(ResolvesAction("alias", nodeKeys, hasAction) && ResolvesAction("alias::q", nodeKeys, hasAction),
          "alias: a node key resolves when its node's name is an action");
    Check(!ResolvesAction("dead alias", nodeKeys, hasAction), "alias: a node key whose node name is no action fails");
    Check(ResolvesAction("plain", nodeKeys, hasAction) && !ResolvesAction("absent", nodeKeys, hasAction),
          "alias: a name that is no node key is looked up as an action");
    Check(!ResolvesAction("real action", NodeKeys{{"real action", {"nothing"}}}, hasAction),
          "alias: the node key decides, not the key as an action");

    // the intersection: a name missing in one class's general set is not shared; the first set gives the referrer
    Referrers const a = {{"action:x", "strategy:s1"}, {"action:y", "strategy:s1"}};
    Referrers const b = {{"action:x", "strategy:s2"}};
    Referrers const c = {{"action:x", "trigger:t"}, {"action:y", "trigger:t"}};
    Referrers const both = Intersect({&a, &b, &c});
    Check(both.size() == 1 && both.at("action:x") == "strategy:s1", "intersection: only the name every class has");
    Check(Intersect({&a}).size() == 2 && Intersect({}).empty(), "intersection: one set is itself, none is empty");

    // the referrer suffix and a shared line
    Check(StripReferrer("action:x y <- strategy:z") == "action:x y" && StripReferrer("action:x") == "action:x",
          "strip: the referrer part goes, a bare entry stays");
    Known const sharedKnown = ParseKnown("# owner\nshared trigger:abc def\nmage action:x\n");
    Check(sharedKnown.malformed.empty() && sharedKnown.byClass.at("shared").count("trigger:abc def") == 1,
          "parse: a shared line is read like a class line");
    Diff stale = Compare({"action:a"}, found);
    Check(stale.stale.size() == 1 && stale.unexpected.empty(), "diff: a known name that now resolves is stale");

    // The removal case: the same unresolved names against a file text with one line removed must fail
    std::string const file = "warlock action:a\nwarlock action:b\n";
    std::string const cut = "warlock action:a\n";
    std::set<std::string> const live = {"action:a", "action:b"};
    Check(Compare(live, ParseKnown(file).byClass["warlock"]).unexpected.empty(), "removal case: whole file passes");
    Check(Compare(live, ParseKnown(cut).byClass["warlock"]).unexpected.size() == 1,
          "removal case: a line removed from the file fails");
}

// True when comment-stripped text holds "name" (the whole literal) as the first argument of NextAction(, ActionNode(,
// TriggerNode( or supported.push_back(; whitespace and line breaks may sit between the bracket and the literal
bool ReferencesName(std::string const& text, std::string const& name)
{
    std::string const quoted = "\"" + name + "\"";
    static std::vector<std::string> const heads = {"NextAction", "ActionNode", "TriggerNode", "supported.push_back"};
    for (size_t pos = text.find(quoted); pos != std::string::npos; pos = text.find(quoted, pos + 1))
    {
        size_t i = pos;
        while (i > 0 && std::isspace(static_cast<unsigned char>(text[i - 1])))
            --i;
        if (i == 0 || text[i - 1] != '(')
            continue;
        --i;
        while (i > 0 && std::isspace(static_cast<unsigned char>(text[i - 1])))
            --i;
        for (std::string const& head : heads)
            if (i >= head.size() && text.compare(i - head.size(), head.size(), head) == 0)
                return true;
    }
    return false;
}

void TestReferences()
{
    Check(ReferencesName("x = NextAction(\"food\", 1.0f);", "food"), "reference: NextAction(\"food\"");
    Check(ReferencesName("new TriggerNode(\n    \"food\",\n    {})", "food"),
          "reference: TriggerNode( then a line break");
    Check(ReferencesName("supported.push_back(\"food\");", "food"), "reference: supported.push_back(\"food\")");
    Check(ReferencesName("ActionNode ( \"food\" , x)", "food"), "reference: ActionNode with spaces");
    Check(!ReferencesName("HasStrategy(\"food\", x)", "food"), "no reference: HasStrategy");
    Check(!ReferencesName("UseItemAction(botAI, \"eat\", false, \"food\")", "food"), "no reference: later argument");
    Check(!ReferencesName("NextAction(\"food stuff\"", "food"), "no reference: a longer literal");
    Check(!ReferencesName(StripComments("// NextAction(\"food\")\n/* NextAction(\"food\") */"), "food"),
          "no reference: commented out");
}

void TestKnownFile()
{
    std::string const txt = ReadFile("tests/combat-names-known.txt");
    std::string const inc = ReadFile("src/Bot/Engine/CombatNamesKnown.inc");
    std::string const open = "R\"KNOWN(\n";
    size_t const from = inc.find(open);
    size_t const to = inc.rfind(")KNOWN\"");
    Check(from != std::string::npos && to != std::string::npos && to >= from + open.size(),
          "embedded copy is readable");
    if (from == std::string::npos || to == std::string::npos || to < from + open.size())
        return;
    std::string const body = inc.substr(from + open.size(), to - from - open.size());
    Check(body == txt, "tests/combat-names-known.txt is byte-identical to the embedded copy");

    Known const known = ParseKnown(txt);
    Check(known.malformed.empty(), "the known file has no malformed line");

    // CF.16/CF.17: a shared entry is a name only an instance script (or another module) speaks: no file outside
    // src/Ai/Raid and src/Ai/Dungeon refers to it (a base file that still does is a dead reference to delete)
    int bannedEntries = 0;
    int sharedEntries = 0;
    int outsideInstance = 0;
    auto const shared = known.byClass.find("shared");
    std::vector<std::string> const files = SourceFiles("src");
    if (shared != known.byClass.end())
        for (std::string const& entry : shared->second)
        {
            ++sharedEntries;
            std::string const name = entry.substr(entry.find(':') + 1);
            for (std::string const& file : files)
            {
                if (file.rfind("src/Ai/Raid/", 0) == 0 || file.rfind("src/Ai/Dungeon/", 0) == 0)
                    continue;
                if (ReferencesName(StripComments(ReadFile(file)), name))
                {
                    std::printf("  shared entry %s is spoken by %s\n", entry.c_str(), file.c_str());
                    ++outsideInstance;
                }
            }
        }
    Check(outsideInstance == 0, "every shared entry is named only under src/Ai/Raid or src/Ai/Dungeon (" +
                                    std::to_string(sharedEntries) + " entries)");
    for (auto const& [cls, entries] : known.byClass)
        for (std::string const& entry : entries)
        {
            std::string const name = entry.substr(entry.find(':') + 1);
            if (entry.rfind("strategy:", 0) == 0 && (name == "caster aoe" || name == "boost" || name == "cast time"))
                ++bannedEntries;
        }
    Check(bannedEntries == 0, "the known file lists no strategy caster aoe, boost or cast time");
}

void TestFactoryLiterals()
{
    std::vector<std::string> const fixture = FactoryStrategyLiterals(
        "engine->addStrategy(\"a\", false); engine->addStrategiesNoInit(\"b\", \"c::q\", nullptr); // "
        "addStrategy(\"z\")\n"
        "engine->addStrategies(\"d\", nullptr);");
    Check(fixture == std::vector<std::string>({"a", "b", "c", "d"}),
          "factory scan reads the literals of the three calls");

    std::set<std::string> const registered = RegisteredNames("src");
    Check(registered.size() > 500, "creators scan finds the registrations (" + std::to_string(registered.size()) + ")");
    std::vector<std::string> const used = FactoryStrategyLiterals(ReadFile("src/Bot/Factory/AiFactory.cpp"));
    Check(used.size() > 20, "factory scan finds the names AiFactory adds (" + std::to_string(used.size()) + ")");
    int unknown = 0;
    for (std::string const& name : used)
        if (!registered.count(name))
        {
            std::printf("  AiFactory adds '%s': no creators[\"%s\"] under src\n", name.c_str(), name.c_str());
            ++unknown;
        }
    Check(unknown == 0, "every strategy name AiFactory adds is registered somewhere");
    Check(!registered.count("cast time") && !registered.count("caster aoe"), "no cast time or caster aoe registration");
}

void TestMultiplierLiterals()
{
    int definitions = 0;
    int computed = 0;
    std::vector<std::string> flags;
    std::vector<std::string> sites;
    for (std::string const& file : SourceFiles("src"))
    {
        std::string const text = ReadFile(file);
        if (text.find("GetValue") == std::string::npos)
            continue;
        Scan const scan = ScanMultipliers(text, file, &definitions);
        flags.insert(flags.end(), scan.flags.begin(), scan.flags.end());
        computed += scan.computed;
        sites.insert(sites.end(), scan.sites.begin(), scan.sites.end());
    }
    for (std::string const& flag : flags)
        std::printf("  literal other than 0 or 1: %s\n", flag.c_str());
    Check(definitions > 100, "multiplier scan finds the definitions (" + std::to_string(definitions) + ")");
    Check(flags.empty(), "no multiplier GetValue returns a literal other than 0 or 1");
    for (std::string const& site : sites)
        std::printf("  computed: %s\n", site.c_str());
    std::printf("info: %d computed returns (a call or a member; not checkable lexically)\n", computed);
}
}  // namespace

int main()
{
    TestPureNames();
    TestScanner();
    TestReferences();
    TestKnownFile();
    TestFactoryLiterals();
    TestMultiplierLiterals();
    std::printf("%d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
