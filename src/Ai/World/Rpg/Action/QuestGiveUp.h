/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_QUESTGIVEUP_H
#define PLAYERBOTS_QUESTGIVEUP_H

#include "NewRpgInfo.h"
#include "StatusEndTypes.h"

class PlayerbotAI;

// A quest order given up (questing WT35c). Every give-up ends the order through RpgStatus::End(Abandoned, cause); the
// status core calls Noted for a DoQuest status ending so, whoever ended it, before the Status row
namespace QuestGiveUp
{
// The give-up's report, once per order run (Pursuit::Report): one QuestFail row (the cause, the quest, the whole
// minutes pursued), the bot's failure record (QuestFailRecord::Note) and the quest stat (RecordQuestAbandoned).
// Map thread, O(1), no database access
void Noted(PlayerbotAI* botAI, NewRpgInfo::DoQuest const& data, char const* cause);
// questing WQ19a: an order's end (DoQuest::order set), whoever ended it: its one QuestOrder row. The order's own ends
// come with their word (done, arrived, time, dry); any other end of the status counts as replaced. The one writer of
// a refused take (WQ18c): an own end gives up the planned takes it leaves open at their giver
// (QuestFieldExec::RefusedTakes), one Noted with cannot_take each. Returns the order's own end
// (QuestFieldExec::OwnEnd; questing WQ18b), none when something took the bot over. Map thread, O(log)
std::optional<Planner::QuestOrderEnd> OrderEnded(PlayerbotAI* botAI, NewRpgInfo::DoQuest const& data,
                                                 StatusEnd::Outcome outcome, char const* word);
}  // namespace QuestGiveUp

#endif
