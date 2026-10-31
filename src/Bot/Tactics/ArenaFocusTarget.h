/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_ARENAFOCUSTARGET_H
#define _PLAYERBOT_ARENAFOCUSTARGET_H

#include <algorithm>
#include <array>
#include <cmath>
#include <string>

#include "ArenaInterrupt.h"
#include "Common.h"
#include "NaturalReaction.h"
#include "SplitMix64.h"
#include "TacticGate.h"

// G4e: the focus target, the pure parts. A bot whose Dexterity reaches Natural.Reaction.Ease.FocusDexterity keeps a
// second enemy (in an arena the moon, else the enemy healer or the CC target) and kicks or controls it at the focus
// ease while its own target stays; below the threshold the same action is priced as a new target (G0h3), as before.
// The game side is PlayerbotAI.cpp's FollowFocusTarget; the state is FocusState.h's FocusTargetState.

enum class FocusSource : uint8
{
    None = 0,
    Moon,
    Healer,
    Cc
};

inline char const* FocusSourceName(FocusSource source)
{
    switch (source)
    {
        case FocusSource::Moon:
            return "moon";
        case FocusSource::Healer:
            return "healer";
        case FocusSource::Cc:
            return "cc";
        default:
            return "none";
    }
}

constexpr uint32 FOCUS_TARGET_PICK_MS = 500;  // the picker's healer scan at most this often, else on a change
constexpr uint64 FOCUS_MOON_SALT = 0x6A09E667F3BCC909ULL;

// The Dexterity gate (skill 0, the skill system off, counts as skill 99)
inline bool FocusTargetCapable(uint8 skill, uint8 dexterity, uint8 focusDexterity)
{
    return skill == 0 || dexterity >= focusDexterity;
}

// The chance the picker takes the plan's moon over the enemy healer, straight from Teamwork 1 to 99
inline float FocusMoonShare(uint8 teamwork, uint32 pctAt1, uint32 pctAt99)
{
    float const at1 = static_cast<float>(std::min<uint32>(pctAt1, 100));
    float const at99 = static_cast<float>(std::min<uint32>(pctAt99, 100));
    return (at1 + (at99 - at1) * SkillPosition(teamwork)) / 100.0f;
}

// One draw per bot, match and moon (the caller's hash), so the answer holds while the moon stays
inline bool FocusTakesMoon(float share, uint64 hash)
{
    return UnitFromHash(SplitMix64(hash ^ FOCUS_MOON_SALT)) < share;
}

// The candidates the bot sees now (each alive, attackable and not its own target)
struct FocusPickInput
{
    bool Moon = false;
    bool Healer = false;
    bool Cc = false;
    bool TakesMoon = true;  // FocusTakesMoon
};

// The moon when the Teamwork draw takes it or there is no healer, else the healer, else the CC target
inline FocusSource PickFocusSource(FocusPickInput const& input)
{
    if (input.Moon && (input.TakesMoon || !input.Healer))
        return FocusSource::Moon;

    if (input.Healer)
        return FocusSource::Healer;

    return input.Cc ? FocusSource::Cc : FocusSource::None;
}

// The ease of an action (SkillMultiplier::EventInputs): on an enemy other than the bot's target the focus ease when it
// is the focus target, else a new target's; on the own target or a friend the button's own
inline ReactionEase FocusActionEase(ReactionEase button, bool otherEnemy, bool onFocus)
{
    if (!otherEnemy || button == ReactionEase::NewTarget)
        return button;

    return onFocus ? ReactionEase::Focus : ReactionEase::NewTarget;
}

// An interrupt on the focus scores the bonus over the other casts of its kind (ArenaInterrupt::Score; a cast not worth
// a kick stays at 0)
inline uint32 FocusKickScore(uint32 score, bool onFocus, uint32 bonus)
{
    return score && onFocus ? score + bonus : score;
}

// What an action on an enemy is for the counters (raw guids, 0: none): on the own target nothing, on the focus a focus
// action, on another enemy a switch
enum class FocusAct : uint8
{
    None = 0,
    Focus,
    Switch
};

inline FocusAct ClassifyFocusAct(uint64 target, uint64 own, uint64 focus)
{
    if (target == 0 || target == own)
        return FocusAct::None;

    return target == focus ? FocusAct::Focus : FocusAct::Switch;
}

// PlayerbotAI::CastSpell selects the cast's target and restores the selection after the cast: the earlier one, or for
// a focus action none (the partner reads the bot's target, DT.12); else the cast's target stays selected, as before.
// A focus action restores it this way also when the cast fails (a failed cast returns early)
inline uint64 FocusSelectionAfterCast(uint64 before, uint64 castTarget, bool focusAct)
{
    if (before != 0)
        return before;

    return focusAct ? 0 : castTarget;
}

constexpr uint8 FOCUS_HEALERS_SEEN_MAX = 8;  // the enemies seen healing in a match (world PvP: the latest 8)

// The enemies seen healing this match (raw guids): one heal seen keeps the enemy a healer for the whole match, so a
// healer whose last casts were a dispel or a fear stays the focus; at the cap the oldest entry makes room
struct FocusHealersSeen
{
    std::array<uint64, FOCUS_HEALERS_SEEN_MAX> Guids{};
    uint8 Count = 0;
    uint8 Next = 0;  // at the cap: the oldest entry, replaced next

    bool Has(uint64 guid) const
    {
        return guid != 0 && std::find(Guids.begin(), Guids.begin() + Count, guid) != Guids.begin() + Count;
    }

    void Add(uint64 guid)
    {
        if (guid == 0 || Has(guid))
            return;

        if (Count < FOCUS_HEALERS_SEEN_MAX)
        {
            Guids[Count++] = guid;
            return;
        }

        Guids[Next] = guid;
        Next = static_cast<uint8>((Next + 1) % FOCUS_HEALERS_SEEN_MAX);
    }

    void Clear()
    {
        Count = 0;
        Next = 0;
    }
};

// The pick itself (raw guid): set at a change, cleared at a new match
struct FocusTargetPick
{
    uint64 Guid = 0;
    FocusSource Source = FocusSource::None;
    uint32 SinceMs = 0;
    uint32 MatchId = 0;  // Battleground::GetInstanceID while in progress, else 0
    uint32 Picks = 0;    // changes to a new focus
    FocusHealersSeen Healers;

    bool Set(uint64 guid, FocusSource source, uint32 nowMs)
    {
        Source = guid ? source : FocusSource::None;
        if (guid == Guid)
            return false;

        Guid = guid;
        SinceMs = nowMs;
        if (guid)
            ++Picks;

        return true;
    }

    void StartMatch(uint32 matchId)
    {
        Guid = 0;
        Source = FocusSource::None;
        SinceMs = 0;
        MatchId = matchId;
        Healers.Clear();
    }
};

namespace ArenaFocusTargetTest
{
// `tactics selftest focus`'s G4e cases (pure; Tactics.cpp's RunFocusSelftest calls them, an offline harness too)
template <typename Check>
void RunFocusTargetSelftest(Check&& check)
{
    // The picker: the moon by the Teamwork draw, else the healer; the moon after all without a healer; the CC target
    // last
    FocusPickInput all;
    all.Moon = all.Healer = all.Cc = true;
    FocusPickInput declined = all;
    declined.TakesMoon = false;
    FocusPickInput noHealer = declined;
    noHealer.Healer = false;
    FocusPickInput noMoon = all;
    noMoon.Moon = false;
    FocusPickInput ccOnly;
    ccOnly.Cc = true;
    check(PickFocusSource(all) == FocusSource::Moon && PickFocusSource(declined) == FocusSource::Healer,
          "the picker does not take the moon by the Teamwork draw, else the healer");
    check(PickFocusSource(noHealer) == FocusSource::Moon && PickFocusSource(noMoon) == FocusSource::Healer &&
              PickFocusSource(ccOnly) == FocusSource::Cc && PickFocusSource(FocusPickInput()) == FocusSource::None,
          "the picker's fallbacks are not: the moon without a healer, the healer without a moon, the CC target, none");

    // Teamwork's share at the dist values (50/100) and its draws over 400 hashes
    constexpr uint32 MoonAt1 = 50;
    constexpr uint32 MoonAt99 = 100;
    check(std::abs(FocusMoonShare(1, MoonAt1, MoonAt99) - 0.5f) < 1e-4f &&
              std::abs(FocusMoonShare(50, MoonAt1, MoonAt99) - 0.75f) < 1e-4f &&
              std::abs(FocusMoonShare(99, MoonAt1, MoonAt99) - 1.0f) < 1e-4f,
          "the moon share is not 0.50 / 0.75 / 1.00 at Teamwork 1 / 50 / 99");
    bool drawsCentre = true;
    std::string drawNote;
    for (uint8 const teamwork : {uint8(1), uint8(50), uint8(99)})
    {
        float const share = FocusMoonShare(teamwork, MoonAt1, MoonAt99);
        uint32 taken = 0;
        for (uint64 index = 0; index < 400; ++index)
            taken += FocusTakesMoon(share, SplitMix64(index * 977 + teamwork)) ? 1 : 0;

        float const rate = static_cast<float>(taken) / 400.0f;
        drawsCentre = drawsCentre && std::abs(rate - share) <= 0.06f && (share < 1.0f || taken == 400);
        drawNote += " " + std::to_string(taken);
    }

    check(drawsCentre, "the moon draws do not centre on the Teamwork share (taken of 400:" + drawNote + ")");
    check(FocusTakesMoon(0.6f, 12345) == FocusTakesMoon(0.6f, 12345), "the moon draw is not stable");

    // Cleared at a new match; a pick of the same enemy keeps its time
    FocusTargetPick pick;
    bool const first = pick.Set(77, FocusSource::Healer, 1000);
    bool const again = pick.Set(77, FocusSource::Healer, 2000);
    check(first && !again && pick.SinceMs == 1000 && pick.Picks == 1, "the same focus counts as a new pick");
    pick.StartMatch(9);
    check(pick.Guid == 0 && pick.Source == FocusSource::None && pick.MatchId == 9,
          "the focus is not cleared at a new match");

    // A healer seen healing stays one for the whole match (its later casts need no heal), forgotten at a new match
    pick.Healers.Add(5);
    pick.Healers.Add(5);
    pick.Healers.Add(0);
    bool const seenKept =
        pick.Healers.Has(5) && !pick.Healers.Has(6) && !pick.Healers.Has(0) && pick.Healers.Count == 1;
    pick.StartMatch(10);
    check(seenKept && !pick.Healers.Has(5) && pick.Healers.Count == 0,
          "a healer seen healing is not kept for the match, or not forgotten at a new match");
    for (uint64 guid = 1; guid <= FOCUS_HEALERS_SEEN_MAX + 1; ++guid)
        pick.Healers.Add(guid);

    check(!pick.Healers.Has(1) && pick.Healers.Has(2) && pick.Healers.Has(FOCUS_HEALERS_SEEN_MAX + 1) &&
              pick.Healers.Count == FOCUS_HEALERS_SEEN_MAX,
          "the seen healers at their cap do not replace the oldest entry");

    // The gate at FocusDexterity 45 (the lab overlay): 44 no, 45 and 46 yes, skill 0 always
    constexpr uint8 Threshold = 45;
    check(!FocusTargetCapable(60, 44, Threshold) && FocusTargetCapable(60, 45, Threshold) &&
              FocusTargetCapable(60, 46, Threshold) && FocusTargetCapable(0, 1, Threshold),
          "the focus gate is not Dexterity >= 45 (skill 0: always)");

    // The prices at the gate: below it the focus costs a new target's price, from it rare < focus < new target
    ReactionParams params;
    params.Ease.Enable = true;
    params.Ease.FocusDexterity = Threshold;
    auto const priceAt = [&params](uint8 dexterity)
    {
        ReactionTypeInputs inputs;
        inputs.Dexterity = dexterity;
        return ComputeReactionTypeState(params, inputs, 1.0f);
    };
    ReactionTypeState const below = priceAt(44);
    ReactionTypeState const at = priceAt(45);
    check(EaseCostMs(below, ReactionEase::Focus) == EaseCostMs(below, ReactionEase::NewTarget) &&
              EaseCostMs(at, ReactionEase::Rare) < EaseCostMs(at, ReactionEase::Focus) &&
              EaseCostMs(at, ReactionEase::Focus) < EaseCostMs(at, ReactionEase::NewTarget),
          "the focus price is not a new target's below Dexterity 45, or not between rare and new target from it");

    // The action's ease class: the focus ease on the focus, a new target's on another enemy, the button's on the own
    // target or a friend; a switch action stays a new target
    check(FocusActionEase(ReactionEase::Common, true, true) == ReactionEase::Focus &&
              FocusActionEase(ReactionEase::Rare, true, true) == ReactionEase::Focus,
          "an action on the focus target is not priced as the focus ease");
    check(FocusActionEase(ReactionEase::Common, true, false) == ReactionEase::NewTarget &&
              FocusActionEase(ReactionEase::Common, false, false) == ReactionEase::Common &&
              FocusActionEase(ReactionEase::Rare, false, true) == ReactionEase::Rare &&
              FocusActionEase(ReactionEase::NewTarget, true, true) == ReactionEase::NewTarget,
          "a second target that is not the focus is not a new target, or the own target's button changed");

    // The kick bonus (dist 50): a heal on the focus over another heal, not over a CC cast or an urgent heal; damage on
    // the focus not over a heal; a cast not worth a kick stays 0
    constexpr uint32 Bonus = 50;
    constexpr uint8 Urgent = 30;
    uint32 const heal = ArenaInterrupt::Score(ArenaInterrupt::Kind::Heal, 80, Urgent);
    uint32 const urgent = ArenaInterrupt::Score(ArenaInterrupt::Kind::Heal, 20, Urgent);
    uint32 const cc = ArenaInterrupt::Score(ArenaInterrupt::Kind::Cc, 80, Urgent);
    uint32 const damage = ArenaInterrupt::Score(ArenaInterrupt::Kind::Damage, 80, Urgent);
    uint32 const focusHeal = FocusKickScore(heal, true, Bonus);
    check(focusHeal > heal && focusHeal < cc && focusHeal < urgent && FocusKickScore(damage, true, Bonus) < heal &&
              FocusKickScore(0, true, Bonus) == 0 && FocusKickScore(heal, false, Bonus) == heal,
          "the focus kick bonus does not rank a focus heal over another heal only");

    // The counters' classes and the own target across a focus action (the selection model of CastSpell)
    constexpr uint64 Own = 11;
    constexpr uint64 Focus = 22;
    constexpr uint64 Other = 33;
    check(ClassifyFocusAct(Own, Own, Focus) == FocusAct::None &&
              ClassifyFocusAct(Focus, Own, Focus) == FocusAct::Focus &&
              ClassifyFocusAct(Other, Own, Focus) == FocusAct::Switch &&
              ClassifyFocusAct(Other, Own, 0) == FocusAct::Switch && ClassifyFocusAct(Own, Own, Own) == FocusAct::None,
          "an action is not: the own target none, the focus a focus action, another enemy a switch");
    check(FocusSelectionAfterCast(Own, Focus, true) == Own && FocusSelectionAfterCast(0, Focus, true) == 0,
          "a cast on the focus leaves the bot's selection on the focus");
    check(FocusSelectionAfterCast(0, Other, false) == Other && FocusSelectionAfterCast(Own, Other, false) == Own,
          "a cast that is no focus action restores the selection otherwise than before G4e");
}
}  // namespace ArenaFocusTargetTest

#endif
