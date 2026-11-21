/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_SPECTABLESTRATEGY_H
#define PLAYERBOTS_SPECTABLESTRATEGY_H

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "CombatStrategy.h"
#include "SpecTable.h"

class ChatHandler;
class PlayerbotAI;
class Trigger;

// A spec's combat from its table (combat-foundation INTERFACE §6): the combat strategy's nodes, the role's positioning
// nodes, then one "spec row" node per row. No default actions, no action node factory, no multipliers. The object
// keeps the bot's standing state of every row and the shared answers of the registered triggers its rows name; both
// are filled from the row triggers (the bot's own context), never from InitTriggers, which only marks a rebuild
class SpecTableStrategy : public CombatStrategy
{
public:
    SpecTableStrategy(PlayerbotAI* botAI, spec::TableView table);

    std::string const getName() override { return name; }
    uint32 GetType() const override { return table.type; }
    void InitTriggers(std::vector<TriggerNode*>& triggers) override;

    spec::TableView const& Table() const { return table; }

    // The row's standing state, after a refresh of every row when the engine was rebuilt or SPELL_ID_REFRESH_MS
    // passed (bot's map thread)
    spec::Standing StandingAt(size_t index, uint32 now);
    void Refresh(uint32 now);
    // Changes when a refresh changes any row's standing state (the combat_table trace)
    uint32 StandingVersion() const { return standingVersion; }

    // One answer per registered trigger object the table names; the address stays valid for the object's life
    spec::SharedAnswer* AnswerFor(Trigger* trigger);

    // casttrace only: per row, ticks live and times executed in the current fight
    struct RowCount
    {
        uint32 live = 0;
        uint32 executed = 0;
    };
    void CountLive(size_t index) { ++counts[index].live; }
    void CountExecuted(size_t index) { ++counts[index].executed; }
    std::vector<RowCount> const& Counts() const { return counts; }
    void ClearCounts();
    uint32 tracedVersion = 0;  // the standing version the last combat_table rows showed

    // The table on the bot's combat engine, or nullptr (not per tick: it walks the engine's strategy names)
    static SpecTableStrategy* Of(PlayerbotAI* botAI);

private:
    spec::TableView table;
    std::string const name;
    std::vector<spec::Standing> standing;
    bool rebuilt = true;
    uint32 refreshedMs = 0;
    uint32 standingVersion = 0;
    std::vector<std::pair<Trigger*, std::unique_ptr<spec::SharedAnswer>>> answers;
    std::vector<RowCount> counts;
};

// The positioning nodes the converted specs had, one base function until CF13 replaces it (INTERFACE §6 item 2)
void RoleNodes(spec::Role role, std::vector<TriggerNode*>& triggers);

// A strategy with a name and nothing else: a row's option once every spec of a class is a table (INTERFACE §7)
class OptionStrategy : public Strategy
{
public:
    OptionStrategy(PlayerbotAI* botAI, std::string optionName) : Strategy(botAI), name(std::move(optionName)) {}

    std::string const getName() override { return name; }

private:
    std::string const name;
};

namespace SpecTableView
{
// `playerbots combat table <bot>`: the snapshot the bot took at its last update after a request (world thread)
bool HandleTableCommand(ChatHandler* handler, char const* args);
// The snapshot's text, taken on the bot's map thread (PlayerbotAI::TakeCombatTableSnapshot)
std::string Describe(PlayerbotAI* botAI);
}  // namespace SpecTableView

#endif
