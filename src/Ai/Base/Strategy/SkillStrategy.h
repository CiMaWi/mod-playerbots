/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_SKILLSTRATEGY_H
#define PLAYERBOTS_SKILLSTRATEGY_H

#include <unordered_map>

#include "Multiplier.h"
#include "Strategy.h"

class Action;
class PlayerbotAI;

// Skill knob 2, noticing (Playerbots.Skill.Notice*). The categories the hidden skill slows down; the order matches the
// counters in PlayerbotAI::SkillNoticeCounters and the names in SkillNoticeCategoryName.
enum class SkillNoticeCategory : uint8
{
    None = 0,
    Interrupt,
    Defensive,
    Dispel,
    CrowdControl,
    CrowdControlBreak,
    TargetSwitch
};

char const* SkillNoticeCategoryName(SkillNoticeCategory category);

// Returns 0 for an action of a notice category until the bot has noticed it in the current window. The notice time
// and the missed windows come from a hash of guid, category and window, so the result is stable within a window and
// costs no roll per tick.
class SkillMultiplier : public Multiplier
{
public:
    SkillMultiplier(PlayerbotAI* botAI) : Multiplier(botAI, "skill") {}

    float GetValue(Action* action) override;

private:
    SkillNoticeCategory GetCategory(Action* action);
    bool IsTargetSwitch(Action* action);
    bool HasNoticed(SkillNoticeCategory category) const;

    // Per bot: the actions of a bot's context live as long as the bot, so the pointer is a stable key
    std::unordered_map<Action*, SkillNoticeCategory> categories;
};

class SkillStrategy : public Strategy
{
public:
    SkillStrategy(PlayerbotAI* botAI) : Strategy(botAI) {}

    void InitMultipliers(std::vector<Multiplier*>& multipliers) override;
    std::string const getName() override { return "skill"; }
};

#endif
