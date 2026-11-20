/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "CombatNames.h"

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <unordered_set>

#include "Action.h"
#include "AiFactory.h"
#include "AiObjectContext.h"
#include "Chat.h"
#include "CombatNamesKnown.inc"
#include "Log.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Strategy.h"
#include "Trigger.h"

namespace CombatNames
{
namespace
{
struct ClassResult
{
    std::atomic<bool> done{false};  // read without the lock by every later bot of the class
    Referrers unresolved;           // from the class's own referrers
    Referrers general;              // from referrers every class shares; the intersection over all classes is `shared`
    uint32 strategies = 0;
    uint32 skipped = 0;  // strategies whose rows are data, and triggers and actions that returned no object
};

std::mutex resultMutex;  // guards the results' maps and the once-set below
ClassResult results[MAX_CLASSES];
bool sharedJudged = false;
Referrers sharedResult;
std::unordered_set<std::string> reportedStrategies;

// The strategy that reads its rows from the database (CustomStrategy::InitTriggers queries twice, with an empty
// qualifier); no synchronous database query belongs in the walk. No other strategy is Qualified or reads data
bool IsDataStrategy(std::string const& name) { return name == "custom"; }

struct Walker
{
    AiObjectContext* context;
    NodeKeys nodeKeys;
    ClassResult& result;

    bool Has(char const* kind, std::string const& name)
    {
        if (std::string(kind) == "trigger")
            return context->HasTrigger(name);
        if (std::string(kind) == "strategy")
            return context->HasStrategy(name);
        return ResolvesAction(name, nodeKeys, [this](std::string const& action) { return context->HasAction(action); });
    }

    void Note(bool general, char const* kind, std::string const& name, std::string const& referrer)
    {
        if (!Has(kind, name))
            (general ? result.general : result.unresolved).emplace(Entry(kind, name), referrer);
    }

    void Handlers(bool general, std::vector<NextAction> const& handlers, std::string const& referrer)
    {
        for (NextAction handler : handlers)
            Note(general, "action", handler.getName(), referrer);
    }

    void Nodes(bool general, std::vector<TriggerNode*>& nodes, std::string const& referrer)
    {
        for (TriggerNode* node : nodes)
        {
            Note(general, "trigger", node->getName(), referrer);
            Handlers(general, node->getHandlers(), referrer);
            delete node;
        }
        nodes.clear();
    }
};

// What the engine would look up for the class, judged where the referrer speaks: every strategy of the class (not the
// active set: coarse on purpose) with its trigger rows, default actions and action nodes, then every registered
// trigger's handlers and every action's prerequisites, alternatives and continuers. A referrer every class shares
// fills `general`, a class's own `unresolved`. Action names resolve as the engine resolves them (ResolvesAction)
void Collect(Player* bot, PlayerbotAI* botAI, ClassResult& result)
{
    std::unique_ptr<AiObjectContext> context(AiFactory::createAiObjectContext(bot, botAI));
    Walker walker{context.get(), {}, result};

    std::set<std::string> const strategyNames = context->GetSupportedStrategies();

    // First pass: the node factory keys and the names of the nodes they create
    for (std::string const& name : strategyNames)
    {
        Strategy* strategy = IsDataStrategy(name) ? nullptr : context->GetStrategy(name);
        if (!strategy)
            continue;
        for (auto const& factory : strategy->actionNodeFactories.creators)
            if (std::unique_ptr<ActionNode> node{factory.second(botAI)})
                walker.nodeKeys[factory.first].insert(node->getName());
    }

    for (std::string const& name : strategyNames)
    {
        if (IsDataStrategy(name))
        {
            ++result.skipped;
            continue;
        }
        bool const general = context->IsGeneralStrategy(name);
        std::string const referrer = "strategy:" + name;
        Strategy* strategy = context->GetStrategy(name);
        if (!strategy)
        {
            walker.Note(general, "strategy", name, referrer);
            continue;
        }
        ++result.strategies;

        std::vector<TriggerNode*> nodes;
        strategy->InitTriggers(nodes);
        strategy->InitReactionTriggers(nodes);
        walker.Nodes(general, nodes, referrer);
        walker.Handlers(general, strategy->getDefaultActions(), referrer);

        for (auto const& factory : strategy->actionNodeFactories.creators)
        {
            std::unique_ptr<ActionNode> node(factory.second(botAI));
            if (!node)
                continue;
            walker.Note(general, "action", node->getName(), referrer);
            walker.Handlers(general, node->ownPrerequisites(), referrer);
            walker.Handlers(general, node->ownAlternatives(), referrer);
            walker.Handlers(general, node->ownContinuers(), referrer);
        }
    }

    for (std::string const& name : context->GetSupportedTriggers())
    {
        Trigger* trigger = context->GetTrigger(name);
        if (!trigger)
        {
            ++result.skipped;
            continue;
        }
        walker.Handlers(context->IsGeneralTrigger(name), trigger->getHandlers(), "trigger:" + name);
    }

    for (std::string const& name : context->GetSupportedActions())
    {
        Action* action = context->GetAction(name);
        if (!action)
        {
            ++result.skipped;
            continue;
        }
        bool const general = context->IsGeneralAction(name);
        std::string const referrer = "action:" + name;
        walker.Handlers(general, action->getPrerequisites(), referrer);
        walker.Handlers(general, action->getAlternatives(), referrer);
        walker.Handlers(general, action->getContinuers(), referrer);
    }
}

// The classes that have no result yet, as " name name"
std::string MissingClasses()
{
    std::string missing;
    for (uint8 classId = 1; classId < MAX_CLASSES; ++classId)
        if (ClassName(classId)[0] && !results[classId].done.load(std::memory_order_acquire))
            missing += std::string(" ") + ClassName(classId);
    return missing;
}

// R3 for one section: unexpected and stale entries against the known file are errors
void ReportAgainstKnown(std::string const& section, Referrers const& found)
{
    Known const known = ParseKnown(COMBAT_NAMES_KNOWN);
    auto const it = known.byClass.find(section);
    std::set<std::string> const none;
    Diff const diff = Compare(Entries(found), it == known.byClass.end() ? none : it->second);
    for (std::string const& entry : diff.unexpected)
        LOG_ERROR("playerbots", "Combat names: {}: {} does not resolve and is not in combat-names-known.txt", section,
                  entry);
    for (std::string const& entry : diff.stale)
        LOG_ERROR("playerbots", "Combat names: {}: {} is in combat-names-known.txt but resolves now", section, entry);
}

std::string Lower(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return std::tolower(c); });
    return text;
}
}  // namespace

void OnEnginesBuilding(Player* bot, PlayerbotAI* botAI)
{
    uint8 const classId = bot->getClass();
    if (classId >= MAX_CLASSES || results[classId].done.load(std::memory_order_acquire))
        return;

    std::lock_guard<std::mutex> lock(resultMutex);
    ClassResult& result = results[classId];
    if (result.done.load(std::memory_order_relaxed))
        return;

    Collect(bot, botAI, result);
    result.done.store(true, std::memory_order_release);
    LOG_INFO("playerbots", "Combat names: {}: {} unresolved (playerbots combat names {}); {} strategies, {} skipped",
             ClassName(classId), result.unresolved.size(), ClassName(classId), result.strategies, result.skipped);
    ReportAgainstKnown(ClassName(classId), result.unresolved);

    // shared: the names unresolved in every class, judged when the tenth class has its result
    if (sharedJudged || !MissingClasses().empty())
        return;
    std::vector<Referrers const*> sets;
    for (uint8 id = 1; id < MAX_CLASSES; ++id)
        if (ClassName(id)[0])
            sets.push_back(&results[id].general);
    sharedResult = Intersect(sets);
    sharedJudged = true;
    LOG_INFO("playerbots", "Combat names: shared: {} unresolved (playerbots combat names shared)", sharedResult.size());
    ReportAgainstKnown("shared", sharedResult);
}

bool FirstUnknownStrategy(std::string const& name)
{
    std::lock_guard<std::mutex> lock(resultMutex);
    return reportedStrategies.insert(name).second;
}

bool HandleNamesCommand(ChatHandler* handler, char const* args)
{
    std::string const wanted = Lower(args ? args : "");
    uint32 total = 0;
    uint32 sections = 0;
    std::string missing;

    std::lock_guard<std::mutex> lock(resultMutex);
    for (uint8 classId = 1; classId < MAX_CLASSES; ++classId)
    {
        std::string const name = ClassName(classId);
        if (name.empty() || (!wanted.empty() && wanted != name))
            continue;
        if (!results[classId].done.load(std::memory_order_acquire))
        {
            missing += " " + name;
            continue;
        }
        ++sections;
        for (auto const& item : results[classId].unresolved)
        {
            handler->PSendSysMessage("COMBAT names {} {} <- {}", name, item.first, item.second);
            ++total;
        }
    }
    if (wanted.empty() || wanted == "shared")
    {
        if (sharedJudged)
        {
            ++sections;
            for (auto const& item : sharedResult)
            {
                handler->PSendSysMessage("COMBAT names shared {} <- {}", item.first, item.second);
                ++total;
            }
        }
        else
            handler->PSendSysMessage("COMBAT names: shared not judged, classes without a result:{}", MissingClasses());
    }
    handler->PSendSysMessage("COMBAT names: {} unresolved in {} sections", total, sections);
    if (!missing.empty())
        handler->PSendSysMessage("COMBAT names: no result yet (no bot of the class built engines):{}", missing);
    return true;
}

bool HandleSelftestCommand(ChatHandler* handler, char const* /*args*/)
{
    Known const known = ParseKnown(COMBAT_NAMES_KNOWN);
    uint32 checks = 0;
    uint32 failures = 0;

    for (std::string const& line : known.malformed)
    {
        ++checks;
        ++failures;
        handler->PSendSysMessage("FAIL known file line is malformed: {}", line);
    }

    auto check = [&](std::string const& section, Referrers const& found)
    {
        ++checks;
        auto const it = known.byClass.find(section);
        std::set<std::string> const none;
        Diff const diff = Compare(Entries(found), it == known.byClass.end() ? none : it->second);
        for (std::string const& entry : diff.unexpected)
        {
            ++failures;
            handler->PSendSysMessage("FAIL {} {} not in combat-names-known.txt", section, entry);
        }
        for (std::string const& entry : diff.stale)
        {
            ++failures;
            handler->PSendSysMessage("FAIL stale {} {} resolves now", section, entry);
        }
    };

    std::lock_guard<std::mutex> lock(resultMutex);
    for (uint8 classId = 1; classId < MAX_CLASSES; ++classId)
    {
        std::string const name = ClassName(classId);
        if (!name.empty() && results[classId].done.load(std::memory_order_acquire))
            check(name, results[classId].unresolved);
    }
    if (sharedJudged)
        check("shared", sharedResult);
    else
        handler->PSendSysMessage("shared not judged: classes without a result:{}", MissingClasses());

    handler->PSendSysMessage("COMBAT selftest: {} checks, {} failures ({})", checks, failures,
                             failures ? "FAIL" : "PASS");
    return true;
}
}  // namespace CombatNames
