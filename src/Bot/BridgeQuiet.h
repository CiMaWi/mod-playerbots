/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_BRIDGEQUIET_H
#define _PLAYERBOT_BRIDGEQUIET_H

class Player;

// Speak no reply: a bridge act runs playerbots' own command actions, and the bridge's reply speaks for the bot, so
// their reply lines (TellMaster, TellMasterNoFacing, TellError) stay unsaid while the act's quiet scope lives. Both
// scopes are per thread: an act runs wholly on the thread that admitted it. Included only by the Tell functions'
// file and the act runner, never by PlayerbotAI.h.
namespace PlayerbotBridge
{
// A Tell made while an act is open but not quiet is a line of the bot's own beside the bridge's: one world log row.
constexpr char const* OwnLineEventType = "BridgeOwnLine";
// LC28a: a canned line the bot started itself and really said (src tell: its say without a master; broadcast); replies
// to a player's chat or orders are not counted; the bridge's own lines are OwnLineEventType rows
constexpr char const* CannedLineEventType = "CannedLine";

struct ThreadActState
{
    int Acts = 0;
    int Quiet = 0;
};

inline ThreadActState& ActState()
{
    thread_local ThreadActState state;
    return state;
}

enum class TellFate
{
    Speak,        // no act open
    Hold,         // an act's quiet scope: the line stays unsaid
    SpeakCounted  // an act open without its quiet scope: said, and counted
};

inline TellFate FateOfTell(ThreadActState const& state)
{
    if (state.Quiet > 0)
        return TellFate::Hold;

    return state.Acts > 0 ? TellFate::SpeakCounted : TellFate::Speak;
}

// One open act (the act runner's step).
class ActScope
{
public:
    ActScope() { ++ActState().Acts; }
    ~ActScope() { --ActState().Acts; }
    ActScope(ActScope const&) = delete;
    ActScope& operator=(ActScope const&) = delete;
};

// The bot's own reply lines stay unsaid while this lives (around a row's effect).
class QuietScope
{
public:
    QuietScope() { ++ActState().Quiet; }
    ~QuietScope() { --ActState().Quiet; }
    QuietScope(QuietScope const&) = delete;
    QuietScope& operator=(QuietScope const&) = delete;
};

// The Tell functions' one check: true when the line stays unsaid; writes an OwnLineEventType row for a line said
// while an act is open. Defined in BridgeAbilities.cpp.
bool HoldsTell(Player* bot);
// LC28a: a CannedLine row for a say without a master with no act open (the canned_lines night check)
void NoteCannedTell(Player* bot);
}  // namespace PlayerbotBridge

#endif
