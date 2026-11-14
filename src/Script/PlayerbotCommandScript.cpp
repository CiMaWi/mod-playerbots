/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <limits>
#include <list>
#include <sstream>

#include "AcquaintanceMgr.h"
#include "ActivityBudget.h"
#include "ArenaRecruit.h"
#include "ArenaTeamFormation.h"
#include "BankCommands.h"
#include "BankMailCommands.h"
#include "BattleGroundTactics.h"
#include "BotProfileMgr.h"
#include "BotRecordMgr.h"
#include "BotTraitMgr.h"
#include "BridgeBotFacts.h"
#include "BridgeBotState.h"
#include "CellImpl.h"
#include "Chat.h"
#include "ChatCommandTags.h"
#include "DBCStores.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "GuildTaskMgr.h"
#include "LabCopies.h"
#include "LadderMgr.h"
#include "MatchLog.h"
#include "MatchTrace.h"
#include "Matchmaker.h"
#include "NaturalTrace.h"
#include "NewRpgAction.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "PerfMonitor.h"
#include "PlannerService.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "PlayerbotFactory.h"
#include "PlayerbotMgr.h"
#include "Population.h"
#include "PvpGearMgr.h"
#include "PvpTest.h"
#include "RandomPlayerbotFactory.h"
#include "RandomPlayerbotMgr.h"
#include "RouteCheck.h"
#include "RouteQuery.h"
#include "ScriptMgr.h"
#include "StringConvert.h"
#include "Tactics.h"
#include "TestConstraints.h"
#include "TestPlayer.h"
#include "ThreadCheck.h"
#include "Tokenize.h"
#include "TransportMgr.h"
#include "TravelGraph.h"
#include "TravelPlaces.h"
#include "WorldLog.h"
#include "WorldPvp.h"
#include "WorldSession.h"
#include "WorldThreadMonitor.h"
#include "LightweightPerf.h"
#include "Metric.h"

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
            {"party", TestPlayerMgr::HandlePartyCommand, SEC_GAMEMASTER, Console::Yes},
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
            {"comps", ArenaTeamFormation::HandleCompsCommand, SEC_GAMEMASTER, Console::Yes},
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

        static ChatCommandTable playerbotsTacticsCoverCommandTable = {
            {"scan", TacticCommands::HandleCoverCommand, SEC_GAMEMASTER, Console::Yes},
        };

        static ChatCommandTable playerbotsTacticsJumpCommandTable = {
            {"scan", TacticCommands::HandleJumpScanCommand, SEC_GAMEMASTER, Console::Yes},
            {"test", TacticCommands::HandleJumpTestCommand, SEC_GAMEMASTER, Console::Yes},
        };

        static ChatCommandTable playerbotsTacticsCommandTable = {
            {"show", TacticCommands::HandleShowCommand, SEC_GAMEMASTER, Console::Yes},
            {"stats", TacticCommands::HandleStatsCommand, SEC_GAMEMASTER, Console::Yes},
            {"selftest", TacticCommands::HandleSelftestCommand, SEC_GAMEMASTER, Console::Yes},
            {"force", TacticCommands::HandleForceCommand, SEC_GAMEMASTER, Console::Yes},
            {"moon", TacticCommands::HandleMoonCommand, SEC_GAMEMASTER, Console::Yes},
            {"ensuretrinket", TacticCommands::HandleEnsureTrinketCommand, SEC_GAMEMASTER, Console::Yes},
            {"interrupt", TacticCommands::HandleInterruptCommand, SEC_GAMEMASTER, Console::Yes},
            {"cover", playerbotsTacticsCoverCommandTable},
            {"jump", playerbotsTacticsJumpCommandTable},
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

        static ChatCommandTable playerbotsPopulationCommandTable = {
            {"status", Population::HandleStatusCommand, SEC_GAMEMASTER, Console::Yes},
            {"selftest", Population::HandleSelftestCommand, SEC_GAMEMASTER, Console::Yes},
            {"resize", Population::HandleResizeCommand, SEC_CONSOLE, Console::Yes},
        };

        static ChatCommandTable playerbotsRouteCheckCommandTable = {
            {"links", RouteCheck::HandleLinksCommand, SEC_PLAYER, Console::Yes},
            {"pairs", RouteCheck::HandlePairsCommand, SEC_GAMEMASTER, Console::Yes},
        };

        static ChatCommandTable playerbotsRouteCommandTable = {
            {"status", TravelGraph::HandleStatusCommand, SEC_GAMEMASTER, Console::Yes},
            {"selftest", TravelGraph::HandleSelftestCommand, SEC_GAMEMASTER, Console::Yes},
            {"go", HandleRouteGoCommand, SEC_GAMEMASTER, Console::Yes},
            {"stop", HandleRouteStopCommand, SEC_GAMEMASTER, Console::Yes},
            {"transport", HandleRouteTransportCommand, SEC_GAMEMASTER, Console::Yes},
            {"hold", HandleRouteHoldCommand, SEC_GAMEMASTER, Console::Yes},
            {"release", HandleRouteReleaseCommand, SEC_GAMEMASTER, Console::Yes},
            {"constraint", HandleRouteConstraintCommand, SEC_GAMEMASTER, Console::Yes},
            {"constraints", HandleRouteConstraintsCommand, SEC_PLAYER, Console::Yes},
            {"aboard", HandleRouteAboardCommand, SEC_GAMEMASTER, Console::Yes},
            {"links", HandleRouteLinksCommand, SEC_PLAYER, Console::Yes},
            {"query", RouteQuery::HandleCommand, SEC_PLAYER, Console::Yes},
            {"check", playerbotsRouteCheckCommandTable},
        };

        static ChatCommandTable playerbotsPlannerCommandTable = {
            {"status", Planner::HandleStatusCommand, SEC_GAMEMASTER, Console::Yes},
            {"selftest", Planner::HandleSelftestCommand, SEC_GAMEMASTER, Console::Yes},
            {"indexes", Planner::HandleIndexesCommand, SEC_GAMEMASTER, Console::Yes},
            {"hub", Planner::HandleHubCommand, SEC_GAMEMASTER, Console::Yes},
            {"service", Planner::HandleServiceCommand, SEC_GAMEMASTER, Console::Yes},
            {"crowd", Planner::HandleCrowdCommand, SEC_GAMEMASTER, Console::Yes},
            {"quest", Planner::HandleQuestCommand, SEC_GAMEMASTER, Console::Yes},
            {"questuse", Planner::HandleQuestUseCommand, SEC_PLAYER, Console::Yes},
            {"needs", Planner::HandleNeedsCommand, SEC_GAMEMASTER, Console::Yes},
            {"circuit", Planner::HandleCircuitCommand, SEC_GAMEMASTER, Console::Yes},
            {"goals", Planner::HandleGoalsCommand, SEC_GAMEMASTER, Console::Yes},
            {"goal", Planner::HandleGoalCommand, SEC_GAMEMASTER, Console::Yes},
        };

        static ChatCommandTable playerbotsMailCommandTable = {
            {"status", BankMail::HandleMailStatusCommand, SEC_PLAYER, Console::Yes},
            {"selftest", BankMail::HandleMailSelftestCommand, SEC_PLAYER, Console::Yes},
            {"visit", BankMail::HandleMailVisitCommand, SEC_GAMEMASTER, Console::Yes},
        };

        static ChatCommandTable playerbotsBankCommandTable = {
            {"status", BankMail::HandleBankStatusCommand, SEC_PLAYER, Console::Yes},
            {"selftest", BankMail::HandleBankSelftestCommand, SEC_PLAYER, Console::Yes},
            {"visit", BankMail::HandleBankVisitCommand, SEC_GAMEMASTER, Console::Yes},
        };

        static ChatCommandTable playerbotsWorldLogCommandTable = {
            {"selftest", WorldLog::HandleSelftestCommand, SEC_GAMEMASTER, Console::Yes},
            {"case", WorldLog::HandleCaseCommand, SEC_GAMEMASTER, Console::Yes},
            {"status", WorldLog::HandleStatusCommand, SEC_GAMEMASTER, Console::Yes},
        };

        // World PvP (WP23): enthusiasm shows a bot's value and thresholds and sets the value
        static ChatCommandTable playerbotsFightCommandTable = {
            {"enthusiasm", HandleFightEnthusiasmCommand, SEC_GAMEMASTER, Console::Yes},
        };

        // Living conversation LC8: what the conversation bridge reads of a bot (LC9 adds facts; learnflight is the
        // record test's fixture, LC6)
        static ChatCommandTable playerbotsBridgeCommandTable = {
            {"state", HandleBridgeStateCommand, SEC_GAMEMASTER, Console::Yes},
            {"facts", HandleBridgeFactsCommand, SEC_GAMEMASTER, Console::Yes},
            {"learnflight", HandleBridgeLearnFlightCommand, SEC_GAMEMASTER, Console::Yes},
        };

        static ChatCommandTable playerbotsCommandTable = {
            {"bot", HandlePlayerbotCommand, SEC_PLAYER, Console::No},
            {"exec", HandleExecCommand, SEC_GAMEMASTER, Console::Yes},
            {"inspect", HandleInspectCommand, SEC_GAMEMASTER, Console::Yes},
            {"gtask", HandleGuildTaskCommand, SEC_GAMEMASTER, Console::Yes},
            {"pmon", HandlePerfMonCommand, SEC_GAMEMASTER, Console::Yes},
            {"worldthread", WorldThreadMonitor::HandleCommand, SEC_GAMEMASTER, Console::Yes},
            {"tickperf", HandleTickPerfCommand, SEC_PLAYER, Console::Yes},
            {"rndbot", HandleRandomPlayerbotCommand, SEC_GAMEMASTER, Console::Yes},
            {"skill", BotProfileMgr::HandleSkillCommand, SEC_GAMEMASTER, Console::Yes},
            {"traits", BotTraitMgr::HandleTraitsCommand, SEC_GAMEMASTER, Console::Yes},
            {"record", BotRecordMgr::HandleRecordCommand, SEC_GAMEMASTER, Console::Yes},
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
            {"population", playerbotsPopulationCommandTable},
            {"route", playerbotsRouteCommandTable},
            {"planner", playerbotsPlannerCommandTable},
            {"mail", playerbotsMailCommandTable},
            {"bank", playerbotsBankCommandTable},
            {"worldlog", playerbotsWorldLogCommandTable},
            {"fight", playerbotsFightCommandTable},
            {"bridge", playerbotsBridgeCommandTable},
        };

        static ChatCommandTable wlogCommandTable = {
            {"mark", WorldLog::HandleMarkCommand, SEC_PLAYER, Console::No},
        };

        static ChatCommandTable commandTable = {
            {"playerbots", playerbotsCommandTable},
            {"wlog", wlogCommandTable},
        };

        return commandTable;
    }

    static bool HandleTickPerfCommand(ChatHandler* handler, char const* args)
    {
        if (handler->GetSession() || !PlayerbotThreadCheck::IsWorldThread())
        {
            handler->SendSysMessage("Tick performance snapshots require the world-thread console");
            return false;
        }
        std::string_view const command(args ? args : "");
        if (command != "reset" && command != "take")
            return false;

        handler->PSendSysMessage("TICKPERF {}", LightweightPerf::Snapshot(command == "reset", sMetric->IsEnabled()));
        return true;
    }

    static bool HandlePlayerbotCommand(ChatHandler* handler, char const* args)
    {
        return PlayerbotMgr::HandlePlayerbotMgrCommand(handler, args);
    }

    // Sends its own error so the core does not append its generic help text.
    static PlayerbotAI* GetOnlineBotAI(ChatHandler* handler, PlayerIdentifier const& target)
    {
        Player* player = target.GetConnectedPlayer();
        PlayerbotAI* botAI = player ? PlayerbotsMgr::instance().GetPlayerbotAI(player) : nullptr;
        if (!botAI)
            handler->SendErrorMessage("{} is not an online bot", target.GetName());
        return botAI;
    }

    // From the console or SOAP there is no player object to send the command, so the bot sends it to itself.
    // The bot's security check always lets a bot command itself.
    static bool HandleExecCommand(ChatHandler* handler, PlayerIdentifier target, Tail command)
    {
        PlayerbotAI* botAI = GetOnlineBotAI(handler, target);
        if (!botAI)
            return false;

        if (command.empty())
        {
            handler->SendErrorMessage("Usage: .playerbots exec <bot> <command> (state-changing commands; "
                                      "query replies go to the bot's master - use inspect)");
            return false;
        }

        Player* from = handler->GetSession() ? handler->GetSession()->GetPlayer() : botAI->GetBot();
        botAI->HandleCommand(CHAT_MSG_WHISPER, std::string(command), from);
        handler->PSendSysMessage("{}: '{}' sent", target.GetName(), std::string(command));
        return true;
    }

    // `playerbots route go <bot> <place>` (WT8, tests): sends a persistent bot on a planned trip to a named place
    // (TravelPlaces.h) or "x,y,z,map". World thread: the order waits for the bot's map thread, which checks the bot
    // again and logs `[Route] <bot> travel to <place>: <n> legs, <s> s` once the route returns
    static bool HandleRouteGoCommand(ChatHandler* handler, PlayerIdentifier target, Tail place)
    {
        PlayerbotAI* botAI = GetOnlineBotAI(handler, target);
        if (!botAI)
            return false;
        Player* bot = botAI->GetBot();
        std::string const text(place);
        std::optional<TravelRoute::Point> const dest = TravelPlaces::Resolve(text);
        if (!dest)
        {
            handler->SendErrorMessage("Usage: .playerbots route go <bot> <place>|<x,y,z,map>; places: {}",
                                      TravelPlaces::Names());
            return false;
        }

        char const* refusal = nullptr;
        MapEntry const* destMap = sMapStore.LookupEntry(dest->mapId);
        if (!TravelGraph::Instance().IsReady())
            refusal = "the travel graph is not ready";
        else if (!PlayerbotFactory::KeepsPossessions(bot))
            refusal = "not a persistent bot (honest travel, random bot, no real master)";
        else if (!botAI->HasStrategy("new rpg", BOT_STATE_NON_COMBAT))
            refusal = "the bot does not run the new rpg strategy";
        else if (!bot->IsAlive())
            refusal = "dead";
        else if (bot->IsInFlight())
            refusal = "in flight";
        else if (bot->GetMap()->Instanceable())
            refusal = "in an instance";
        else if (!destMap || destMap->Instanceable())
            refusal = "the destination is not on a continent";
        if (refusal)
        {
            handler->SendErrorMessage("ROUTE go: {} refused: {}", target.GetName(), refusal);
            return false;
        }

        TravelPlaces::Place const* named = TravelPlaces::Find(text);
        TravelOrder order;
        order.dest = WorldPosition(dest->mapId, dest->x, dest->y, dest->z);
        order.place = named ? named->name : text;
        order.postedMs = getMSTime();
        handler->PSendSysMessage("ROUTE go: {} -> {} ({}, {:.0f}, {:.0f}, {:.0f}) ordered; see [Route] {} travel to {}",
                                 target.GetName(), order.place, dest->mapId, dest->x, dest->y, dest->z,
                                 target.GetName(), order.place);
        TravelOrders::Post(botAI, std::move(order));
        return true;
    }

    static std::string LowerCase(std::string text)
    {
        std::transform(text.begin(), text.end(), text.begin(),
                       [](unsigned char letter) { return static_cast<char>(std::tolower(letter)); });
        return text;
    }

    // A transport link's name: its gameobject's ("Ship (The Bravery)", "Subway", "Undervator")
    static std::string TransportName(uint32 entry)
    {
        GameObjectTemplate const* info = sObjectMgr->GetGameObjectTemplate(entry);
        return info && !info->name.empty() ? info->name : "transport " + std::to_string(entry);
    }

    // `playerbots route transport <bot> <name>|<entry>` (WT19, tests): sends a persistent bot over a transport. The
    // travel graph's transport links of that gameobject entry, or with that part of its name (any case); of them the
    // one starting nearest the bot on its map, else the first. The order's destination is that link's end: the route
    // search picks the way (its [Route] line shows whether it takes the transport). A Deeprun Tram car starts on map
    // 369: from Stormwind or Ironforge name the car's entry
    static bool HandleRouteTransportCommand(ChatHandler* handler, PlayerIdentifier target, Tail name)
    {
        PlayerbotAI* botAI = GetOnlineBotAI(handler, target);
        if (!botAI)
            return false;
        Player* bot = botAI->GetBot();
        TravelGraph const& graph = TravelGraph::Instance();
        char const* refusal = nullptr;
        if (!graph.IsReady())
            refusal = "the travel graph is not ready";
        else if (!PlayerbotFactory::KeepsPossessions(bot))
            refusal = "not a persistent bot (honest travel, random bot, no real master)";
        else if (!botAI->HasStrategy("new rpg", BOT_STATE_NON_COMBAT))
            refusal = "the bot does not run the new rpg strategy";
        else if (!bot->IsAlive())
            refusal = "dead";
        else if (bot->IsInFlight())
            refusal = "in flight";
        else if (bot->GetMap()->Instanceable())
            refusal = "in an instance";
        if (refusal)
        {
            handler->SendErrorMessage("ROUTE transport: {} refused: {}", target.GetName(), refusal);
            return false;
        }

        std::string const text = LowerCase(std::string(name));
        bool const byEntry = !text.empty() && std::all_of(text.begin(), text.end(),
                                                          [](unsigned char letter) { return std::isdigit(letter); });
        uint32 const entry = byEntry ? static_cast<uint32>(std::strtoul(text.c_str(), nullptr, 10)) : 0;
        TravelGraphLink const* chosen = nullptr;
        float chosenYards = std::numeric_limits<float>::max();
        std::vector<uint32> entries;  // every transport of the graph, for the usage line
        for (std::uint32_t i = 0; i < graph.LinkCount(); ++i)
        {
            TravelGraphLink const& link = graph.GetLink(i);
            if (link.type != TravelLinkType::Transport)
                continue;
            if (std::find(entries.begin(), entries.end(), link.object) == entries.end())
                entries.push_back(link.object);
            bool const matches =
                byEntry ? link.object == entry
                        : !text.empty() && LowerCase(TransportName(link.object)).find(text) != std::string::npos;
            if (!matches)
                continue;
            TravelGraphNode const& from = graph.GetNode(link.from);
            float const yards = from.mapId == bot->GetMapId() ? bot->GetExactDist(from.x, from.y, from.z)
                                                              : std::numeric_limits<float>::max();
            if (!chosen || yards < chosenYards)
            {
                chosen = &link;
                chosenYards = yards;
            }
        }
        if (!chosen)
        {
            std::string names;
            for (uint32 known : entries)
                names += (names.empty() ? "" : ", ") + TransportName(known) + " " + std::to_string(known);
            handler->SendErrorMessage("Usage: .playerbots route transport <bot> <name>|<entry>; transports: {}", names);
            return false;
        }

        TravelGraphNode const& from = graph.GetNode(chosen->from);
        TravelGraphNode const& to = graph.GetNode(chosen->to);
        TravelOrder order;
        order.dest = WorldPosition(to.mapId, to.x, to.y, to.z);
        order.place = TransportName(chosen->object);
        order.postedMs = getMSTime();
        handler->PSendSysMessage(
            "ROUTE transport: {} -> {} {} from node {} ({}, {:.0f}, {:.0f}, {:.0f}) to node {} ({}, "
            "{:.0f}, {:.0f}, {:.0f}) ordered; see [Route] {} travel to {}",
            target.GetName(), order.place, chosen->object, from.id, from.mapId, from.x, from.y, from.z, to.id, to.mapId,
            to.x, to.y, to.z, target.GetName(), order.place);
        TravelOrders::Post(botAI, std::move(order));
        return true;
    }

    // `playerbots fight enthusiasm <bot> [set <0..100>]`: the bot's enthusiasm, its thresholds and the cached terms;
    // `set` writes the value (a cheat). The world thread runs commands between map updates, so the bot's AI is read
    // and written here directly
    static bool HandleFightEnthusiasmCommand(ChatHandler* handler, PlayerIdentifier target, Tail rest)
    {
        PlayerbotAI* botAI = GetOnlineBotAI(handler, target);
        return botAI && WorldPvp::HandleEnthusiasmCommand(handler, botAI, std::string_view(rest));
    }

    // `playerbots route stop <bot>`: ends the bot's trip (RPG_TRAVEL -> IDLE) at its next status update
    static bool HandleRouteStopCommand(ChatHandler* handler, PlayerIdentifier target)
    {
        PlayerbotAI* botAI = GetOnlineBotAI(handler, target);
        if (!botAI)
            return false;
        TravelOrder order;
        order.stop = true;
        order.place = "stop";
        order.postedMs = getMSTime();
        TravelOrders::Post(botAI, std::move(order));
        handler->PSendSysMessage("ROUTE stop: {} ordered", target.GetName());
        return true;
    }

    // A test's constraint change (TestConstraints, functional tests): posted like a travel order, so the bot's map
    // thread applies it ahead of the AI's delays and writes a Constraint row
    static void PostConstraints(PlayerbotAI* botAI, uint16 on, uint16 off)
    {
        TravelOrder order;
        order.on = on;
        order.off = off;
        order.postedMs = getMSTime();
        TravelOrders::Post(botAI, std::move(order));
    }

    // `playerbots route constraint <bot> <name>[,<name>..] on|off`: sets or clears any row of `route constraints`
    static bool HandleRouteConstraintCommand(ChatHandler* handler, PlayerIdentifier target, Tail rest)
    {
        PlayerbotAI* botAI = GetOnlineBotAI(handler, target);
        if (!botAI)
            return false;
        std::string_view text(rest);
        while (!text.empty() && text.back() == ' ')
            text.remove_suffix(1);
        size_t const cut = text.find_last_of(' ');
        std::string_view const state = cut == std::string_view::npos ? std::string_view() : text.substr(cut + 1);
        bool const on = state == "on";
        TestConstraints::Parsed const parsed =
            TestConstraints::Parse(cut == std::string_view::npos ? std::string_view() : text.substr(0, cut));
        if ((!on && state != "off") || (!parsed.set && parsed.bad.empty()))
        {
            handler->SendErrorMessage("Usage: .playerbots route constraint <bot> <name>[,<name>..] on|off; names: {}",
                                      TestConstraints::Names());
            return false;
        }
        if (!parsed.set)
        {
            handler->SendErrorMessage("ROUTE constraint: unknown '{}' ({})", parsed.bad, TestConstraints::Names());
            return false;
        }
        PostConstraints(botAI, on ? parsed.set : 0, on ? 0 : parsed.set);
        handler->PSendSysMessage("ROUTE constraint: {} {} {} ordered", target.GetName(),
                                 TestConstraints::Format(parsed.set), std::string(state));
        return true;
    }

    // `playerbots route constraints`: the constraint table, a line per row (the runner and the docs take names here)
    static bool HandleRouteConstraintsCommand(ChatHandler* handler)
    {
        for (TestConstraints::Row const& row : TestConstraints::ROWS)
            handler->PSendSysMessage("ROUTE constraint {} {} {}", row.name, TestConstraints::KindName(row.kind),
                                     row.where);
        handler->PSendSysMessage("ROUTE constraints: {}", std::size(TestConstraints::ROWS));
        return true;
    }

    // `playerbots route hold <bot>`: the four choice rows on; the bot ends its status and chooses nothing on its own
    // (no planner goal, random status, errand, inn bind or hearth); orders still run, then it holds again
    static bool HandleRouteHoldCommand(ChatHandler* handler, PlayerIdentifier target)
    {
        PlayerbotAI* botAI = GetOnlineBotAI(handler, target);
        if (!botAI)
            return false;
        PostConstraints(botAI, TestConstraints::HOLD, 0);
        handler->PSendSysMessage("ROUTE hold: {} ordered", target.GetName());
        return true;
    }

    // `playerbots route release <bot> [planner|status|errand|hearth ...]`: the named choices stay off, the other choice
    // rows go; none named = every choice row goes. State rows stay as they are
    static bool HandleRouteReleaseCommand(ChatHandler* handler, PlayerIdentifier target, Tail choices)
    {
        PlayerbotAI* botAI = GetOnlineBotAI(handler, target);
        if (!botAI)
            return false;
        TestConstraints::Parsed const parsed = TestConstraints::Parse(std::string_view(choices));
        uint16 const states = parsed.set & TestConstraints::STATES;
        if (!parsed.bad.empty() || states)
        {
            handler->SendErrorMessage("ROUTE release: unknown choice '{}' ({})",
                                      parsed.bad.empty() ? TestConstraints::Format(states) : parsed.bad,
                                      TestConstraints::Format(TestConstraints::CHOICES));
            return false;
        }
        PostConstraints(botAI, parsed.set, static_cast<uint16>(TestConstraints::HOLD & ~parsed.set));
        handler->PSendSysMessage("ROUTE release: {} ordered (kept off: {})", target.GetName(),
                                 TestConstraints::Format(parsed.set));
        return true;
    }

    // `playerbots route aboard <bot> <entry>` (tests): stands the bot on the deck of that docked ship or zeppelin on
    // its map, as its passenger; the outcome is the world-log row Aboard placed|failed|refused:<why>
    static bool HandleRouteAboardCommand(ChatHandler* handler, PlayerIdentifier target, uint32 entry)
    {
        PlayerbotAI* botAI = GetOnlineBotAI(handler, target);
        if (!botAI)
            return false;
        if (!entry)
        {
            handler->SendErrorMessage("Usage: .playerbots route aboard <bot> <transport entry>");
            return false;
        }
        TravelOrder order;
        order.aboard = entry;
        order.postedMs = getMSTime();
        TravelOrders::Post(botAI, std::move(order));
        handler->PSendSysMessage("ROUTE aboard: {} on {} ordered", target.GetName(), entry);
        return true;
    }

    // `playerbots route links transport|flight`: the loaded travel graph's links of that kind, a line each (the test
    // runner's case generators read them). World thread: the graph is immutable once ready
    static bool HandleRouteLinksCommand(ChatHandler* handler, Tail kind)
    {
        std::string_view const type(kind);
        bool const flights = type == "flight";
        if (!flights && type != "transport")
        {
            handler->SendErrorMessage("Usage: .playerbots route links transport|flight");
            return false;
        }
        TravelGraph const& graph = TravelGraph::Instance();
        if (!graph.IsReady())
        {
            handler->SendErrorMessage("ROUTE links: the travel graph is not ready");
            return false;
        }
        TravelLinkType const wanted = flights ? TravelLinkType::Flight : TravelLinkType::Transport;
        uint32 count = 0;
        for (std::uint32_t i = 0; i < graph.LinkCount(); ++i)
        {
            TravelGraphLink const& link = graph.GetLink(i);
            if (link.type != wanted)
                continue;
            TravelGraphNode const& from = graph.GetNode(link.from);
            TravelGraphNode const& to = graph.GetNode(link.to);
            if (flights)
            {
                TravelTaxiPath const* path = graph.TaxiPathOf(link);
                if (!path)
                    continue;
                bool const alliance = path->teams & TravelGraph::TAXI_ALLIANCE;
                bool const horde = path->teams & TravelGraph::TAXI_HORDE;
                char const* side = alliance && horde ? "N" : alliance ? "A" : horde ? "H" : "-";
                handler->PSendSysMessage(
                    "ROUTE link flight {} {} {} {} {} {} {:.1f} {:.1f} {:.1f} {} {} {:.1f} {:.1f} {:.1f}", link.object,
                    path->from, path->to, side, from.id, from.mapId, from.x, from.y, from.z, to.id, to.mapId, to.x,
                    to.y, to.z);
            }
            else
            {
                TransportTemplate const* motion = sTransportMgr->GetTransportTemplate(link.object);
                uint32 const periodSeconds = motion ? motion->pathTime / IN_MILLISECONDS : 0;  // 0: an elevator
                handler->PSendSysMessage(
                    "ROUTE link transport {} {} {} {} {:.1f} {:.1f} {:.1f} {} {} {:.1f} {:.1f} {:.1f}", link.object,
                    periodSeconds, from.id, from.mapId, from.x, from.y, from.z, to.id, to.mapId, to.x, to.y, to.z);
            }
            ++count;
        }
        handler->PSendSysMessage("ROUTE links: {} {}", count, std::string(type));
        return true;
    }

    static bool HandleInspectCommand(ChatHandler* handler, PlayerIdentifier target, Tail command)
    {
        PlayerbotAI* botAI = GetOnlineBotAI(handler, target);
        if (!botAI)
            return false;

        if (command.empty())
        {
            handler->SendErrorMessage(
                "Usage: .playerbots inspect <bot> <query> (state, position, tpos, "
                "movement, target, hp, strategy, action, values, travel, budget, "
                "constraints, hold)");
            return false;
        }

        std::string const result = botAI->HandleRemoteCommand(std::string(command));
        handler->PSendSysMessage("{}: {}", target.GetName(), result.empty() ? "<empty>" : result);
        return true;
    }

    // LC8: the bot's state facts as the conversation bridge reads them (lab tests; the wire carries `around` from
    // LC10): "AROUND <bot> <n> <kind> <detail> x<count> <name>" per entry, then "SERVANT <bot> <name|none>" and
    // "MOUNT <bot> <name|none>", then "DEED <bot> <sentence>" per recent deed (LC7). Console: ReadBotState reads only
    // on the world thread, which a game client's command is not on.
    static bool HandleBridgeStateCommand(ChatHandler* handler, PlayerIdentifier target)
    {
        PlayerbotAI* botAI = GetOnlineBotAI(handler, target);
        if (!botAI)
            return false;

        PlayerbotBridge::BotStateFacts facts;
        if (!PlayerbotBridge::ReadBotState(target.GetGUID(), ObjectGuid::Empty, facts))
        {
            handler->SendErrorMessage("{}: no state (only the console reads it, for a bot in the world)",
                                      target.GetName());
            return false;
        }

        handler->PSendSysMessage("Bridge state of {}: {} around", target.GetName(), facts.around.size());
        for (std::size_t index = 0; index < facts.around.size(); ++index)
        {
            PlayerbotBridge::AroundEntry const& entry = facts.around[index];
            handler->PSendSysMessage("AROUND {} {} {} {} x{} {}", target.GetName(), index + 1,
                                     PlayerbotBridge::AroundKindWord(entry.Kind), entry.Detail, entry.Count,
                                     entry.Name);
        }
        handler->PSendSysMessage("SERVANT {} {}", target.GetName(),
                                 facts.servantName.empty() ? "none" : facts.servantName);
        handler->PSendSysMessage("MOUNT {} {}", target.GetName(), facts.mountName.empty() ? "none" : facts.mountName);
        for (std::string const& deed : facts.deeds)
            handler->PSendSysMessage("DEED {} {}", target.GetName(), deed);
        return true;
    }

    // LC9: the bot's lasting facts as the conversation bridge reads them (lab tests; the wire carries them from LC10):
    // "FACT <bot> phrase <text>", "FACT <bot> craft <name> <rank word>", "FACT <bot> useful <text>", "FACT <bot>
    // servant <name>", "FACT <bot> standing <faction> <word>", "FACT <bot> guild <name|none>", "FACT <bot> relatives
    // none". Console only, like `bridge state`.
    static bool HandleBridgeFactsCommand(ChatHandler* handler, PlayerIdentifier target)
    {
        PlayerbotAI* botAI = GetOnlineBotAI(handler, target);
        if (!botAI)
            return false;

        PlayerbotBridge::BotFacts facts;
        if (!PlayerbotBridge::ReadBotFacts(target.GetGUID(), facts))
        {
            handler->SendErrorMessage("{}: no facts (only the console reads them, for a bot in the world)",
                                      target.GetName());
            return false;
        }

        std::string const& name = target.GetName();
        handler->PSendSysMessage("Bridge facts of {}", name);
        handler->PSendSysMessage("FACT {} phrase {}", name, facts.phrase);
        for (auto const& [craft, rank] : facts.crafts)
            handler->PSendSysMessage("FACT {} craft {} {}", name, craft, rank);
        for (std::string const& useful : facts.useful)
            handler->PSendSysMessage("FACT {} useful {}", name, useful);
        for (std::string const& servant : facts.servants)
            handler->PSendSysMessage("FACT {} servant {}", name, servant);
        for (auto const& [faction, word] : facts.standing)
            handler->PSendSysMessage("FACT {} standing {} {}", name, faction, word);
        handler->PSendSysMessage("FACT {} guild {}", name, facts.guild.empty() ? "none" : facts.guild);
        handler->PSendSysMessage("FACT {} relatives {}", name, facts.relatives.empty() ? "none" : "some");
        return true;
    }

    // LC6, a test fixture (only a test calls it): the bot learns the flight point of its nearest flight master within
    // FLIGHT_MASTER_YARDS through the engine's own path, WorldSession::SendLearnNewTaxiNode, which fires
    // OnPlayerLearnTaxiNode (the world log's TaxiNode row and the record's flight row). A point the bot knew is
    // forgotten first, so that it is learnt now. "FLIGHT <bot>: ok, node N at <flight master>, was known 0|1", else an
    // error. Console: the world thread, maps idle.
    static bool HandleBridgeLearnFlightCommand(ChatHandler* handler, PlayerIdentifier target)
    {
        PlayerbotAI* botAI = GetOnlineBotAI(handler, target);
        if (!botAI)
            return false;

        PLAYERBOT_CHECK_WORLD_THREAD("HandleBridgeLearnFlightCommand");
        constexpr float FLIGHT_MASTER_YARDS = 30.0f;
        Player* const bot = botAI->GetBot();
        std::list<Unit*> units;
        Acore::AnyUnitInObjectRangeCheck check(bot, FLIGHT_MASTER_YARDS);
        Acore::UnitListSearcher<Acore::AnyUnitInObjectRangeCheck> searcher(bot, units, check);
        Cell::VisitObjects(bot, searcher, FLIGHT_MASTER_YARDS);
        Creature* master = nullptr;
        for (Unit* const unit : units)
        {
            Creature* const creature = unit->ToCreature();
            if (!creature || !creature->IsAlive() || !(creature->GetNpcFlags() & UNIT_NPC_FLAG_FLIGHTMASTER))
                continue;
            if (!master || bot->GetDistance(creature) < bot->GetDistance(master))
                master = creature;
        }
        if (!master)
        {
            handler->SendErrorMessage("FLIGHT {}: error, no flight master within {} yd", bot->GetName(),
                                      static_cast<uint32>(FLIGHT_MASTER_YARDS));
            return false;
        }

        uint32 const node = sObjectMgr->GetNearestTaxiNode(*master, bot->GetTeamId(true));
        if (!node)
        {
            handler->SendErrorMessage("FLIGHT {}: error, {} has no flight point of the bot's side", bot->GetName(),
                                      master->GetName());
            return false;
        }

        bool const known = bot->m_taxi.IsTaximaskNodeKnown(node);
        if (known)
        {
            // The mask as the character saves it, one number per field, without this node's bit
            std::ostringstream saved;
            saved << bot->m_taxi;
            std::string const fields = saved.str();
            std::string mask;
            uint32 field = 0;
            for (std::string_view const token : Acore::Tokenize(fields, ' ', false))
            {
                uint32 value = Acore::StringTo<uint32>(token).value_or(0);
                if (field == (node - 1) / 32)
                    value &= ~(1u << ((node - 1) % 32));
                mask += Acore::StringFormat("{}{}", field ? " " : "", value);
                ++field;
            }
            bot->m_taxi.LoadTaxiMask(mask);
        }

        bot->GetSession()->SendLearnNewTaxiNode(master);
        if (!bot->m_taxi.IsTaximaskNodeKnown(node))
        {
            handler->SendErrorMessage("FLIGHT {}: error, node {} at {} not learnt", bot->GetName(), node,
                                      master->GetName());
            return false;
        }

        handler->PSendSysMessage("FLIGHT {}: ok, node {} at {}, was known {}", bot->GetName(), node, master->GetName(),
                                 known ? 1 : 0);
        return true;
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
