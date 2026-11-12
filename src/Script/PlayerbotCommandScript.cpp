/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <limits>

#include "AcquaintanceMgr.h"
#include "ActivityBudget.h"
#include "ArenaRecruit.h"
#include "ArenaTeamFormation.h"
#include "BattleGroundTactics.h"
#include "BotProfileMgr.h"
#include "BotTraitMgr.h"
#include "Chat.h"
#include "ChatCommandTags.h"
#include "DBCStores.h"
#include "FightBook.h"
#include "GuildTaskMgr.h"
#include "HoldRules.h"
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
#include "ScriptMgr.h"
#include "Tactics.h"
#include "TestPlayer.h"
#include "TravelGraph.h"
#include "TravelPlaces.h"
#include "WorldLog.h"
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

        static ChatCommandTable playerbotsRouteCommandTable = {
            {"status", TravelGraph::HandleStatusCommand, SEC_GAMEMASTER, Console::Yes},
            {"selftest", TravelGraph::HandleSelftestCommand, SEC_GAMEMASTER, Console::Yes},
            {"go", HandleRouteGoCommand, SEC_GAMEMASTER, Console::Yes},
            {"stop", HandleRouteStopCommand, SEC_GAMEMASTER, Console::Yes},
            {"transport", HandleRouteTransportCommand, SEC_GAMEMASTER, Console::Yes},
            {"hold", HandleRouteHoldCommand, SEC_GAMEMASTER, Console::Yes},
            {"release", HandleRouteReleaseCommand, SEC_GAMEMASTER, Console::Yes},
        };

        static ChatCommandTable playerbotsPlannerCommandTable = {
            {"status", Planner::HandleStatusCommand, SEC_GAMEMASTER, Console::Yes},
            {"selftest", Planner::HandleSelftestCommand, SEC_GAMEMASTER, Console::Yes},
            {"indexes", Planner::HandleIndexesCommand, SEC_GAMEMASTER, Console::Yes},
            {"hub", Planner::HandleHubCommand, SEC_GAMEMASTER, Console::Yes},
            {"service", Planner::HandleServiceCommand, SEC_GAMEMASTER, Console::Yes},
            {"crowd", Planner::HandleCrowdCommand, SEC_GAMEMASTER, Console::Yes},
            {"quest", Planner::HandleQuestCommand, SEC_GAMEMASTER, Console::Yes},
            {"circuit", Planner::HandleCircuitCommand, SEC_GAMEMASTER, Console::Yes},
            {"goals", Planner::HandleGoalsCommand, SEC_GAMEMASTER, Console::Yes},
            {"goal", Planner::HandleGoalCommand, SEC_GAMEMASTER, Console::Yes},
        };

        static ChatCommandTable playerbotsWorldLogCommandTable = {
            {"selftest", WorldLog::HandleSelftestCommand, SEC_GAMEMASTER, Console::Yes},
            {"case", WorldLog::HandleCaseCommand, SEC_GAMEMASTER, Console::Yes},
            {"status", WorldLog::HandleStatusCommand, SEC_GAMEMASTER, Console::Yes},
        };

        // World PvP (WP1): list and bot read; ready and meet are lab fixtures (Playerbots.WorldPvp.TestCommands)
        static ChatCommandTable playerbotsFightCommandTable = {
            {"list", HandleFightListCommand, SEC_PLAYER, Console::Yes},
            {"bot", HandleFightBotCommand, SEC_PLAYER, Console::Yes},
            {"ready", HandleFightReadyCommand, SEC_GAMEMASTER, Console::Yes},
            {"meet", HandleFightMeetCommand, SEC_GAMEMASTER, Console::Yes},
        };

        static ChatCommandTable playerbotsCommandTable = {
            {"bot", HandlePlayerbotCommand, SEC_PLAYER, Console::No},
            {"exec", HandleExecCommand, SEC_GAMEMASTER, Console::Yes},
            {"inspect", HandleInspectCommand, SEC_GAMEMASTER, Console::Yes},
            {"gtask", HandleGuildTaskCommand, SEC_GAMEMASTER, Console::Yes},
            {"pmon", HandlePerfMonCommand, SEC_GAMEMASTER, Console::Yes},
            {"worldthread", WorldThreadMonitor::HandleCommand, SEC_GAMEMASTER, Console::Yes},
            {"rndbot", HandleRandomPlayerbotCommand, SEC_GAMEMASTER, Console::Yes},
            {"skill", BotProfileMgr::HandleSkillCommand, SEC_GAMEMASTER, Console::Yes},
            {"traits", BotTraitMgr::HandleTraitsCommand, SEC_GAMEMASTER, Console::Yes},
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
            {"worldlog", playerbotsWorldLogCommandTable},
            {"fight", playerbotsFightCommandTable},
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
        TravelOrders::Post(bot->GetGUID(), std::move(order));
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
        TravelOrders::Post(bot->GetGUID(), std::move(order));
        return true;
    }

    // `playerbots fight list`: the active fights on the GM's map (every open-world map from the console)
    static bool HandleFightListCommand(ChatHandler* handler) { return WorldPvp::HandleListCommand(handler); }

    // `playerbots fight bot <bot>`: the bot's fight memory, answered by the bot's next update
    static bool HandleFightBotCommand(ChatHandler* handler, PlayerIdentifier target)
    {
        PlayerbotAI* botAI = GetOnlineBotAI(handler, target);
        return botAI && WorldPvp::HandleBotCommand(handler, botAI);
    }

    // `playerbots fight ready <bot>` (labs): the bot's next chance starts a fight when the hard rules pass (WP4)
    static bool HandleFightReadyCommand(ChatHandler* handler, PlayerIdentifier target)
    {
        PlayerbotAI* botAI = GetOnlineBotAI(handler, target);
        return botAI && WorldPvp::HandleReadyCommand(handler, botAI);
    }

    // `playerbots fight meet <bot> <enemy bot>` (labs): places the enemy bot beside the bot, PvP flagged, no attack
    static bool HandleFightMeetCommand(ChatHandler* handler, PlayerIdentifier target, PlayerIdentifier enemy)
    {
        PlayerbotAI* botAI = GetOnlineBotAI(handler, target);
        PlayerbotAI* enemyAI = botAI ? GetOnlineBotAI(handler, enemy) : nullptr;
        return enemyAI && WorldPvp::HandleMeetCommand(handler, botAI, enemyAI);
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
        TravelOrders::Post(botAI->GetBot()->GetGUID(), std::move(order));
        handler->PSendSysMessage("ROUTE stop: {} ordered", target.GetName());
        return true;
    }

    // A test's hold (HoldRules, functional tests): posted like a travel order, so the bot's map thread applies it
    static void PostHold(PlayerbotAI* botAI, uint8 mask)
    {
        TravelOrder order;
        order.hold = mask;
        order.postedMs = getMSTime();
        TravelOrders::Post(botAI->GetBot()->GetGUID(), std::move(order));
    }

    // `playerbots route hold <bot>`: the bot ends its status and chooses nothing on its own (no planner goal, random
    // status, errand, inn bind or hearth); orders (`route go|transport|stop`) still run, then it holds again
    static bool HandleRouteHoldCommand(ChatHandler* handler, PlayerIdentifier target)
    {
        PlayerbotAI* botAI = GetOnlineBotAI(handler, target);
        if (!botAI)
            return false;
        PostHold(botAI, HoldRules::ALL);
        handler->PSendSysMessage("ROUTE hold: {} ordered", target.GetName());
        return true;
    }

    // `playerbots route release <bot> [planner|status|errand|hearth ...]`: the named choices stay off; none = full
    static bool HandleRouteReleaseCommand(ChatHandler* handler, PlayerIdentifier target, Tail choices)
    {
        PlayerbotAI* botAI = GetOnlineBotAI(handler, target);
        if (!botAI)
            return false;
        uint8 mask = 0;
        std::string_view rest(choices);
        while (!rest.empty())
        {
            size_t const start = rest.find_first_not_of(' ');
            if (start == std::string_view::npos)
                break;
            rest.remove_prefix(start);
            size_t const end = rest.find(' ');
            std::string_view const word = rest.substr(0, end);
            uint8 const bit = HoldRules::ParseChoice(word);
            if (!bit)
            {
                handler->SendErrorMessage("ROUTE release: unknown choice '{}' ({})", std::string(word),
                                          HoldRules::CHOICE_NAMES);
                return false;
            }
            mask |= bit;
            rest.remove_prefix(end == std::string_view::npos ? rest.size() : end);
        }
        PostHold(botAI, mask);
        handler->PSendSysMessage("ROUTE release: {} ordered (kept off: {})", target.GetName(), HoldRules::Format(mask));
        return true;
    }

    static bool HandleInspectCommand(ChatHandler* handler, PlayerIdentifier target, Tail command)
    {
        PlayerbotAI* botAI = GetOnlineBotAI(handler, target);
        if (!botAI)
            return false;

        if (command.empty())
        {
            handler->SendErrorMessage("Usage: .playerbots inspect <bot> <query> (state, position, tpos, "
                                      "movement, target, hp, strategy, action, values, travel, budget)");
            return false;
        }

        std::string const result = botAI->HandleRemoteCommand(std::string(command));
        handler->PSendSysMessage("{}: {}", target.GetName(), result.empty() ? "<empty>" : result);
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
