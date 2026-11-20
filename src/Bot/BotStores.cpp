/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "BotStores.h"

#include <string>
#include <vector>

#include "DatabaseEnv.h"
#include "Player.h"
#include "PlayerbotsDatabase.h"
#include "StringFormat.h"
#include "ThreadCheck.h"

namespace
{
struct Forget
{
    std::string name;
    void (*forget)(uint32 guid);
};

struct Stores
{
    std::vector<Forget> forgets;
};

Stores& GetStores()
{
    static Stores stores;
    return stores;
}
}  // namespace

void BotStores::RegisterForget(char const* name, void (*forget)(uint32 guid))
{
    std::vector<Forget>& forgets = GetStores().forgets;
    for (Forget& known : forgets)
        if (known.name == name)
        {
            known.forget = forget;
            return;
        }

    forgets.push_back({name, forget});
}

uint32 BotStores::RowCount(BotRowDb db)
{
    uint32 count = 0;
    for (BotRow const& row : BotRows)
        if (row.db == db)
            ++count;

    return count;
}

void BotStores::Delete(uint32 guid, uint32 account)
{
    PLAYERBOT_CHECK_WORLD_THREAD("BotStores::Delete");

    // Memory first: nothing may write the bot's rows back after the delete
    for (Forget const& forget : GetStores().forgets)
        forget.forget(guid);

    CharacterDatabaseTransaction characters = CharacterDatabase.BeginTransaction();
    PlayerbotsDatabaseTransaction playerbots = PlayerbotsDatabase.BeginTransaction();
    for (BotRow const& row : BotRows)
    {
        if (row.db == BotRowDb::Playerbots)
            playerbots->Append(Acore::StringFormat("DELETE FROM {} WHERE {} = {}", row.table, row.column, guid));
        else
            characters->Append(row.byItem
                                   ? Acore::StringFormat("DELETE FROM {} WHERE {} IN (SELECT guid FROM "
                                                         "item_instance WHERE owner_guid = {})",
                                                         row.table, row.column, guid)
                                   : Acore::StringFormat("DELETE FROM {} WHERE {} = {}", row.table, row.column, guid));
    }

    CharacterDatabase.CommitTransaction(characters);
    PlayerbotsDatabase.CommitTransaction(playerbots);

    // The core's delete (CHAR_DELETE_REMOVE): the character, its items, pets, mail, social lists, achievements, quests,
    // guild membership and the character cache entry; the account stays, and its free slot takes the replacement. It
    // runs a few synchronous mail queries on this thread, hence the callers' caps.
    Player::DeleteFromDB(guid, account, true, true);
}

void BotStores::SweepPlayerbotsRows(std::string const& characterDb, std::string const& botAccountIds)
{
    for (BotRow const& row : BotRows)
        if (row.db == BotRowDb::Playerbots)
            PlayerbotsDatabase.DirectExecute(
                Acore::StringFormat("DELETE FROM {} WHERE {} <> 0 AND {} NOT IN (SELECT guid FROM {}.characters WHERE "
                                    "account NOT IN ({}))",
                                    row.table, row.column, row.column, characterDb, botAccountIds));
}
