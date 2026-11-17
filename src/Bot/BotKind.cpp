/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "BotKind.h"

#include <atomic>

#include "BotKindRules.h"
#include "Config.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "RandomPlayerbotMgr.h"

namespace
{
// Written on the world thread at config load, read on every map thread: relaxed, as nothing else is published with it
std::atomic<bool> s_livingWorld{false};
std::atomic<bool> s_followsTargets{false};

BotKindRules::MasterKind MasterKindOf(Player* master)
{
    if (!master)
        return BotKindRules::MasterKind::None;

    if (IsRealPlayer(master))
        return BotKindRules::MasterKind::RealPlayer;

    return IsSelfBot(master) ? BotKindRules::MasterKind::SelfBot : BotKindRules::MasterKind::Bot;
}
}  // namespace

bool BotKind::LivingWorld() { return s_livingWorld.load(std::memory_order_relaxed); }

bool BotKind::FollowsTargets() { return s_followsTargets.load(std::memory_order_relaxed); }

void BotKind::LoadLivingWorld()
{
    // Population::LoadConfig and PlayerbotAIConfig::Initialize both call this, in either order, and on ".reload config"
    // only the first of them runs again
    bool const living = BotKindRules::LivingWorld(sPlayerbotAIConfig.Ladder.Role == LADDER_ROLE_ARENA,
                                                  sPlayerbotAIConfig.ArenaOnlyEnable);
    s_livingWorld.store(living, std::memory_order_relaxed);

    // Read from sConfigMgr, so ".reload config" switches it (Population::LoadConfig runs again)
    bool const followKey = sConfigMgr->GetOption<bool>("Playerbots.Population.FollowTargets", true);
    s_followsTargets.store(BotKindRules::FollowsTargets(living, followKey), std::memory_order_relaxed);
}

bool BotKind::IsPersistentBot(Player* bot)
{
    PlayerbotAI* botAI = bot ? GET_PLAYERBOT_AI(bot) : nullptr;
    return botAI && botAI->IsPersistentBot();
}

bool BotKind::KeepsPossessions(Player* bot)
{
    PlayerbotAI* botAI = bot ? GET_PLAYERBOT_AI(bot) : nullptr;
    return botAI && botAI->KeepsPossessions();
}

// B changes within a session only when the bot's master becomes the bot itself (IsRandomBot's selfbot term), and the
// manager's set gains a bot before its login and loses it with its logout: refreshed after the AI is attached
// (PlayerbotsMgr::AddPlayerbotData) and in SetMaster
void PlayerbotAI::RefreshKind()
{
    randomBot.store(bot && sRandomPlayerbotMgr.IsRandomBot(bot), std::memory_order_relaxed);
}

bool PlayerbotAI::IsPersistentBot() const
{
    return BotKindRules::IsPersistentBot(BotKind::LivingWorld(), randomBot.load(std::memory_order_relaxed));
}

bool PlayerbotAI::KeepsPossessions()
{
    // The master is looked up only for a persistent bot
    bool const persistent = IsPersistentBot();
    return persistent && BotKindRules::KeepsPossessions(persistent, MasterKindOf(master));
}
