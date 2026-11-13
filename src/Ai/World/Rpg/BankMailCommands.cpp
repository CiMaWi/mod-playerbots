/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "BankMailCommands.h"

#include <sstream>
#include <string>
#include <vector>

#include "BankMailState.h"
#include "Chat.h"
#include "Log.h"
#include "MailRulesInput.h"
#include "MailRulesSelftest.h"
#include "MailVisit.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "PlayerbotFactory.h"
#include "Playerbots.h"
#include "Timer.h"

namespace BankMail
{
namespace
{
// The command lists at most this many mails, one line each
constexpr size_t MAILS_SHOWN = 20;
}  // namespace

bool HandleMailStatusCommand(ChatHandler* handler, char const* args)
{
    std::istringstream in(args ? args : "");
    std::string name;
    in >> name;
    if (name.empty())
    {
        handler->SendErrorMessage("Usage: playerbots mail status <bot name>");
        return false;
    }
    std::string const asked = name;
    Player* bot = normalizePlayerName(name) ? ObjectAccessor::FindPlayerByName(name) : nullptr;
    PlayerbotAI* botAI = bot && bot->IsInWorld() ? GET_PLAYERBOT_AI(bot) : nullptr;
    if (!botAI)
    {
        handler->SendErrorMessage("MAIL: no bot {} online", asked);
        return false;
    }

    // Read only, on the world thread (BankMailState.h's rule for the mail list): the knowledge is printed as stored,
    // the waiting count is also counted again now
    MailKnowledge const& known = botAI->rpgInfo.bankMail.mail;
    BankMailConfig const& config = sPlayerbotAIConfig.BankMail;
    uint32 const errand = MailErrandWorth(known.waiting, config.MailDefaultWorthCopper, known.seenCopper,
                                          known.expectedCopper, DEFAULT_ORDERLINESS_WEIGHT);
    handler->PSendSysMessage(
        "MAIL {}: waiting {} (counted now {}, core unread {}), seen {}, expected {}, errand worth {}, {} mails in the "
        "list, refreshed {}, keeps possessions {}",
        bot->GetName(), known.waiting, CountWaitingMail(bot), bot->unReadMails, known.seenCopper, known.expectedCopper,
        errand, bot->GetMails().size(),
        known.refreshedMs ? std::to_string(getMSTimeDiff(known.refreshedMs, getMSTime()) / IN_MILLISECONDS) + " s ago"
                          : std::string("never"),
        PlayerbotFactory::KeepsPossessions(bot) ? "yes" : "no");

    BagSpace const space = BuildBagSpace(bot);
    VisitPlan const plan = PlanMailVisit(BuildMailIn(botAI), space, BuildMailOptions());
    handler->PSendSysMessage("  bags: {} free slots, {} partial stacks; at a mailbox now: {}", space.freeSlots,
                             space.stackRoom.size(), DescribePlan(plan));
    for (size_t i = 0; i < plan.verdicts.size() && i < MAILS_SHOWN; ++i)
        handler->PSendSysMessage("  {}", DescribeVerdict(plan.verdicts[i]));
    if (plan.verdicts.size() > MAILS_SHOWN)
        handler->PSendSysMessage("  ... {} more mails", plan.verdicts.size() - MAILS_SHOWN);
    return true;
}

bool HandleMailVisitCommand(ChatHandler* handler, char const* args)
{
    std::istringstream in(args ? args : "");
    std::string name;
    in >> name;
    if (name.empty())
    {
        handler->PSendSysMessage("MAIL visits: {} (usage: playerbots mail visit <bot name>)", MailVisitCounters());
        return true;
    }
    std::string const asked = name;
    Player* bot = normalizePlayerName(name) ? ObjectAccessor::FindPlayerByName(name) : nullptr;
    PlayerbotAI* botAI = bot && bot->IsInWorld() ? GET_PLAYERBOT_AI(bot) : nullptr;
    if (!botAI)
    {
        handler->SendErrorMessage("MAIL visit: no bot {} online", asked);
        return false;
    }

    // Lab fixtures, arena bots and bots with a real master keep today's behaviour (the chat command `mail`)
    char const* refusal = nullptr;
    if (!PlayerbotFactory::KeepsPossessions(bot))
        refusal = "the bot does not keep its possessions (not a persistent bot)";
    else if (!bot->IsAlive())
        refusal = "the bot is dead";
    else if (!botAI->HasStrategy("new rpg", BOT_STATE_NON_COMBAT))
        refusal = "the bot has no new rpg strategy";
    else
        refusal = VisitStartRefusal(botAI->rpgInfo.bankMail, VisitKind::Mail);
    // World thread, between the map updates: the bot's place and its visit state are read and set at rest
    std::optional<MailboxSpot> const spot = refusal ? std::nullopt : FindMailboxSpot(bot, refusal);
    // A test command (WB.12): the bot stands near a mailbox; getting to a far one is the travel errand's job
    if (spot && !WithinVisitReach(spot->distance))
    {
        handler->SendErrorMessage("MAIL visit {}: refused, {} (the nearest mailbox is {:.0f} yd away, reach {:.0f} yd)",
                                  bot->GetName(), VISIT_TOO_FAR, spot->distance, VISIT_COMMAND_REACH_YARDS);
        return false;
    }
    if (!spot || !BeginMailVisit(botAI, *spot))
    {
        handler->SendErrorMessage("MAIL visit {}: refused, {}", bot->GetName(), refusal ? refusal : "no visit");
        return false;
    }

    handler->PSendSysMessage("MAIL visit {}: started at ({:.0f}, {:.0f}) on map {}, {:.0f} yd away", bot->GetName(),
                             spot->x, spot->y, spot->mapId, spot->distance);
    return true;
}

bool HandleMailSelftestCommand(ChatHandler* handler, char const* /*args*/)
{
    std::vector<std::string> lines;
    MailSelftestDetail::Result const result = RunMailRulesSelftest(lines);
    for (std::string const& line : lines)
        if (line.rfind("FAIL:", 0) == 0)
            handler->PSendSysMessage("MAIL selftest {}", line);
    handler->PSendSysMessage("{}", lines.back());
    LOG_INFO("playerbots", "{}", lines.back());
    return !result.failed;
}
}  // namespace BankMail
