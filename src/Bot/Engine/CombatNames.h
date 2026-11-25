/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_COMBATNAMES_H
#define PLAYERBOTS_COMBATNAMES_H

// The name check (combat-foundation CF4, R3): a strategy, trigger or action name that does not resolve is listed, not
// a silent no-op. The first part is pure (no core headers; tests/combat-names-selftest.cpp compiles it alone): the
// base-name split the engine does, the "kind:name" entry, the action-node alias resolution, the known-file parse, the
// intersection for the shared section and the diff. The engine part (one check
// per class when the first bot of the class builds its engines; the console commands) is declared below the pure part.

#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace CombatNames
{
// What NamedObjectContextList::create does with a name: the part before "::" is looked up in the creators
inline std::string BaseName(std::string const& name) { return name.substr(0, name.find("::")); }

// kind is "strategy", "trigger" or "action"
inline std::string Entry(std::string const& kind, std::string const& name) { return kind + ":" + BaseName(name); }

// A class's lowercase name as the known file spells it; "" for an id that is no class
inline char const* ClassName(uint8_t classId)
{
    switch (classId)
    {
        case 1:
            return "warrior";
        case 2:
            return "paladin";
        case 3:
            return "hunter";
        case 4:
            return "rogue";
        case 5:
            return "priest";
        case 6:
            return "deathknight";
        case 7:
            return "shaman";
        case 8:
            return "mage";
        case 9:
            return "warlock";
        case 11:
            return "druid";
        default:
            return "";
    }
}

// A section's findings: "kind:name" entry -> the first referrer, "strategy:<key>", "trigger:<key>" or "action:<key>"
using Referrers = std::map<std::string, std::string>;

// Action node key -> the names of the nodes the class's strategies create under it. The engine creates a node by the
// key (Engine::CreateActionNode) and then looks its own name up as the action (Engine::InitializeAction)
using NodeKeys = std::map<std::string, std::set<std::string>>;

// An action name as the engine resolves it: a key of a node factory resolves iff one of its nodes' own names is an
// action; any other name is looked up as an action itself. Coarse on purpose: any strategy of the class counts
inline bool ResolvesAction(std::string const& name, NodeKeys const& nodeKeys,
                           std::function<bool(std::string const&)> const& hasAction)
{
    std::string const base = BaseName(name);
    auto const it = nodeKeys.find(base);
    if (it == nodeKeys.end())
        return hasAction(base);
    for (std::string const& nodeName : it->second)
        if (hasAction(nodeName))
            return true;
    return false;
}

// "<entry> <- <referrer>" -> "<entry>"; a line without the suffix stays as it is
inline std::string StripReferrer(std::string const& line)
{
    size_t const at = line.find(" <- ");
    return at == std::string::npos ? line : line.substr(0, at);
}

inline std::set<std::string> Entries(Referrers const& found)
{
    std::set<std::string> entries;
    for (auto const& item : found)
        entries.insert(item.first);
    return entries;
}

// The entries every set holds, each with the referrer of the first set that holds it (pass the sets by class id)
inline Referrers Intersect(std::vector<Referrers const*> const& sets)
{
    Referrers shared;
    if (sets.empty())
        return shared;
    for (auto const& item : *sets.front())
    {
        bool inAll = true;
        for (Referrers const* other : sets)
            if (other->find(item.first) == other->end())
                inAll = false;
        if (inAll)
            shared.insert(item);
    }
    return shared;
}

struct Known
{
    std::map<std::string, std::set<std::string>> byClass;  // section (a class name or "shared") -> "kind:name" entries
    std::vector<std::string> malformed;                    // lines that are neither blank, comment nor an entry
};

// The engine's three kinds, and a spec table's row checks (CF6: row-trigger, row-option, row-spell, row-clash,
// row-group; CF7: row-life)
inline bool IsKind(std::string const& kind)
{
    return kind == "strategy" || kind == "trigger" || kind == "action" || kind == "row-trigger" ||
           kind == "row-option" || kind == "row-spell" || kind == "row-clash" || kind == "row-group" ||
           kind == "row-life";
}

// One entry per line: "<class or shared> <kind>:<name>" (the name may hold spaces); "#" starts a comment line, blank
// lines count for nothing
inline Known ParseKnown(std::string const& text)
{
    Known known;
    size_t pos = 0;
    while (pos <= text.size())
    {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos)
            end = text.size();
        std::string line = text.substr(pos, end - pos);
        pos = end + 1;
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        size_t first = line.find_first_not_of(" \t");
        if (first == std::string::npos || line[first] == '#')
            continue;
        line = line.substr(first);
        size_t space = line.find(' ');
        size_t colon = space == std::string::npos ? std::string::npos : line.find(':', space);
        if (colon == std::string::npos || colon == space + 1 || colon + 1 >= line.size() ||
            !IsKind(line.substr(space + 1, colon - space - 1)))
        {
            known.malformed.push_back(line);
            continue;
        }
        known.byClass[line.substr(0, space)].insert(line.substr(space + 1));
    }
    return known;
}

struct Diff
{
    std::vector<std::string> unexpected;  // found unresolved, not in the known set
    std::vector<std::string> stale;       // in the known set, resolves now
};

inline Diff Compare(std::set<std::string> const& found, std::set<std::string> const& known)
{
    Diff diff;
    for (std::string const& name : found)
        if (known.find(name) == known.end())
            diff.unexpected.push_back(name);
    for (std::string const& name : known)
        if (found.find(name) == found.end())
            diff.stale.push_back(name);
    return diff;
}
}  // namespace CombatNames

class ChatHandler;
class Player;
class PlayerbotAI;

namespace CombatNames
{
// Runs the check for the bot's class once (a flag per class under a mutex; later bots only read the flag); called by
// AiFactory::createCombatEngine before the engine's strategies are added
void OnEnginesBuilding(Player* bot, PlayerbotAI* botAI);

// Names already reported by Engine::addStrategy, so a name that does not resolve is logged once
bool FirstUnknownStrategy(std::string const& name);

// `playerbots combat names [class|shared]`
bool HandleNamesCommand(ChatHandler* handler, char const* args);
}  // namespace CombatNames

#endif
