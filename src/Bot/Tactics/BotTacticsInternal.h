/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_BOTTACTICSINTERNAL_H
#define _PLAYERBOT_BOTTACTICSINTERNAL_H

#include <atomic>

#include "ArenaInterrupt.h"
#include "ArenaInterruptSpells.h"
#include "Define.h"
#include "ObjectGuid.h"

class Player;
class PlayerbotAI;
class SpellInfo;
class Unit;
struct FakeBotState;
struct InterruptConfig;
struct TurnBotState;

// The tactic followers' game side that more than one of PlayerbotAI.cpp and the src/Bot/Tactics .cpp files calls;
// only these files and src/Bot/Debug/PvpTestCounters.cpp include this header.

// G5b: what the bot's interrupt (a kick, a silence, a stun) hits, read before the cast: an instant kick ends the bar
// within the cast itself
struct InterruptHit
{
    InterruptUse Use = InterruptUse::None;
    bool Bar = false;
    ArenaInterrupt::Kind CastKind = ArenaInterrupt::Kind::Other;
    uint32 SpellId = 0;
    uint32 CastMs = 0;
    uint32 ElapsedMs = 0;
};

// What CastSpell reads for the tactics before prepare() (the target's DR level, the kick's hit, the own target
// before the cast) and hands to BotTactics::AfterCast
struct CastNote
{
    ObjectGuid oldSel;
    ObjectGuid ownBefore;
    InterruptHit interruptHit;
    uint32 drGroup = 0;
    bool counting = false;
    bool followsKick = false;
    bool atImmune = false;
    bool notesFocus = false;
};

enum class FakeStep : uint8
{
    Pending,
    Done,
    Cancelled
};

extern std::atomic<bool> fakeTacticSeen;

uint32 NaturalGcdMs(Player* bot, SpellInfo const* info);
bool StopOwnCastForKickDue(PlayerbotAI* botAI, Player* bot, uint32 ownLeftMs, uint32 nowMs);
void ObserveOpponents(PlayerbotAI* botAI, Player* bot, bool observe, uint32 nowMs);
void FeedNaturalTrack(PlayerbotAI* botAI, Player* bot, uint32 nowMs);
bool InterruptFollows(InterruptConfig const& config);
// The definition gives count its default (true); callers in other files pass it.
void NoteInterruptHoldEnd(PlayerbotAI* botAI, InterruptHoldEnd end, uint32 heldMs, char const* cause, uint32 nowMs,
                          bool count);
void FollowInterruptFight(PlayerbotAI* botAI, Player* bot, uint32 nowMs);
void FollowFocusTarget(PlayerbotAI* botAI, Player* bot, uint32 nowMs);
InterruptHit ReadInterruptHit(Player* bot, SpellInfo const* spellInfo, Unit* target);
void CountInterruptHit(PlayerbotAI* botAI, Unit* target, InterruptHit const& hit, uint32 nowMs);
void NoteInterruptKick(PlayerbotAI* botAI, Player* bot, SpellInfo const* spellInfo, InterruptHit const& hit,
                       uint32 nowMs);
uint32 GetPvpTestCastGroup(SpellInfo const* spellInfo);
void NoteFocusAct(PlayerbotAI* botAI, Player* bot, SpellInfo const* spellInfo, Unit* target, ObjectGuid selectionBefore,
                  ObjectGuid ownBefore, bool counting, uint32 drGroup);
void DecideFakeCast(PlayerbotAI* botAI, Player* bot, uint32 spellId, uint32 nowMs);
FakeStep StepFakeCast(PlayerbotAI* botAI, Player* bot, FakeBotState& state, uint32 nowMs, uint32 tickMs);
bool StepNaturalTurn(Player* bot, TurnBotState& state, uint32 nowMs);

#endif
