/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "TestConstraintStates.h"

#include <string>

#include "BotTravel.h"
#include "CombatRecord.h"
#include "EventLog.h"
#include "GoalInterface.h"
#include "ItemWorthRules.h"
#include "Log.h"
#include "NewRpgInternal.h"
#include "NewRpgStatusEnd.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "TestConstraints.h"
#include "TestOrders.h"
#include "Timer.h"
#include "WorldLog.h"

namespace
{
// nofight: immune to NPCs, no creature fights the unit (Unit::_IsValidAttackTarget); the flag ends its PvE combat
void SetNoFight(Unit* unit, bool on)
{
    if (unit && unit->IsImmuneToNPC() != on)
        unit->SetImmuneToNPC(on);
}

void NoFight(Player& bot, bool on)
{
    SetNoFight(&bot, on);
    for (Unit* controlled : bot.m_Controlled)
        SetNoFight(controlled, on);
}

// A near teleport keeps the AI's combat target and the bot chases it across the map: when nofight is first applied the
// bot drops its own target once and goes back to the non-combat engine (as HandleTeleportAck's far branch does)
void DropOwnTarget(Player& bot)
{
    PlayerbotAI* botAI = PlayerbotsMgr::instance().GetPlayerbotAI(&bot);
    if (!botAI || !botAI->GetAiObjectContext())
        return;

    AiObjectContext* context = botAI->GetAiObjectContext();
    context->GetValue<Unit*>("old target")->Set(nullptr);
    context->GetValue<Unit*>("current target")->Set(nullptr);
    context->GetValue<GuidVector>("prioritized targets")->Reset();
    context->GetValue<ObjectGuid>("pull target")->Set(ObjectGuid::Empty);
    // a hunter's Auto Shot outlives its target, and the core's IsNonMeleeSpellCast(false) counts it: the bot would
    // loot, use and equip nothing for the rest of the session
    bot.InterruptSpell(CURRENT_AUTOREPEAT_SPELL);
    bot.AttackStop();
    botAI->ChangeEngine(BOT_STATE_NON_COMBAT);
}

void ApplyNoFight(Player& bot)
{
    bool const first = !bot.IsImmuneToNPC();
    NoFight(bot, true);
    if (first)
        DropOwnTarget(bot);
}
void UndoNoFight(Player& bot) { NoFight(bot, false); }
void ApplyNoDamage(Player& bot) { bot.SetCommandStatusOn(CHEAT_GOD); }
void UndoNoDamage(Player& bot) { bot.SetCommandStatusOff(CHEAT_GOD); }
// casttrace on: the combat record's hooks find the bot (a no-op once it is in the record's registry; KeepStates
// repeats it). Off: out of the registry, and the known abilities are written again once it is set again.
void ApplyCombatTrace(Player& bot) { CombatRecord::Track(&bot); }
void UndoCombatTrace(Player& bot)
{
    CombatRecord::Untrack(&bot);
    if (PlayerbotAI* botAI = GET_PLAYERBOT_AI(&bot))
        botAI->combatKnownTraced = false;
}

// The rows with an apply and undo (TestConstraints::APPLIED); the other rows only set a bit their reader checks
constexpr TestConstraints::StateOps<Player> STATE_OPS[] = {
    {TestConstraints::NOFIGHT, ApplyNoFight, UndoNoFight},
    {TestConstraints::NODAMAGE, ApplyNoDamage, UndoNoDamage},
    {TestConstraints::CASTTRACE, ApplyCombatTrace, UndoCombatTrace},
};
static_assert(TestConstraints::Covered(STATE_OPS) == TestConstraints::APPLIED, "every applied row has apply and undo");
}  // namespace

// A test's constraint change: the state rows applied or undone, one Constraint row whose amount is the ms from the
// command to the take (also on an idle bot). A hold row turning on ends the current status unless a trip comes with
// it, so the bot stands idle; the idle gate then keeps it there. The planner's next snapshot sees the hearth bit; the
// errand row turning off forgets the last errand check (TestConstraints::FreesErrands): an errand check stamped before
// the hold (an inn trip, the login) would otherwise keep the released bot from its errands for the retry time (WB8b
// run 1: up to 5 minutes)
void TravelOrders::ApplyConstraints(PlayerbotAI* botAI, Player* bot, TravelOrder const& order, bool tripFollows)
{
    uint32 const before = botAI->testConstraints;
    uint32 const after = TestConstraints::Next(before, order.on, order.off);
    botAI->testConstraints = after;
    botAI->SetWork(BotWork::Constraints, (after & TestConstraints::STATES) != 0);
    botAI->planner.snapshotDue = true;
    if (TestConstraints::FreesErrands(before, after))
        WorldTravel::ForgetErrandChecks(
            botAI->GetAiObjectContext()->GetValue<WorldTravel::SlotMeasure>("slot measure")->RefGet());
    // A release ends the planner goals the bot holds (GoalApply::Release); the snapshot due above gives its first goal.
    // A status that runs on (a route order on a held bot) is not the goal's: the hold ended the goal's own
    if (TestConstraints::Releases(before, after) && botAI->planner.goals)
    {
        Planner::GoalApply::BotGoals& goals = *botAI->planner.goals;
        Planner::Goals::GoalKind const kind = goals.slot.Query().kind;
        uint32 const target = goals.slot.Query().target;
        if (Planner::GoalApply::Release(goals))
            LOG_INFO("playerbots", "[Route] {} goal {}:{} ended by the release", bot->GetName(),
                     Planner::Goals::KindWord(kind), target);
    }
    TestConstraints::ChangeStates(STATE_OPS, *bot, before, after);
    std::string const set = TestConstraints::Format(after);
    uint32 const latencyMs = GetMSTimeDiffToNow(order.postedMs);
    LOG_INFO("playerbots", "[Route] {} constraints: {} (taken after {} ms)", bot->GetName(), set, latencyMs);
    EventLog::Emit(bot, EventLog::EventType::Constraint, set.c_str(), 0, latencyMs);
    if (!TestConstraints::EndsStatus(before, after) || tripFollows)
        return;
    // An ordered end: a trip writes its abort rows with the source abort:held (the status core's trip arm)
    RpgStatus::End(botAI, StatusEnd::Outcome::Ordered, {.word = "hold", .source = "abort:held"});
}

void TravelOrders::KeepConstraints(Player* bot, uint32 constraints)
{
    TestConstraints::KeepStates(STATE_OPS, *bot, constraints);
}

void TravelOrders::FlushWalk(PlayerbotAI* botAI)
{
    if (TestConstraints::FlushesWalks(botAI->testConstraints))
        botAI->GetTravel().FlushMeshMoves(getMSTime());
}
