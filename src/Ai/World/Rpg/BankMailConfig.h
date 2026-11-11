/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_BANKMAILCONFIG_H
#define _PLAYERBOT_BANKMAILCONFIG_H

#include "Define.h"

// The numbers of persistent bots' mail and bank visits (Playerbots.BankMail.*), read once at config load
// (PlayerbotAIConfig::BankMail); the initial values are the conf.dist defaults. Kept apart from PlayerbotAIConfig.h
// so that later members do not rebuild every user of that header. The knowledge they act on is in BankMailState.h.
struct BankMailConfig
{
    // Copper a bot expects from one unread mail whose content it does not know
    uint32 MailDefaultWorthCopper = 100;
    // A bank item counts as kept for later while the level it is due at is at most this many levels above the bot's
    uint32 KeepLevelHorizon = 5;
    // Seconds a mailbox or bank visit (the walk included) may take before it is given up
    uint32 MailVisitDeadlineSeconds = 900;
    uint32 BankVisitDeadlineSeconds = 900;
    // Seconds one step of a visit (a take from a mail, a deposit) may wait for the server's answer
    uint32 VisitStepSeconds = 10;
    // Mails one visit opens at most (the client's inbox shows 50)
    uint32 MailsPerVisit = 50;
};

#endif
