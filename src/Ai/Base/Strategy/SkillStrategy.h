/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_SKILLSTRATEGY_H
#define PLAYERBOTS_SKILLSTRATEGY_H

#include <array>
#include <string>
#include <unordered_map>

#include "Multiplier.h"
#include "NaturalReaction.h"
#include "Strategy.h"

class Action;
class PlayerbotAI;
class Unit;

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
    TargetSwitch,
    // A reaction only (G0h3-b: avoid aoe, Playerbots.Natural.Reaction.Movement.Enable): no notice counter, no D4 delay
    Movement
};

char const* SkillNoticeCategoryName(SkillNoticeCategory category);
// The category of an action by its name (None for an action the skill strategy leaves alone)
SkillNoticeCategory SkillNoticeCategoryOf(std::string const& actionName);

// Returns 0 for an action of a notice category until the bot has noticed it in the current window. The notice time
// and the missed windows come from a hash of guid, category and window, so the result is stable within a window and
// costs no roll per tick. With Playerbots.Natural.Reaction.Enable (in its Contexts) the delay is a human reaction time
// from the event's start instead (NaturalReaction.h): one event per category, the bot's focus, a view-cone penalty.
class SkillMultiplier : public Multiplier
{
public:
    SkillMultiplier(PlayerbotAI* botAI) : Multiplier(botAI, "skill") {}

    float GetValue(Action* action) override;

private:
    // An action's notice category and the ease of its button (G0h3), resolved once per action and bot
    struct ActionClass
    {
        SkillNoticeCategory Category = SkillNoticeCategory::None;
        ReactionEase Ease = ReactionEase::Common;
    };

    ActionClass const& GetClass(Action* action);
    bool IsTargetSwitch(Action* action, Unit*& target);
    bool HasNoticed(SkillNoticeCategory category) const;
    bool HasReacted(SkillNoticeCategory category, ReactionEase ease, Action* action, Unit* target);
    // G0h3's inputs of an event at its start, from values the bot has already built
    ReactionEventInputs EventInputs(SkillNoticeCategory category, ReactionEase ease, Unit* target, uint32 nowMs);
    // G0h3-b at an event's start: the bot's own cast within it, and for a kick a stop for it or the busy skip
    void MeetBusy(ReactionEvent& event, SkillNoticeCategory category, Action* action, Unit* target, uint32 nowMs,
                  bool starting);

    // Per bot: the actions of a bot's context live as long as the bot, so the pointer is a stable key
    std::unordered_map<Action*, ActionClass> categories;
    // Natural reaction, per bot (touched only by the bot's own update): the current event per category, whether that
    // event's context uses the natural path, and the bot's focus
    static constexpr size_t ReactionCategories = static_cast<size_t>(SkillNoticeCategory::Movement);
    std::array<ReactionEvent, ReactionCategories> reactionEvents{};
    std::array<bool, ReactionCategories> reactionNatural{};
    ReactionFocus reactionFocus;
};

class SkillStrategy : public Strategy
{
public:
    SkillStrategy(PlayerbotAI* botAI) : Strategy(botAI) {}

    void InitMultipliers(std::vector<Multiplier*>& multipliers) override;
    std::string const getName() override { return "skill"; }
};

#endif
