/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "Upkeep.h"

#include "BotKind.h"
#include "EventLog.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"

bool Upkeep::Granted(Player* bot, UpkeepKind kind) { return Granted(bot, kind, BotKind::KeepsPossessions(bot)); }

bool Upkeep::Granted(Player* /*bot*/, UpkeepKind /*kind*/, bool keepsPossessions) { return Granted(keepsPossessions); }

void Upkeep::Note(Player* bot, UpkeepKind kind, char const* source, uint32 id, int64 amount)
{
    EventLog::Honesty(bot, HonestyKindOf(kind), source, id, amount);
}

bool Upkeep::FoodCheat(PlayerbotAI* botAI)
{
    return botAI && botAI->HasCheat(BotCheatMask::food) && Granted(botAI->GetBot(), UpkeepKind::Food);
}

uint32 Upkeep::EquippedAmmoId(Player const* bot)
{
    uint32 const ammo = bot->GetUInt32Value(PLAYER_AMMO_ID);
    return ammo && bot->HasItemCount(ammo, 1) ? ammo : 0;
}
