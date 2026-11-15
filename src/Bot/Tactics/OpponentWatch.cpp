/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include <algorithm>

#include "ArenaDefensive.h"
#include "ArenaInterrupt.h"
#include "Battleground.h"
#include "BotTacticsInternal.h"
#include "DefensiveTacticStrategy.h"
#include "Group.h"
#include "Log.h"
#include "NaturalReaction.h"
#include "PerfMonitor.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "PvpTestCounters.h"
#include "Spell.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "Unit.h"

namespace
{
constexpr uint64 BusyWatchSalt = 0x510E527FADE682D1ULL;
}  // namespace

// The GCD a spell starts (Spell::TriggerGlobalCooldown without the spell mods)
uint32 NaturalGcdMs(Player* bot, SpellInfo const* info)
{
    int32 gcd = static_cast<int32>(info->StartRecoveryTime);
    if (gcd < 1000 || gcd > 1500)
        return static_cast<uint32>(std::max(0, gcd));

    if (info->StartRecoveryCategory == 133 && gcd == 1500 && info->DmgClass != SPELL_DAMAGE_CLASS_MELEE &&
        info->DmgClass != SPELL_DAMAGE_CLASS_RANGED && !info->HasAttribute(SPELL_ATTR0_USES_RANGED_SLOT) &&
        !info->HasAttribute(SPELL_ATTR0_IS_ABILITY))
        gcd = static_cast<int32>(gcd * bot->GetFloatValue(UNIT_MOD_CAST_SPEED));

    return static_cast<uint32>(std::clamp(gcd, 1000, 1500));
}

namespace
{
// The bot's own interrupt by class (the interrupt triggers' spells; a warlock's is its pet's and needs no stop)
char const* OwnInterruptName(uint8 playerClass)
{
    switch (playerClass)
    {
        case CLASS_MAGE:
            return "counterspell";
        case CLASS_SHAMAN:
            return "wind shear";
        case CLASS_ROGUE:
            return "kick";
        case CLASS_DEATH_KNIGHT:
            return "mind freeze";
        case CLASS_HUNTER:
            return "silencing shot";
        case CLASS_PALADIN:
            return "hammer of justice";
        case CLASS_DRUID:
            return "bash";
        default:
            return nullptr;
    }
}
}  // namespace

// User note 1 (G0h3-b): a casting bot sees its target's cast bar like any player. Once its reaction is over (its draw
// and the shortcut's think cost, end to end from the bar's start) it stops its own cast for a ready kick of an
// important cast that its own cast would end too late for; the engine's kick follows in the same check. One stop
// per enemy cast (the trigger that picks the kick may not fire: a mage kicks only an enemy healer, a resto shaman's
// heal outranks its wind shear), and none within REACTION_STOP_BACKOFF_MS after a stop the kick did not follow.
bool StopOwnCastForKickDue(PlayerbotAI* botAI, Player* bot, uint32 ownLeftMs, uint32 nowMs)
{
    ReactionParams const& params = sPlayerbotAIConfig.NaturalReaction;
    AiObjectContext* const context = botAI->GetAiObjectContext();
    Unit* const enemy = context->GetValue<Unit*>("current target")->Get();
    if (!enemy || !enemy->IsInWorld() || !enemy->IsAlive() || !bot->IsHostileTo(enemy))
        return false;

    Spell* const cast = enemy->GetCurrentSpell(CURRENT_GENERIC_SPELL);
    if (!cast || cast->getState() != SPELL_STATE_PREPARING || cast->GetCastTime() <= 0 || !cast->GetSpellInfo())
        return false;

    uint32 const castMs = static_cast<uint32>(cast->GetCastTime());
    uint32 const enemyLeft = static_cast<uint32>(std::clamp(cast->GetCastTimeRemaining(), 0, cast->GetCastTime()));
    if (!StopOwnCastForKick(params.Reeval, castMs, enemyLeft, ownLeftMs))
        return false;

    // One decision per enemy cast, due from the bar's start (a pushback moves the end within half the cast)
    ReactionBusyState& busy = botAI->GetReactionBusy();
    uint32 const enemyGuid = enemy->GetGUID().GetCounter();
    uint32 const spellId = cast->GetSpellInfo()->Id;
    uint32 const endMs = nowMs + enemyLeft;
    if (!busy.Watches(enemyGuid, spellId, endMs, castMs / 2))
    {
        uint64 const guid = bot->GetGUID().GetCounter();
        uint32 ready = ReactionMs(botAI->GetReactionState(), params.MinMs,
                                  SplitMix64((guid << 32) ^ enemyGuid ^ endMs ^ BusyWatchSalt));
        if (params.Think.Enable)
            ready += botAI->GetReactionTypes().ThinkShortcutMs;

        uint32 const tick =
            params.ServerTickMs >= 0 ? static_cast<uint32>(params.ServerTickMs) : botAI->GetLastUpdateDiff() / 2;
        busy.Watch(enemyGuid, spellId, endMs, nowMs - (castMs - enemyLeft) + EndToEndTargetMs(params, ready, tick));
    }

    if (!busy.StopDue(nowMs))
        return false;

    char const* const kickName = OwnInterruptName(bot->getClass());
    uint32 const kickId = kickName ? context->GetValue<uint32>("spell id", kickName)->Get() : 0;
    if (!kickId || !botAI->IsInterruptableSpellCasting(enemy, kickName) || !botAI->CanCastSpell(kickId, enemy, true))
        return false;

    LOG_TRACE("playerbots.reaction", "busy stop skill {} spell {} cast {} left {} own {} late {} at {}",
              botAI->GetPvpSkill(), spellId, castMs, enemyLeft, ownLeftMs, nowMs - busy.WatchDueMs, nowMs);
    busy.NoteStop(nowMs, enemyGuid, kickId);
    if (PerfMonitor::IsEnabled())
        PerfMonitor::CountBlock(sPerfMonitor.acquire(PERF_MON_TOTAL, "NaturalReaction::StoppedOwnCast"));

    return true;
}

namespace
{
// G5b: whether the bot is the one of its group that counts what its side sees once (the lowest guid of the bots alive
// in its map): every bot of a side watches the same enemy heal, tactics.intHealDone counts it once
bool CountsForGroup(Player* bot)
{
    Group* const group = bot->GetGroup();
    if (!group)
        return true;

    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* const member = ref->GetSource();
        if (member && member != bot && member->IsInWorld() && member->IsAlive() && member->IsInMap(bot) &&
            member->GetGUID().GetCounter() < bot->GetGUID().GetCounter() && GET_PLAYERBOT_AI(member))
            return false;
    }

    return true;
}
}  // namespace

// The opponent memory (G0h3-b, DT.12): at the bot's own checks in a started match, the cast bars of its target and
// of its attackers (as many as the memory holds); a bar gone before its end without a visible interrupt (a school
// lock, a silence, a stun or fear, death) was cancelled. Cleared when the match changes or ends.
void ObserveOpponents(PlayerbotAI* botAI, Player* bot, bool observe, uint32 nowMs)
{
    OpponentMemory& memory = botAI->GetOpponentMemory();
    Battleground* const battleground = bot->GetBattleground();
    uint32 const matchId =
        battleground && battleground->GetStatus() == STATUS_IN_PROGRESS ? battleground->GetInstanceID() : 0;
    if (matchId != memory.MatchId)
    {
        if (memory.MatchId != 0)
        {
            uint32 enemies = 0;
            uint32 casts = 0;
            uint32 cancels = 0;
            for (OpponentMemorySlot const& slot : memory.Slots)
            {
                if (slot.Guid == 0)
                    continue;

                ++enemies;
                casts += slot.CastsSeen;
                cancels += slot.CancelsSeen;
            }

            LOG_TRACE("playerbots.reaction", "memory clear match {} skill {} enemies {} casts {} cancels {} at {}",
                      memory.MatchId, botAI->GetPvpSkill(), enemies, casts, cancels, nowMs);
        }

        memory.Clear(matchId);
    }

    if (!observe || matchId == 0)
        return;

    uint32 const tolerance = sPlayerbotAIConfig.NaturalReaction.Memory.EndToleranceMs;
    AiObjectContext* const context = botAI->GetAiObjectContext();
    // G5d: the enemies seen with a defensive up and their health (the Defensive tactic's opponent memory), at most
    // every DEFENSIVE_OBSERVE_MS
    bool defensiveScan = false;
    if (sPlayerbotAIConfig.TacticDefensive.OpponentEnable && botAI->TacticEnabledHere(TacticId::Defensive))
    {
        DefensiveBotState& defensive = botAI->GetDefensiveState();
        defensiveScan = defensive.LastObserveMs == 0 || nowMs - defensive.LastObserveMs >= DEFENSIVE_OBSERVE_MS;
        if (defensiveScan)
            defensive.LastObserveMs = std::max(nowMs, 1u);
    }

    uint32 seen = 0;
    auto const look = [&](Unit* unit)
    {
        if (!unit || !unit->IsInWorld() || !unit->IsPlayer() || !bot->IsHostileTo(unit))
            return;

        ++seen;
        uint32 const guid = unit->GetGUID().GetCounter();
        if (defensiveScan)
            ObserveEnemyDefensive(botAI, unit, nowMs);

        OpponentMemorySlot const* const slot = memory.Find(guid);
        Spell* const spell = unit->GetCurrentSpell(CURRENT_GENERIC_SPELL);
        if (spell && spell->getState() == SPELL_STATE_PREPARING && spell->GetCastTime() > 0 && spell->GetSpellInfo())
        {
            uint32 const left = static_cast<uint32>(std::clamp(spell->GetCastTimeRemaining(), 0, spell->GetCastTime()));
            uint32 const elapsed = static_cast<uint32>(spell->GetCastTime()) - left;
            uint32 const spellId = spell->GetSpellInfo()->Id;
            // The bar seen before (begun before the last look), maybe pushed back: the same cast
            bool const same =
                slot && slot->ViewEndMs != 0 && slot->SpellId == spellId && elapsed >= nowMs - slot->LastSeenMs;
            memory.NoteCast(guid, spellId, same ? slot->ViewEndMs : nowMs + left, nowMs, tolerance);
            return;
        }

        if (!slot || slot->ViewEndMs == 0)
            return;

        Player* const player = unit->ToPlayer();
        bool const interrupted =
            !unit->IsAlive() || unit->HasUnitState(UNIT_STATE_STUNNED | UNIT_STATE_CONFUSED | UNIT_STATE_FLEEING) ||
            unit->HasUnitFlag(UNIT_FLAG_SILENCED) || (player && player->HasSpellCooldown(slot->SpellId));
        // G5b: an enemy heal seen to its end (tactics.intHealDone), once per heal for the bot's group
        if (!interrupted && botAI->IsPvpTestCounting() &&
            static_cast<int32>(slot->ViewEndMs - nowMs) <= static_cast<int32>(tolerance) &&
            ArenaInterrupt::Classify(InterruptSpellFactsOf(sSpellMgr->GetSpellInfo(slot->SpellId))) ==
                ArenaInterrupt::Kind::Heal &&
            CountsForGroup(bot))
            botAI->CountTactic(TacticCounter::IntHealDone);

        memory.NoteCastGone(guid, nowMs, tolerance, interrupted);
    };

    Unit* const target = context->GetValue<Unit*>("current target")->Get();
    look(target);
    for (ObjectGuid const& guid : context->GetValue<GuidVector>("attackers")->LazyGet())
    {
        if (seen >= OPPONENT_MEMORY_SLOTS)
            break;

        if (!target || guid != target->GetGUID())
            look(botAI->GetUnit(guid));
    }
}
