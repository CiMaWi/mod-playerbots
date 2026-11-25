/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOT_NEWRPGDOFISH_H
#define PLAYERBOT_NEWRPGDOFISH_H

#include "NewRpgBaseAction.h"
#include "StatusEndRules.h"

class GameObject;

// A fishing trip (professions plan WR16b, PLAN 3.12; the rules are FishRules.h): the bot walks to a stand point probed
// at the trip's water (a live pool near the place, else open water), takes its pole in hand, casts, clicks its bobber
// when a fish bites (the loot pipeline stores the catch) and ends the trip in a fight, at full bags, after its time or
// when the water gives nothing. Every end of the status gives the weapon back and writes one FishTrip row (EndTrip)
class NewRpgDoFishAction : public NewRpgBaseAction
{
public:
    NewRpgDoFishAction(PlayerbotAI* botAI) : NewRpgBaseAction(botAI, "new rpg do fish") {}
    bool Execute(Event event) override;

    // The status core at every end of the status, before it is replaced (NewRpgStatusEnd.cpp, also at a reset): the
    // cast stopped, the bobber's loot released, the weapon and off-hand back in hand, the FishTrip row (source: the
    // end word; TimedOut writes "deadline")
    static void EndTrip(PlayerbotAI* botAI, NewRpgInfo::DoFish const& data, StatusEnd::Outcome outcome,
                        char const* word);
    // The combat engine took over (PlayerbotAI::ChangeEngineOnCombat): a fishing trip ends with "fight"
    static void OnCombat(PlayerbotAI* botAI);
    // A fishing trip near its stand keeps the bot on foot (the mount policy, CheckMountStateAction::AskMountPolicy):
    // fishing is not cast mounted, and the mount action remounted the bot after each dismount (WR16b run 3)
    static bool KeepsOnFoot(PlayerbotAI* botAI, Player* bot);
    // The status core's deadline of a fishing trip (NewRpgStatus.cpp): past its general bound, Fish::PastDeadline
    static bool PastDeadline(PlayerbotAI* botAI);
    // The bot's AI held through its fishing channel (PlayerbotAI::UpdateAI, as HoldTameChannel): the bobber watched
    // every WATCH_TICK_MS and clicked at a bite, no engine pass. The core gives a bite 5 s (FISHING_BOBBER_READY_TIME);
    // the engine's ticks while the line was out came 4 to 12 s apart (WR16b runs 4 to 6), and most bites got away
    static bool HoldFishChannel(PlayerbotAI* botAI);
    static constexpr uint32 WATCH_TICK_MS = 500;

private:
    // A stand point and the water it faces; false when the place has none (the trip ends)
    bool Probe(NewRpgInfo::DoFish& data);
    // The pole in the main hand (the weapon and off-hand remembered); false when it could not be put on
    bool PoleInHand(NewRpgInfo::DoFish& data);
    // The cast in flight: the bite clicked, the miss or the refusal counted. True while it waits
    bool Watch(NewRpgInfo::DoFish& data);
    void Finish(char const* word);
    // A test bot's trace of the branch a tick waits in, one "[Probe]" line at each change
    void Trace(NewRpgInfo::DoFish const& data, char const* where);

    char const* traced = nullptr;
};

#endif
