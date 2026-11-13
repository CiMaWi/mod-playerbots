/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "BridgeIntentEffect.h"

#include <list>

#include "ObjectAccessor.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "PositionValue.h"
#include "ThreadCheck.h"

namespace PlayerbotBridge
{
PostMapJoin::PostMapJoin() : _thread(std::this_thread::get_id()) {}

bool PostMapJoin::Current() const
{
    // Unlike the diagnostic checker, an uninitialized world thread never grants authority.
    return _thread == std::this_thread::get_id() &&
           _thread == PlayerbotThreadCheck::worldThread.load(std::memory_order_relaxed);
}

namespace
{
// The strategies the follow and stay commands change (ChatShortcutActions.cpp), in Baseline::Strategies bit order,
// first for the non-combat state, then for the combat state. Runaway and guard are there because adding follow or stay
// drops them: the four are one sibling group (MovementStrategyContext, Engine::addStrategy).
char const* const MovementStrategies[] = {"follow", "stay", "runaway", "guard", "passive", "grind", "move from group"};
BotState const MovementStates[] = {BOT_STATE_NON_COMBAT, BOT_STATE_COMBAT};

NamedPosition ReadPosition(PositionMap& positions, std::string const& name)
{
    NamedPosition result;
    PositionMap::iterator const found = positions.find(name);
    if (found == positions.end() || !found->second.isSet())
        return result;

    result.Set = true;
    result.X = found->second.x;
    result.Y = found->second.y;
    result.Z = found->second.z;
    result.MapId = found->second.mapId;
    return result;
}

void WritePosition(PositionMap& positions, std::string const& name, NamedPosition const& position)
{
    if (position.Set)
        positions[name].Set(position.X, position.Y, position.Z, position.MapId);
    else if (positions.find(name) != positions.end())
        positions[name].Reset();
}

// The connected bot's AI, or null for an offline player or one that is no longer a bot. inWorld is false while the
// bot is out of the world, e.g. during a far teleport: it comes back, so that is not a logout.
PlayerbotAI* ConnectedBot(uint64_t guid, bool& inWorld)
{
    Player* bot = ObjectAccessor::FindConnectedPlayer(ObjectGuid(guid));
    PlayerbotAI* ai = bot ? GET_PLAYERBOT_AI(bot) : nullptr;
    inWorld = bot && bot->IsInWorld();
    return ai && ai->IsBotAI() ? ai : nullptr;
}

bool ReadPart(PlayerbotAI* ai, uint64_t bot, Part part, Baseline& out)
{
    out = Baseline();
    out.Bot = bot;
    out.Held = part;
    switch (part)
    {
        case Part::Movement:
        {
            Value<PositionMap&>* positions = ai->GetAiObjectContext()->GetValue<PositionMap&>("position");
            if (!positions)
                return false;

            uint16_t bit = 1;
            for (BotState state : MovementStates)
            {
                for (char const* name : MovementStrategies)
                {
                    if (ai->HasStrategy(name, state))
                        out.Strategies |= bit;

                    bit <<= 1;
                }
            }

            out.Stay = ReadPosition(positions->Get(), "stay");
            out.Return = ReadPosition(positions->Get(), "return");
            return true;
        }
        case Part::Formation:
        {
            UntypedValue* formation = ai->GetAiObjectContext()->GetUntypedValue("formation");
            if (!formation)
                return false;

            out.Formation = formation->Save();
            return true;
        }
        case Part::ManaSave:
        {
            Value<double>* level = ai->GetAiObjectContext()->GetValue<double>("mana save level");
            if (!level)
                return false;

            out.ManaSaveLevel = level->Get();
            return true;
        }
        case Part::FocusHeal:
        {
            Value<std::list<ObjectGuid>>* targets =
                ai->GetAiObjectContext()->GetValue<std::list<ObjectGuid>>("focus heal targets");
            if (!targets)
                return false;

            for (ObjectGuid const& target : targets->Get())
                out.FocusHealTargets.push_back(target.GetRawValue());

            out.FocusHealStrategy = ai->HasStrategy("focus heal targets", BOT_STATE_COMBAT);
            return true;
        }
        default:
            return false;
    }
}

// Writes target's part over current, which ReadPart just filled: only the strategies and values that differ.
bool WritePart(PlayerbotAI* ai, Baseline const& current, Baseline const& target)
{
    switch (target.Held)
    {
        case Part::Movement:
        {
            Value<PositionMap&>* positions = ai->GetAiObjectContext()->GetValue<PositionMap&>("position");
            if (!positions)
                return false;

            uint16_t bit = 1;
            for (BotState state : MovementStates)
            {
                std::string changes;
                for (char const* name : MovementStrategies)
                {
                    if ((current.Strategies & bit) != (target.Strategies & bit))
                    {
                        changes += changes.empty() ? "" : ",";
                        changes += (target.Strategies & bit) ? "+" : "-";
                        changes += name;
                    }

                    bit <<= 1;
                }

                if (!changes.empty())
                    ai->ChangeStrategy(changes, state);
            }

            if (current.Stay != target.Stay)
                WritePosition(positions->Get(), "stay", target.Stay);

            if (current.Return != target.Return)
                WritePosition(positions->Get(), "return", target.Return);

            return true;
        }
        case Part::Formation:
        {
            UntypedValue* formation = ai->GetAiObjectContext()->GetUntypedValue("formation");
            return formation && formation->Load(target.Formation);
        }
        case Part::ManaSave:
        {
            Value<double>* level = ai->GetAiObjectContext()->GetValue<double>("mana save level");
            if (!level)
                return false;

            level->Set(target.ManaSaveLevel);
            return true;
        }
        case Part::FocusHeal:
        {
            Value<std::list<ObjectGuid>>* targets =
                ai->GetAiObjectContext()->GetValue<std::list<ObjectGuid>>("focus heal targets");
            if (!targets)
                return false;

            std::list<ObjectGuid> list;
            for (uint64_t guid : target.FocusHealTargets)
                list.push_back(ObjectGuid(guid));

            targets->Set(list);
            if (current.FocusHealStrategy != target.FocusHealStrategy)
                ai->ChangeStrategy(target.FocusHealStrategy ? "+focus heal targets" : "-focus heal targets",
                                   BOT_STATE_COMBAT);

            return true;
        }
        default:
            return false;
    }
}
}  // namespace

bool Capture(PostMapJoin const& phase, uint64_t bot, Part part, Baseline& out)
{
    out = Baseline();
    if (!phase.Current())
        return false;

    bool inWorld = false;
    PlayerbotAI* ai = ConnectedBot(bot, inWorld);
    if (!ai || !inWorld || !ReadPart(ai, bot, part, out))
    {
        out = Baseline();
        return false;
    }

    return true;
}

RestoreResult Restore(PostMapJoin const& phase, uint64_t bot, Baseline const& before, Baseline const& applied)
{
    if (!phase.Current() || before.Held == Part::None || before.Held != applied.Held)
        return RestoreResult::Rejected;

    // Another bot's baseline is a caller's mix-up, never a reason to write.
    if (before.Bot != bot || applied.Bot != bot)
        return RestoreResult::Rejected;

    bool inWorld = false;
    PlayerbotAI* ai = ConnectedBot(bot, inWorld);
    if (!ai)
        return RestoreResult::Gone;

    // A teleporting bot is back in the world soon; the caller retries rather than dropping the bridge's state.
    if (!inWorld)
        return RestoreResult::Failed;

    Baseline current;
    if (!ReadPart(ai, bot, before.Held, current))
        return RestoreResult::Failed;

    if (current == before)
        return RestoreResult::Restored;

    // The same rule as a plan ledger's (Baseline::operator==; Movement by its strategies only).
    if (current != applied)
        return RestoreResult::Superseded;

    return WritePart(ai, current, before) ? RestoreResult::Restored : RestoreResult::Failed;
}
}  // namespace PlayerbotBridge
