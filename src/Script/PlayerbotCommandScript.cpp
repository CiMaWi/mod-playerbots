/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "AcquaintanceMgr.h"
#include "ActivityBudget.h"
#include "ArenaRecruit.h"
#include "ArenaTeamFormation.h"
#include "BattleGroundTactics.h"
#include "BotProfileMgr.h"
#include "Chat.h"
#include "GuildTaskMgr.h"
#include "LabCopies.h"
#include "LadderMgr.h"
#include "MatchLog.h"
#include "MatchTrace.h"
#include "Matchmaker.h"
#include "NaturalTrace.h"
#include "ObjectAccessor.h"
#include "PerfMonitor.h"
#include "PlayerbotAIConfig.h"
#include "PlayerbotMgr.h"
#include "PvpGearMgr.h"
#include "PvpTest.h"
#include "RandomPlayerbotFactory.h"
#include "RandomPlayerbotMgr.h"
#include "ScriptMgr.h"
#include "Tactics.h"
#include "TestPlayer.h"
#include "WorldThreadMonitor.h"

using namespace Acore::ChatCommands;

class playerbots_commandscript : public CommandScript
{
public:
    playerbots_commandscript() : CommandScript("playerbots_commandscript") {}

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable playerbotsDebugCommandTable = {
            {"bg", HandleDebugBGCommand, SEC_GAMEMASTER, Console::Yes},
        };

        static ChatCommandTable playerbotsAccountCommandTable = {
            {"setKey", HandleSetSecurityKeyCommand, SEC_PLAYER, Console::No},
            {"link", HandleLinkAccountCommand, SEC_PLAYER, Console::No},
            {"linkedAccounts", HandleViewLinkedAccountsCommand, SEC_PLAYER, Console::No},
            {"unlink", HandleUnlinkAccountCommand, SEC_PLAYER, Console::No},
        };

        static ChatCommandTable playerbotsTestPlayerCommandTable = {
            {"login", TestPlayerMgr::HandleLoginCommand, SEC_GAMEMASTER, Console::Yes},
            {"route", TestPlayerMgr::HandleRouteCommand, SEC_GAMEMASTER, Console::Yes},
            {"logout", TestPlayerMgr::HandleLogoutCommand, SEC_GAMEMASTER, Console::Yes},
            {"status", TestPlayerMgr::HandleStatusCommand, SEC_GAMEMASTER, Console::Yes},
            {"whisper", TestPlayerMgr::HandleWhisperCommand, SEC_GAMEMASTER, Console::Yes},
            {"group", TestPlayerMgr::HandleGroupCommand, SEC_GAMEMASTER, Console::Yes},
            {"arenaqueue", TestPlayerMgr::HandleArenaQueueCommand, SEC_GAMEMASTER, Console::Yes},
        };

        static ChatCommandTable playerbotsPvpTestPlanCommandTable = {
            {"start", PvpTestMgr::HandlePlanStartCommand, SEC_GAMEMASTER, Console::Yes},
            {"status", PvpTestMgr::HandlePlanStatusCommand, SEC_GAMEMASTER, Console::Yes},
            {"stop", PvpTestMgr::HandlePlanStopCommand, SEC_GAMEMASTER, Console::Yes},
        };

        static ChatCommandTable playerbotsPvpTestCommandTable = {
            {"arena", PvpTestMgr::HandleArenaCommand, SEC_GAMEMASTER, Console::Yes},
            {"duel", PvpTestMgr::HandleDuelCommand, SEC_GAMEMASTER, Console::Yes},
            {"brawl", PvpTestMgr::HandleBrawlCommand, SEC_GAMEMASTER, Console::Yes},
            {"plan", playerbotsPvpTestPlanCommandTable},
            {"status", PvpTestMgr::HandleStatusCommand, SEC_GAMEMASTER, Console::Yes},
            {"stop", PvpTestMgr::HandleStopCommand, SEC_GAMEMASTER, Console::Yes},
        };

        static ChatCommandTable playerbotsArenaCommandTable = {
            {"resetratings", HandleArenaResetRatingsCommand, SEC_GAMEMASTER, Console::Yes},
            {"teams", ArenaTeamFormation::HandleTeamsCommand, SEC_GAMEMASTER, Console::Yes},
            {"recruit", ArenaRecruit::HandleRecruitCommand, SEC_GAMEMASTER, Console::Yes},
        };

        static ChatCommandTable playerbotsSituationCommandTable = {
            {"status", ActivityBudget::HandleStatusCommand, SEC_GAMEMASTER, Console::Yes},
            {"force", ActivityBudget::HandleForceCommand, SEC_GAMEMASTER, Console::Yes},
        };

        static ChatCommandTable playerbotsLadderPoolCommandTable = {
            {"status", LadderMgr::HandlePoolStatusCommand, SEC_GAMEMASTER, Console::Yes},
            {"build", LadderMgr::HandlePoolBuildCommand, SEC_GAMEMASTER, Console::Yes},
            {"selftest", LadderMgr::HandlePoolSelftestCommand, SEC_GAMEMASTER, Console::Yes},
        };

        static ChatCommandTable playerbotsLadderMatchmakerCommandTable = {
            {"status", Matchmaker::HandleStatusCommand, SEC_GAMEMASTER, Console::Yes},
            {"candidates", Matchmaker::HandleCandidatesCommand, SEC_GAMEMASTER, Console::Yes},
            {"selftest", Matchmaker::HandleSelftestCommand, SEC_GAMEMASTER, Console::Yes},
        };

        static ChatCommandTable playerbotsLadderCommandTable = {
            {"depart", LadderMgr::HandleDepartCommand, SEC_GAMEMASTER, Console::Yes},
            {"arrive", LadderMgr::HandleArriveCommand, SEC_GAMEMASTER, Console::Yes},
            {"status", LadderMgr::HandleStatusCommand, SEC_GAMEMASTER, Console::Yes},
            {"teams", LadderMgr::HandleTeamsCommand, SEC_GAMEMASTER, Console::Yes},
            {"pin", LadderMgr::HandlePinCommand, SEC_GAMEMASTER, Console::Yes},
            {"drain", LadderMgr::HandleDrainCommand, SEC_GAMEMASTER, Console::Yes},
            {"undrain", LadderMgr::HandleUndrainCommand, SEC_GAMEMASTER, Console::Yes},
            {"pool", playerbotsLadderPoolCommandTable},
            {"matchmaker", playerbotsLadderMatchmakerCommandTable},
        };

        static ChatCommandTable playerbotsPvpGearCommandTable = {
            {"reset", PvpGearMgr::HandleResetCommand, SEC_GAMEMASTER, Console::Yes},
            {"show", PvpGearMgr::HandleShowCommand, SEC_GAMEMASTER, Console::Yes},
            {"check", PvpGearMgr::HandleCheckCommand, SEC_GAMEMASTER, Console::Yes},
            {"selftest", PvpGearMgr::HandleSelftestCommand, SEC_GAMEMASTER, Console::Yes},
            {"status", PvpGearMgr::HandleStatusCommand, SEC_GAMEMASTER, Console::Yes},
            {"stats", PvpGearMgr::HandleStatsCommand, SEC_GAMEMASTER, Console::Yes},
            {"grant", PvpGearMgr::HandleGrantCommand, SEC_GAMEMASTER, Console::Yes},
        };

        static ChatCommandTable playerbotsMatchLogCommandTable = {
            {"status", MatchLog::HandleStatusCommand, SEC_GAMEMASTER, Console::Yes},
            {"selftest", MatchLog::HandleSelftestCommand, SEC_GAMEMASTER, Console::Yes},
            {"traceselftest", MatchTrace::HandleSelftestCommand, SEC_GAMEMASTER, Console::Yes},
        };

        static ChatCommandTable playerbotsTacticsCommandTable = {
            {"show", TacticCommands::HandleShowCommand, SEC_GAMEMASTER, Console::Yes},
            {"stats", TacticCommands::HandleStatsCommand, SEC_GAMEMASTER, Console::Yes},
            {"selftest", TacticCommands::HandleSelftestCommand, SEC_GAMEMASTER, Console::Yes},
        };

        static ChatCommandTable playerbotsAttributesCommandTable = {
            {"pin", LabCopyMgr::HandlePinCommand, SEC_GAMEMASTER, Console::Yes},
            {"unpin", LabCopyMgr::HandleUnpinCommand, SEC_GAMEMASTER, Console::Yes},
            {"show", LabCopyMgr::HandleShowCommand, SEC_GAMEMASTER, Console::Yes},
            {"reapply", LabCopyMgr::HandleReapplyCommand, SEC_GAMEMASTER, Console::Yes},
            {"logout", LabCopyMgr::HandleLogoutCommand, SEC_GAMEMASTER, Console::Yes},
        };

        static ChatCommandTable playerbotsArenaOnlyCommandTable = {
            {"status", RandomPlayerbotMgr::HandleArenaOnlyStatusCommand, SEC_GAMEMASTER, Console::Yes},
            {"selftest", RandomPlayerbotMgr::HandleArenaOnlySelftestCommand, SEC_GAMEMASTER, Console::Yes},
            {"kill", HandleArenaOnlyKillCommand, SEC_GAMEMASTER, Console::Yes},
            {"botstate", HandleArenaOnlyBotStateCommand, SEC_GAMEMASTER, Console::Yes},
            {"force", RandomPlayerbotMgr::HandleArenaOnlyForceCommand, SEC_GAMEMASTER, Console::Yes},
        };

        static ChatCommandTable playerbotsNaturalCommandTable = {
            {"trace", NaturalTrace::HandleTraceCommand, SEC_GAMEMASTER, Console::Yes},
            {"sample", NaturalTrace::HandleSampleCommand, SEC_GAMEMASTER, Console::Yes},
            {"organic", NaturalTrace::HandleOrganicCommand, SEC_GAMEMASTER, Console::Yes},
            {"status", NaturalTrace::HandleStatusCommand, SEC_GAMEMASTER, Console::Yes},
            {"selftest", NaturalTrace::HandleSelftestCommand, SEC_GAMEMASTER, Console::Yes},
        };

        static ChatCommandTable playerbotsCommandTable = {
            {"bot", HandlePlayerbotCommand, SEC_PLAYER, Console::No},
            {"gtask", HandleGuildTaskCommand, SEC_GAMEMASTER, Console::Yes},
            {"pmon", HandlePerfMonCommand, SEC_GAMEMASTER, Console::Yes},
            {"worldthread", WorldThreadMonitor::HandleCommand, SEC_GAMEMASTER, Console::Yes},
            {"rndbot", HandleRandomPlayerbotCommand, SEC_GAMEMASTER, Console::Yes},
            {"skill", BotProfileMgr::HandleSkillCommand, SEC_GAMEMASTER, Console::Yes},
            {"acquaintances", AcquaintanceMgr::HandleCommand, SEC_GAMEMASTER, Console::Yes},
            {"debug", playerbotsDebugCommandTable},
            {"account", playerbotsAccountCommandTable},
            {"testplayer", playerbotsTestPlayerCommandTable},
            {"pvptest", playerbotsPvpTestCommandTable},
            {"arena", playerbotsArenaCommandTable},
            {"situation", playerbotsSituationCommandTable},
            {"ladder", playerbotsLadderCommandTable},
            {"pvpgear", playerbotsPvpGearCommandTable},
            {"matchlog", playerbotsMatchLogCommandTable},
            {"tactics", playerbotsTacticsCommandTable},
            {"attributes", playerbotsAttributesCommandTable},
            {"arenaonly", playerbotsArenaOnlyCommandTable},
            {"natural", playerbotsNaturalCommandTable},
        };

        static ChatCommandTable commandTable = {
            {"playerbots", playerbotsCommandTable},
        };

        return commandTable;
    }

    static bool HandlePlayerbotCommand(ChatHandler* handler, char const* args)
    {
        return PlayerbotMgr::HandlePlayerbotMgrCommand(handler, args);
    }

    static bool HandleRandomPlayerbotCommand(ChatHandler* handler, char const* args)
    {
        return RandomPlayerbotMgr::HandlePlayerbotConsoleCommand(handler, args);
    }

    // M17c.3 check of the in-place revive of a waiting bot: "kill [name]" kills the named random bot, or without a name
    // the first grouped bot that waits outside a battleground (queued first); "botstate <name>" shows its state.
    // Refused unless Playerbots.ArenaOnly.Enable is on (lab diagnostics only). Console, world thread.
    static bool HandleArenaOnlyKillCommand(ChatHandler* handler, char const* args)
    {
        if (!sPlayerbotAIConfig.ArenaOnlyEnable)
        {
            handler->PSendSysMessage("ARENAONLY kill: refused, Playerbots.ArenaOnly.Enable is off");
            return true;
        }

        std::string const name = args ? args : "";
        Player* target = nullptr;
        if (!name.empty())
        {
            target = ObjectAccessor::FindPlayerByName(name);
            if (target && !sRandomPlayerbotMgr.IsRandomBot(target))
                target = nullptr;
        }
        else
        {
            Player* fallback = nullptr;
            for (auto const& [guid, bot] : sRandomPlayerbotMgr.GetAllBots())
            {
                if (!bot || !bot->IsInWorld() || !bot->IsAlive() || !bot->GetGroup() || bot->InBattleground() ||
                    bot->IsBeingTeleported())
                    continue;

                if (bot->InBattlegroundQueue())
                {
                    target = bot;
                    break;
                }

                if (!fallback)
                    fallback = bot;
            }

            if (!target)
                target = fallback;
        }

        if (!target || !target->IsInWorld() || !target->IsAlive() || target->InBattleground())
        {
            handler->PSendSysMessage("ARENAONLY kill: no alive random bot outside a battleground found ({})", name);
            return true;
        }

        bool const queued = target->InBattlegroundQueue();
        bool const grouped = target->GetGroup() != nullptr;
        target->KillSelf(false);
        handler->PSendSysMessage("ARENAONLY kill: bot {} killed on map {}, grouped {}, queued {}, dead {}",
                                 target->GetName(), target->GetMapId(), grouped, queued, target->isDead());
        return true;
    }

    static bool HandleArenaOnlyBotStateCommand(ChatHandler* handler, char const* args)
    {
        std::string const name = args ? args : "";
        Player* bot = name.empty() ? nullptr : ObjectAccessor::FindPlayerByName(name, false);
        if (!bot)
        {
            handler->PSendSysMessage("ARENAONLY botstate: {} not online", name);
            return true;
        }

        handler->PSendSysMessage(
            "ARENAONLY botstate: bot {} {}, health {}, map {}, in bg {}, grouped {}, queued {}, teleporting {}",
            bot->GetName(), bot->isDead() ? (bot->GetCorpse() ? "ghost" : "dead") : "alive", bot->GetHealth(),
            bot->GetMapId(), bot->InBattleground(), bot->GetGroup() != nullptr, bot->InBattlegroundQueue(),
            bot->IsBeingTeleported());
        return true;
    }

    static bool HandleGuildTaskCommand(ChatHandler* handler, char const* args)
    {
        return GuildTaskMgr::HandleConsoleCommand(handler, args);
    }

    static bool HandlePerfMonCommand(ChatHandler* /*handler*/, char const* args)
    {
        if (!strcmp(args, "reset"))
        {
            sPerfMonitor.Reset();
            return true;
        }

        if (!strcmp(args, "tick"))
        {
            sPerfMonitor.PrintStats(true, false);
            sPerfMonitor.DumpJson(true);
            return true;
        }

        if (!strcmp(args, "stack"))
        {
            sPerfMonitor.PrintStats(false, true);
            sPerfMonitor.DumpJson(false);
            return true;
        }

        if (!strcmp(args, "toggle"))
        {
            sPlayerbotAIConfig.PerfMonEnabled = !sPlayerbotAIConfig.PerfMonEnabled;
            if (sPlayerbotAIConfig.PerfMonEnabled)
                LOG_INFO("playerbots", "Performance monitor enabled");
            else
                LOG_INFO("playerbots", "Performance monitor disabled");
            return true;
        }

        sPerfMonitor.PrintStats();
        sPerfMonitor.DumpJson(false);
        return true;
    }

    static bool HandleArenaResetRatingsCommand(ChatHandler* handler, char const* /*args*/)
    {
        uint32 resetMembers = 0;
        uint32 const resetTeams = RandomPlayerbotFactory::ResetBotArenaTeamRatings(resetMembers);
        handler->PSendSysMessage("Reset the ratings of {} bot arena teams ({} members) to the start values.",
                                 resetTeams, resetMembers);
        return true;
    }

    static bool HandleDebugBGCommand(ChatHandler* handler, char const* args)
    {
        return BGTactics::HandleConsoleCommand(handler, args);
    }

    static bool HandleSetSecurityKeyCommand(ChatHandler* handler, char const* args)
    {
        if (!args || !*args)
        {
            handler->PSendSysMessage("Usage: .playerbots account setKey <securityKey>");
            return false;
        }

        Player* player = handler->GetSession()->GetPlayer();
        std::string key = args;

        PlayerbotMgr* mgr = PlayerbotsMgr::instance().GetPlayerbotMgr(player);
        if (mgr)
        {
            mgr->HandleSetSecurityKeyCommand(player, key);
            return true;
        }
        else
        {
            handler->PSendSysMessage("PlayerbotMgr instance not found.");
            return false;
        }
    }

    static bool HandleLinkAccountCommand(ChatHandler* handler, char const* args)
    {
        if (!args || !*args)
            return false;

        char* accountName = strtok((char*)args, " ");
        char* key = strtok(nullptr, " ");

        if (!accountName || !key)
        {
            handler->PSendSysMessage("Usage: .playerbots account link <accountName> <securityKey>");
            return false;
        }

        Player* player = handler->GetSession()->GetPlayer();

        PlayerbotMgr* mgr = PlayerbotsMgr::instance().GetPlayerbotMgr(player);
        if (mgr)
        {
            mgr->HandleLinkAccountCommand(player, accountName, key);
            return true;
        }
        else
        {
            handler->PSendSysMessage("PlayerbotMgr instance not found.");
            return false;
        }
    }

    static bool HandleViewLinkedAccountsCommand(ChatHandler* handler, char const* /*args*/)
    {
        Player* player = handler->GetSession()->GetPlayer();

        PlayerbotMgr* mgr = PlayerbotsMgr::instance().GetPlayerbotMgr(player);
        if (mgr)
        {
            mgr->HandleViewLinkedAccountsCommand(player);
            return true;
        }
        else
        {
            handler->PSendSysMessage("PlayerbotMgr instance not found.");
            return false;
        }
    }

    static bool HandleUnlinkAccountCommand(ChatHandler* handler, char const* args)
    {
        if (!args || !*args)
            return false;

        char* accountName = strtok((char*)args, " ");
        if (!accountName)
        {
            handler->PSendSysMessage("Usage: .playerbots account unlink <accountName>");
            return false;
        }

        Player* player = handler->GetSession()->GetPlayer();

        PlayerbotMgr* mgr = PlayerbotsMgr::instance().GetPlayerbotMgr(player);
        if (mgr)
        {
            mgr->HandleUnlinkAccountCommand(player, accountName);
            return true;
        }
        else
        {
            handler->PSendSysMessage("PlayerbotMgr instance not found.");
            return false;
        }
    }
};

void AddPlayerbotsCommandscripts() { new playerbots_commandscript(); }
