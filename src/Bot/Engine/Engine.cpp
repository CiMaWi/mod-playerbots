/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "Engine.h"

#include <algorithm>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

#include "Action.h"
#include "CensusMgr.h"
#include "CombatTrace.h"
#include "Event.h"
#include "PerfMonitor.h"
#include "Playerbots.h"
#include "Queue.h"
#include "Strategy.h"
#include "TestConstraints.h"
#include "TestProbe.h"
#include "Timer.h"
#include "WorldLogBotState.h"

namespace
{
// Event::GetSource() returns a copy; the world log's recorder reads the trigger name without one
struct EventSourceAccess : Event
{
    static std::string const& Of(Event const& event) { return event.*(&EventSourceAccess::source); }
};
}  // namespace

Engine::Engine(PlayerbotAI* botAI, AiObjectContext* factory) : PlayerbotAIAware(botAI), aiObjectContext(factory)
{
    lastRelevance = 0.0f;
    testMode = false;
}

bool ActionExecutionListeners::Before(Action* action, Event event)
{
    bool result = true;
    for (std::list<ActionExecutionListener*>::iterator i = listeners.begin(); i != listeners.end(); i++)
    {
        result &= (*i)->Before(action, event);
    }

    return result;
}

void ActionExecutionListeners::After(Action* action, bool executed, Event event)
{
    for (std::list<ActionExecutionListener*>::iterator i = listeners.begin(); i != listeners.end(); i++)
    {
        (*i)->After(action, executed, event);
    }
}

bool ActionExecutionListeners::OverrideResult(Action* action, bool executed, Event event)
{
    bool result = executed;
    for (std::list<ActionExecutionListener*>::iterator i = listeners.begin(); i != listeners.end(); i++)
    {
        result = (*i)->OverrideResult(action, result, event);
    }

    return result;
}

bool ActionExecutionListeners::AllowExecution(Action* action, Event event)
{
    bool result = true;
    for (std::list<ActionExecutionListener*>::iterator i = listeners.begin(); i != listeners.end(); i++)
    {
        result &= (*i)->AllowExecution(action, event);
    }

    return result;
}

ActionExecutionListeners::~ActionExecutionListeners()
{
    for (std::list<ActionExecutionListener*>::iterator i = listeners.begin(); i != listeners.end(); i++)
    {
        delete *i;
    }

    listeners.clear();
}

Engine::~Engine(void)
{
    Reset();

    // for (std::map<std::string, Strategy*>::iterator i = strategies.begin(); i != strategies.end(); i++)
    // {
    //     Strategy* strategy = i->second;
    //     if (strategy)
    //     {
    //         delete strategy;
    //     }
    // }

    strategies.clear();
}

void Engine::Reset()
{
    strategyTypeMask = 0;

    ActionNode* action = nullptr;

    while ((action = queue.Pop()) != nullptr)
    {
        delete action;
    }

    for (TriggerNode* trigger : triggers)
    {
        delete trigger;
    }

    triggers.clear();

    for (Multiplier* multiplier : multipliers)
    {
        delete multiplier;
    }

    multipliers.clear();

    actionNodeFactories.creators.clear();
}

void Engine::Init()
{
    Reset();

    hasTargetExclusions = false;
    for (std::map<std::string, Strategy*>::iterator i = strategies.begin(); i != strategies.end(); i++)
    {
        Strategy* strategy = i->second;
        strategyTypeMask |= strategy->GetType();
        hasTargetExclusions |= strategy->HasTargetExclusions();
        strategy->InitMultipliers(multipliers);
        strategy->InitTriggers(triggers);
        for (auto &iter : strategy->actionNodeFactories.creators)
        {
            actionNodeFactories.creators[iter.first] = iter.second;
        }
    }

    if (testMode)
    {
        FILE* file = fopen("test.log", "w");
        fprintf(file, "\n");
        fclose(file);
    }
}

bool Engine::DoNextAction(Unit* /*unit*/, uint32 /*depth*/, bool minimal)
{
    LogAction("--- AI Tick ---");

    if (TestProbe::On(botAI->testConstraints))
        TestProbe::Pass(botAI);

    if (sPlayerbotAIConfig.LogValuesPerTick)
        LogValues();

    bool actionExecuted = false;
    ActionBasket* basket = nullptr;
    time_t currentTime = time(nullptr);

    if (!minimal)
        botAI->forceRebuff.RollBuffPendingCycle();

    // Update triggers and push default actions
    ProcessTriggers(minimal);
    PushDefaultActions();

    uint32 iterations = 0;
    uint32 iterationsPerTick = queue.Size() * (minimal ? 2 : sPlayerbotAIConfig.IterationsPerTick);

    // The fight tally (an open fight) and the casttrace detail watch only the combat engine's ticks
    bool const watched = (botAI->fightTally.IsOpen() || TestConstraints::CombatTraced(botAI->testConstraints)) &&
                         botAI->GetState() == BOT_STATE_COMBAT && this == botAI->GetCurrentEngine();
    bool const tallied = watched && botAI->fightTally.IsOpen();
    bool const traced = watched && TestConstraints::CombatTraced(botAI->testConstraints);
    bool noPower = false;
    bool executedCast = false;
    std::optional<CombatTrace::Rejects> rejects;
    if (traced)
        rejects.emplace();

    while (++iterations <= iterationsPerTick)
    {
        basket = queue.Peek();
        if (!basket)
            break;

        float relevance = basket->getRelevance();  // for reference
        bool skipPrerequisites = basket->isSkipPrerequisites();

        if (minimal && (relevance < 100))
            continue;

        Event event = basket->getEvent();
        ActionNode* actionNode = queue.Pop();  // NOTE: Pop() deletes basket
        Action* action = InitializeAction(actionNode);

        if (!action)
        {
            LogAction("A:%s - UNKNOWN", actionNode->getName().c_str());
            if (rejects)
                rejects->Add(botAI, nullptr, actionNode->getName(), CombatTrace::RejectReason::Unknown, relevance);
        }
        else if (action->isUseful())
        {
            Multiplier* vetoing = nullptr;
            float vetoValue = 0.0f;
            // Apply multipliers early to avoid unnecessary iterations
            for (Multiplier* multiplier : multipliers)
            {
                float value;
                {
                    PerfMonitorScope scope(multiplier->GetPerfData());
                    value = multiplier->GetValue(action);
                }

                relevance *= value;
                action->setRelevance(relevance);

                if (relevance <= 0)
                {
                    if (value <= 0)
                        multiplier->NoteBlock();

                    vetoing = multiplier;
                    vetoValue = value;

                    LogAction("Multiplier %s made action %s useless", multiplier->getName().c_str(), action->getName().c_str());
                    break;
                }
            }

            if (action->isPossible() && relevance > 0)
            {
                if (!skipPrerequisites)
                {
                    LogAction("A:%s - PREREQ", action->getName().c_str());

                    if (MultiplyAndPush(actionNode->getPrerequisites(), relevance + 0.002f, false, event, "prereq"))
                    {
                        PushAgain(actionNode, relevance + 0.001f, event);
                        continue;
                    }
                }

                PerfMonitorOperation* pmo =
                    PerfMonitor::IsEnabled()
                        ? sPerfMonitor.start(PERF_MON_ACTION, action->getName(), &aiObjectContext->performanceStack)
                        : nullptr;
                // An action that casts nothing leaves no stale prepare result for the no-power test below
                if (watched)
                    botAI->ClearPrepareResult();
                actionExecuted = ListenAndExecute(action, event);
                if (pmo)
                    pmo->finish();

                if (actionExecuted)
                {
                    LogAction("A:%s - OK", action->getName().c_str());
                    if (tallied)
                        executedCast = CombatTally::IsCast(action);
                    if (traced)
                        CombatTrace::Executed(botAI, action, relevance);
                    MultiplyAndPush(actionNode->getContinuers(), relevance, false, event, "cont");
                    lastRelevance = relevance;
                    delete actionNode;  // Safe memory management
                    break;
                }
                else
                {
                    LogAction("A:%s - FAILED", action->getName().c_str());
                    // CanCastSpell's probe ignores the power cost, so a lack of power shows only in the cast's prepare
                    bool const failedNoPower =
                        watched && botAI->LastPrepareResult() == static_cast<int32>(SPELL_FAILED_NO_POWER);
                    noPower |= failedNoPower;
                    if (rejects)
                        rejects->Add(
                            botAI, action, action->getName(),
                            failedNoPower ? CombatTrace::RejectReason::NoPower : CombatTrace::RejectReason::Failed,
                            relevance);
                    MultiplyAndPush(actionNode->getAlternatives(), relevance + 0.003f, false, event, "alt");
                }
            }
            else
            {
                LogAction("A:%s - IMPOSSIBLE", action->getName().c_str());
                if (rejects)
                    rejects->Add(botAI, action, action->getName(),
                                 vetoing ? CombatTrace::RejectReason::Vetoed : CombatTrace::RejectReason::Impossible,
                                 relevance, vetoing ? vetoing->getName() : std::string(), vetoValue);
                MultiplyAndPush(actionNode->getAlternatives(), relevance + 0.003f, false, event, "alt");
            }
        }
        else
        {
            LogAction("A:%s - USELESS", action->getName().c_str());
            lastRelevance = relevance;
            if (rejects)
                rejects->Add(botAI, action, action->getName(), CombatTrace::RejectReason::Useless, relevance);
        }

        delete actionNode;  // Always delete after processing the action node
    }

    if (time(nullptr) - currentTime > 1)
    {
        LogAction("Execution time exceeded 1 second");
    }

    if (!actionExecuted)
        LogAction("no actions executed");

    if (watched)
    {
        Player* const bot = botAI->GetBot();
        CombatTally::TickKind kind;
        {
            PerfMonitorScope scope(
                PerfMonitor::IsEnabled() && tallied ? sPerfMonitor.acquire(PERF_MON_TOTAL, "Combat tally") : nullptr);
            // Could act: alive, not casting or channelling (an auto shot does not count), in control, off the GCD
            bool const couldAct = !actionExecuted && bot->IsAlive() && !bot->IsNonMeleeSpellCast(false, false, true) &&
                                  !bot->HasUnitState(UNIT_STATE_LOST_CONTROL) &&
                                  !botAI->forceRebuff.IsOnGlobalCooldown();
            kind = CombatTally::Classify(actionExecuted, noPower, couldAct);
            if (tallied)
            {
                CombatManager const& combat = bot->GetCombatManager();
                uint32 const enemies =
                    static_cast<uint32>(combat.GetPvECombatRefs().size() + combat.GetPvPCombatRefs().size());
                botAI->fightTally.Tick(getMSTime(), kind, bot->isMoving(), enemies, executedCast);
            }
        }

        if (traced)
        {
            PerfMonitorScope scope(PerfMonitor::IsEnabled() ? sPerfMonitor.acquire(PERF_MON_TOTAL, "Combat trace")
                                                            : nullptr);
            if (!botAI->combatKnownTraced)
                CombatTrace::Known(botAI);
            if (kind == CombatTally::TickKind::Idle || kind == CombatTally::TickKind::NoPower)
                CombatTrace::Rejected(botAI, *rejects);
        }
    }

    queue.RemoveExpired();

    return actionExecuted;
}

ActionNode* Engine::CreateActionNode(std::string const name)
{
    ActionNode* node = actionNodeFactories.GetContextObject(name, botAI);
    if (node)
        return node;

    return new ActionNode(name,
                          /*P*/ {},
                          /*A*/ {},
                          /*C*/ {});
}

bool Engine::MultiplyAndPush(
    std::vector<NextAction> actions,
    float forceRelevance,
    bool skipPrerequisites,
    Event event,
    char const* pushType
)
{
    bool pushed = false;

    for (NextAction nextAction : actions)
    {
        ActionNode* action = this->CreateActionNode(nextAction.getName());

        this->InitializeAction(action);

        float k = nextAction.getRelevance();

        if (forceRelevance > 0.0f)
        {
            k = forceRelevance;
        }

        if (k > 0)
        {
            this->LogAction("PUSH:%s - %f (%s)", action->getName().c_str(), k, pushType);
            queue.Push(new ActionBasket(action, k, skipPrerequisites, event));
            pushed = true;

            continue;
        }

        delete action;

    }

    return pushed;
}

ActionResult Engine::ExecuteAction(std::string const name, Event event, std::string const qualifier)
{
    bool result = false;

    ActionNode* actionNode = CreateActionNode(name);
    if (!actionNode)
        return ACTION_RESULT_UNKNOWN;

    Action* action = InitializeAction(actionNode);
    if (!action)
    {
        delete actionNode;
        return ACTION_RESULT_UNKNOWN;
    }

    if (!qualifier.empty())
    {
        if (Qualified* q = dynamic_cast<Qualified*>(action))
            q->Qualify(qualifier);
    }

    if (!action->isUseful())
    {
        delete actionNode;
        return ACTION_RESULT_USELESS;
    }

    if (!action->isPossible())
    {
        delete actionNode;
        return ACTION_RESULT_IMPOSSIBLE;
    }

    action->MakeVerbose();

    result = ListenAndExecute(action, event);
    MultiplyAndPush(action->getContinuers(), 0.0f, false, event, "default");

    delete actionNode;

    return result ? ACTION_RESULT_OK : ACTION_RESULT_FAILED;
}

void Engine::addStrategy(std::string const name, bool init)
{
    removeStrategy(name, init);

    if (Strategy* strategy = aiObjectContext->GetStrategy(name))
    {
        std::set<std::string> siblings = aiObjectContext->GetSiblingStrategy(name);
        for (std::set<std::string>::iterator i = siblings.begin(); i != siblings.end(); i++)
            removeStrategy(*i, init);

        LogAction("S:+%s", strategy->getName().c_str());
        strategies[strategy->getName()] = strategy;
    }
    if (init)
        Init();
}

void Engine::addStrategies(std::string first, ...)
{
    addStrategy(first, false);

    va_list vl;
    va_start(vl, first);

    char const* cur;
    do
    {
        cur = va_arg(vl, char const*);
        if (cur)
            addStrategy(cur, false);
    } while (cur);

    Init();

    va_end(vl);
}

void Engine::addStrategiesNoInit(std::string first, ...)
{
    addStrategy(first, false);

    va_list vl;
    va_start(vl, first);

    char const* cur;
    do
    {
        cur = va_arg(vl, char const*);
        if (cur)
            addStrategy(cur, false);
    } while (cur);

    va_end(vl);
}

bool Engine::removeStrategy(std::string const name, bool init)
{
    std::map<std::string, Strategy*>::iterator i = strategies.find(name);
    if (i == strategies.end())
        return false;

    LogAction("S:-%s", name.c_str());
    strategies.erase(i);
    if (init)
        Init();

    return true;
}

void Engine::removeAllStrategies()
{
    strategies.clear();
    Init();
}

void Engine::toggleStrategy(std::string const name)
{
    if (!removeStrategy(name))
        addStrategy(name);
}

bool Engine::HasStrategy(std::string const name) { return strategies.find(name) != strategies.end(); }

Strategy* Engine::GetStrategy(std::string const name)
{
    std::map<std::string, Strategy*>::iterator i = strategies.find(name);
    return i != strategies.end() ? i->second : nullptr;
}

void Engine::ProcessTriggers(bool minimal)
{
    std::unordered_map<Trigger*, Event> fires;
    uint32 now = getMSTime();
    // With no chat command pending every chat command trigger is unset: checking or resetting it is a no-op.
    bool const skipChatCommands = !aiObjectContext->HasPendingChatCommands();
    for (std::vector<TriggerNode*>::iterator i = triggers.begin(); i != triggers.end(); i++)
    {
        TriggerNode* node = *i;
        if (!node)
            continue;

        Trigger* trigger = node->getTrigger();
        if (!trigger)
        {
            trigger = aiObjectContext->GetTrigger(node->getName());
            node->setTrigger(trigger);
        }

        if (!trigger)
            continue;

        if (skipChatCommands && node->IsChatCommand())
            continue;

        if (fires.find(trigger) != fires.end())
            continue;

        if (testMode || trigger->needCheck(now))
        {
            if (minimal && node->getFirstRelevance() < 100)
                continue;

            PerfMonitorOperation* pmo =
                PerfMonitor::IsEnabled()
                    ? sPerfMonitor.start(PERF_MON_TRIGGER, trigger->getName(), &aiObjectContext->performanceStack)
                    : nullptr;
            Event event = trigger->Check();
            if (pmo)
                pmo->finish();

            if (!event)
                continue;

            if (trigger->IsBuffTrigger() && !trigger->IsDebuffTrigger())
                botAI->forceRebuff.NoteBuffProposed();

            fires[trigger] = event;
            LogAction("T:%s", trigger->getName().c_str());
        }
    }

    if (!fires.empty())
    {
        for (std::vector<TriggerNode*>::iterator i = triggers.begin(); i != triggers.end(); i++)
        {
            TriggerNode* node = *i;
            Trigger* trigger = node->getTrigger();
            if (fires.find(trigger) == fires.end())
                continue;

            Event event = fires[trigger];
            MultiplyAndPush(node->getHandlers(), 0.0f, false, event, "trigger");
        }
    }

    for (std::vector<TriggerNode*>::iterator i = triggers.begin(); i != triggers.end(); i++)
    {
        if (skipChatCommands && (*i)->IsChatCommand())
            continue;

        if (Trigger* trigger = (*i)->getTrigger())
            trigger->Reset();
    }
}

void Engine::PushDefaultActions()
{
    for (std::map<std::string, Strategy*>::iterator i = strategies.begin(); i != strategies.end(); i++)
    {
        Strategy* strategy = i->second;
        Event emptyEvent;
        MultiplyAndPush(strategy->getDefaultActions(), 0.0f, false, emptyEvent, "default");
    }
}

std::string const Engine::ListStrategies()
{
    std::string s = "Strategies: ";

    if (strategies.empty())
        return s;

    for (std::map<std::string, Strategy*>::iterator i = strategies.begin(); i != strategies.end(); i++)
    {
        s.append(i->first);
        s.append(", ");
    }

    return s.substr(0, s.length() - 2);
}

std::vector<std::string> Engine::GetStrategies()
{
    std::vector<std::string> result;
    for (std::map<std::string, Strategy*>::iterator i = strategies.begin(); i != strategies.end(); i++)
    {
        result.push_back(i->first);
    }

    return result;
}

void Engine::PushAgain(ActionNode* actionNode, float relevance, Event event)
{
    std::vector<NextAction> nextAction = { NextAction(actionNode->getName(), relevance) };

    MultiplyAndPush(nextAction, relevance, true, event, "again");

    delete actionNode;
}

bool Engine::ContainsStrategy(StrategyType type)
{
    for (std::map<std::string, Strategy*>::iterator i = strategies.begin(); i != strategies.end(); i++)
    {
        if (i->second->GetType() & type)
            return true;
    }
    return false;
}

Action* Engine::InitializeAction(ActionNode* actionNode)
{
    Action* action = actionNode->getAction();
    if (!action)
    {
        action = aiObjectContext->GetAction(actionNode->getName());
        actionNode->setAction(action);
    }

    if (action)
    {
        action->SetReaction(false);
        // Clear duration leftovers so ListenAndExecute doesn't re-apply a stale delay
        action->ResetDuration();
    }

    return action;
}

bool Engine::ListenAndExecute(Action* action, Event event)
{
    bool actionExecuted = false;

    if (action == nullptr)
    {
        LOG_ERROR("playerbots", "Action is nullptr");

        return actionExecuted;
    }

    // Only bots in a PvP test match count executed casts of the skill categories (the match record)
    ObjectGuid countTarget;
    bool const counting = botAI->IsPvpTestCounting();
    // The row of everything a counting bot executes (PerfMonitor on): the base the cost of the counting is read against
    PerfMonitorOperation* countingPmo = counting ? sPerfMonitor.start(PERF_MON_TOTAL, "Counted bot actions") : nullptr;
    uint8 const countCategory = counting ? botAI->BeginExecutedCount(action, countTarget) : 0;

    if (actionExecutionListeners.Before(action, event))
    {
        // Money and items that move inside the action carry its name in the world log's event rows
        EventLog::SourceScope const source(action);
        actionExecuted = actionExecutionListeners.AllowExecution(action, event) ? action->Execute(event) : true;
    }

    if (botAI->HasStrategy("debug", BOT_STATE_NON_COMBAT))
    {
        std::ostringstream out;
        out << "do: ";
        out << action->getName();

        if (actionExecuted)
            out << " 1 (";
        else
            out << " 0 (";

        out << action->getRelevance() << ")";

        if (!event.GetSource().empty())
            out << " [" << event.GetSource() << "]";

        botAI->TellMasterNoFacing(out);
    }

    actionExecuted = actionExecutionListeners.OverrideResult(action, actionExecuted, event);
    actionExecutionListeners.After(action, actionExecuted, event);
    if (TestProbe::On(botAI->testConstraints))
        TestProbe::Act(botAI, action->getName(), actionExecuted);

    // The world log's recorder: what the non-combat and dead engines executed (the combat engine's casts would push
    // the useful entries out of the ring in seconds)
    if (WorldLog::Active() && this == botAI->GetCurrentEngine() && botAI->GetState() != BOT_STATE_COMBAT)
        WorldLog::NoteAction(botAI->GetBot(), botAI->worldLog, action, EventSourceAccess::Of(event), actionExecuted);

    if (countCategory && actionExecuted)
        botAI->EndExecutedCount(countCategory, countTarget);

    if (countingPmo)
        countingPmo->finish();

    if (actionExecuted && action->GetDuration() > 0)
        botAI->SetActionDuration(action);

    return actionExecuted;
}

void Engine::LogAction(char const* format, ...)
{
    Player* bot = botAI->GetBot();
    if (sPlayerbotAIConfig.LogInGroupOnly && (!bot->GetGroup() || !botAI->HasGameClientMaster()) && !testMode)
        return;

    char buf[1024];

    va_list ap;
    va_start(ap, format);
    vsnprintf(buf, sizeof(buf), format, ap);
    va_end(ap);

    lastAction += "|";
    lastAction += buf;
    if (lastAction.size() > 512)
    {
        lastAction = lastAction.substr(512);
        size_t pos = lastAction.find("|");
        lastAction = (pos == std::string::npos ? "" : lastAction.substr(pos));
    }

    if (testMode)
    {
        FILE* file = fopen("test.log", "a");
        fprintf(file, "'%s'", buf);
        fprintf(file, "\n");
        fclose(file);
    }
    else
    {
        LOG_DEBUG("playerbots", "{} {}", bot->GetName().c_str(), buf);
    }
}

std::string const Engine::ResolveStrategyName(std::string const name)
{
    if (HasStrategy(name))
        return name;

    Strategy* strategy = aiObjectContext->GetStrategy(name);
    return strategy ? strategy->getName() : name;
}

void Engine::ChangeStrategy(std::string const names)
{
    std::vector<std::string> splitted = split(names, ',');
    for (std::vector<std::string>::iterator i = splitted.begin(); i != splitted.end(); i++)
    {
        char const* name = i->c_str();
        switch (name[0])
        {
            case '+':
                addStrategy(name + 1);
                break;
            case '-':
                removeStrategy(ResolveStrategyName(name + 1));
                break;
            case '~':
                toggleStrategy(ResolveStrategyName(name + 1));
                break;
            case '?':
                botAI->TellMaster(ListStrategies());
                break;
        }
    }
}

void Engine::LogValues()
{
    if (testMode)
        return;

    Player* bot = botAI->GetBot();
    if (sPlayerbotAIConfig.LogInGroupOnly && (!bot->GetGroup() || !botAI->HasGameClientMaster()))
        return;

    std::string const text = botAI->GetAiObjectContext()->FormatValues();
    LOG_DEBUG("playerbots", "Values for {}: {}", bot->GetName().c_str(), text.c_str());
}

// The test probe (TestProbe.h): counts per test-constrained bot under one lock, only test bots reach it
namespace
{
struct ProbeActs
{
    uint32 ok = 0;
    uint32 fail = 0;
};

struct ProbeBot
{
    uint32 lineMs = 0;
    uint32 updates = 0;
    uint32 internals = 0;
    uint32 passes = 0;
    std::unordered_map<std::string, ProbeActs> acts;
};

constexpr uint32 PROBE_LINE_MS = 30 * IN_MILLISECONDS;
std::mutex probeLock;
std::unordered_map<uint64, ProbeBot> probeBots;

ProbeBot& ProbeOf(PlayerbotAI* botAI) { return probeBots[botAI->GetBot()->GetGUID().GetRawValue()]; }
}  // namespace

void TestProbe::Update(PlayerbotAI* botAI, uint32 delayMs)
{
    Player* bot = botAI->GetBot();
    if (!bot)
        return;

    std::string acts;
    ProbeBot counts;
    {
        std::lock_guard<std::mutex> lock(probeLock);
        ProbeBot& probe = ProbeOf(botAI);
        ++probe.updates;
        uint32 const nowMs = getMSTime();
        if (!probe.lineMs)
            probe.lineMs = nowMs;
        if (getMSTimeDiff(probe.lineMs, nowMs) < PROBE_LINE_MS)
            return;

        std::vector<std::pair<std::string, ProbeActs>> top(probe.acts.begin(), probe.acts.end());
        size_t const shown = std::min<size_t>(3, top.size());
        auto const busier = [](auto const& a, auto const& b)
        { return a.second.ok + a.second.fail > b.second.ok + b.second.fail; };
        std::partial_sort(top.begin(), top.begin() + shown, top.end(), busier);
        for (size_t i = 0; i < shown; ++i)
            acts += Acore::StringFormat(" {}:{}/{}", top[i].first, top[i].second.ok, top[i].second.fail);
        counts.updates = probe.updates;
        counts.internals = probe.internals;
        counts.passes = probe.passes;
        probe = ProbeBot();
        probe.lineMs = nowMs;
    }
    LOG_INFO("playerbots",
             "[Probe] {} upd {} int {} pass {} state {} combat {} rpg {} delay {} newrpg {} alive {} rows {} acts{}",
             bot->GetName(), counts.updates, counts.internals, counts.passes, static_cast<uint32>(botAI->GetState()),
             bot->IsInCombat() ? 1 : 0, static_cast<uint32>(botAI->rpgInfo.GetStatus()), delayMs,
             botAI->HasStrategy("new rpg", BOT_STATE_NON_COMBAT) ? 1 : 0, bot->IsAlive() ? 1 : 0,
             botAI->testConstraints, acts.empty() ? " none" : acts);
}

void TestProbe::Internal(PlayerbotAI* botAI)
{
    std::lock_guard<std::mutex> lock(probeLock);
    ++ProbeOf(botAI).internals;
}

void TestProbe::Pass(PlayerbotAI* botAI)
{
    std::lock_guard<std::mutex> lock(probeLock);
    ++ProbeOf(botAI).passes;
}

void TestProbe::Act(PlayerbotAI* botAI, std::string const& action, bool executed)
{
    std::lock_guard<std::mutex> lock(probeLock);
    ProbeActs& acts = ProbeOf(botAI).acts[action];
    ++(executed ? acts.ok : acts.fail);
}
