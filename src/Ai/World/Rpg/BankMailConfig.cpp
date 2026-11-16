/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "BankMailConfig.h"

#include <array>

#include "ConfigTable.h"

namespace
{
using Row = ConfigRow<BankMailConfig>;

constexpr std::array<Row, 6> BankMailRows = {{
    {"Mail.DefaultWorthCopper", &BankMailConfig::MailDefaultWorthCopper, 0, 10000},
    {"Bank.KeepLevelHorizon", &BankMailConfig::KeepLevelHorizon, 0, 20},
    {"Mail.VisitDeadlineSeconds", &BankMailConfig::MailVisitDeadlineSeconds, 60, 3600},
    {"Bank.VisitDeadlineSeconds", &BankMailConfig::BankVisitDeadlineSeconds, 60, 3600},
    {"VisitStepSeconds", &BankMailConfig::VisitStepSeconds, 1, 60},
    {"Mail.PerVisit", &BankMailConfig::MailsPerVisit, 1, 100},
}};
}  // namespace

ConfigFamily<BankMailConfig> const BankMailFamily{"Playerbots.BankMail.", BankMailRows};
