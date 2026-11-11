/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "BankCommands.h"

#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "BankMailState.h"
#include "BankRulesInput.h"
#include "BankRulesSelftest.h"
#include "BankVisit.h"
#include "Chat.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotFactory.h"
#include "Playerbots.h"

namespace BankMail
{
namespace
{
// The command lists at most this many kept items and this many verdicts, one line each
constexpr size_t LINES_SHOWN = 20;

std::string ItemName(uint32 entry)
{
    ItemTemplate const* proto = sObjectMgr->GetItemTemplate(entry);
    return proto ? proto->Name1 : std::string("?");
}
}  // namespace

bool HandleBankStatusCommand(ChatHandler* handler, char const* args)
{
    std::istringstream in(args ? args : "");
    std::string name;
    in >> name;
    if (name.empty())
    {
        handler->SendErrorMessage("Usage: playerbots bank status <bot name>");
        return false;
    }
    std::string const asked = name;
    Player* bot = normalizePlayerName(name) ? ObjectAccessor::FindPlayerByName(name) : nullptr;
    PlayerbotAI* botAI = bot && bot->IsInWorld() ? GET_PLAYERBOT_AI(bot) : nullptr;
    if (!botAI)
    {
        handler->SendErrorMessage("BANK: no bot {} online", asked);
        return false;
    }

    // Read only, on the world thread between the map updates: the rule is applied to the bags and the bank now and
    // printed beside what the bot knows (BankKnowledge, rebuilt at login and after a visit)
    BankIn const bank = BuildBankIn(botAI);
    std::vector<KeptEntry> const kept = KeptInBank(bank);
    uint32 dueCount = 0;
    uint32 dueWorth = 0;
    for (KeptEntry const& entry : kept)
    {
        if (entry.dueLevel > bank.bot.level)
            continue;
        dueCount += entry.count;
        dueWorth += entry.laterWorth;
    }
    BankKnowledge const& known = botAI->rpgInfo.bankMail.bank;
    handler->PSendSysMessage(
        "BANK {}: kept {} (due {}, worth {}), free slots {}/{}, level {}, keeps possessions {}; "
        "known: {} kept, {} due, {} free{}",
        bot->GetName(), kept.size(), dueCount, dueWorth, bank.bankSpace.freeSlots, BANK_ITEM_SLOTS, bank.bot.level,
        PlayerbotFactory::KeepsPossessions(bot) ? "yes" : "no", known.kept.size(), known.dueCount, known.freeSlots,
        known.built ? "" : " (not built)");
    for (size_t i = 0; i < kept.size() && i < LINES_SHOWN; ++i)
        handler->PSendSysMessage("  kept {} {} x{}: later {} at level {} ({})", kept[i].entry, ItemName(kept[i].entry),
                                 kept[i].count, kept[i].laterWorth, kept[i].dueLevel, kept[i].provider);
    if (kept.size() > LINES_SHOWN)
        handler->PSendSysMessage("  ... {} more kept", kept.size() - LINES_SHOWN);

    BankPlan const plan = PlanBankVisit(bank);
    handler->PSendSysMessage("  bags: {} free slots; at a banker now: {}", bank.bagSpace.freeSlots, DescribePlan(plan));
    for (size_t i = 0; i < plan.verdicts.size() && i < LINES_SHOWN; ++i)
        handler->PSendSysMessage("  {} ({})", DescribeVerdict(plan.verdicts[i]), ItemName(plan.verdicts[i].entry));
    if (plan.verdicts.size() > LINES_SHOWN)
        handler->PSendSysMessage("  ... {} more", plan.verdicts.size() - LINES_SHOWN);
    return true;
}

bool HandleBankVisitCommand(ChatHandler* handler, char const* args)
{
    std::istringstream in(args ? args : "");
    std::string name;
    in >> name;
    if (name.empty())
    {
        handler->PSendSysMessage("BANK visits: {} (usage: playerbots bank visit <bot name>)", BankVisitCounters());
        return true;
    }
    std::string const asked = name;
    Player* bot = normalizePlayerName(name) ? ObjectAccessor::FindPlayerByName(name) : nullptr;
    PlayerbotAI* botAI = bot && bot->IsInWorld() ? GET_PLAYERBOT_AI(bot) : nullptr;
    if (!botAI)
    {
        handler->SendErrorMessage("BANK visit: no bot {} online", asked);
        return false;
    }

    // Lab fixtures, arena bots and bots with a real master keep today's behaviour (the chat command `bank`); a bot
    // runs one visit at a time
    char const* refusal = nullptr;
    if (!PlayerbotFactory::KeepsPossessions(bot))
        refusal = "the bot does not keep its possessions (not a persistent bot)";
    else if (!bot->IsAlive())
        refusal = "the bot is dead";
    else if (!botAI->HasStrategy("new rpg", BOT_STATE_NON_COMBAT))
        refusal = "the bot has no new rpg strategy";
    else if (botAI->rpgInfo.bankMail.bankVisit.phase != BankVisitPhase::None)
        refusal = "a visit runs already";
    else if (botAI->rpgInfo.bankMail.mailVisit.phase != MailVisitPhase::None)
        refusal = "a mailbox visit runs";
    // World thread, between the map updates: the bot's place and its visit state are read and set at rest
    std::optional<BankerSpot> const spot = refusal ? std::nullopt : FindBankerSpot(bot, refusal);
    if (!spot || !BeginBankVisit(botAI, *spot))
    {
        handler->SendErrorMessage("BANK visit {}: refused, {}", bot->GetName(), refusal ? refusal : "no visit");
        return false;
    }

    handler->PSendSysMessage("BANK visit {}: started at ({:.0f}, {:.0f}) on map {}, {:.0f} yd away", bot->GetName(),
                             spot->x, spot->y, spot->mapId, spot->distance);
    return true;
}

bool HandleBankSelftestCommand(ChatHandler* handler, char const* /*args*/)
{
    std::vector<std::string> lines;
    BankSelftestDetail::Result const result = RunBankRulesSelftest(lines);
    for (std::string const& line : lines)
        if (line.rfind("FAIL:", 0) == 0)
            handler->PSendSysMessage("BANK selftest {}", line);
    handler->PSendSysMessage("{}", lines.back());
    LOG_INFO("playerbots", "{}", lines.back());
    return !result.failed;
}
}  // namespace BankMail
