/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "Queue.h"

#include "Log.h"
#include "PlayerbotAIConfig.h"
#include "SpecTable.h"

void Queue::Push(ActionBasket* action)
{
    if (!action)
    {
        return;
    }

    for (ActionBasket* basket : actions)
    {
        if (action->getAction()->getName() == basket->getAction()->getName())
        {
            updateExistingBasket(basket, action);
            return;
        }
    }

    actions.push_back(action);
}

ActionNode* Queue::Pop()
{
    ActionBasket* highestRelevanceBasket = findHighestRelevanceBasket();
    if (!highestRelevanceBasket)
    {
        return nullptr;
    }

    return extractAndDeleteBasket(highestRelevanceBasket);
}

ActionBasket* Queue::Peek()
{
    return findHighestRelevanceBasket();
}

uint32 Queue::Size()
{
    return actions.size();
}

void Queue::RemoveExpired()
{
    uint32 const expiryTime = sPlayerbotAIConfig.ExpireActionTime;
    for (std::list<ActionBasket*>::iterator i = actions.begin(); i != actions.end();)
    {
        ActionBasket* basket = *i;
        spec::BasketRelevance relevance{basket->getRelevance(), basket->getKept()};
        if (spec::EndOfTick(relevance) && !(expiryTime && basket->isExpired(expiryTime)))
        {
            basket->setRelevance(relevance.relevance);
            ++i;
            continue;
        }

        i = actions.erase(i);
        delete basket->getAction();
        delete basket;
    }
}

// Private helper methods
void Queue::updateExistingBasket(ActionBasket* existing, ActionBasket* newBasket)
{
    spec::BasketRelevance merged{existing->getRelevance(), existing->getKept()};
    spec::Merge(merged, {newBasket->getRelevance(), newBasket->getKept()});
    existing->setRelevance(merged.relevance);
    existing->setKept(merged.kept);

    if (ActionNode* actionNode = newBasket->getAction())
    {
        delete actionNode;
    }

    delete newBasket;
}

ActionBasket* Queue::findHighestRelevanceBasket() const
{
    if (actions.empty())
    {
        return nullptr;
    }

    float maxRelevance = -1.0f;
    ActionBasket* selection = nullptr;

    for (ActionBasket* basket : actions)
    {
        if (!basket)
        {
            continue;
        }

        if (basket->getRelevance() > maxRelevance)
        {
            maxRelevance = basket->getRelevance();
            selection = basket;
        }
    }

    return selection;
}

ActionNode* Queue::extractAndDeleteBasket(ActionBasket* basket)
{
    ActionNode* action = basket->getAction();
    actions.remove(basket);
    delete basket;
    return action;
}
