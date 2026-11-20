/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "FollowMasterStrategy.h"

std::vector<NextAction> FollowMasterStrategy::getDefaultActions()
{
    return {
        NextAction("follow", 1.0f)
    };
}

void FollowMasterStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    // The follower's step at its master's vessel ahead of the idle actions, checked at every check (WT70d-b): a lift
    // stands a few seconds, the follow action (relevance 1) runs only when nothing else does
    triggers.push_back(new TriggerNode("follow aboard", {NextAction("follow aboard", ACTION_MOVE)}));
}
