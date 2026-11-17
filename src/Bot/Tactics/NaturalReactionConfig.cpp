/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "NaturalReactionConfig.h"

#include <algorithm>
#include <array>
#include <string>

#include "ConfigTable.h"
#include "TacticScope.h"

namespace
{
using ReactionRow = ConfigRow<ReactionParams>;
using ChoiceRow = ConfigRow<ReactionChoiceParams>;
using LoadRow = ConfigRow<ReactionLoadParams>;
using EaseRow = ConfigRow<ReactionEaseParams>;
using ThinkRow = ConfigRow<ReactionThinkParams>;
using AnticipateRow = ConfigRow<ReactionAnticipateParams>;
using QueueRow = ConfigRow<ReactionQueueParams>;
using ReevalRow = ConfigRow<ReactionReevalParams>;
using MemoryRow = ConfigRow<ReactionMemoryParams>;

// The caps of the old reads: every time in ms 0 to 10 s, a percentage 0 to 100
constexpr uint32 MaxMs = 10000;
constexpr uint32 MaxPercent = 100;

// "Arena,Battleground,World,Pve", All or None into the mask; the text stays in ContextsText
bool ParseReactionContexts(std::string const& text, ReactionParams& out)
{
    std::string error;
    return ParseTacticScope(text, out.Contexts, error);
}

// Behind and switch costs: Max at least Min (the old reads raised Max silently)
void FinishReaction(ReactionParams& out)
{
    out.BehindMaxMs = std::max(out.BehindMinMs, out.BehindMaxMs);
    out.SwitchMaxMs = std::max(out.SwitchMinMs, out.SwitchMaxMs);
}

constexpr std::array<ReactionRow, 16> ReactionRows = {{
    {"MedianMsAtSkill1", &ReactionParams::MedianMsAtSkill1, 50.0f, 10000.0f, ConfigKind::TuningListed},
    {"MedianMsAtSkill99", &ReactionParams::MedianMsAtSkill99, 50.0f, 10000.0f, ConfigKind::TuningListed},
    {"SigmaAtSkill1", &ReactionParams::SigmaAtSkill1, 0.0f, 2.0f, ConfigKind::TuningListed},
    {"SigmaAtSkill99", &ReactionParams::SigmaAtSkill99, 0.0f, 2.0f, ConfigKind::TuningListed},
    {"MinMs", &ReactionParams::MinMs, 0, MaxMs, ConfigKind::TuningListed},
    {"BehindPenaltyMinMs", &ReactionParams::BehindMinMs, 0, MaxMs, ConfigKind::TuningListed},
    {"BehindPenaltyMaxMs", &ReactionParams::BehindMaxMs, 0, MaxMs, ConfigKind::TuningListed},
    {"SwitchCostMinMs", &ReactionParams::SwitchMinMs, 0, MaxMs, ConfigKind::TuningListed},
    {"SwitchCostMaxMs", &ReactionParams::SwitchMaxMs, 0, MaxMs, ConfigKind::TuningListed},
    {"ConeDegrees", &ReactionParams::ConeDegrees, 10.0f, 360.0f, ConfigKind::TuningListed},
    {"FocusTimeoutMs", &ReactionParams::FocusTimeoutMs, 0, MaxMs, ConfigKind::TuningListed},
    {"PingMs", &ReactionParams::PingMs, 0, MaxMs, ConfigKind::TuningListed},
    {"DisplayMs", &ReactionParams::DisplayMs, 0, MaxMs, ConfigKind::TuningListed},
    {"ServerTickMs", &ReactionParams::ServerTickMs, -1, 1000, ConfigKind::TuningListed},
    {"Positioning.Enable", &ReactionParams::PositioningEnable, 0, 0, ConfigKind::TuningListed},
    {"Contexts", &ReactionParams::ContextsText, 0, 0, ConfigKind::TuningListed, ConfigOutOfRange::Clamp,
     &ParseReactionContexts},
}};

constexpr std::array<ChoiceRow, 3> ChoiceRows = {{
    {"MsPerDoublingAtSkill1", &ReactionChoiceParams::MsPerDoublingAt1, 0.0f, 2000.0f},
    {"MsPerDoublingAtSkill99", &ReactionChoiceParams::MsPerDoublingAt99, 0.0f, 2000.0f},
    {"MaxOptions", &ReactionChoiceParams::MaxOptions, 1, 64},
}};

constexpr std::array<LoadRow, 7> LoadRows = {{
    {"MsPerEventAtSkill1", &ReactionLoadParams::MsPerEventAt1, 0, MaxMs},
    {"MsPerEventAtSkill99", &ReactionLoadParams::MsPerEventAt99, 0, MaxMs},
    {"MissPercentPerEventAtSkill1", &ReactionLoadParams::MissPercentPerEventAt1, 0, MaxPercent},
    {"MissPercentPerEventAtSkill99", &ReactionLoadParams::MissPercentPerEventAt99, 0, MaxPercent},
    {"TopProtectAtSkill1", &ReactionLoadParams::TopProtectAt1, 0.0f, 1.0f},
    {"TopProtectAtSkill99", &ReactionLoadParams::TopProtectAt99, 0.0f, 1.0f},
    {"MaxEvents", &ReactionLoadParams::MaxEvents, 1, 5},
}};

uint32_t& EaseAt1(ReactionEaseParams& out, uint32_t index) { return out.MsAt1[index]; }
uint32_t& EaseAt99(ReactionEaseParams& out, uint32_t index) { return out.MsAt99[index]; }

// Strictly rising at both ends, so at every Dexterity (the old reads raised a value silently)
void FinishEase(ReactionEaseParams& out)
{
    for (uint32 index = 1; index < REACTION_EASE_COUNT; ++index)
    {
        out.MsAt1[index] = std::max(out.MsAt1[index], out.MsAt1[index - 1] + 1);
        out.MsAt99[index] = std::max(out.MsAt99[index], out.MsAt99[index - 1] + 1);
    }
}

using EaseAt = ConfigElement<ReactionEaseParams, uint32_t>;

// One row per ease and end, the names of ReactionEase (Common, Rare, Focus, NewTarget)
constexpr std::array<EaseRow, 9> EaseRows = {{
    {"CommonMsAtSkill1", EaseAt{&EaseAt1, 0}, 0, MaxMs},
    {"CommonMsAtSkill99", EaseAt{&EaseAt99, 0}, 0, MaxMs},
    {"RareMsAtSkill1", EaseAt{&EaseAt1, 1}, 0, MaxMs},
    {"RareMsAtSkill99", EaseAt{&EaseAt99, 1}, 0, MaxMs},
    {"FocusMsAtSkill1", EaseAt{&EaseAt1, 2}, 0, MaxMs},
    {"FocusMsAtSkill99", EaseAt{&EaseAt99, 2}, 0, MaxMs},
    {"NewTargetMsAtSkill1", EaseAt{&EaseAt1, 3}, 0, MaxMs},
    {"NewTargetMsAtSkill99", EaseAt{&EaseAt99, 3}, 0, MaxMs},
    {"FocusDexterity", &ReactionEaseParams::FocusDexterity, 1, 99, ConfigKind::TuningListed},
}};

constexpr std::array<ThinkRow, 8> ThinkRows = {{
    {"ShortcutMsAtSkill1", &ReactionThinkParams::ShortcutMsAt1, 0, MaxMs},
    {"ShortcutMsAtSkill99", &ReactionThinkParams::ShortcutMsAt99, 0, MaxMs},
    {"DeepMsAtSkill1", &ReactionThinkParams::DeepMsAt1, 0, MaxMs},
    {"DeepMsAtSkill99", &ReactionThinkParams::DeepMsAt99, 0, MaxMs},
    {"DeepPercentAtSkill1", &ReactionThinkParams::DeepPercentAt1, 0, MaxPercent},
    {"DeepPercentAtSkill99", &ReactionThinkParams::DeepPercentAt99, 0, MaxPercent},
    {"PatienceWaitMsAt0", &ReactionThinkParams::PatienceWaitMsAt0, 0, MaxMs},
    {"PatienceWaitMsAt100", &ReactionThinkParams::PatienceWaitMsAt100, 0, MaxMs},
}};

constexpr std::array<AnticipateRow, 2> AnticipateRows = {{
    {"ShareAtSkill1", &ReactionAnticipateParams::ShareAt1, 0.0f, 0.95f},
    {"ShareAtSkill99", &ReactionAnticipateParams::ShareAt99, 0.0f, 0.95f},
}};

// Max at least Min at both ends (the old reads raised Max silently)
void FinishQueue(ReactionQueueParams& out)
{
    out.MaxMsAt1 = std::max(out.MinMsAt1, out.MaxMsAt1);
    out.MaxMsAt99 = std::max(out.MinMsAt99, out.MaxMsAt99);
}

constexpr std::array<QueueRow, 4> QueueRows = {{
    {"MinMsAtSkill1", &ReactionQueueParams::MinMsAt1, 0, MaxMs},
    {"MaxMsAtSkill1", &ReactionQueueParams::MaxMsAt1, 0, MaxMs},
    {"MinMsAtSkill99", &ReactionQueueParams::MinMsAt99, 0, MaxMs},
    {"MaxMsAtSkill99", &ReactionQueueParams::MaxMsAt99, 0, MaxMs},
}};

constexpr std::array<ReevalRow, 4> ReevalRows = {{
    {"Enable", &ReactionReevalParams::Enable, 0, 0, ConfigKind::NotYet},
    {"ImportantCastMs", &ReactionReevalParams::ImportantCastMs, 0, MaxMs},
    {"MinOwnRemainingMs", &ReactionReevalParams::MinOwnRemainingMs, 0, MaxMs},
    {"BusyMarginMs", &ReactionReevalParams::BusyMarginMs, 0, MaxMs},
}};

constexpr std::array<MemoryRow, 2> MemoryRows = {{
    {"MinCasts", &ReactionMemoryParams::MinCasts, 1, 100},
    {"EndToleranceMs", &ReactionMemoryParams::EndToleranceMs, 0, MaxMs},
}};
}  // namespace

ConfigFamily<ReactionParams> const ReactionFamily{"Playerbots.Natural.Reaction.", ReactionRows, &FinishReaction};
ConfigFamily<ReactionChoiceParams> const ReactionChoiceFamily{"Playerbots.Natural.Reaction.Choice.", ChoiceRows};
ConfigFamily<ReactionLoadParams> const ReactionLoadFamily{"Playerbots.Natural.Reaction.Load.", LoadRows};
ConfigFamily<ReactionEaseParams> const ReactionEaseFamily{"Playerbots.Natural.Reaction.Ease.", EaseRows, &FinishEase};
ConfigFamily<ReactionThinkParams> const ReactionThinkFamily{"Playerbots.Natural.Reaction.Think.", ThinkRows};
ConfigFamily<ReactionAnticipateParams> const ReactionAnticipateFamily{"Playerbots.Natural.Reaction.Anticipate.",
                                                                      AnticipateRows};
ConfigFamily<ReactionQueueParams> const ReactionQueueFamily{"Playerbots.Natural.Reaction.Queue.", QueueRows,
                                                            &FinishQueue};
ConfigFamily<ReactionReevalParams> const ReactionReevalFamily{"Playerbots.Natural.Reaction.Reeval.", ReevalRows};
ConfigFamily<ReactionMemoryParams> const ReactionMemoryFamily{"Playerbots.Natural.Reaction.Memory.", MemoryRows};
