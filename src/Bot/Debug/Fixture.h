/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_FIXTURE_H
#define _PLAYERBOT_FIXTURE_H

class ChatHandler;

// Test fixture console command (combat foundation CF2a), on the world thread: gives a test bot a named spec, its
// trainer spells and level-appropriate gear through the factory's own functions, and reads the result back as
// AiFactory sees it. Test-only; nothing here runs unless a scenario types it.
namespace Fixture
{
// playerbots fixture <verb> <bot> [<name>]
//   spec <bot> list                  one line per premade spec of the bot's class: `<n> <name> tab=<0..2>`
//   spec <bot> <name>                the named spec's talents for the bot's level (the factory's
//                                    InitTalentsBySpecNo, reset first) and its glyphs; `frost` finds `frost pve`
//   spells <bot>                     the class and trainer spells of the bot's level (InitClassSpells,
//                                    InitAvailableSpells); the reply names the count taught
//   gear <bot>                       the level's equipment (InitEquipment, InitAmmo) at the random-gear quality limit,
//                                    no enchants or gems; a worn PvP loadout is left as it is
//   show <bot>                       one FIXTURE show line: class, level, the spec AiFactory sees, points, slots
//   check <bot> <name> [gear]        the verdict a scenario reads: `FIXTURE check <bot>: ok|gap|error, ...`
bool HandleCommand(ChatHandler* handler, char const* args);
}  // namespace Fixture

#endif
