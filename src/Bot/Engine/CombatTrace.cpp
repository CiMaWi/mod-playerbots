/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "CombatTrace.h"

#include <algorithm>
#include <functional>
#include <optional>
#include <string_view>

#include "Action.h"
#include "AiFactory.h"
#include "AiObjectContext.h"
#include "CombatRecord.h"
#include "DBCStores.h"
#include "EventLog.h"
#include "GenericSpellActions.h"
#include "Group.h"
#include "MovementActions.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotSpellRepository.h"
#include "Playerbots.h"
#include "SpecTableStrategy.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "TestConstraints.h"
#include "Timer.h"
#include "WorldLog.h"
#include "WorldLogFormat.h"

namespace
{
constexpr std::string_view TARGET_NONE = "none";
constexpr std::string_view TARGET_SELF = "self";
constexpr std::string_view CHOICE_KNOWN = "known";
constexpr std::string_view CHOICE_EXECUTED = "executed";
constexpr std::array<std::string_view, 12> REJECT_WORDS = {"unknown",    "useless", "vetoed",   "not_known",
                                                           "cooldown",   "range",   "sight",    "target",
                                                           "impossible", "failed",  "no_power", "power_gate"};

static_assert(static_cast<uint8>(CombatTally::PowerKind::Mana) == POWER_MANA);
static_assert(static_cast<uint8>(CombatTally::PowerKind::Rage) == POWER_RAGE);
static_assert(static_cast<uint8>(CombatTally::PowerKind::Focus) == POWER_FOCUS);
static_assert(static_cast<uint8>(CombatTally::PowerKind::Energy) == POWER_ENERGY);
static_assert(static_cast<uint8>(CombatTally::PowerKind::RunicPower) == POWER_RUNIC_POWER);
static_assert(REJECT_WORDS.size() == static_cast<std::size_t>(CombatTrace::RejectReason::PowerGate) + 1);

CombatTally::TickReading ReadingOf(Player* player)
{
    Powers const power = player->getPowerType();
    CombatTally::TickReading reading;
    reading.healthPct = static_cast<uint32>(player->GetHealthPct());
    reading.powerPct = CombatTally::Percent(player->GetPower(power), player->GetMaxPower(power));
    return reading;
}

std::string_view TargetKind(Player* bot, Unit* target)
{
    if (!target)
        return TARGET_NONE;
    if (target == bot)
        return TARGET_SELF;

    bool const player = target->IsPlayer();
    if (bot->IsFriendlyTo(target))
        return player ? "friend_player" : "friend_npc";
    return player ? "enemy_player" : "enemy_npc";
}
}  // namespace

namespace CombatTally
{
void OnEnter(Player* player)
{
    if (!player || !WorldLog::Active())
        return;

    PlayerbotAI* botAI = GET_PLAYERBOT_AI(player);
    if (!botAI)
        return;

    Group const* group = player->GetGroup();
    botAI->fightTally.Open(getMSTime(), group ? group->GetMembersCount() : 1, ReadingOf(player),
                           PlayerbotAI::IsRanged(player));
    if (botAI->combatRecord)
        botAI->combatRecord->NewFight();
}

void OnLeave(Player* player, bool logout)
{
    if (!player)
        return;

    PlayerbotAI* botAI = GET_PLAYERBOT_AI(player);
    if (!botAI)
        return;

    // Unit::ClearInCombat can repeat the leave edge, and a logout follows its own: the second close returns nothing
    std::optional<Summary> const fight = botAI->fightTally.Close(getMSTime());
    if (!fight)
        return;
    if (TestConstraints::CombatTraced(botAI->testConstraints))
        CombatTrace::Rows(botAI);

    Write(player, *fight, logout ? "logout" : (player->IsAlive() ? "alive" : "died"));
    // casttrace: the damage rows follow the Fight pair
    if (botAI->combatRecord)
        CombatRecord::WriteDealt(player, *botAI->combatRecord);
}

void Write(Player* player, Summary const& fight, char const* outcome)
{
    uint32 const counts = WorldLogFormat::PackVisitCounts(fight.casts, fight.maxEnemies, fight.groupSize);
    EventLog::Emit(player, EventLog::EventType::Fight, outcome, FightId(counts),
                   static_cast<int64>(FightAmount(fight)));

    Powers const power = player->getPowerType();
    uint32 const more = MoreId(player->GetLevel(), player->getClass(), AiFactory::GetPlayerSpecTab(player), power,
                               fight.lowestHealthPct, Percent(player->GetPower(power), player->GetMaxPower(power)));
    EventLog::Emit(player, EventLog::EventType::FightMore, outcome, more, static_cast<int64>(MoreAmount(fight)));
}

bool IsCast(Action* action) { return dynamic_cast<CastSpellAction*>(action) != nullptr; }

bool IsMove(Action* action) { return dynamic_cast<MovementAction*>(action) != nullptr; }
}  // namespace CombatTally

namespace CombatTrace
{
void Rejects::Add(PlayerbotAI* botAI, Action* action, std::string const& name, RejectReason reason, float relevance,
                  std::string const& veto, float vetoValue)
{
    if (count >= REJECTS_PER_TICK)
        return;

    Reject& row = rows[count++];
    row.name = name;
    row.veto = veto;
    row.relevance = relevance;
    row.vetoValue = vetoValue;
    row.spell = SpellOf(botAI, action);
    row.reason = reason;
}

RejectReason ReasonOf(CombatTally::CastRefusal refusal)
{
    switch (refusal)
    {
        case CombatTally::CastRefusal::NotKnown:
            return RejectReason::NotKnown;
        case CombatTally::CastRefusal::Cooldown:
            return RejectReason::Cooldown;
        case CombatTally::CastRefusal::Range:
            return RejectReason::Range;
        case CombatTally::CastRefusal::Sight:
            return RejectReason::Sight;
        case CombatTally::CastRefusal::Target:
            return RejectReason::Target;
        default:
            return RejectReason::Impossible;
    }
}

void AddPowerGate(PlayerbotAI* botAI, Rejects& rejects)
{
    CombatTally::PowerGate const& gate = botAI->powerGate;
    if (!gate.NotedThisTick() || gate.Row() == CombatTally::NO_ROW)
        return;

    SpecTableStrategy* const table = SpecTableStrategy::Of(botAI);
    if (!table || gate.Row() >= table->Table().count)
        return;

    rejects.Add(botAI, nullptr, std::string(table->Table().rows[gate.Row()].action), RejectReason::PowerGate, 0.0f);
}

uint32 SpellOf(PlayerbotAI* botAI, Action* action)
{
    CastSpellAction* const cast = dynamic_cast<CastSpellAction*>(action);
    if (!botAI || !cast)
        return 0;

    AiObjectContext* const context = botAI->GetAiObjectContext();
    return context ? context->GetValue<uint32>("spell id", cast->getSpell())->Get() : 0;
}

void Executed(PlayerbotAI* botAI, Action* action, float relevance)
{
    if (!botAI || !action || !WorldLog::Active())
        return;

    Player* const bot = botAI->GetBot();
    std::string const name = action->getName();
    WorldLog::Decision(bot, WorldLogFormat::DecisionKind::CombatAction, name, TargetKind(bot, action->GetTarget()),
                       relevance, {}, SpellOf(botAI, action));

    if (SpecTableStrategy* const table = SpecTableStrategy::Of(botAI))
        if (int const row = spec::RowOfRelevance(table->Table(), name, relevance); row >= 0)
            table->CountExecuted(static_cast<std::size_t>(row));
}

void Rejected(PlayerbotAI* botAI, Rejects const& rejects, std::size_t limit, uint32 nowMs)
{
    if (!botAI || !WorldLog::Active())
        return;

    Player* const bot = botAI->GetBot();
    CombatRecord::State& state = CombatRecord::StateOf(botAI);
    for (std::size_t i = 0; i < std::min(rejects.count, limit); ++i)
    {
        Reject const& row = rejects.rows[i];
        std::size_t const pair = std::hash<std::string>{}(row.name) * 31 + static_cast<std::size_t>(row.reason);
        if (!state.RejectDue(pair, nowMs))
            continue;

        WorldLogFormat::Alternatives alternatives;
        if (row.reason == RejectReason::Vetoed && !row.veto.empty())
            alternatives.Add({row.veto, row.vetoValue});
        WorldLog::Decision(bot, WorldLogFormat::DecisionKind::CombatReject, row.name,
                           REJECT_WORDS[static_cast<std::size_t>(row.reason)], row.relevance, alternatives, row.spell);
    }
}

void Known(PlayerbotAI* botAI)
{
    if (!botAI)
        return;

    botAI->combatKnownTraced = true;
    Player* const bot = botAI->GetBot();
    if (!bot || !WorldLog::Active())
        return;

    // As the spell list (ListSpellsAction): not removed, active, in the active spec, not passive; plus a class skill
    // line, and only the highest rank the bot knows
    for (auto const& [spellId, spell] : bot->GetSpellMap())
    {
        if (!spell || spell->State == PLAYERSPELL_REMOVED || !spell->Active ||
            !(spell->specMask & bot->GetActiveSpecMask()))
            continue;

        SpellInfo const* const info = sSpellMgr->GetSpellInfo(spellId);
        if (!info || info->IsPassive())
            continue;

        SkillLineAbilityEntry const* const ability = PlayerbotSpellRepository::Instance().GetSkillLine(spellId);
        SkillLineEntry const* const line = ability ? sSkillLineStore.LookupEntry(ability->SkillLine) : nullptr;
        if (!line || line->categoryId != SKILL_CATEGORY_CLASS)
            continue;

        SpellInfo const* const next = info->GetNextRankSpell();
        if (next && bot->HasSpell(next->Id))
            continue;

        WorldLog::Decision(bot, WorldLogFormat::DecisionKind::CombatKnown, CHOICE_KNOWN, info->SpellName[0], 0.0f, {},
                           spellId);
    }
}

void Table(PlayerbotAI* botAI, bool first)
{
    SpecTableStrategy* const table = SpecTableStrategy::Of(botAI);
    if (!table || !WorldLog::Active())
        return;

    spec::TableView const& view = table->Table();
    uint32 const now = getMSTime();
    std::vector<spec::Standing> states;
    states.reserve(view.count);
    for (std::size_t i = 0; i < view.count; ++i)
        states.push_back(table->StandingAt(i, now));
    if (!first && table->tracedVersion == table->StandingVersion())
        return;

    table->tracedVersion = table->StandingVersion();
    Player* const bot = botAI->GetBot();
    std::vector<spec::Node> const nodes = spec::Nodes(view);
    for (std::size_t i = 0; i < view.count; ++i)
        WorldLog::Decision(bot, WorldLogFormat::DecisionKind::CombatTable, view.rows[i].action,
                           spec::StandingWord(states[i]), nodes[i].relevance, {}, static_cast<uint32>(i));
}

void Rows(PlayerbotAI* botAI)
{
    SpecTableStrategy* const table = SpecTableStrategy::Of(botAI);
    if (!table)
        return;

    if (WorldLog::Active())
    {
        spec::TableView const& view = table->Table();
        Player* const bot = botAI->GetBot();
        for (std::size_t i = 0; i < view.count; ++i)
        {
            SpecTableStrategy::RowCount const& count = table->Counts()[i];
            WorldLogFormat::Alternatives alternatives;
            alternatives.Add({CHOICE_EXECUTED, static_cast<float>(count.executed)});
            WorldLog::Decision(bot, WorldLogFormat::DecisionKind::CombatRows, view.rows[i].action,
                               spec::KindWord(view.rows[i].kind), static_cast<float>(count.live), alternatives,
                               static_cast<uint32>(i));
        }
    }
    table->ClearCounts();
}
}  // namespace CombatTrace
