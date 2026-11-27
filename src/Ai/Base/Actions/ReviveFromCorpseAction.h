/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_REVIVEFROMCORPSEACTION_H
#define PLAYERBOTS_REVIVEFROMCORPSEACTION_H

#include "MovementActions.h"

class PlayerbotAI;

struct GraveyardStruct;

class ReviveFromCorpseAction : public MovementAction
{
public:
    ReviveFromCorpseAction(PlayerbotAI* botAI) : MovementAction(botAI, "revive from corpse") {}

    bool Execute(Event event) override;
};

class FindCorpseAction : public MovementAction
{
public:
    FindCorpseAction(PlayerbotAI* botAI) : MovementAction(botAI, "find corpse") {}

    bool Execute(Event event) override;
    bool isUseful() override;
};

class SpiritHealerAction : public MovementAction
{
public:
    SpiritHealerAction(PlayerbotAI* botAI, std::string const name = "spirit healer") : MovementAction(botAI, name) {}

    GraveyardStruct const* GetGrave(bool startZone);
    bool Execute(Event event) override;
    bool isUseful() override;
};

// AW9: a risen bot recovers before it takes up its work (awareness INTERFACE.md §3.6): it moves off a place someone
// reaches, or stops, eats and drinks what it has and sits. Asked by the trigger `recovering`
class RecoverAction : public MovementAction
{
public:
    RecoverAction(PlayerbotAI* botAI) : MovementAction(botAI, "recover") {}

    bool Execute(Event event) override;
    bool isUseful() override;
};

// AW37b: judges the way of the route the bot's trip was given (awareness INTERFACE.md §4.8): a trip of its own with no
// safe way is ended and its goal written to the record. Asked by the trigger `way danger`, once per route
class WayDangerAction : public Action
{
public:
    WayDangerAction(PlayerbotAI* botAI) : Action(botAI, "way danger") {}

    bool Execute(Event event) override;
};

#endif
