/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_BOTPROFESSIONMGR_H
#define PLAYERBOTS_BOTPROFESSIONMGR_H

#include <optional>

#include "Define.h"
#include "ProfessionRules.h"

class ChatHandler;
class Player;

// A persistent bot's professions as part of its person (professions plan WR2, ProfessionRules.h), stored beside the
// traits in playerbots_bot_professions, one BotRowStore row per bot (CS27). All rows are loaded with one query at
// startup (world thread; the startup check names the update when the table is missing); a bot gets its person at login,
// new and changed rows are written asynchronously. LoadOrCreate also writes from a bot's map thread (the factory): the
// store's lock keeps a bot's saves in order. Find reads from any thread and returns a copy; nothing queries after
// startup. A bot's delete drops its row through the forget the load registers (BotStores::Delete).
class BotProfessionMgr
{
public:
    static BotProfessionMgr& instance()
    {
        static BotProfessionMgr instance;
        return instance;
    }

    // Startup, world thread: loads every row and registers the store's forget
    void Load();
    // The bot's own thread (its login on the world thread, the factory on its map): the bot's person; a bot without
    // one gets the professions it holds (WR.7) or, holding none, the first roll, and it is saved. Reads only memory.
    ProfessionRules::BotProfessions LoadOrCreate(Player* bot);
    // The bot's own thread: the professions the factory and the trainers go by. A bot that keeps its possessions
    // (BotKind::KeepsPossessions: a persistent bot no player leads) has its stored person (LoadOrCreate); any other
    // bot (a real master's, an arena instance's, a lab fixture's) its stored person if it once got one, else the
    // rules' answer (held professions first, WR.7), which is not stored
    ProfessionRules::BotProfessions PersonOf(Player* bot);
    // Any thread: a copy of the bot's person; none when it has none yet
    std::optional<ProfessionRules::BotProfessions> Find(uint32 guid);
    // The bot's own thread: the skill path's second choice (WR11) stores a second gatherer in place of the person's
    // craft while the bot holds neither; true when the person changed (origin Chosen)
    bool ChooseSecond(Player* bot, ProfessionRules::Skill craft, ProfessionRules::Skill gatherer);

    // A ProfessionHeld row per profession skill the bot holds (skill id, value); source names the moment
    static void EmitHeld(Player* bot, char const* source);
    // A ProfessionHeld row when a bot learns (old value 0) or loses (new value 0) a profession skill
    static void NoteSkillChange(Player* bot, uint32 skillId, uint32 oldValue, uint32 newValue);

    // .playerbots professions [<bot name|guid> [set <profession>... | reroll] | sample <N> | selftest |
    //                          index [<profession>] [dump [<file>]] | path <bot>]
    static bool HandleProfessionsCommand(ChatHandler* handler, char const* args);

private:
    BotProfessionMgr() = default;
};

#endif
