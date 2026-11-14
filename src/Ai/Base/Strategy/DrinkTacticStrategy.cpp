/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "DrinkTacticStrategy.h"

#include <algorithm>
#include <array>

#include "Battleground.h"
#include "Event.h"
#include "Group.h"
#include "Log.h"
#include "PerfMonitor.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "PvpTestCounters.h"
#include "StringFormat.h"
#include "Timer.h"
#include "Upkeep.h"

// G3b: the Drink tactic's game side (the policy in ArenaDrink.h, the numbers in DrinkConfig). In arenas and
// battlegrounds a bot that knows the tactic drinks in preparation (gates closed, more than PrepMinLeftMs left) and in
// a safe window of the match (out of combat, nobody near or in sight, the partners healthy); the old food rule's
// "drink" is zeroed for it there (TacticDrinkMultiplier), so a low-effort bot goes without. The sense (enemies, rays,
// partners) is read at most once per second per bot and only once the cheap tests say a drink is wanted.

namespace
{
DrinkBotState& DrinkState(PlayerbotAI* botAI)
{
    return botAI->GetAiObjectContext()->GetValue<DrinkBotState&>("drink state")->Get();
}

// The numbers, read again at most every DRINK_PARAMS_MS
DrinkParams const& BotDrinkParams(PlayerbotAI* botAI, Player* bot, DrinkBotState& state, uint32 nowMs)
{
    if (state.ParamsMs && nowMs - state.ParamsMs < DRINK_PARAMS_MS)
        return state.Params;

    DrinkGateInputs in;
    in.Skill = botAI->GetPvpSkill();
    in.FullAbility = in.Skill == 0 || botAI->HasFullSkillAbility();
    in.Knowledge = botAI->GetAttribute(BotAttribute::Knowledge);
    in.Effort = botAI->GetSkillEffort();
    state.Params = ComputeDrinkParams(sPlayerbotAIConfig.TacticDrink, in);
    state.Healer = PlayerbotAI::IsHeal(bot);
    state.ParamsMs = std::max(nowMs, 1u);
    return state.Params;
}

// The bot's battleground (arena or not); another one (or none) starts a fresh state
Battleground* DrinkBattleground(Player* bot, DrinkBotState& state)
{
    Battleground* bg = bot->GetBattleground();
    uint32 const instance = bg ? bg->GetInstanceID() : 0;
    if (state.Instance != instance)
    {
        DrinkResetMatch(state);
        state.Instance = instance;
    }

    return bg;
}

// Sitting with the drink aura (the food cheat's, or a drink's regeneration)
bool IsDrinking(Player* bot)
{
    return bot->IsSitState() && (bot->HasAura(DRINK_CHEAT_AURA) || bot->HasAuraType(SPELL_AURA_MOD_POWER_REGEN));
}

// The food cheat, or a drink in the bags (the old DrinkAction's item test)
bool HasDrinkToUse(PlayerbotAI* botAI)
{
    return Upkeep::FoodCheat(botAI) || botAI->GetAiObjectContext()->GetValue<uint32>("item count", "drink")->Get() > 0;
}

// What the bot itself shows, with no look at the others
DrinkSituation ReadCheap(Player* bot, Battleground* bg, DrinkBotState const& state)
{
    DrinkSituation s;
    s.HasMana = bot->GetMaxPower(POWER_MANA) > 0;
    s.ManaPct = s.HasMana ? static_cast<uint32>(bot->GetPowerPct(POWER_MANA)) : 100;
    s.Healer = state.Healer;
    s.Drinking = IsDrinking(bot);
    s.InCombat = bot->IsInCombat();
    s.Waiting = bg->GetStatus() == STATUS_WAIT_JOIN;
    s.InProgress = bg->GetStatus() == STATUS_IN_PROGRESS;
    s.LeftMs = bg->GetStartDelayTime();
    return s;
}

// The partners: in an arena every group member in the map, in a battleground the alive ones within
// DRINK_PARTNER_YARDS (a dead partner far away in a big team is no reason)
void ReadPartners(Player* bot, bool arena, bool& alive, uint32& minHpPct)
{
    alive = true;
    minHpPct = 100;
    Group* group = bot->GetGroup();
    if (!group)
        return;

    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (!member || member == bot || !member->IsInWorld() || member->GetMap() != bot->GetMap())
            continue;

        if (!arena && bot->GetDistance(member) > DRINK_PARTNER_YARDS)
            continue;

        if (!member->IsAlive())
        {
            if (arena)
                alive = false;

            continue;
        }

        minHpPct = std::min<uint32>(minHpPct, static_cast<uint32>(member->GetHealthPct()));
    }
}

// The sense: the nearest enemy, whether one of the nearest sees the bot (the spell check's ray: at most
// DRINK_MAX_RAYS of them, none past 100 yd), the partners. Arenas read "fight enemies", battlegrounds "nearest enemy
// players". At most once per second unless forced (a drink about to start).
void TakeSense(PlayerbotAI* botAI, Player* bot, Battleground* bg, DrinkBotState& state, uint32 nowMs, bool force)
{
    if (!force && !DrinkSenseDue(state, nowMs))
        return;

    PerfMonitorScope scope(PerfMonitor::IsEnabled() ? sPerfMonitor.acquire(PERF_MON_TOTAL, "Drink::Sense") : nullptr);
    AiObjectContext* context = botAI->GetAiObjectContext();
    GuidVector const enemies =
        context->GetValue<GuidVector>(bg->isArena() ? "fight enemies" : "nearest enemy players")->Get();
    std::array<std::pair<float, Unit*>, DRINK_MAX_ENEMIES> seen;
    uint32 count = 0;
    for (ObjectGuid const guid : enemies)
    {
        if (count >= DRINK_MAX_ENEMIES)
            break;

        Unit* unit = botAI->GetUnit(guid);
        if (!unit || !unit->IsInWorld() || !unit->IsAlive() || unit->GetMap() != bot->GetMap())
            continue;

        seen[count++] = {bot->GetExactDist(unit), unit};
    }

    std::sort(seen.begin(), seen.begin() + count,
              [](auto const& left, auto const& right) { return left.first < right.first; });
    float const nearest = count ? seen[0].first : DRINK_NO_ENEMY_YARDS;
    bool inLos = false;
    for (uint32 index = 0; index < std::min(count, DRINK_MAX_RAYS) && !inLos; ++index)
    {
        if (seen[index].first <= 100.0f && bot->IsWithinLOSInMap(seen[index].second, VMAP::ModelIgnoreFlags::M2))
            inLos = true;
    }

    bool partnersAlive = true;
    uint32 partnerMinHpPct = 100;
    ReadPartners(bot, bg->isArena(), partnersAlive, partnerMinHpPct);
    DrinkNoteSense(state, sPlayerbotAIConfig.TacticDrink, nowMs, nearest, inLos, partnersAlive, partnerMinHpPct);
}

void FillSense(DrinkSituation& s, DrinkBotState const& state, uint32 nowMs)
{
    s.EnemyYards = state.EnemyYards;
    s.EnemyInLos = state.EnemyInLos;
    s.SinceEnemyMs = DrinkSinceEnemyMs(state, nowMs);
    s.PartnersAlive = state.PartnersAlive;
    s.PartnerMinHpPct = state.PartnerMinHpPct;
}

// A started drink that ended: counted and logged when it was broken. Every update of the triggers calls this.
void SettleDrink(PlayerbotAI* botAI, Player* bot, DrinkBotState& state, uint32 nowMs)
{
    if (!state.StartMs)
        return;

    uint32 const heldMs = nowMs - state.StartMs;
    DrinkEnd const end = DrinkSettle(state, IsDrinking(bot), bot->IsInCombat(), nowMs);
    if (end != DrinkEnd::BrokenCombat && end != DrinkEnd::BrokenEarly)
        return;

    botAI->CountTactic(TacticCounter::DrinkBroken);
    LOG_TRACE("playerbots", "Drink: {} drink broken ({}) after {} ms, at {} ms", bot->GetName(),
              end == DrinkEnd::BrokenCombat ? "combat" : "stood early", heldMs, nowMs);
}

// A bot that wants a drink and has none: counted and logged at most every NoDrinkLogMs
void NoteNoDrink(PlayerbotAI* botAI, Player* bot, DrinkBotState& state, uint32 nowMs, char const* kind)
{
    state.NoDrinkUntilMs = std::max(nowMs + DRINK_PARAMS_MS, 1u);
    if (!DrinkNoItemDue(state, sPlayerbotAIConfig.TacticDrink, nowMs))
        return;

    botAI->CountTactic(TacticCounter::DrinkNoItem);
    LOG_TRACE("playerbots", "Drink: {} has no drink ({} drink wanted, mana {:.0f} %), at {} ms", bot->GetName(), kind,
              bot->GetPowerPct(POWER_MANA), nowMs);
}

bool NoDrinkHeld(DrinkBotState const& state, uint32 nowMs)
{
    return state.NoDrinkUntilMs && static_cast<int32>(nowMs - state.NoDrinkUntilMs) < 0;
}

// The preparation verdict (the cheat or an item last: the item count is asked only for a drink that would start)
DrinkWhy JudgePrep(PlayerbotAI* botAI, Player* bot, Battleground* bg, DrinkBotState& state, uint32 nowMs,
                   DrinkSituation& s)
{
    DrinkConfig const& config = sPlayerbotAIConfig.TacticDrink;
    DrinkParams const& params = BotDrinkParams(botAI, bot, state, nowMs);
    s = ReadCheap(bot, bg, state);
    DrinkWhy why = PrepDrinkVerdict(config, params, s);
    if (why == DrinkWhy::Go && !HasDrinkToUse(botAI))
    {
        s.HasDrink = false;
        why = PrepDrinkVerdict(config, params, s);
    }

    state.LastWhy = why;
    return why;
}

// The safe-window verdict. Drinking comes back as it is: the caller asks DrinkBreakVerdict then.
DrinkWhy JudgeSafe(PlayerbotAI* botAI, Player* bot, Battleground* bg, DrinkBotState& state, uint32 nowMs,
                   DrinkSituation& s, bool freshSense)
{
    DrinkConfig const& config = sPlayerbotAIConfig.TacticDrink;
    DrinkParams const& params = BotDrinkParams(botAI, bot, state, nowMs);
    s = ReadCheap(bot, bg, state);
    // A running drink of a bot that knows the tactic (a preparation drink that runs into the match too) asks the break
    // test whatever its mana is
    if (params.Known && s.Drinking && s.InProgress && !s.InCombat)
    {
        TakeSense(botAI, bot, bg, state, nowMs, freshSense);
        FillSense(s, state, nowMs);
        state.LastWhy = DrinkWhy::Drinking;
        state.LastBreak = DrinkBreakVerdict(config, s);
        return DrinkWhy::Drinking;
    }

    DrinkWhy why = SafeDrinkCheap(config, params, s);
    if (why != DrinkWhy::Go)
    {
        state.LastWhy = why;
        return why;
    }

    TakeSense(botAI, bot, bg, state, nowMs, freshSense);
    FillSense(s, state, nowMs);
    why = SafeDrinkVerdict(config, params, s);
    if (why == DrinkWhy::Go && !HasDrinkToUse(botAI))
    {
        s.HasDrink = false;
        why = SafeDrinkVerdict(config, params, s);
    }

    state.LastWhy = why;
    return why;
}

// The start of a drink: counted, noted and logged (the line the exit greps: "starts (safe) ... in LOS no")
void NoteStarted(PlayerbotAI* botAI, Player* bot, Battleground* bg, DrinkBotState& state, uint32 nowMs, bool prep,
                 uint32 holdMs)
{
    DrinkNoteStart(state, nowMs, prep, Upkeep::FoodCheat(botAI) && holdMs ? nowMs + holdMs : 0);
    botAI->CountTactic(TacticCounter::Drinks);
    if (prep)
        botAI->CountTactic(TacticCounter::PrepDrinks);

    botAI->NoteTacticUse(TacticId::Drink);
    std::string enemy = "none";
    if (state.EnemyYards < DRINK_NO_ENEMY_YARDS)
        enemy = Acore::StringFormat("{:.0f}", state.EnemyYards);

    LOG_TRACE("playerbots",
              "Drink: {} starts ({}), mana {:.0f} %, nearest enemy {} yd, in LOS {}, left {} ms, hold {} ms, at {} ms",
              bot->GetName(), prep ? "prep" : "safe", bot->GetPowerPct(POWER_MANA), enemy,
              state.EnemyInLos ? "yes" : "no", std::max<int32>(bg->GetStartDelayTime(), 0), holdMs, nowMs);
}

// Stand up, for a drink that must end: the aura goes with the seat (the real drink's) or by hand (the cheat's)
void StandUp(Player* bot)
{
    bot->SetStandState(UNIT_STAND_STATE_STAND);
    bot->RemoveAurasDueToSpell(DRINK_CHEAT_AURA);
}
}  // namespace

void TacticDrinkStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    triggers.push_back(new TriggerNode("prep drink", {NextAction("prep drink", ACTION_NORMAL + 2)}));
    triggers.push_back(new TriggerNode("safe to drink", {NextAction("safe drink", ACTION_NORMAL + 2)}));
}

void TacticDrinkStrategy::InitMultipliers(std::vector<Multiplier*>& multipliers)
{
    multipliers.push_back(new TacticDrinkMultiplier(botAI));
}

// The old food rule's "drink" (UseFoodStrategy: mana below HighMana, no regard for enemies) is off for a bot that knows
// this tactic in an arena or battleground; the tactic's own drinks are DrinkActions too and pass. A bot below the
// knowledge threshold keeps today's rule. (A master's "drink" order in a battleground is zeroed too: bots in matches
// have no master to speak of.)
float TacticDrinkMultiplier::GetValue(Action* action)
{
    if (!action || !botAI->TacticEnabledHere(TacticId::Drink))
        return 1.0f;

    if (!dynamic_cast<DrinkAction*>(action) || dynamic_cast<PrepDrinkAction*>(action) ||
        dynamic_cast<SafeDrinkAction*>(action))
        return 1.0f;

    if (!bot->GetBattleground())
        return 1.0f;

    DrinkBotState& state = DrinkState(botAI);
    uint32 const nowMs = getMSTime();
    DrinkBattleground(bot, state);
    return BotDrinkParams(botAI, bot, state, nowMs).Known ? 0.0f : 1.0f;
}

bool PrepDrinkTrigger::IsActive()
{
    if (!botAI->TacticEnabledHere(TacticId::Drink))
        return false;

    Battleground* bg = bot->GetBattleground();
    if (!bg || !bot->IsAlive())
        return false;

    DrinkBotState& state = DrinkState(botAI);
    uint32 const nowMs = getMSTime();
    DrinkBattleground(bot, state);
    SettleDrink(botAI, bot, state, nowMs);
    if (bg->GetStatus() != STATUS_WAIT_JOIN || NoDrinkHeld(state, nowMs))
        return false;

    DrinkSituation s;
    DrinkWhy const why = JudgePrep(botAI, bot, bg, state, nowMs, s);
    if (why == DrinkWhy::NoDrink)
        NoteNoDrink(botAI, bot, state, nowMs, "prep");

    return why == DrinkWhy::Go;
}

bool SafeToDrinkTrigger::IsActive()
{
    if (!botAI->TacticEnabledHere(TacticId::Drink))
        return false;

    Battleground* bg = bot->GetBattleground();
    if (!bg || !bot->IsAlive())
        return false;

    DrinkBotState& state = DrinkState(botAI);
    uint32 const nowMs = getMSTime();
    DrinkBattleground(bot, state);
    SettleDrink(botAI, bot, state, nowMs);
    if (bg->GetStatus() != STATUS_IN_PROGRESS || NoDrinkHeld(state, nowMs))
        return false;

    DrinkSituation s;
    DrinkWhy const why = JudgeSafe(botAI, bot, bg, state, nowMs, s, false);
    if (why == DrinkWhy::NoDrink)
        NoteNoDrink(botAI, bot, state, nowMs, "safe");

    // A running drink asks the break test: the trigger fires to stand the bot up
    if (why == DrinkWhy::Drinking)
        return state.LastBreak != DrinkBreakWhy::None;

    return why == DrinkWhy::Go;
}

// The trigger decided; Execute asks again with a fresh sense, so that no drink starts on a second-old look
bool PrepDrinkAction::isUseful() { return botAI->TacticEnabledHere(TacticId::Drink); }

bool PrepDrinkAction::Execute(Event /*event*/)
{
    Battleground* bg = bot->GetBattleground();
    if (!bg)
        return false;

    DrinkBotState& state = DrinkState(botAI);
    uint32 const nowMs = getMSTime();
    DrinkSituation s;
    if (JudgePrep(botAI, bot, bg, state, nowMs, s) != DrinkWhy::Go)
        return false;

    // The log line shows what the bot sees (the gates are closed: it does not decide anything)
    TakeSense(botAI, bot, bg, state, nowMs, true);
    if (!DrinkAction::Execute(Event("prep drink", "drink")))
        return false;

    // The cheat holds the bot in its seat for the action's delay (PlayerbotAI::UpdateAIInternal stands it up when the
    // delay falls below a second): capped, so that the break test is not asleep for long
    if (GetDuration() > DRINK_HOLD_MAX_MS)
        SetDuration(DRINK_HOLD_MAX_MS);

    NoteStarted(botAI, bot, bg, state, nowMs, true, GetDuration());
    return true;
}

bool SafeDrinkAction::isUseful() { return botAI->TacticEnabledHere(TacticId::Drink); }

bool SafeDrinkAction::Execute(Event /*event*/)
{
    Battleground* bg = bot->GetBattleground();
    if (!bg)
        return false;

    DrinkBotState& state = DrinkState(botAI);
    uint32 const nowMs = getMSTime();
    DrinkSituation s;
    DrinkWhy const why = JudgeSafe(botAI, bot, bg, state, nowMs, s, true);
    if (why == DrinkWhy::Drinking)
    {
        // The break: an enemy within BreakEnemyYards or in sight, a partner dead or hurt
        DrinkBreakWhy const reason = state.LastBreak;
        if (reason == DrinkBreakWhy::None)
            return false;

        StandUp(bot);
        ++state.Broken;
        state.StartMs = 0;
        state.ExpectedEndMs = 0;
        botAI->CountTactic(TacticCounter::DrinkBroken);
        LOG_TRACE("playerbots", "Drink: {} drink broken ({}), nearest enemy {:.0f} yd, in LOS {}, at {} ms",
                  bot->GetName(), DrinkBreakWhyName(reason), std::min(state.EnemyYards, 9999.0f),
                  state.EnemyInLos ? "yes" : "no", nowMs);
        return true;
    }

    if (why != DrinkWhy::Go)
        return false;

    if (!DrinkAction::Execute(Event("safe drink", "drink")))
        return false;

    if (GetDuration() > DRINK_HOLD_MAX_MS)
        SetDuration(DRINK_HOLD_MAX_MS);

    NoteStarted(botAI, bot, bg, state, nowMs, false, GetDuration());
    return true;
}
