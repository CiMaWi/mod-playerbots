/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "NaturalMove.h"

#include "AiObjectContext.h"
#include "BotTacticsInternal.h"
#include "Map.h"
#include "NaturalPath.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "Unit.h"

// G2b: the tracking ring's feed at the bot's own checks while it fights where it tracks naturally (TrackFeed): the
// current target's true position, as ReachCombatTo's own TrackObserve. Fed only by chase calls, a chase after a pause
// (an in-range cast, a held move) found the ring stale, started it again and ran to the target's true position. Cost
// per check in a fight: NaturalTrackHere (cached), one value read and a compare; a ring push at most once per sample
// gap (50 to 111 ms); nothing out of combat or with Natural.Track.Enable 0.
void FeedNaturalTrack(PlayerbotAI* botAI, Player* bot, uint32 nowMs)
{
    if (!bot->IsInCombat() || !NaturalTrackHere(botAI, nowMs))
        return;

    Unit* const target = botAI->GetAiObjectContext()->GetValue<Unit*>("current target")->Get();
    if (!target || target == bot || !target->IsInWorld() || !target->IsInMap(bot) || !target->IsAlive() ||
        bot->IsFriendlyTo(target))
        return;

    TrackFeed(botAI->GetTrackState(), sPlayerbotAIConfig.NaturalTrack, target->GetGUID().GetRawValue(), nowMs,
              target->GetPositionX(), target->GetPositionY(), target->GetPositionZ());
}

// G2b: whether the bot tracks naturally now (NaturalMove.h); as NaturalFacingHere with the Track options
bool NaturalTrackHere(PlayerbotAI* botAI, uint32 nowMs)
{
    TrackConfig const& config = sPlayerbotAIConfig.NaturalTrack;
    if (!config.Enable || !botAI->TacticEnabledHere(TacticId::Natural))
        return false;

    TacticContext const context = botAI->GetTacticContext();
    if (config.Contexts & TacticContextBit(context))
        return true;

    // Out of PvP only in a real player's sight, and never in a dungeon or raid (their scripted chases stay exact)
    Player* const bot = botAI->GetBot();
    Map* const map = bot ? bot->GetMap() : nullptr;
    if (!config.OutsidePvpInSight || context != TacticContext::Pve || !map || map->Instanceable())
        return false;

    TrackBotState& state = botAI->GetTrackState();
    if (state.InSightCheckedMs == 0 || nowMs - state.InSightCheckedMs >= config.InSightCacheMs)
    {
        state.InSight = botAI->HasPlayerNearby(static_cast<float>(config.InSightYards));
        state.InSightCheckedMs = std::max(nowMs, 1u);
    }

    return state.InSight;
}

// G2c: whether the bot walks natural paths now (NaturalPath.h); as NaturalTrackHere with the Path options
bool NaturalPathHere(PlayerbotAI* botAI, uint32 nowMs)
{
    PathConfig const& config = sPlayerbotAIConfig.NaturalPath;
    if (!config.Enable || !botAI->TacticEnabledHere(TacticId::Natural))
        return false;

    TacticContext const context = botAI->GetTacticContext();
    if (config.Contexts & TacticContextBit(context))
        return true;

    // Out of PvP only in a real player's sight, and never in a dungeon or raid (their scripted paths stay exact)
    Player* const bot = botAI->GetBot();
    Map* const map = bot ? bot->GetMap() : nullptr;
    if (!config.OutsidePvpInSight || context != TacticContext::Pve || !map || map->Instanceable())
        return false;

    PathBotState& state = botAI->GetPathState();
    if (state.InSightCheckedMs == 0 || nowMs - state.InSightCheckedMs >= config.InSightCacheMs)
    {
        state.InSight = botAI->HasPlayerNearby(static_cast<float>(config.InSightYards));
        state.InSightCheckedMs = std::max(nowMs, 1u);
    }

    return state.InSight;
}
