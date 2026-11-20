/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_BOTPASTMGR_H
#define PLAYERBOTS_BOTPASTMGR_H

#include <mutex>
#include <string>
#include <vector>

#include "Define.h"
#include "PastRules.h"

class ChatHandler;

// Every bot's past (living-conversation LC20a, PastRules.h): background statements (playerbots_bot_background) and
// colour lines (playerbots_bot_colour). Both tables load with one query each at startup (world thread); an edit
// changes memory under one short lock and queues its asynchronous write (PlayerbotsDatabase.Execute, FIFO like the
// record's). No call queries the database after startup; readers get copies. A deleted bot's rows go with
// BotStores::Delete (BotStores.h), its memory with Forget.
class BotPastMgr
{
public:
    static BotPastMgr& Instance()
    {
        static BotPastMgr instance;
        return instance;
    }

    // Startup, world thread
    void Load();
    // Any thread: copies, by ordinal and by event id
    std::vector<PastRules::Statement> Statements(uint32 guid);
    std::vector<PastRules::Colour> Colours(uint32 guid);
    // World thread (the colour checks the bot's record row). None when applied and written
    PastRules::Refusal SetStatement(uint32 guid, PastRules::Statement statement);
    PastRules::Refusal Revise(uint32 guid, uint8 ordinal, uint8 version, std::string text);
    PastRules::Refusal SetColour(uint32 guid, PastRules::Colour colour);
    // World thread: the bot's past, from memory and both tables (the console's clear)
    void Clear(uint32 guid);
    // World thread. A deleted bot: drops its past from memory (BotStores::Delete deletes the rows)
    void Forget(uint32 guid);

    // .playerbots past [<bot name|guid> [set <n> <text> | revise <n> <version> <text> | colour <event id> <text> |
    //   event <event id> | clear]]; event id 0 names the bot's newest permanent row (tests)
    static bool HandlePastCommand(ChatHandler* handler, char const* args);

private:
    BotPastMgr() = default;

    std::mutex _lock;
    PastRules::Book _book;
};

#endif
