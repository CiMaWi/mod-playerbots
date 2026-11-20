/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "AiObjectContext.h"
#include "DKAiObjectContext.h"
#include "DruidAiObjectContext.h"
#include "Helpers.h"
#include "HunterAiObjectContext.h"
#include "MageAiObjectContext.h"
#include "PaladinAiObjectContext.h"
#include "PriestAiObjectContext.h"
#include "RogueAiObjectContext.h"
#include "ShamanAiObjectContext.h"
#include "WarlockAiObjectContext.h"
#include "WarriorAiObjectContext.h"
#include <mutex>

SharedNamedObjectContextList<Strategy> AiObjectContext::sharedStrategyContexts;
SharedNamedObjectContextList<Action> AiObjectContext::sharedActionContexts;
SharedNamedObjectContextList<Trigger> AiObjectContext::sharedTriggerContexts;
SharedNamedObjectContextList<UntypedValue> AiObjectContext::sharedValueContexts;

AiObjectContext::AiObjectContext(PlayerbotAI* botAI, SharedNamedObjectContextList<Strategy>& sharedStrategyContext,
                                 SharedNamedObjectContextList<Action>& sharedActionContext,
                                 SharedNamedObjectContextList<Trigger>& sharedTriggerContext,
                                 SharedNamedObjectContextList<UntypedValue>& sharedValueContext)
    : PlayerbotAIAware(botAI),
      strategyContexts(sharedStrategyContext),
      actionContexts(sharedActionContext),
      triggerContexts(sharedTriggerContext),
      valueContexts(sharedValueContext)
{
}

void AiObjectContext::BuildAllSharedContexts()
{
    // Contexts only map names to compile-time creators, so a reload has nothing to refresh.
    static std::once_flag built;
    std::call_once(
        built,
        []()
        {
            AiObjectContext::BuildSharedContexts();
            PriestAiObjectContext::BuildSharedContexts();
            MageAiObjectContext::BuildSharedContexts();
            WarlockAiObjectContext::BuildSharedContexts();
            WarriorAiObjectContext::BuildSharedContexts();
            ShamanAiObjectContext::BuildSharedContexts();
            PaladinAiObjectContext::BuildSharedContexts();
            DruidAiObjectContext::BuildSharedContexts();
            HunterAiObjectContext::BuildSharedContexts();
            RogueAiObjectContext::BuildSharedContexts();
            DKAiObjectContext::BuildSharedContexts();
        });
}

void AiObjectContext::BuildSharedContexts()
{
    BuildSharedStrategyContexts(sharedStrategyContexts);
    BuildSharedActionContexts(sharedActionContexts);
    BuildSharedTriggerContexts(sharedTriggerContexts);
    BuildSharedValueContexts(sharedValueContexts);
}

std::vector<std::string> AiObjectContext::Save()
{
    std::vector<std::string> result;

    std::set<std::string> names = valueContexts.GetCreated();
    for (std::set<std::string>::iterator i = names.begin(); i != names.end(); ++i)
    {
        UntypedValue* value = GetUntypedValue(*i);
        if (!value)
            continue;

        std::string const data = value->Save();
        if (data == "?")
            continue;

        std::string const name = *i;
        std::ostringstream out;
        out << name;

        out << ">" << data;
        result.push_back(out.str());
    }

    return result;
}

void AiObjectContext::Load(std::vector<std::string> data)
{
    for (std::vector<std::string>::iterator i = data.begin(); i != data.end(); ++i)
    {
        std::string const row = *i;
        std::vector<std::string> parts = split(row, '>');
        if (parts.size() != 2)
            continue;

        std::string const name = parts[0];
        std::string const text = parts[1];

        UntypedValue* value = GetUntypedValue(name);
        if (!value)
            continue;

        value->Load(text);
    }
}

Strategy* AiObjectContext::GetStrategy(std::string const name)
{
    return strategyContexts.GetContextObject(name, botAI);
}

std::set<std::string> AiObjectContext::GetSiblingStrategy(std::string const name)
{
    return strategyContexts.GetSiblings(name);
}

Trigger* AiObjectContext::GetTrigger(std::string const name) { return triggerContexts.GetContextObject(name, botAI); }

Action* AiObjectContext::GetAction(std::string const name) { return actionContexts.GetContextObject(name, botAI); }

UntypedValue* AiObjectContext::GetUntypedValue(std::string const name)
{
    return valueContexts.GetContextObject(name, botAI);
}

std::set<std::string> AiObjectContext::GetValues() { return valueContexts.GetCreated(); }

std::set<std::string> AiObjectContext::GetSupportedStrategies() { return strategyContexts.supports(); }

std::set<std::string> AiObjectContext::GetSupportedActions() { return actionContexts.supports(); }

std::set<std::string> AiObjectContext::GetSupportedTriggers() { return triggerContexts.supports(); }

namespace
{
template <class List>
bool HasCreator(List const& list, std::string const& name)
{
    return list.creators.find(name.substr(0, name.find("::"))) != list.creators.end();
}
}  // namespace

namespace
{
template <class T>
bool SameCreator(std::function<T*(PlayerbotAI*)> const& a, std::function<T*(PlayerbotAI*)> const& b)
{
    if (a.target_type() != b.target_type())
        return false;
    using Function = T* (*)(PlayerbotAI*);
    Function const* pa = a.template target<Function>();
    Function const* pb = b.template target<Function>();
    return pa && pb ? *pa == *pb : true;  // a lambda of the same type is the same lambda
}

template <class ClassList, class BaseList>
bool IsGeneral(ClassList const& classList, BaseList const& baseList, std::string const& name)
{
    std::string const base = name.substr(0, name.find("::"));
    auto const classIt = classList.creators.find(base);
    auto const baseIt = baseList.creators.find(base);
    return classIt != classList.creators.end() && baseIt != baseList.creators.end() &&
           SameCreator(classIt->second, baseIt->second);
}
}  // namespace

bool AiObjectContext::IsGeneralStrategy(std::string const& name) const
{
    return IsGeneral(strategyContexts, sharedStrategyContexts, name);
}

bool AiObjectContext::IsGeneralTrigger(std::string const& name) const
{
    return IsGeneral(triggerContexts, sharedTriggerContexts, name);
}

bool AiObjectContext::IsGeneralAction(std::string const& name) const
{
    return IsGeneral(actionContexts, sharedActionContexts, name);
}

bool AiObjectContext::HasStrategy(std::string const& name) const { return HasCreator(strategyContexts, name); }

bool AiObjectContext::HasTrigger(std::string const& name) const { return HasCreator(triggerContexts, name); }

bool AiObjectContext::HasAction(std::string const& name) const { return HasCreator(actionContexts, name); }

std::string const AiObjectContext::FormatValues()
{
    std::ostringstream out;
    std::set<std::string> names = valueContexts.GetCreated();
    for (std::set<std::string>::iterator i = names.begin(); i != names.end(); ++i, out << "|")
    {
        UntypedValue* value = GetUntypedValue(*i);
        if (!value)
            continue;

        std::string const text = value->Format();
        if (text == "?")
            continue;

        out << "{" << *i << "=" << text << "}";
    }

    return out.str();
}
