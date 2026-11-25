/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "QuestFailRecord.h"

#include <atomic>

#include "BotKind.h"
#include "BotStores.h"
#include "DatabaseEnv.h"
#include "GameTime.h"
#include "Log.h"
#include "Player.h"
#include "PlayerbotsDatabase.h"
#include "ThreadCheck.h"

namespace
{
constexpr std::uint32_t FLUSH_MS = 300 * IN_MILLISECONDS;  // the world thread writes the changed rows this often

std::atomic<bool> recordOn{false};  // set after the load; a give-up before it is not recorded
QuestFail::Book book;
std::uint32_t flushTimer = 0;
std::uint64_t rowsWritten = 0;

std::uint32_t FlushNow()
{
    PLAYERBOT_CHECK_WORLD_THREAD("QuestFailRecord::FlushNow");
    flushTimer = 0;
    std::vector<QuestFail::Written> const rows = book.TakeDirty();
    if (rows.empty())
        return 0;
    PlayerbotsDatabaseTransaction transaction = PlayerbotsDatabase.BeginTransaction();
    for (QuestFail::Written const& written : rows)
        transaction->Append(QuestFail::UpsertText(written));
    PlayerbotsDatabase.CommitTransaction(transaction);
    rowsWritten += rows.size();
    return static_cast<std::uint32_t>(rows.size());
}
}  // namespace

namespace QuestFailRecord
{
void Load()
{
    PLAYERBOT_CHECK_WORLD_THREAD("QuestFailRecord::Load");
    if (recordOn.load(std::memory_order_acquire))
        return;
    if (QueryResult result = PlayerbotsDatabase.Query(
            "SELECT guid, quest_id, give_ups, last_cause, minutes, last_time FROM playerbots_bot_quest_fail"))
    {
        do
        {
            Field* fields = result->Fetch();
            QuestFail::Row row;
            row.giveUps = fields[2].Get<uint32>();
            row.lastCause = fields[3].Get<std::string>();
            row.minutes = fields[4].Get<uint32>();
            row.lastTime = fields[5].Get<uint32>();
            book.Restore(fields[0].Get<uint32>(), fields[1].Get<uint32>(), std::move(row));
        } while (result->NextRow());
    }
    BotStores::RegisterForget("quest failures", [](uint32 guid) { book.Forget(guid); });
    recordOn.store(true, std::memory_order_release);
    auto const [bots, rows] = book.Size();
    LOG_INFO("playerbots", "Quest failure record: loaded {} rows of {} bots", rows, bots);
}

void Note(Player* bot, std::uint32_t questId, std::string_view cause, std::uint32_t minutes)
{
    if (!bot || !questId || !recordOn.load(std::memory_order_acquire) || !BotKind::KeepsPossessions(bot))
        return;
    book.Note(bot->GetGUID().GetCounter(), questId, cause, minutes,
              static_cast<std::uint32_t>(GameTime::GetGameTime().count()));
}

void Update(std::uint32_t diff)
{
    if (!recordOn.load(std::memory_order_relaxed))
        return;
    flushTimer += diff;
    if (flushTimer >= FLUSH_MS)
        FlushNow();
}

void Shutdown()
{
    if (!recordOn.load(std::memory_order_acquire))
        return;
    std::uint32_t const rows = FlushNow();
    recordOn.store(false, std::memory_order_release);
    LOG_INFO("playerbots", "Quest failure record: {} rows written at shutdown, {} since startup", rows, rowsWritten);
}

std::optional<QuestFail::Row> Find(std::uint32_t guid, std::uint32_t questId) { return book.Find(guid, questId); }

std::vector<std::pair<std::uint32_t, QuestFail::Row>> RowsOf(std::uint32_t guid) { return book.RowsOf(guid); }

std::vector<std::uint32_t> GivenUpQuests(std::uint32_t guid, std::uint32_t limit)
{
    return QuestFail::GivenUp(book.RowsOf(guid), limit);
}

QuestFail::Book const& Rows() { return book; }
}  // namespace QuestFailRecord
