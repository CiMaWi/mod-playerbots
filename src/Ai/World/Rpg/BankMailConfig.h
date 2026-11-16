/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_BANKMAILCONFIG_H
#define _PLAYERBOT_BANKMAILCONFIG_H

#include "Define.h"

template <typename Struct>
struct ConfigFamily;

// The numbers of persistent bots' mail and bank visits (Playerbots.BankMail.*), read once at config load
// (PlayerbotAIConfig::BankMail) through the rows in BankMailConfig.cpp; the initial values are the defaults. Kept apart
// from PlayerbotAIConfig.h so that later members do not rebuild every user of that header. The knowledge they act on is
// in BankMailState.h. A bot knows that mail waits and what it expects from it, not what is inside.
struct BankMailConfig
{
    // Mail.DefaultWorthCopper (0-10000): copper a bot expects from one unread mail whose content it does not know
    uint32 MailDefaultWorthCopper = 100;
    // Bank.KeepLevelHorizon (0-20): a bank item counts as kept for later while the level it is due at is at most this
    // many levels above the bot's
    uint32 KeepLevelHorizon = 5;
    // Mail.VisitDeadlineSeconds, Bank.VisitDeadlineSeconds (60-3600): seconds a mailbox or bank visit (the walk
    // included) may take before it is given up
    uint32 MailVisitDeadlineSeconds = 900;
    uint32 BankVisitDeadlineSeconds = 900;
    // VisitStepSeconds (1-60): seconds one step of a visit (a take from a mail, a deposit) may wait for the server's
    // answer
    uint32 VisitStepSeconds = 10;
    // Mail.PerVisit (1-100): mails one mailbox visit opens at most (the client's inbox shows 50)
    uint32 MailsPerVisit = 50;
};

extern ConfigFamily<BankMailConfig> const BankMailFamily;

#endif
