/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_NATURALTRACE_H
#define _PLAYERBOT_NATURALTRACE_H

#include <atomic>
#include <string>

#include "Common.h"

class Battleground;
class ChatHandler;
class Player;
class Spell;
class SpellInfo;
class Unit;

// G0e: the natural tracer (Playerbots.Natural.Trace.*). A traced unit (a bot of a harness match with trace=1, a
// player of an organic arena match chosen by its match id, or a player switched on by a GM) leaves at most one "pos"
// row per grid slot of IntervalMs (100 ms: position, facing, movement flags, moving/spline, run speed, target, victim,
// health %, the current cast's spell, state, elapsed and remaining time), plus event rows (cast start, finish,
// cancel, interrupt received, target change, and for a real player its exact movement packets). The rows are plain
// values, buffered per unit and formatted on PvpTestRecordWriter's thread into <Dir>/trace-<YYYYMMDD>.jsonl, one JSON
// object per line. Skill is never written: rows carry the match id, which joins them to the match record.
// Threads: the samples and most events come from the unit's map thread; the registry (guid -> state) is guarded by
// one mutex, which a hook takes only for a unit whose lock-free mark is set. While nothing is traced, every hook
// returns after one relaxed load (Any).
class NaturalTrace
{
public:
    // The off path of every hook: 0 while no unit is traced (always with Enable = 0)
    static bool Any() { return Live.load(std::memory_order_relaxed) != 0; }

    // Map thread: the pre-gate point at the top of PlayerbotAI::UpdateAI (bots) and OnPlayerAfterUpdate (other
    // players): a target change row at once, a pos row when the unit's next 100 ms slot is due
    static void Sample(Player* player);
    // Map thread, PlayerbotAI::CastSpell after a successful prepare: a bot's cast with a cast time begins
    static void OnBotCastStart(Player* bot, Spell* spell, Unit* target);
    // PlayerbotAI::SpellInterrupted: the bot's cast was interrupted
    static void OnInterrupted(Player* bot, uint32 spellId);
    // The spell hooks of MatchTraceSpellScript (one script for both tracers)
    static void OnSpellPrepare(Spell* spell, Unit* caster, SpellInfo const* spellInfo);
    static void OnSpellCast(Spell* spell, Unit* caster, SpellInfo const* spellInfo);
    static void OnSpellCastCancel(Spell* spell, Unit* caster, SpellInfo const* spellInfo, bool bySelf);
    // A traced player logs out: its trace ends
    static void OnLogout(Player* player);

    // Map thread, MatchLog::OnArenaStart: with Organic, an arena match chosen by SampleEvery traces all its players
    // (when they fit under MaxUnits)
    static void OnArenaStart(Battleground* bg, std::string const& matchId);
    // A harness match with trace=1 is traced when SampleEvery chooses its match id
    static bool Selected(std::string const& matchId);
    // World thread (harness, commands): traces a player; false when off, already traced or MaxUnits is reached
    static bool Begin(Player* player, std::string const& matchId, char side, char const* source);
    // MatchLog::OnEnd and the harness tear-down: the traces of a match end
    static void EndMatch(std::string const& matchId);
    // Server shutdown: every trace ends (before the writer stops)
    static void Shutdown();

    // .playerbots natural trace <player> on|off | sample <N|off|config> | organic <on|off|config> | status | selftest
    static bool HandleTraceCommand(ChatHandler* handler, char const* args);
    static bool HandleSampleCommand(ChatHandler* handler, char const* args);
    static bool HandleOrganicCommand(ChatHandler* handler, char const* args);
    static bool HandleStatusCommand(ChatHandler* handler, char const* args);
    static bool HandleSelftestCommand(ChatHandler* handler, char const* args);

    // The number of traced units of the live registry
    static inline std::atomic<uint32> Live{0};
};

#endif
