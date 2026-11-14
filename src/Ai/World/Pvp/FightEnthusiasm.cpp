/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "FightEnthusiasm.h"

#include <cmath>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "BotTraitMgr.h"
#include "CellImpl.h"
#include "CensusMgr.h"
#include "Chat.h"
#include "FightBook.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Group.h"
#include "Map.h"
#include "PerfMonitor.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "Random.h"
#include "StringConvert.h"
#include "StringFormat.h"
#include "Timer.h"
#include "WorldLog.h"
#include "WorldLogFormat.h"

namespace WorldPvp
{
namespace
{
// The one-off edits' event rows (world-log.md): source the cause, id the value before, amount the value after; a
// death's second row carries the victim's plain join threshold
constexpr char const* EVENT_ENTHUSIASM = "PvpEnthusiasm";
constexpr char const* SOURCE_DEATH_JOIN = "death_join";
// The entering rows' reasons (world-log.md)
constexpr char const* REASON_ENTHUSIASM = "enthusiasm";
constexpr char const* REASON_FACTION = "faction_attacked";
constexpr char const* REASON_GROUP = "group_attacked";

EnthusiasmValues Values() { return FromConfig(sPlayerbotAIConfig.WorldPvp); }

// In PvP combat with an enemy player on an open-world map (battlegrounds and arenas leave the value alone)
bool EngagedNow(Player* bot)
{
    Map const* const map = bot->GetMap();
    return map && !map->Instanceable() && !bot->GetCombatManager().GetPvPCombatRefs().empty();
}

// The bot's enthusiasm, made and seeded at random on first use
BotEnthusiasm& Of(PlayerbotAI* botAI, uint32 nowMs, EnthusiasmValues const& values)
{
    std::unique_ptr<BotEnthusiasm>& own = botAI->enthusiasm;
    if (!own)
        own = std::make_unique<BotEnthusiasm>();
    Seed(*own, rand32(), nowMs, values);
    return *own;
}

void ReadContext(PlayerbotAI* botAI, Player* bot, BotEnthusiasm& state, uint32 nowMs, EnthusiasmValues const& values)
{
    float aggression = 0.0f;
    float caution = 0.0f;
    float helpfulness = 0.0f;
    float confidence = 0.0f;
    if (std::optional<TraitRules::BotTraits> const traits = BotTraitMgr::instance().Find(bot->GetGUID().GetCounter()))
    {
        aggression = TraitRules::Unit(*traits, TraitRules::Trait::Aggression);
        caution = TraitRules::Unit(*traits, TraitRules::Trait::Caution);
        helpfulness = TraitRules::Unit(*traits, TraitRules::Trait::Helpfulness);
        confidence = TraitRules::Unit(*traits, TraitRules::Trait::Confidence);
    }
    NewTraits(state, aggression, caution, helpfulness, confidence, nowMs, values);
    Player* const master = botAI->GetMaster();
    state.realMaster = master && master != bot && IsRealPlayer(master);
    state.prohibitedZone = sPlayerbotAIConfig.IsPvpProhibited(bot->GetZoneId(), bot->GetAreaId());
}

// LocalBalance over the living players within balanceYards: the bot, its side and the enemies it sees. The lists are
// the thread's, kept between calls
float BalanceAround(Player* bot, EnthusiasmValues const& values)
{
    PerfMonitorScope scope(PerfMonitor::IsEnabled() ? sPerfMonitor.acquire(PERF_MON_TOTAL, "WorldPvp balance")
                                                    : nullptr);
    thread_local std::vector<Player*> players;
    thread_local std::vector<BalanceEntry> entries;
    players.clear();
    entries.clear();
    Acore::AnyPlayerInObjectRangeCheck check(bot, values.balanceYards, true, true);
    Acore::PlayerListSearcher<Acore::AnyPlayerInObjectRangeCheck> searcher(bot, players, check);
    Cell::VisitObjects(bot, searcher, values.balanceYards);
    uint32 const level = bot->GetLevel();
    entries.push_back({true, Colour::Yellow, bot->GetHealthPct() / 100.0f});
    for (Player* const player : players)
    {
        if (player == bot)
            continue;
        bool const ownSide = player->GetTeamId() == bot->GetTeamId();
        if (!ownSide && !bot->CanSeeOrDetect(player))
            continue;
        entries.push_back({ownSide, ColourOf(level, player->GetLevel()), player->GetHealthPct() / 100.0f});
    }
    return LocalBalance(entries, values);
}

// The engaged edge, the context when due, and the balance when due while the bot is engaged or could enter (`force`:
// all of it now, for the command)
void Refresh(PlayerbotAI* botAI, Player* bot, BotEnthusiasm& state, uint32 nowMs, EnthusiasmValues const& values,
             bool force)
{
    Observe(state, EngagedNow(bot), nowMs, values);
    if (force || ContextDue(state, nowMs))
        ReadContext(botAI, bot, state, nowMs, values);
    if (force || (BalanceDue(state, nowMs) && (state.engaged || !SkipsScan(state, nowMs, values))))
        NewBalance(state, BalanceAround(bot, values), GraveyardYards(bot) <= values.graveyardYards, nowMs, values);
}

struct Entering
{
    bool enters = false;
    BanReason ban = BanReason::None;
    JoinFacts facts;
    float join = 0.0f;
    double value = 0.0;
};

// The bans, the join threshold and the check for one enemy, from what the bot sees of it now: whether its victim is
// a player of the bot's faction, of its group or its real master
Entering Evaluate(PlayerbotAI* botAI, Player* bot, BotEnthusiasm const& state, Player* enemy, uint32 nowMs,
                  EnthusiasmValues const& values)
{
    Entering entering;
    Colour const colour = ColourOf(bot->GetLevel(), enemy->GetLevel());
    Unit* const victim = enemy->GetVictim();
    Player* const victimPlayer = victim ? victim->GetCharmerOrOwnerPlayerOrPlayerItself() : nullptr;
    bool const faction = victimPlayer && victimPlayer->GetTeamId() == bot->GetTeamId();
    Group* const group = bot->GetGroup();
    bool const groupAttacked = faction && group && victimPlayer != bot && victimPlayer->GetGroup() == group;

    BanFacts ban;
    ban.colour = colour;
    ban.targetAttacksFaction = faction;
    ban.prohibitedZone = state.prohibitedZone;
    ban.onTransport = bot->GetTransport() != nullptr;
    ban.inInstance = bot->GetMap()->Instanceable();
    ban.realMaster = state.realMaster;
    ban.mastersFight = state.realMaster && victimPlayer && victimPlayer == botAI->GetMaster();
    entering.ban = Banned(ban);
    if (entering.ban != BanReason::None)
        return entering;

    entering.facts = FactsFor(state, colour, faction, groupAttacked);
    entering.join = JoinThreshold(entering.facts, values);
    entering.value = ValueAt(state, nowMs, values);
    entering.enters = Enters(entering.value, entering.join, state.flee);
    return entering;
}

// The entering row: fight_start (reason enthusiasm) or fight_join (faction_attacked, group_attacked) with the value,
// the join threshold, the colour and the terms applied
void RecordEntering(Player* bot, Entering const& entering, EnthusiasmValues const& values)
{
    if (!WorldLog::Active())
        return;

    bool const joining = FightOngoing(entering.facts);
    WorldLogFormat::Alternatives alternatives{WorldLogFormat::Alternative{"join", entering.join}};
    alternatives.Add({"colour", static_cast<float>(entering.facts.colour)});
    alternatives.Add({"terms", static_cast<float>(JoinTerms(entering.facts, values))});
    char const* const reason = !joining                             ? REASON_ENTHUSIASM
                               : entering.facts.targetAttacksGroup ? REASON_GROUP
                                                                   : REASON_FACTION;
    WorldLog::Decision(bot,
                       joining ? WorldLogFormat::DecisionKind::FightJoin : WorldLogFormat::DecisionKind::FightStart,
                       joining ? CHOICE_JOIN : CHOICE_START, reason, static_cast<float>(entering.value), alternatives,
                       0);
}

// A plain threshold of the command and the death row: a yellow target, nobody of the bot's side attacked, no
// graveyard row unless asked, the cached balance
float PlainJoin(BotEnthusiasm const& state, bool faction, bool group, bool graveyard, Colour colour,
                EnthusiasmValues const& values)
{
    JoinFacts facts = FactsFor(state, colour, faction, group);
    facts.nearGraveyard = graveyard;
    return JoinThreshold(facts, values);
}

// One one-off edit on a bot that starts by the rules, with its row(s)
void EditOne(Player* player, EditCause cause, uint32 nowMs, EnthusiasmValues const& values)
{
    PlayerbotAI* const botAI = GET_PLAYERBOT_AI(player);
    if (!botAI || (!botAI->enthusiasm && !StartsByRules(botAI)))
        return;

    BotEnthusiasm& state = Of(botAI, nowMs, values);
    if (ContextDue(state, nowMs))
        ReadContext(botAI, player, state, nowMs, values);
    double const before = EditBy(state, cause, nowMs, values);
    double const after = ValueAt(state, nowMs, values);
    EventLog::Emit(player, EVENT_ENTHUSIASM, Word(cause), static_cast<uint32>(std::lround(before)),
                   static_cast<int64>(std::lround(after)));
    if (cause == EditCause::Death)
        EventLog::Emit(player, EVENT_ENTHUSIASM, SOURCE_DEATH_JOIN, 0,
                       static_cast<int64>(std::lround(
                           PlainJoin(state, false, false, state.nearGraveyard, Colour::Yellow, values))));
}
}  // namespace

void RefreshEnthusiasm(PlayerbotAI* botAI)
{
    uint32 const nowMs = getMSTime();
    EnthusiasmValues const values = Values();
    Refresh(botAI, botAI->GetBot(), Of(botAI, nowMs, values), nowMs, values, false);
}

bool ScanSkipped(PlayerbotAI* botAI)
{
    BotEnthusiasm const* const state = botAI->enthusiasm.get();
    return state && SkipsScan(*state, getMSTime(), Values());
}

bool MayEnter(PlayerbotAI* botAI, Unit* unit, char const* refusal)
{
    Player* const bot = botAI->GetBot();
    Player* const enemy = unit->ToPlayer();
    BotEnthusiasm const* const state = botAI->enthusiasm.get();
    if (!enemy || !state || refusal || !enemy->IsAlive() || enemy->GetTeamId() == bot->GetTeamId())
        return false;
    uint32 const nowMs = getMSTime();
    if (IsPick(*state, enemy->GetGUID().GetRawValue(), nowMs))
        return true;
    if (!bot->IsWithinDist(enemy, static_cast<float>(sPlayerbotAIConfig.WorldPvp.ScanYards)) ||
        LeftAlone(botAI, enemy))
        return false;
    return Evaluate(botAI, bot, *state, enemy, nowMs, Values()).enters;
}

bool NoteEntering(PlayerbotAI* botAI, Player* enemy)
{
    BotEnthusiasm* const state = botAI->enthusiasm.get();
    if (!state)
        return false;
    uint32 const nowMs = getMSTime();
    uint64 const guid = enemy->GetGUID().GetRawValue();
    if (IsPick(*state, guid, nowMs))
        return true;
    Player* const bot = botAI->GetBot();
    EnthusiasmValues const values = Values();
    Entering const entering = Evaluate(botAI, bot, *state, enemy, nowMs, values);
    if (!entering.enters)
        return false;
    SetPick(*state, guid, nowMs);
    RecordEntering(bot, entering, values);
    return true;
}

bool IsEnteredTarget(PlayerbotAI* botAI, Unit* unit)
{
    BotEnthusiasm const* const state = botAI && unit ? botAI->enthusiasm.get() : nullptr;
    return state && IsPick(*state, unit->GetGUID().GetRawValue(), getMSTime());
}

void NoteEngaged(PlayerbotAI* botAI, Player* bot, uint32 nowMs)
{
    if (!botAI->enthusiasm && (!EngagedNow(bot) || !StartsByRules(botAI)))
        return;
    EnthusiasmValues const values = Values();
    Refresh(botAI, bot, Of(botAI, nowMs, values), nowMs, values, false);
}

void NoteEnthusiasmDeath(Player* killer, Player* killed)
{
    if (!killer || !killed || killer == killed)
        return;
    uint32 const nowMs = getMSTime();
    EnthusiasmValues const values = Values();
    EditOne(killed, EditCause::Death, nowMs, values);
    EditOne(killer, EditCause::Kill, nowMs, values);
    // Only members on the victim's map: it is this thread's; a member elsewhere is another map thread's
    Group* const group = killed->GetGroup();
    if (!group)
        return;
    for (GroupReference* reference = group->GetFirstMember(); reference; reference = reference->next())
    {
        Player* const member = reference->GetSource();
        if (member && member != killed && member != killer && member->IsInWorld() &&
            member->GetMap() == killed->GetMap() && member->IsWithinDist(killed, values.groupDeathYards))
            EditOne(member, EditCause::GroupDeath, nowMs, values);
    }
}

bool HandleEnthusiasmCommand(ChatHandler* handler, PlayerbotAI* botAI, std::string_view rest)
{
    while (!rest.empty() && rest.front() == ' ')
        rest.remove_prefix(1);
    while (!rest.empty() && rest.back() == ' ')
        rest.remove_suffix(1);
    EnthusiasmValues const values = Values();
    std::optional<double> set;
    if (!rest.empty())
    {
        std::string_view const verb = rest.substr(0, rest.find(' '));
        std::string_view number = verb.size() < rest.size() ? rest.substr(verb.size() + 1) : std::string_view();
        while (!number.empty() && number.front() == ' ')
            number.remove_prefix(1);
        set = verb == "set" ? Acore::StringTo<double>(number) : std::nullopt;
        if (!set || !std::isfinite(*set) || *set < 0.0 || *set > values.max)
        {
            handler->SendErrorMessage("Usage: .playerbots fight enthusiasm <bot> [set <0..{:.0f}>]", values.max);
            return false;
        }
    }

    Player* const bot = botAI->GetBot();
    uint32 const nowMs = getMSTime();
    BotEnthusiasm& state = Of(botAI, nowMs, values);
    Refresh(botAI, bot, state, nowMs, values, true);
    if (set)
        state.state = {*set, nowMs};

    std::string const prefix = "FIGHT enthusiasm: " + bot->GetName();
    double const value = ValueAt(state, nowMs, values);
    std::string seed;
    if (!state.seedShown)
    {
        seed = Acore::StringFormat(" seed {:.2f}", state.seed);
        state.seedShown = true;
    }
    handler->PSendSysMessage("{} value {:.2f}{}{}", prefix, value, seed, set ? " set" : "");
    handler->PSendSysMessage("{} flee {:.2f} caution {:.2f}", prefix, state.flee, state.caution);
    handler->PSendSysMessage("{} join {:.2f} aggression {:.2f}", prefix,
                             PlainJoin(state, false, false, false, Colour::Yellow, values), state.aggression);
    handler->PSendSysMessage("{} join_faction {:.2f} helpfulness {:.2f}", prefix,
                             PlainJoin(state, true, false, false, Colour::Yellow, values), state.helpfulness);
    handler->PSendSysMessage("{} join_group {:.2f}", prefix,
                             PlainJoin(state, true, true, false, Colour::Yellow, values));
    handler->PSendSysMessage("{} join_graveyard {:.2f} near {}", prefix,
                             PlainJoin(state, false, false, true, Colour::Yellow, values), state.nearGraveyard ? 1 : 0);
    handler->PSendSysMessage("{} join_green {:.2f}", prefix,
                             PlainJoin(state, false, false, false, Colour::Green, values));
    handler->PSendSysMessage("{} lowest {:.2f} skips {}", prefix, state.lowestJoin,
                             SkipsScan(state, nowMs, values) ? 1 : 0);
    handler->PSendSysMessage("{} balance {:.2f} factor {:.2f} shift {:.2f}", prefix, state.balance, state.factor,
                             state.joinShift);
    handler->PSendSysMessage("{} engaged {} for {} s", prefix, state.engaged ? 1 : 0,
                             state.engaged ? (nowMs - state.engagedMs) / IN_MILLISECONDS : 0);
    bool const picked = IsPick(state, state.pickGuid, nowMs);
    handler->PSendSysMessage("{} pick {} left {} s", prefix, picked ? ObjectGuid(state.pickGuid).GetCounter() : 0,
                             picked ? (PICK_MS - (nowMs - state.pickMs)) / IN_MILLISECONDS : 0);
    return true;
}
}  // namespace WorldPvp
