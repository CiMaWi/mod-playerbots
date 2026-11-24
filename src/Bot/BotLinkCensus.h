/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_BOTLINKCENSUS_H
#define _PLAYERBOT_BOTLINKCENSUS_H

// MC4: the link census of the core (ReachCounter.h) learns the references only the module knows: a bot's master,
// its group leader and the targets it stores. Registers the families and the one walker; call once at world start,
// before the first map update.
void RegisterBotLinkCensus();

#endif
