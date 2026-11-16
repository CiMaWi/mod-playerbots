/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_CONFIGTABLES_H
#define _PLAYERBOT_CONFIGTABLES_H

#include <string>

class ChatHandler;

// The config tables of the server (ConfigTable.h): every family's walk and the unknown-key scan at config load, and the
// print command. Kept out of PlayerbotAIConfig.h so that adding a family does not rebuild every user of that header.

// Walks every family into its struct, then warns once per key that a config file sets under a family's prefix and no
// row knows (a retired row knows its key). Config load and `.reload config` only.
void LoadConfigTables();

// `playerbots config [prefix]`: one line per row under the prefix, after a "CONFIG <N> rows" line
void PrintConfigTables(ChatHandler* handler, std::string const& prefix);

void AddSC_config_tables();

#endif
