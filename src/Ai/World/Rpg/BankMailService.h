/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_BANKMAILSERVICE_H
#define _PLAYERBOT_BANKMAILSERVICE_H

#include "BankMailState.h"
#include "BankRules.h"
#include "Define.h"
#include "VisitLifecycle.h"

class Player;

// What reads or steps a bot's bank and mail state (BankMailState.h) with the rules or the game: the visit
// refusal, the errand test, the reset, the mail count and the watch's facts. Apart from the state so that
// NewRpgInfo.h holds the state without the bank and mail rules.
namespace BankMail
{
// One visit at a time per bot: why a visit of the kind may not start now, or nullptr
inline char const* VisitStartRefusal(BotBankMail const& state, VisitKind kind)
{
    return VisitStartRefusal(kind, state.mailVisit.phase != MailVisitPhase::None,
                             state.bankVisit.phase != BankVisitPhase::None,
                             state.craftVisit.phase != CraftVisitPhase::None);
}

// What can start a trip without full bags (WB8b, the errand trigger): worth the bot saw or expects in its mail, or a
// kept item come due at the level. Unopened mail alone never pays a detour (WB.4), so it only joins trips
inline bool ErrandWaits(BotBankMail const& state, uint8 level)
{
    return state.mail.seenCopper || state.mail.expectedCopper || DueAt(state.bank.kept, level).count;
}

// A far teleport or a PlayerbotAI::Reset(true) renews NewRpgInfo (RpgStatus::Reset keeps only the route
// requests). The bot's knowledge (waiting mail, seen worth, bank slots, kept items) survives it; a running visit
// was ended with its row just before (EndVisits), so nothing of it is left to keep.
inline BotBankMail AfterReset(BotBankMail const& before)
{
    BotBankMail after;
    after.mail = before.mail;
    after.bank = before.bank;
    return after;
}

// Mail list reads (Player::GetMails) happen only on the bot's own map thread inside its AI update or on the world
// thread; never on a planner worker, never for another bot from a map thread, and no Mail* is kept past the call.
uint16 CountWaitingMail(Player const* bot);
void RefreshMailKnowledge(Player const* bot, MailKnowledge& mail);

// The watch's facts about a visit to a spot on mapId that started at startedMs
VisitWatchIn WatchInOf(Player const* bot, uint32 mapId, uint32 startedMs, uint32 limitMs);
}  // namespace BankMail

#endif
