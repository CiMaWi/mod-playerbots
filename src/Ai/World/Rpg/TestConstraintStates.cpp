/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "TestConstraintStates.h"

#include <string>

#include "BotTravel.h"
#include "EventLog.h"
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
void ReadWhereUsed(Player&) {}  // a row that only sets a bit its reader checks (TestConstraints.h names each reader)
// casttrace off: the known abilities are written again once it is set again (not in the apply: KeepStates repeats it)
void UndoCombatTrace(Player& bot)
{
    if (PlayerbotAI* botAI = GET_PLAYERBOT_AI(&bot))
        botAI->combatKnownTraced = false;
}

// The state rows' apply and undo (TestConstraints::ROWS)
constexpr TestConstraints::StateOps<Player> STATE_OPS[] = {
    {TestConstraints::NOFIGHT, ApplyNoFight, UndoNoFight},
    {TestConstraints::NODAMAGE, ApplyNoDamage, UndoNoDamage},
    {TestConstraints::FLYFAIL, ReadWhereUsed, ReadWhereUsed},
    {TestConstraints::FLYABORT, ReadWhereUsed, ReadWhereUsed},
    {TestConstraints::NODESTROY, ReadWhereUsed, ReadWhereUsed},
    {TestConstraints::NOTAXI, ReadWhereUsed, ReadWhereUsed},
    {TestConstraints::NOMOUNT, ReadWhereUsed, ReadWhereUsed},
    {TestConstraints::FLEE, ReadWhereUsed, ReadWhereUsed},
    {TestConstraints::NOUSE, ReadWhereUsed, ReadWhereUsed},
    {TestConstraints::NOSPEND, ReadWhereUsed, ReadWhereUsed},
    {TestConstraints::REALMASTER, ReadWhereUsed, ReadWhereUsed},
    {TestConstraints::ONSIGHT, ReadWhereUsed, ReadWhereUsed},
    {TestConstraints::NOFLY, ReadWhereUsed, ReadWhereUsed},
    {TestConstraints::NOCAST, ReadWhereUsed, ReadWhereUsed},
    {TestConstraints::NOVENDOR, ReadWhereUsed, ReadWhereUsed},
    {TestConstraints::NOPLAN, ReadWhereUsed, ReadWhereUsed},
    {TestConstraints::FREERIDE, ReadWhereUsed, ReadWhereUsed},
    {TestConstraints::SHORTGIVEUP, ReadWhereUsed, ReadWhereUsed},
    {TestConstraints::WALKFLUSH, ReadWhereUsed, ReadWhereUsed},
    {TestConstraints::CASTTRACE, ReadWhereUsed, UndoCombatTrace},
    {TestConstraints::ANYPATH, ReadWhereUsed, ReadWhereUsed},
};
static_assert(TestConstraints::Covered(STATE_OPS) == TestConstraints::STATES, "every state row has apply and undo");
}  // namespace

// A test's constraint change: the state rows applied or undone, one Constraint row whose amount is the ms from the
// command to the take (also on an idle bot). A choice row turning on ends the current status unless a trip comes with
// it, so the bot stands idle; the idle gate then keeps it there. The planner's next snapshot sees the hearth bit; the
// errand row turning off forgets the last errand check (TestConstraints::FreesErrands)
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
