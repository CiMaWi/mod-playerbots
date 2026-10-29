/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_LABCOPIES_H
#define _PLAYERBOT_LABCOPIES_H

#include <string>
#include <unordered_map>
#include <vector>

#include "Common.h"

class ChatHandler;
class Player;

// Lab copies (G0j-b): several bots of one class, race and level pinned to a template bot's skill, attributes, tactic
// gate values, talents and stored PvP loadout, so a lab plays many copies of one character at once (mirror pair
// tests). In memory, and only on a lab (Playerbots.Ladder.ServerId lab*): every pin elsewhere is refused, and so is a
// copy or template whose ladder team is hosted away. The copy's own talent link is kept at the pin; the unpin, the
// copy's logout (before the logout's save) and the server shutdown (before the bots log out) learn it again and drop
// the loadout pin. A crash while pinned leaves the template's talents in the lab's character rows (lab bots are
// throw-away; a lab refresh or a talent reset fixes them). The unpin gives the profile skill back, not a skill forced
// by a command before the pin. Copies share the gate values, not the per-use dice (rolled with the copy's own guid),
// so their tactic use is alike, not identical. The match records carry the template's guid (copyOf), so the reports
// count the copies' matches as one character's. World thread only (the commands, the harness's gather and tear-down,
// the logout and shutdown hooks): MapMgr::Update waits for the map threads before the commands and the world scripts
// run, so a map thread never sees half a pin.
class LabCopyMgr
{
public:
    static LabCopyMgr& Instance()
    {
        static LabCopyMgr instance;
        return instance;
    }

    // Pins the copy to the template; false with the reason in error. detail: what the pin did
    bool Pin(Player* templatePlayer, Player* copy, std::string& error, std::string& detail);
    // The copy's own skill, attributes, talents and loadout again; false with the reason when it is not a copy. An
    // offline copy (the logout hook missed it) loses its loadout pin and keeps its record, so an unpin once it is
    // online learns its talents back.
    bool Unpin(uint32 copyGuid, std::string& error, std::string& detail);
    // A bot logs out (PlayerbotHolder::LogoutPlayerBot before its save, the before-logout hook): a copy gets its own
    // talents back and loses its pins. O(1); returns at once while nothing is pinned
    void OnLogout(Player* bot);
    // Server shutdown: every copy unpinned before the bots log out
    void UnpinAll();
    bool Any() const { return !_pins.empty(); }
    // The template of a pinned copy, else 0
    uint32 TemplateOf(uint32 copyGuid) const;
    bool IsTemplate(uint32 guid) const;
    std::vector<uint32> Copies() const;
    // Playerbots.Ladder.ServerId starts with "lab": pins (and the harness's copyA / copyB) only there
    static bool IsLabServer();

    // .playerbots attributes pin <template> <bot>[,<bot>...] | unpin <bot>[,<bot>...]|all | show [<bot>] |
    // reapply [<bot>|all] | logout <bot> (a lab's random bot logs out: the copies' logout path, forced)
    static bool HandlePinCommand(ChatHandler* handler, char const* args);
    static bool HandleUnpinCommand(ChatHandler* handler, char const* args);
    static bool HandleShowCommand(ChatHandler* handler, char const* args);
    static bool HandleReapplyCommand(ChatHandler* handler, char const* args);
    static bool HandleLogoutCommand(ChatHandler* handler, char const* args);

private:
    LabCopyMgr() = default;

    struct PinRecord
    {
        uint32 Template = 0;
        std::string OwnTalents;  // the copy's talent link at the pin
        bool TalentsChanged = false;
    };

    std::unordered_map<uint32, PinRecord> _pins;
};

#endif
