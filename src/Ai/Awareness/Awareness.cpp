/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "Awareness.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include "BotTravel.h"
#include "CellImpl.h"
#include "Chat.h"
#include "ChatHelper.h"
#include "Corpse.h"
#include "Creature.h"
#include "DBCStores.h"
#include "DangerFold.h"
#include "DatabaseEnv.h"
#include "EventLog.h"
#include "EventLogFormat.h"
#include "EventWanted.h"
#include "GameGraveyard.h"
#include "GhostWait.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Group.h"
#include "HeldTrip.h"
#include "Log.h"
#include "Map.h"
#include "MotionMaster.h"
#include "MoveSpline.h"
#include "NewRpgStatusEnd.h"
#include "ObjectAccessor.h"
#include "PathGenerator.h"
#include "PerfMonitor.h"
#include "PlannerIndexes.h"
#include "PlannerService.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "RouteService.h"
#include "SkillPathBridge.h"
#include "StartingHistoryRules.h"
#include "StringConvert.h"
#include "StringFormat.h"
#include "TestConstraints.h"
#include "Timer.h"
#include "TravelGraph.h"
#include "TravelLegRules.h"
#include "TravelMgr.h"
#include "TravelPlaces.h"
#include "Upkeep.h"
#include "UpkeepRules.h"
#include "Util.h"
#include "WaypointMgr.h"
#include "World.h"
#include "WorldLog.h"
#include "WorldSession.h"

namespace Awareness
{
using AwarenessRules::Busy;
using AwarenessRules::Kind;
using AwarenessRules::Motion;
using AwarenessRules::Picture;
using AwarenessRules::Rank;
using AwarenessRules::Role;
using AwarenessRules::Seen;
using AwarenessRules::Side;
using AwarenessRules::UnitType;

namespace
{
// Tuning value: the sight range, the range `possible targets` uses; planar, so a count of spawns compares
constexpr float SIGHT_YARDS = 100.0f;
// Tuning value: how far an observed aggro distance may lie from the reach the picture named (PLAN AW4); a
// check's within= sets another bound
constexpr float REACH_BOUND_YARDS = 2.0f;

// Totals since the server started; relaxed, they only count
std::atomic<std::uint64_t> reads{0};
std::atomic<std::uint64_t> fills{0};
std::atomic<std::uint64_t> fillsFull{0};
std::atomic<std::uint64_t> fillsAttackerLeftOut{0};
std::atomic<std::uint64_t> fillMicros{0};
// The tickperf window's start; read and written only by the world-thread console
std::uint64_t meterReads = 0;
std::uint64_t meterFills = 0;
std::uint64_t meterMicros = 0;

std::size_t SideIndex(Side side) { return static_cast<std::size_t>(side); }

Rank RankOf(Creature const* creature)
{
    if (creature->IsDungeonBoss())
        return Rank::Boss;
    switch (creature->GetCreatureTemplate()->rank)
    {
        case CREATURE_ELITE_ELITE:
            return Rank::Elite;
        case CREATURE_ELITE_RAREELITE:
        case CREATURE_ELITE_RARE:
            return Rank::Rare;
        case CREATURE_ELITE_WORLDBOSS:
            return Rank::Boss;
        default:
            return Rank::Normal;
    }
}

// Tuning value: the classes whose players fight with spells
bool CastsByClass(std::uint8_t playerClass)
{
    return playerClass == CLASS_MAGE || playerClass == CLASS_PRIEST || playerClass == CLASS_WARLOCK;
}

Role RoleOf(Player* bot)
{
    if (PlayerbotAI::IsTank(bot))
        return Role::Tank;
    if (PlayerbotAI::IsHeal(bot))
        return Role::Healer;
    return PlayerbotAI::IsRanged(bot) ? Role::Ranged : Role::Melee;
}

// A creature's kind of movement is its current generator (a pause keeps it); `generator` is unused for the others
Motion MotionOf(Unit const* unit, bool byGenerator, MovementGeneratorType generator)
{
    if (!byGenerator)
        return unit->isMoving() ? Motion::Other : Motion::Standing;
    switch (generator)
    {
        case IDLE_MOTION_TYPE:
            return Motion::Standing;
        case RANDOM_MOTION_TYPE:
        case ANIMAL_RANDOM_MOTION_TYPE:
            return Motion::Wandering;
        case WAYPOINT_MOTION_TYPE:
        case ESCORT_MOTION_TYPE:
        case FORMATION_MOTION_TYPE:
            return Motion::Path;
        case HOME_MOTION_TYPE:
            return Motion::Home;
        case CHASE_MOTION_TYPE:
            return Motion::Chasing;
        default:
            return Motion::Other;
    }
}

// The one grid visit: creatures, pets and players in one pass, both sides
struct Worker
{
    Player* bot;
    Group* group;
    Store& store;

    void Visit(PlayerMapType& m)
    {
        for (PlayerMapType::iterator itr = m.begin(); itr != m.end(); ++itr)
            Offer(itr->GetSource());
    }

    void Visit(CreatureMapType& m)
    {
        for (CreatureMapType::iterator itr = m.begin(); itr != m.end(); ++itr)
            Offer(itr->GetSource());
    }

    template <class NOT_INTERESTED>
    void Visit(GridRefMgr<NOT_INTERESTED>&)
    {
    }

    bool InGroup(ObjectGuid guid) const { return guid == bot->GetGUID() || (group && group->IsMember(guid)); }

    void Offer(Unit* unit)
    {
        if (unit == bot || !unit->IsInWorld() || !unit->InSamePhase(bot) || unit->GetExactDist2d(bot) > SIGHT_YARDS)
            return;
        Creature* creature = unit->ToCreature();
        // What cannot fight: a trigger, an unattackable unit, a critter nobody fights
        if ((creature && creature->IsTrigger()) || unit->HasUnitFlag(UNIT_FLAG_NON_ATTACKABLE) ||
            unit->HasUnitFlag(UNIT_FLAG_NON_ATTACKABLE_2) || (unit->IsCritter() && !unit->IsInCombat()))
            return;
        if (!unit->IsAlive())
        {
            if (store.deadSkipped < DEAD_KEPT)
                store.deadEntries[store.deadSkipped] = unit->GetEntry();
            ++store.deadSkipped;
            return;
        }
        // A dead bot (a ghost or a body not yet released) is asked with the engine's distance check, the call the
        // engine makes for what a client is shown (core PlayerUpdates.cpp): a ghost sees the living only near its
        // corpse, and without that check it is refused every living creature (core Object.cpp, the corpse rule)
        if (!(bot->IsAlive() ? bot->CanSeeOrDetect(unit) : bot->CanSeeOrDetect(unit, false, true)))
            return;
        AwarenessRules::Admit(store.picture, Describe(unit, creature));
    }

    Seen Describe(Unit* unit, Creature* creature) const
    {
        Seen seen;
        seen.guid = unit->GetGUID().GetRawValue();
        seen.entry = unit->GetEntry();
        Player* player = unit->ToPlayer();
        bool const pet = !player && unit->IsControlledByPlayer();
        ObjectGuid const owner = pet ? unit->GetCharmerOrOwnerGUID() : ObjectGuid::Empty;
        if (player)
        {
            seen.type = UnitType::Player;
            seen.playerClass = player->getClass();
        }
        else if (pet)
        {
            seen.type = UnitType::Pet;
            seen.ownerGuid = owner.GetRawValue();
        }

        Unit* victim = unit->GetVictim();
        bool const hostile = unit->IsHostileTo(bot);
        if (!hostile && unit->IsFriendlyTo(bot))
        {
            seen.side = Side::Ally;
            if (player)
                seen.kind = InGroup(unit->GetGUID()) ? Kind::Group : Kind::Player;
            else if (pet)
                seen.kind = InGroup(owner) ? Kind::Group : Kind::Player;
            else
                seen.kind = creature && creature->IsGuard() ? Kind::Guard : Kind::Creature;
        }
        else if (hostile)
        {
            seen.side = Side::Enemy;
            seen.canAttackNow = player && player->GetTeamId() != bot->GetTeamId() && bot->IsValidAttackTarget(unit) &&
                                unit->IsValidAttackTarget(bot);
        }
        else
        {
            // A neutral unit is an enemy while it fights the bot's side
            bool const fightsOurs = unit->IsInCombat() && victim && (victim == bot || victim->IsFriendlyTo(bot));
            seen.side = fightsOurs ? Side::Enemy : Side::Neither;
        }

        seen.levelDiff = static_cast<std::int16_t>(int(unit->GetLevel()) - int(bot->GetLevel()));
        seen.rank = creature && !pet ? RankOf(creature) : Rank::Normal;
        seen.guard = creature && !pet && creature->IsGuard();
        seen.casts = player ? CastsByClass(seen.playerClass) : unit->GetMaxPower(POWER_MANA) > 0;
        seen.creatureType = unit->GetCreatureType();
        seen.healthPct = unit->GetHealthPct();
        seen.x = unit->GetPositionX();
        seen.y = unit->GetPositionY();
        seen.z = unit->GetPositionZ();
        // A pet follows its owner: its home is where it stands
        Position const& home = creature && !pet ? creature->GetHomePosition() : static_cast<Position const&>(*unit);
        seen.homeX = home.GetPositionX();
        seen.homeY = home.GetPositionY();
        seen.homeZ = home.GetPositionZ();
        // The engine's proximity aggro (Creature::CanStartAttack): the template's detection range, not the fixed 20
        // of GetAttackDistance. A civilian and a creature immune to players start no attack on the bot (its first
        // refusals); an evading creature has the reach it will have at home, `busy` says it evades (AW7)
        if (creature && !pet && seen.side == Side::Enemy && creature->HasReactState(REACT_AGGRESSIVE) &&
            !creature->IsCivilian() && !creature->IsImmuneToPC())
            seen.reachYards = creature->GetAggroRange(bot) + creature->m_CombatDistance;

        MovementGeneratorType const generator =
            creature ? creature->GetMotionMaster()->GetCurrentMovementGeneratorType() : NULL_MOTION_TYPE;
        seen.motion = MotionOf(unit, creature && !pet, generator);
        if (creature && creature->IsInEvadeMode())
            seen.busy = Busy::Evading;
        else if (unit->IsInCombat())
            seen.busy = Busy::Fighting;
        else if (creature && generator == WAYPOINT_MOTION_TYPE)
        {
            seen.busy = Busy::OnPath;
            // Its velocity: toward the spline's next point at the spline's speed; 0 and 0 while it waits at a node
            Movement::MoveSpline const* spline = creature->movespline;
            if (spline && spline->Initialized() && !spline->Finalized())
            {
                G3D::Vector3 const next = spline->CurrentDestination();
                float const dx = next.x - seen.x;
                float const dy = next.y - seen.y;
                float const yards = std::hypot(dx, dy);
                if (yards > 0.1f)
                {
                    seen.moveX = dx / yards * spline->Velocity();
                    seen.moveY = dy / yards * spline->Velocity();
                }
            }
        }
        seen.fightsGuid = victim ? victim->GetGUID().GetRawValue() : 0;
        seen.taggedByOther = creature && creature->hasLootRecipient() && !creature->isTappedBy(bot);
        return seen;
    }
};

void FillSelf(PlayerbotAI* botAI, Player* bot, AwarenessRules::Self& self)
{
    self.guid = bot->GetGUID().GetRawValue();
    self.x = bot->GetPositionX();
    self.y = bot->GetPositionY();
    self.z = bot->GetPositionZ();
    self.level = bot->GetLevel();
    self.playerClass = bot->getClass();
    self.role = RoleOf(bot);
    self.healthPct = bot->GetHealthPct();
    self.powerPct = bot->GetPowerPct(bot->getPowerType());
    self.avgItemLevel = static_cast<float>(botAI->GetEquipGearScore(bot));
    Unit* victim = bot->GetVictim();
    self.fightsGuid = victim ? victim->GetGUID().GetRawValue() : 0;
}

void Fill(PlayerbotAI* botAI, Player* bot, Store& store, std::uint32_t nowMs)
{
    PerfMonitorOperation* pmo =
        PerfMonitor::IsEnabled() ? sPerfMonitor.start(PERF_MON_VALUE, "surroundings", nullptr) : nullptr;
    auto const started = std::chrono::steady_clock::now();

    Picture& picture = store.picture;
    picture = Picture{};
    store.deadSkipped = 0;
    FillSelf(botAI, bot, picture.self);
    Worker worker{bot, bot->GetGroup(), store};
    Cell::VisitObjects(bot, worker, SIGHT_YARDS);
    AwarenessRules::LinkUp(picture);
    // Line of sight only for the kept units: a crowded town pays at most PICTURE_SIZE tests
    for (std::size_t i = 0; i < picture.count; ++i)
    {
        Unit* unit = ObjectAccessor::GetUnit(*bot, ObjectGuid(picture.units[i].guid));
        picture.units[i].inSight = unit && bot->IsWithinLOSInMap(unit);
    }
    picture.builtMs = nowMs;
    store.built = true;

    bool full = false;
    for (std::uint16_t const left : picture.leftOut)
        full = full || left > 0;
    if (full)
    {
        fillsFull.fetch_add(1, std::memory_order_relaxed);
        // Whether a unit that fights the bot was left out: only asked when the array was full
        bool attackerOut = false;
        if (bot->IsInCombat())
            for (Unit* attacker : bot->getAttackers())
                attackerOut = attackerOut || AwarenessRules::SlotOf(picture, attacker->GetGUID().GetRawValue()) ==
                                                 AwarenessRules::NOT_IN_PICTURE;
        if (attackerOut)
            fillsAttackerLeftOut.fetch_add(1, std::memory_order_relaxed);
    }

    store.fillUs = static_cast<std::uint32_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - started).count());
    fills.fetch_add(1, std::memory_order_relaxed);
    fillMicros.fetch_add(store.fillUs, std::memory_order_relaxed);
    if (pmo)
        pmo->finish();
}

std::string NameOf(Player* bot, Seen const& unit)
{
    Unit* found = ObjectAccessor::GetUnit(*bot, ObjectGuid(unit.guid));
    return found ? found->GetName() : Acore::StringFormat("#{}", unit.entry);
}

std::string Whom(Seen const& unit)
{
    if (!unit.fightsGuid)
        return "nobody";
    if (unit.fightsIndex == AwarenessRules::THE_BOT)
        return "bot";
    if (unit.fightsIndex == AwarenessRules::NOT_IN_PICTURE)
        return "outside";
    return Acore::StringFormat("#{}", unit.fightsIndex);
}

std::string TotalsText()
{
    return Acore::StringFormat("fills {} full {} attacker-left-out {}", fills.load(std::memory_order_relaxed),
                               fillsFull.load(std::memory_order_relaxed),
                               fillsAttackerLeftOut.load(std::memory_order_relaxed));
}

// The whole listing of `aw look`: the header, one line per unit, the done line
std::vector<std::string> Listing(Player* bot, Store const& store)
{
    Picture const& picture = store.picture;
    AwarenessRules::Self const& self = picture.self;
    std::string const name = bot->GetName();
    std::vector<std::string> lines;
    lines.reserve(picture.count + 2);
    lines.push_back(Acore::StringFormat(
        "AW look {}: lvl {} {} ilvl {:.0f} hp {:.0f}% pw {:.0f}% at {:.1f} {:.1f} {:.1f} | units {} left out e{} a{} "
        "n{} dead skipped {} | built {} ago | fill {} us | totals {}",
        name, self.level, ChatHelper::FormatClass(self.playerClass), self.avgItemLevel, self.healthPct, self.powerPct,
        self.x, self.y, self.z, picture.count, picture.leftOut[SideIndex(Side::Enemy)],
        picture.leftOut[SideIndex(Side::Ally)], picture.leftOut[SideIndex(Side::Neither)], store.deadSkipped,
        getMSTimeDiff(picture.builtMs, getMSTime()), store.fillUs, TotalsText()));
    std::array<std::uint32_t, static_cast<std::size_t>(Side::Count)> sides{};
    for (std::size_t i = 0; i < picture.count; ++i)
    {
        Seen const& unit = picture.units[i];
        ++sides[SideIndex(unit.side)];
        lines.push_back(Acore::StringFormat(
            "AW unit {} {} | {} {} | {} | d{:+} {} | hp {:.0f}% | {:.1f} yd | reach {:.1f} | {} -> {} | {}{}", i,
            NameOf(bot, unit), AwarenessRules::Word(unit.side), AwarenessRules::Word(unit.kind),
            AwarenessRules::Word(unit.type), int(unit.levelDiff), AwarenessRules::Word(unit.rank), unit.healthPct,
            AwarenessRules::DistanceToDisc(unit, self.x, self.y), unit.reachYards, AwarenessRules::Word(unit.busy),
            Whom(unit), unit.inSight ? "sight" : "no sight",
            unit.moveX != 0.0f || unit.moveY != 0.0f
                ? Acore::StringFormat(" | move {:.2f} {:.2f} at {:.1f} {:.1f}", unit.moveX, unit.moveY, unit.x, unit.y)
                : std::string()));
    }
    lines.push_back(Acore::StringFormat("AW look done: {} units {} enemies {} allies {} neither {}", name,
                                        picture.count, sides[SideIndex(Side::Enemy)], sides[SideIndex(Side::Ally)],
                                        sides[SideIndex(Side::Neither)]));
    return lines;
}

// The bot's store after a Look, or null with the error sent when the bot is not in the world
Store* LookedUp(ChatHandler* handler, PlayerbotAI* botAI)
{
    Player* bot = botAI->GetBot();
    if (!bot || !bot->IsInWorld())
    {
        handler->SendErrorMessage("AW: the bot is not in the world");
        return nullptr;
    }
    Look(botAI);
    return botAI->awareness.get();
}

std::string_view Trimmed(std::string_view text)
{
    while (!text.empty() && text.front() == ' ')
        text.remove_prefix(1);
    while (!text.empty() && text.back() == ' ')
        text.remove_suffix(1);
    return text;
}

float AssistRadius() { return sWorld->getFloatConfig(CONFIG_CREATURE_FAMILY_ASSISTANCE_RADIUS); }

// The engine's questions on the place test's answers: whether each would assist the target against the bot and
// whether the target sees it. Slots of `picture`, which must be the bot's current picture
AwarenessRules::UnitList JoinersIn(Player* bot, Picture const& picture, std::int16_t slot)
{
    AwarenessRules::UnitList joiners;
    if (slot < 0 || slot >= picture.count)
        return joiners;
    Seen const& centre = picture.units[slot];
    Creature* target = ObjectAccessor::GetCreature(*bot, ObjectGuid(centre.guid));
    // The engine calls no help for a pet or a charmed creature
    if (!target || target->IsPet() || target->IsCharmed())
        return joiners;
    float const radius = AssistRadius();
    for (std::size_t i = 0; i < picture.count; ++i)
    {
        Seen const& unit = picture.units[i];
        if (!AwarenessRules::WouldAssistByPlace(unit, centre, radius))
            continue;
        Creature* neighbour = ObjectAccessor::GetCreature(*bot, ObjectGuid(unit.guid));
        if (neighbour && neighbour->CanAssistTo(target, bot) && target->IsWithinLOSInMap(neighbour))
            joiners.refs[joiners.size++] = static_cast<std::int16_t>(i);
    }
    return joiners;
}

// The creature a test names: `<entry>` (the nearest of that entry), `guid:<counter>`, or a name (`_` for a space,
// the nearest of that name). Its slot, or NOT_IN_PICTURE
std::int16_t PickTarget(Player* bot, Picture const& picture, std::string_view text)
{
    Optional<std::uint32_t> const entry = Acore::StringTo<std::uint32_t>(text);
    Optional<std::uint32_t> const counter =
        text.substr(0, 5) == "guid:" ? Acore::StringTo<std::uint32_t>(text.substr(5)) : Optional<std::uint32_t>();
    std::string name(text);
    std::replace(name.begin(), name.end(), '_', ' ');
    std::int16_t best = AwarenessRules::NOT_IN_PICTURE;
    for (std::size_t i = 0; i < picture.count; ++i)
    {
        Seen const& unit = picture.units[i];
        if (unit.type != UnitType::Creature)
            continue;
        bool matches = false;
        if (entry)
            matches = unit.entry == *entry;
        else if (counter)
            matches = ObjectGuid(unit.guid).GetCounter() == *counter;
        else
            matches = StringEqualI(NameOf(bot, unit), name);
        if (matches && (best < 0 || AwarenessRules::Nearer(unit, picture.units[best], picture.self.x, picture.self.y)))
            best = static_cast<std::int16_t>(i);
    }
    return best;
}

// The unit `aw judge ... sides|target` names: a name first (`_` for a space; a player, a pet or a creature, the
// nearest of that name), else PickTarget's `<entry>` or `guid:<counter>`, which name creatures only (a player's counter
// can equal a creature's). Its slot, or NOT_IN_PICTURE
std::int16_t PickUnit(Player* bot, Picture const& picture, std::string_view text)
{
    std::string name(text);
    std::replace(name.begin(), name.end(), '_', ' ');
    std::int16_t best = AwarenessRules::NOT_IN_PICTURE;
    for (std::size_t i = 0; i < picture.count; ++i)
    {
        Seen const& unit = picture.units[i];
        if (StringEqualI(NameOf(bot, unit), name) &&
            (best < 0 || AwarenessRules::Nearer(unit, picture.units[best], picture.self.x, picture.self.y)))
            best = static_cast<std::int16_t>(i);
    }
    return best >= 0 ? best : PickTarget(bot, picture, text);
}

// A strength in thousandths as the rules' figure (2.500 for a player at its level and full health)
double Figure(std::uint32_t thousandths) { return static_cast<double>(thousandths) / 1000.0; }

// The slots of a list as ` #a,#b`, empty for none
std::string SlotsText(AwarenessRules::UnitList const& list)
{
    std::string text;
    for (std::uint8_t i = 0; i < list.size; ++i)
        text += Acore::StringFormat("{}#{}", i ? "," : " ", list.refs[i]);
    return text;
}

// Who would join the named target, as `aw look` and `aw check` print it
struct Join
{
    std::int16_t target = AwarenessRules::NOT_IN_PICTURE;
    AwarenessRules::UnitList joiners;
    std::string names;  // `<name> #<slot> <yd from the target>`, comma separated
    std::string line;   // `AW join <target> #<slot> entry <e> guid <counter> radius <yd> -> <n>: <names>`
};

Join JoinOf(Player* bot, Picture const& picture, std::string_view text)
{
    Join join;
    join.target = PickTarget(bot, picture, text);
    if (join.target < 0)
    {
        join.line = Acore::StringFormat("AW join {}: not a creature in the picture", text);
        return join;
    }
    Seen const& target = picture.units[join.target];
    join.joiners = JoinersIn(bot, picture, join.target);
    for (std::uint8_t i = 0; i < join.joiners.size; ++i)
    {
        Seen const& unit = picture.units[join.joiners.refs[i]];
        join.names += Acore::StringFormat("{}{} #{} {:.1f} yd", i ? ", " : "", NameOf(bot, unit), join.joiners.refs[i],
                                          AwarenessRules::PlanarDistance(unit.x, unit.y, target.x, target.y));
    }
    join.line = Acore::StringFormat("AW join {} #{} entry {} guid {} radius {:.1f} -> {}: {}", NameOf(bot, target),
                                    join.target, target.entry, ObjectGuid(target.guid).GetCounter(), AssistRadius(),
                                    join.joiners.size, join.names.empty() ? "nobody" : join.names);
    return join;
}

constexpr char const* CHECK_USAGE =
    "Usage: .playerbots aw check <bot> [enemies|allies|neither|group|guards|enemyplayers|sight|nosight=|>=<n>] "
    "[entries=<e1,e2..> spawns=|>=<n>] [target=<entry|guid:<counter>|name> [joins=|>=<n>]] [reach=<target>] "
    "[reached=<tenths of yd> [within=<yd>]] [busy=<entry>:<idle|path|evading|dead|fighting>] "
    "[fights=<entry>:<bot|player name>]";

// What a check asks for
struct CheckWants
{
    struct Count
    {
        std::string_view name;
        bool atLeast = false;
        std::uint32_t value = 0;
    };
    // busy=<entry>:<state> and fights=<entry>:<whom>
    struct Of
    {
        std::uint32_t entry = 0;
        std::string_view word;
    };
    std::vector<Count> counts;
    std::vector<std::uint32_t> entries;
    std::string_view target;
    std::string_view reach;
    Optional<std::uint32_t> reachedTenths;
    Optional<float> within;
    std::vector<Of> busy;
    std::vector<Of> fights;

    bool Has(std::string_view name) const
    {
        return std::any_of(counts.begin(), counts.end(), [name](Count const& c) { return c.name == name; });
    }
};

bool IsCountWord(std::string_view name)
{
    for (std::string_view const word :
         {"enemies", "allies", "neither", "group", "guards", "spawns", "joins", "sight", "nosight", "enemyplayers"})
        if (name == word)
            return true;
    return false;
}

bool IsBusyWord(std::string_view word)
{
    for (std::uint8_t busy = 0; busy < static_cast<std::uint8_t>(Busy::Count); ++busy)
        if (word == AwarenessRules::Word(static_cast<Busy>(busy)))
            return true;
    return false;
}

// One `<entry>:<word>` value
bool ParseOf(std::string_view value, CheckWants::Of& of)
{
    std::size_t const colon = value.find(':');
    if (colon == std::string_view::npos || colon + 1 >= value.size())
        return false;
    Optional<std::uint32_t> const entry = Acore::StringTo<std::uint32_t>(value.substr(0, colon));
    of.entry = entry.value_or(0);
    of.word = value.substr(colon + 1);
    return entry.has_value();
}

// The check's words; false for a malformed check, a fixture fault found before any look
bool ParseCheck(std::string_view rest, CheckWants& wants)
{
    std::string_view text = Trimmed(rest);
    if (text.empty())
        return false;
    while (!text.empty())
    {
        std::size_t const cut = text.find(' ');
        std::string_view const token = text.substr(0, cut);
        text = cut == std::string_view::npos ? std::string_view() : Trimmed(text.substr(cut));
        std::size_t const eq = token.find('=');
        if (eq == std::string_view::npos || eq == 0 || eq + 1 >= token.size())
            return false;
        bool const atLeast = token[eq - 1] == '>';
        std::string_view const name = token.substr(0, atLeast ? eq - 1 : eq);
        std::string_view const value = token.substr(eq + 1);
        if (IsCountWord(name))
        {
            Optional<std::uint32_t> const number = Acore::StringTo<std::uint32_t>(value);
            if (!number)
                return false;
            wants.counts.push_back({name, atLeast, *number});
            continue;
        }
        if (atLeast)
            return false;
        if (name == "entries")
        {
            for (std::string_view list = value; !list.empty();)
            {
                std::size_t const comma = list.find(',');
                Optional<std::uint32_t> const entry = Acore::StringTo<std::uint32_t>(list.substr(0, comma));
                if (!entry)
                    return false;
                wants.entries.push_back(*entry);
                list = comma == std::string_view::npos ? std::string_view() : list.substr(comma + 1);
            }
        }
        else if (name == "target")
            wants.target = value;
        else if (name == "reach")
            wants.reach = value;
        else if (name == "reached")
        {
            wants.reachedTenths = Acore::StringTo<std::uint32_t>(value);
            if (!wants.reachedTenths)
                return false;
        }
        else if (name == "within")
        {
            wants.within = Acore::StringTo<float>(value);
            if (!wants.within || *wants.within < 0.0f)
                return false;
        }
        else if (name == "busy" || name == "fights")
        {
            CheckWants::Of of;
            if (!ParseOf(value, of) || (name == "busy" && !IsBusyWord(of.word)))
                return false;
            (name == "busy" ? wants.busy : wants.fights).push_back(of);
        }
        else
            return false;
    }
    // entries= and spawns= come together, joins= needs its target, within= belongs to reached=
    bool const paired = wants.entries.empty() != wants.Has("spawns");
    return paired && (!wants.Has("joins") || !wants.target.empty()) && (!wants.within || wants.reachedTenths);
}
// The picture, rebuilt first when older than its age: FRESH_IN_FIGHT_MS in a fight, for a dead bot (a body or a
// ghost) and for every `strict` read (the point judgement and the search, AW7), FRESH_OUTSIDE_MS otherwise
Picture const& Read(PlayerbotAI* botAI, bool strict)
{
    std::unique_ptr<Store>& own = botAI->awareness;
    if (!own)
        own = std::make_unique<Store>();
    Store& store = *own;
    reads.fetch_add(1, std::memory_order_relaxed);
    Player* bot = botAI->GetBot();
    if (!bot || !bot->IsInWorld())
        return store.picture;
    std::uint32_t const nowMs = getMSTime();
    bool const inFight = strict || bot->IsInCombat() || !bot->IsAlive();
    if (!store.built || !AwarenessRules::IsFresh(getMSTimeDiff(store.picture.builtMs, nowMs), inFight))
        Fill(botAI, bot, store, nowMs);
    return store.picture;
}

// The search's walkable test of one candidate from the centre; `ground` gets the map's height there
bool WalkableFrom(Player* bot, Position const& centre, AwarenessRules::Spot const& spot, float& ground)
{
    Map* map = bot->GetMap();
    float const centreZ = centre.GetPositionZ();
    ground = map->GetHeight(bot->GetPhaseMask(), spot.x, spot.y, centreZ + AwarenessRules::WALK_Z_YARDS, true,
                            2.0f * AwarenessRules::WALK_Z_YARDS);
    if (ground <= INVALID_HEIGHT || std::fabs(ground - centreZ) > AwarenessRules::WALK_Z_YARDS)
        return false;
    if (map->IsInWater(bot->GetPhaseMask(), spot.x, spot.y, ground, bot->GetCollisionHeight()))
        return false;
    float const straight = AwarenessRules::PlanarDistance(centre.GetPositionX(), centre.GetPositionY(), spot.x, spot.y);
    if (straight < 1.0f)
        return true;
    PathGenerator path(bot);
    path.CalculatePath(centre.GetPositionX(), centre.GetPositionY(), centreZ, spot.x, spot.y, ground, false);
    // A complete path on the mesh: no shortcut, no partial or cut path, no end off the mesh
    PathType const type = path.GetPathType();
    if (!(type & PATHFIND_NORMAL) || (type & (PATHFIND_SHORTCUT | PATHFIND_INCOMPLETE | PATHFIND_NOPATH |
                                              PATHFIND_NOT_USING_PATH | PATHFIND_SHORT | PATHFIND_FARFROMPOLY)))
        return false;
    Movement::PointsArray const& points = path.GetPath();
    float length = 0.0f;
    for (std::size_t i = 1; i < points.size(); ++i)
        length += AwarenessRules::PlanarDistance(points[i - 1].x, points[i - 1].y, points[i].x, points[i].y);
    return length <= AwarenessRules::WALK_DETOUR * straight + AwarenessRules::WALK_DETOUR_YARDS;
}

// The words after expect=, comma separated, each one of `known`
bool ParseExpect(std::string_view text, std::vector<std::string_view>& words,
                 std::vector<std::string_view> const& known)
{
    while (!text.empty())
    {
        std::size_t const comma = text.find(',');
        std::string_view const word = text.substr(0, comma);
        if (std::find(known.begin(), known.end(), word) == known.end())
            return false;
        words.push_back(word);
        text = comma == std::string_view::npos ? std::string_view() : text.substr(comma + 1);
    }
    return !words.empty();
}

// The words of a command, split on spaces
std::vector<std::string_view> Words(std::string_view text)
{
    std::vector<std::string_view> words;
    text = Trimmed(text);
    while (!text.empty())
    {
        std::size_t const space = text.find(' ');
        words.push_back(text.substr(0, space));
        text = space == std::string_view::npos ? std::string_view() : Trimmed(text.substr(space));
    }
    return words;
}

std::string AnswerText(Player* bot, AwarenessRules::PointAnswer const& answer)
{
    std::string name = "none";
    if (answer.nearest)
    {
        Unit* nearest = ObjectAccessor::GetUnit(*bot, ObjectGuid(answer.nearest));
        name = Acore::StringFormat("{} guid {}", nearest ? nearest->GetName() : std::string("?"),
                                   ObjectGuid(answer.nearest).GetCounter());
    }
    return Acore::StringFormat(
        "{} reaching {} nearest {} clearance {:.1f} top d{:+} {} {} {} weight {:.3f} support {:.3f}",
        answer.clear ? "clear" : "reached", answer.reaching, name, answer.clearance, int(answer.topLevelDiff),
        AwarenessRules::Word(answer.topRank), answer.inView ? "inview" : "outview",
        AwarenessRules::Takeable(answer) ? "takeable" : "untakeable", Figure(answer.weight), Figure(answer.support));
}

// The reply of `aw judge` and `aw safe`: the line, and with expect= the test's verdict line
void Reply(ChatHandler* handler, Player* bot, std::string const& line, std::vector<std::string_view> const& expect,
           std::vector<std::string_view> const& got)
{
    LOG_INFO("playerbots", "{}", line);
    if (expect.empty())
    {
        handler->SendSysMessage(line);
        return;
    }
    std::string missed;
    for (std::string_view const word : expect)
        if (std::find(got.begin(), got.end(), word) == got.end())
            missed += Acore::StringFormat("{}{}", missed.empty() ? "" : ",", word);
    std::string const reply =
        missed.empty() ? Acore::StringFormat("FIXTURE check {}: ok, {}", bot->GetName(), line)
                       : Acore::StringFormat("FIXTURE check {}: gap, want {}; {}", bot->GetName(), missed, line);
    LOG_INFO("playerbots", "{}", reply);
    handler->SendSysMessage(reply);
}

constexpr char const* JUDGE_USAGE =
    "Usage: .playerbots aw judge <bot> point <x> <y> <z> | from <self|entry|guid:<counter>|name> "
    "<here|home|ahead <yd>|behind <yd>|toward <yd>|away <yd>> "
    "[expect=<clear|reached|inview|outview|takeable|untakeable|supported|unsupported,..>] | "
    "sides <self|name|entry|guid:<counter>> [expect=<holds|outweighed>] | "
    "target <name|entry|guid:<counter>> [expect=<take|leave|none|strong|many|unknown|joins0,..>]";
constexpr char const* SAFE_USAGE =
    "Usage: .playerbots aw safe <bot> <radius 0..46.5> [expect=<found|none|clear|reached,..>]";
// The search's largest radius: the centre and 6 rings fill CANDIDATES_MAX
constexpr float SAFE_RADIUS_MAX = 6.0f * AwarenessRules::RING_STEP_YARDS;
}  // namespace

Picture const& Look(PlayerbotAI* botAI) { return Read(botAI, false); }

AwarenessRules::PointAnswer JudgePoint(PlayerbotAI* botAI, Position const& position)
{
    PerfMonitorOperation* pmo =
        PerfMonitor::IsEnabled() ? sPerfMonitor.start(PERF_MON_VALUE, "awareness point", nullptr) : nullptr;
    Picture const& picture = Read(botAI, true);
    Player* bot = botAI->GetBot();
    AwarenessRules::PointAnswer answer;
    if (bot && bot->IsInWorld())
        answer = AwarenessRules::JudgePoint(picture, position.GetPositionX(), position.GetPositionY());
    if (pmo)
        pmo->finish();
    return answer;
}

AwarenessRules::SidesAnswer JudgeSides(PlayerbotAI* botAI, std::uint64_t unitGuid)
{
    PerfMonitorOperation* pmo =
        PerfMonitor::IsEnabled() ? sPerfMonitor.start(PERF_MON_VALUE, "awareness sides", nullptr) : nullptr;
    Picture const& picture = Read(botAI, true);
    Player* bot = botAI->GetBot();
    AwarenessRules::SidesAnswer answer;
    std::int16_t const slot = unitGuid ? AwarenessRules::SlotOf(picture, unitGuid) : AwarenessRules::THE_BOT;
    // A unit not in the picture is in no fight the picture knows: it holds with theirs 0, the answer's default
    if (bot && bot->IsInWorld() && slot != AwarenessRules::NOT_IN_PICTURE)
        answer = AwarenessRules::JudgeSides(picture, slot);
    if (pmo)
        pmo->finish();
    return answer;
}

AwarenessRules::TargetAnswer JudgeTarget(PlayerbotAI* botAI, std::uint64_t targetGuid)
{
    PerfMonitorOperation* pmo =
        PerfMonitor::IsEnabled() ? sPerfMonitor.start(PERF_MON_VALUE, "awareness target", nullptr) : nullptr;
    Picture const& picture = Read(botAI, true);
    Player* bot = botAI->GetBot();
    AwarenessRules::TargetAnswer answer;
    if (bot && bot->IsInWorld())
    {
        // The joiners on this same picture: JoinersOf would Look again, and a rebuild between would renumber the slots
        std::int16_t const slot = AwarenessRules::SlotOf(picture, targetGuid);
        answer = AwarenessRules::JudgeTarget(picture, AwarenessRules::THE_BOT, slot, JoinersIn(bot, picture, slot));
    }
    if (pmo)
        pmo->finish();
    return answer;
}

AwarenessRules::SafePoint FindSafePoint(PlayerbotAI* botAI, Position const& centre, float radius)
{
    PerfMonitorOperation* pmo =
        PerfMonitor::IsEnabled() ? sPerfMonitor.start(PERF_MON_VALUE, "awareness safe", nullptr) : nullptr;
    Picture const& picture = Read(botAI, true);
    Player* bot = botAI->GetBot();
    AwarenessRules::SafePoint safe;
    if (!bot || !bot->IsInWorld())
    {
        if (pmo)
            pmo->finish();
        return safe;
    }
    std::array<AwarenessRules::Spot, AwarenessRules::CANDIDATES_MAX> spots{};
    std::size_t const count =
        AwarenessRules::RingSpots(centre.GetPositionX(), centre.GetPositionY(), radius, spots.data(), spots.size());

    // The kept tests while the centre stays: the same map and instance, place and radius
    std::unique_ptr<WalkCache>& cache = botAI->awareness->walks;
    if (!cache)
        cache = std::make_unique<WalkCache>();
    if (cache->mapId != bot->GetMapId() || cache->instanceId != bot->GetInstanceId() ||
        cache->x != centre.GetPositionX() || cache->y != centre.GetPositionY() || cache->z != centre.GetPositionZ() ||
        cache->radius != radius)
    {
        *cache = WalkCache{};
        cache->mapId = bot->GetMapId();
        cache->instanceId = bot->GetInstanceId();
        cache->x = centre.GetPositionX();
        cache->y = centre.GetPositionY();
        cache->z = centre.GetPositionZ();
        cache->radius = radius;
    }
    WalkCache& kept = *cache;
    auto walkable = [bot, &centre, &spots, &kept](std::size_t index, bool mayTest)
    {
        if (kept.walk[index] != AwarenessRules::Walk::Unknown)
            return AwarenessRules::WalkAnswer{kept.walk[index], false, kept.ground[index]};
        if (!mayTest)
            return AwarenessRules::WalkAnswer{};
        float ground = centre.GetPositionZ();
        bool const ok = WalkableFrom(bot, centre, spots[index], ground);
        kept.walk[index] = ok ? AwarenessRules::Walk::Yes : AwarenessRules::Walk::No;
        kept.ground[index] = ground;
        return AwarenessRules::WalkAnswer{kept.walk[index], true, ground};
    };
    safe =
        AwarenessRules::FindSafePoint(picture, bot->GetPositionX(), bot->GetPositionY(), spots.data(), count, walkable);
    if (pmo)
        pmo->finish();
    return safe;
}

// A test's point taken from a unit's place at the command (AW7b: a walking creature moves on between two console
// lines): `here`, a creature's `home`, or yards `ahead` or `behind` (the unit's facing), `toward` or `away` (from the
// bot); the unit is `self` or a creature of the picture (PickTarget's words). The point's z is the ground there
static Optional<Position> PointFrom(PlayerbotAI* botAI, Player* bot, std::vector<std::string_view> const& words,
                                    std::string& said)
{
    Unit* unit = bot;
    if (words[1] != "self")
    {
        Picture const& picture = Read(botAI, true);
        std::int16_t const slot = PickTarget(bot, picture, words[1]);
        unit = slot < 0 ? nullptr : ObjectAccessor::GetUnit(*bot, ObjectGuid(picture.units[slot].guid));
        if (!unit)
            return {};
    }
    std::string_view const how = words[2];
    Optional<float> const yards = words.size() == 4 ? Acore::StringTo<float>(words[3]) : Optional<float>();
    float x = unit->GetPositionX();
    float y = unit->GetPositionY();
    float z = unit->GetPositionZ();
    if (how == "home" && words.size() == 3)
    {
        Creature const* creature = unit->ToCreature();
        if (!creature)
            return {};
        float o = 0.0f;
        creature->GetHomePosition(x, y, z, o);
    }
    else if (how == "here" && words.size() == 3)
    {
    }
    else if (yards && *yards >= 0.0f && *yards <= 200.0f &&
             (how == "ahead" || how == "behind" || how == "toward" || how == "away"))
    {
        float angle = unit->GetOrientation();
        if (how == "behind")
            angle += static_cast<float>(M_PI);
        else if (how == "toward")
            angle = unit->GetAbsoluteAngle(bot->GetPositionX(), bot->GetPositionY());
        else if (how == "away")
            angle = unit->GetAbsoluteAngle(bot->GetPositionX(), bot->GetPositionY()) + static_cast<float>(M_PI);
        x += *yards * std::cos(angle);
        y += *yards * std::sin(angle);
        float const ground = bot->GetMap()->GetHeight(bot->GetPhaseMask(), x, y, z + AwarenessRules::WALK_Z_YARDS, true,
                                                      2.0f * AwarenessRules::WALK_Z_YARDS);
        if (ground > INVALID_HEIGHT)
            z = ground;
    }
    else
        return {};
    said = Acore::StringFormat("from {} guid {} {}{} ({}), ", unit->GetName(), unit->GetGUID().GetCounter(), how,
                               yards ? Acore::StringFormat(" {:.1f}", *yards) : std::string(),
                               unit->isMoving() ? "moving" : "still");
    return Position(x, y, z);
}

// `aw judge <bot> sides|target <unit>`: the call's answer about the unit PickUnit names (`self` the bot), as a line
static bool JudgeUnitCommand(ChatHandler* handler, PlayerbotAI* botAI, Player* bot, std::string_view word, bool sides,
                             std::vector<std::string_view> const& expect)
{
    std::uint64_t guid = bot->GetGUID().GetRawValue();
    std::string name = bot->GetName();
    if (word != "self")
    {
        Picture const& picture = Read(botAI, true);
        std::int16_t const slot = PickUnit(bot, picture, word);
        if (slot < 0)
        {
            handler->SendErrorMessage(
                Acore::StringFormat("AW judge {}: error, no unit {} in the picture", bot->GetName(), std::string(word)),
                false);
            return false;
        }
        guid = picture.units[slot].guid;
        name = NameOf(bot, picture.units[slot]);
    }
    std::string text;
    std::vector<std::string_view> got;
    if (sides)
    {
        AwarenessRules::SidesAnswer const answer = JudgeSides(botAI, guid);
        text = Acore::StringFormat("{} ours {:.3f} theirs {:.3f} helpers {}{} enemies {}{}",
                                   answer.holds ? "holds" : "outweighed", Figure(answer.ours), Figure(answer.theirs),
                                   answer.helpers.size, SlotsText(answer.helpers), answer.enemies.size,
                                   SlotsText(answer.enemies));
        got = {answer.holds ? "holds" : "outweighed"};
    }
    else
    {
        AwarenessRules::TargetAnswer const answer = JudgeTarget(botAI, guid);
        text = Acore::StringFormat("{} reason {} ours {:.3f} weight {:.3f} joins {}{}", AwarenessRules::Word(answer),
                                   AwarenessRules::Word(answer.reason), Figure(answer.ours), Figure(answer.weight),
                                   answer.joins.size, SlotsText(answer.joins));
        got = {AwarenessRules::Word(answer), AwarenessRules::Word(answer.reason)};
        if (!answer.joins.size)
            got.push_back("joins0");
    }
    Store const& store = *botAI->awareness;
    for (std::string const& line : Listing(bot, store))
        LOG_INFO("playerbots", "{}", line);
    std::string const line = Acore::StringFormat("AW judge {}: {} {} guid {}: {} | built {} ms ago", bot->GetName(),
                                                 sides ? "sides" : "target", name, ObjectGuid(guid).GetCounter(), text,
                                                 getMSTimeDiff(store.picture.builtMs, getMSTime()));
    Reply(handler, bot, line, expect, got);
    return true;
}

bool HandleJudgeCommand(ChatHandler* handler, PlayerbotAI* botAI, std::string_view rest)
{
    std::vector<std::string_view> words = Words(rest);
    bool const bySides = !words.empty() && words[0] == "sides";
    bool const byTarget = !words.empty() && words[0] == "target";
    std::vector<std::string_view> expect;
    bool expectOk = true;
    if (!words.empty() && words.back().substr(0, 7) == "expect=")
    {
        std::vector<std::string_view> const known =
            bySides    ? std::vector<std::string_view>{"holds", "outweighed"}
            : byTarget ? std::vector<std::string_view>{"take", "leave", "none", "strong", "many", "unknown", "joins0"}
                       : std::vector<std::string_view>{"clear",    "reached",    "inview",    "outview",
                                                       "takeable", "untakeable", "supported", "unsupported"};
        expectOk = ParseExpect(words.back().substr(7), expect, known);
        words.pop_back();
    }
    if (bySides || byTarget)
    {
        if (!expectOk || words.size() != 2)
        {
            handler->SendErrorMessage(JUDGE_USAGE);
            return false;
        }
        Player* bot = botAI->GetBot();
        if (!bot || !bot->IsInWorld())
        {
            handler->SendErrorMessage("AW: the bot is not in the world");
            return false;
        }
        return JudgeUnitCommand(handler, botAI, bot, words[1], bySides, expect);
    }
    bool const byPoint = words.size() == 4 && words[0] == "point";
    bool const byUnit = (words.size() == 3 || words.size() == 4) && words[0] == "from";
    Optional<float> const x = byPoint ? Acore::StringTo<float>(words[1]) : Optional<float>();
    Optional<float> const y = byPoint ? Acore::StringTo<float>(words[2]) : Optional<float>();
    Optional<float> const z = byPoint ? Acore::StringTo<float>(words[3]) : Optional<float>();
    if (!expectOk || (!byUnit && (!byPoint || !x || !y || !z)))
    {
        handler->SendErrorMessage(JUDGE_USAGE);
        return false;
    }
    Player* bot = botAI->GetBot();
    if (!bot || !bot->IsInWorld())
    {
        handler->SendErrorMessage("AW: the bot is not in the world");
        return false;
    }
    std::string said;
    Optional<Position> const taken = byUnit ? PointFrom(botAI, bot, words, said) : Optional<Position>();
    if (byUnit && !taken)
    {
        handler->SendErrorMessage(
            Acore::StringFormat(
                "AW judge {}: error, no point {} (the unit is not in the picture, or the word does not fit it)",
                bot->GetName(), std::string(rest)),
            false);
        return false;
    }
    Position const point = byUnit ? *taken : Position(*x, *y, *z);
    AwarenessRules::PointAnswer const answer = JudgePoint(botAI, point);
    Store const& store = *botAI->awareness;
    for (std::string const& line : Listing(bot, store))
        LOG_INFO("playerbots", "{}", line);
    std::string const line =
        Acore::StringFormat("AW judge {}: {}point {:.1f} {:.1f} {:.1f}, {:.1f} yd from the bot: {} | built {} ms ago",
                            bot->GetName(), said, point.GetPositionX(), point.GetPositionY(), point.GetPositionZ(),
                            AwarenessRules::PlanarDistance(bot->GetPositionX(), bot->GetPositionY(),
                                                           point.GetPositionX(), point.GetPositionY()),
                            AnswerText(bot, answer), getMSTimeDiff(store.picture.builtMs, getMSTime()));
    std::vector<std::string_view> got = {answer.clear ? "clear" : "reached", answer.inView ? "inview" : "outview",
                                         AwarenessRules::Takeable(answer) ? "takeable" : "untakeable"};
    // supported: the allies standing by outweigh the units reaching the point; unsupported: none stands by
    if (answer.support > answer.weight)
        got.push_back("supported");
    if (!answer.support)
        got.push_back("unsupported");
    Reply(handler, bot, line, expect, got);
    return true;
}

bool HandleSafeCommand(ChatHandler* handler, PlayerbotAI* botAI, std::string_view rest)
{
    std::vector<std::string_view> const words = Words(rest);
    std::vector<std::string_view> expect;
    bool const sized = words.size() == 1 || words.size() == 2;
    Optional<float> const radius = sized ? Acore::StringTo<float>(words[0]) : Optional<float>();
    bool const expectOk =
        words.size() != 2 || (words[1].substr(0, 7) == "expect=" &&
                              ParseExpect(words[1].substr(7), expect, {"found", "none", "clear", "reached"}));
    if (!sized || !radius || *radius < 0.0f || *radius > SAFE_RADIUS_MAX || !expectOk)
    {
        handler->SendErrorMessage(SAFE_USAGE);
        return false;
    }
    Player* bot = botAI->GetBot();
    if (!bot || !bot->IsInWorld())
    {
        handler->SendErrorMessage("AW: the bot is not in the world");
        return false;
    }
    Position const centre(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ());
    auto const started = std::chrono::steady_clock::now();
    AwarenessRules::SafePoint const safe = FindSafePoint(botAI, centre, *radius);
    auto const micros =
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - started).count();
    Store const& store = *botAI->awareness;
    for (std::string const& line : Listing(bot, store))
        LOG_INFO("playerbots", "{}", line);
    std::string place = safe.outOfTests ? "none found (out of tests, the next call goes on)" : "none found";
    if (safe.found)
        place = Acore::StringFormat(
            "found #{} at {:.1f} {:.1f} {:.1f}, {:.1f} yd from the centre: {}", safe.index, safe.x, safe.y, safe.z,
            AwarenessRules::PlanarDistance(centre.GetPositionX(), centre.GetPositionY(), safe.x, safe.y),
            AnswerText(bot, safe.answer));
    std::string const line =
        Acore::StringFormat("AW safe {}: radius {:.1f} centre {:.1f} {:.1f} {:.1f}: {} | unseen {} tests {} | {} us",
                            bot->GetName(), *radius, centre.GetPositionX(), centre.GetPositionY(),
                            centre.GetPositionZ(), place, safe.unseen, safe.tests, micros);
    std::vector<std::string_view> const got = {safe.found ? "found" : "none",
                                               safe.found && safe.clear ? "clear" : "reached"};
    Reply(handler, bot, line, expect, got);
    return true;
}

AwarenessRules::UnitList JoinersOf(PlayerbotAI* botAI, std::uint64_t targetGuid)
{
    Picture const& picture = Look(botAI);
    Player* bot = botAI->GetBot();
    if (!bot || !bot->IsInWorld())
        return AwarenessRules::UnitList{};
    return JoinersIn(bot, picture, AwarenessRules::SlotOf(picture, targetGuid));
}

bool HandleLookCommand(ChatHandler* handler, PlayerbotAI* botAI, std::string_view rest)
{
    std::string_view const text = Trimmed(rest);
    std::string_view const target = text.substr(0, 7) == "target=" ? text.substr(7) : std::string_view();
    if (!text.empty() && (target.empty() || target.find(' ') != std::string_view::npos))
    {
        handler->SendErrorMessage("Usage: .playerbots aw look <bot> [target=<entry|guid:<counter>|name>]");
        return false;
    }
    Store const* store = LookedUp(handler, botAI);
    if (!store)
        return false;
    Player* bot = botAI->GetBot();
    for (std::string const& line : Listing(bot, *store))
        handler->SendSysMessage(line);
    if (!target.empty())
        handler->SendSysMessage(JoinOf(bot, store->picture, target).line);
    return true;
}

bool HandleCheckCommand(ChatHandler* handler, PlayerbotAI* botAI, std::string_view rest)
{
    // Parse first: a malformed check is a fixture fault, before any look
    CheckWants wants;
    if (!ParseCheck(rest, wants))
    {
        handler->SendErrorMessage(CHECK_USAGE);
        return false;
    }

    Store* store = LookedUp(handler, botAI);
    if (!store)
        return false;
    Player* bot = botAI->GetBot();
    for (std::string const& line : Listing(bot, *store))
        LOG_INFO("playerbots", "{}", line);

    Picture const& picture = store->picture;
    std::string matched;
    std::string gaps;
    auto const add = [&matched, &gaps](bool ok, std::string const& part)
    {
        std::string& into = ok ? matched : gaps;
        into += into.empty() ? part : "; " + part;
    };

    Join join;
    if (!wants.target.empty())
    {
        join = JoinOf(bot, picture, wants.target);
        LOG_INFO("playerbots", "{}", join.line);
        if (join.target < 0)
            add(false, Acore::StringFormat("target {} not a creature in the picture", wants.target));
        else if (!wants.Has("joins"))
            add(true, Acore::StringFormat("target {} joins {}", wants.target, join.joiners.size));
    }

    std::uint32_t enemies = 0;
    std::uint32_t allies = 0;
    std::uint32_t neither = 0;
    std::uint32_t group = 0;
    std::uint32_t guards = 0;
    std::uint32_t enemyPlayers = 0;
    std::uint32_t sight = 0;
    std::uint32_t entrySeen = 0;
    std::uint32_t entryDead = 0;
    auto const listed = [&wants](std::uint32_t entry)
    { return std::find(wants.entries.begin(), wants.entries.end(), entry) != wants.entries.end(); };
    for (std::size_t i = 0; i < picture.count; ++i)
    {
        Seen const& unit = picture.units[i];
        enemies += unit.side == Side::Enemy;
        allies += unit.side == Side::Ally;
        neither += unit.side == Side::Neither;
        group += unit.kind == Kind::Group;
        guards += unit.kind == Kind::Guard;
        enemyPlayers += unit.side == Side::Enemy && unit.type == UnitType::Player;
        sight += unit.inSight;
        entrySeen += unit.type == UnitType::Creature && listed(unit.entry);
    }
    for (std::size_t i = 0; i < std::min<std::size_t>(store->deadSkipped, DEAD_KEPT); ++i)
        entryDead += listed(store->deadEntries[i]);

    for (CheckWants::Count const& want : wants.counts)
    {
        std::uint32_t got = 0;
        if (want.name == "enemies")
            got = enemies;
        else if (want.name == "allies")
            got = allies;
        else if (want.name == "neither")
            got = neither;
        else if (want.name == "group")
            got = group;
        else if (want.name == "guards")
            got = guards;
        else if (want.name == "enemyplayers")
            got = enemyPlayers;
        else if (want.name == "sight")
            got = sight;
        else if (want.name == "nosight")
            got = picture.count - sight;
        else if (want.name == "joins")
            got = join.joiners.size;
        else
            got = entrySeen + entryDead;
        bool const ok =
            (want.atLeast ? got >= want.value : got == want.value) && (want.name != "joins" || join.target >= 0);
        std::string part =
            Acore::StringFormat("{} want {}{} got {}", want.name, want.atLeast ? ">=" : "", want.value, got);
        if (want.name == "spawns")
            part += Acore::StringFormat(" (seen {} dead {})", entrySeen, entryDead);
        else if (want.name == "joins" && !join.names.empty())
            part += Acore::StringFormat(" ({})", join.names);
        add(ok, part);
    }

    if (!wants.reach.empty())
    {
        std::int16_t const slot = PickTarget(bot, picture, wants.reach);
        if (slot < 0)
            add(false, Acore::StringFormat("reach {} not a creature in the picture", wants.reach));
        else
        {
            Seen const& unit = picture.units[slot];
            store->namedReach = unit.reachYards;
            store->namedReachEntry = unit.entry;
            add(true,
                Acore::StringFormat("reach {} #{} entry {} guid {} named {:.1f} yd at {:.1f} yd", NameOf(bot, unit),
                                    slot, unit.entry, ObjectGuid(unit.guid).GetCounter(), unit.reachYards,
                                    AwarenessRules::DistanceToDisc(unit, picture.self.x, picture.self.y)));
        }
    }
    if (wants.reachedTenths)
    {
        float const observed = static_cast<float>(*wants.reachedTenths) / 10.0f;
        float const bound = wants.within.value_or(REACH_BOUND_YARDS);
        if (store->namedReach < 0.0f)
            add(false, Acore::StringFormat("reached {:.1f} yd: no reach named (reach= first)", observed));
        else
        {
            float const diff = observed - store->namedReach;
            add(std::fabs(diff) <= bound,
                Acore::StringFormat("reached {:.1f} named {:.1f} (entry {}) diff {:+.1f} within {:.1f}", observed,
                                    store->namedReach, store->namedReachEntry, diff, bound));
        }
    }

    for (CheckWants::Of const& want : wants.busy)
    {
        std::uint32_t of = 0;
        std::uint32_t got = 0;
        std::string states;
        for (std::size_t i = 0; i < picture.count; ++i)
        {
            Seen const& unit = picture.units[i];
            if (unit.type != UnitType::Creature || unit.entry != want.entry)
                continue;
            ++of;
            got += want.word == AwarenessRules::Word(unit.busy);
            states += Acore::StringFormat("{}{}", states.empty() ? "" : ",", AwarenessRules::Word(unit.busy));
        }
        add(got >= 1, Acore::StringFormat("busy {}:{} got {} of {} ({})", want.entry, want.word, got, of,
                                          states.empty() ? "none" : states));
    }

    for (CheckWants::Of const& want : wants.fights)
    {
        // The link must name the bot, or a unit of the picture that is this player
        bool const toBot = want.word == "bot";
        Player* const whom = toBot ? nullptr : ObjectAccessor::FindPlayerByName(std::string(want.word));
        std::uint64_t const whomGuid = whom ? whom->GetGUID().GetRawValue() : 0;
        std::uint32_t of = 0;
        std::uint32_t got = 0;
        std::string links;
        for (std::size_t i = 0; i < picture.count; ++i)
        {
            Seen const& unit = picture.units[i];
            if (unit.type != UnitType::Creature || unit.entry != want.entry)
                continue;
            ++of;
            bool const link =
                AwarenessRules::Fights(unit) &&
                (toBot ? unit.fightsIndex == AwarenessRules::THE_BOT
                       : unit.fightsIndex >= 0 && whomGuid && picture.units[unit.fightsIndex].guid == whomGuid);
            got += link;
            links += Acore::StringFormat("{}{}", links.empty() ? "" : ",", Whom(unit));
        }
        add(got >= 1,
            Acore::StringFormat("fights {}:{} got {} of {} (links {}{})", want.entry, want.word, got, of,
                                links.empty() ? "none" : links, toBot || whom ? "" : "; no such player online"));
    }

    std::string const tail = Acore::StringFormat(
        "units {} left out e{} a{} n{} dead {}", picture.count, picture.leftOut[SideIndex(Side::Enemy)],
        picture.leftOut[SideIndex(Side::Ally)], picture.leftOut[SideIndex(Side::Neither)], store->deadSkipped);
    std::string const reply = gaps.empty()
                                  ? Acore::StringFormat("FIXTURE check {}: ok, {}; {}", bot->GetName(), matched, tail)
                                  : Acore::StringFormat("FIXTURE check {}: gap, {}; matched {}; {}", bot->GetName(),
                                                        gaps, matched.empty() ? "none" : matched, tail);
    LOG_INFO("playerbots", "{}", reply);
    handler->SendSysMessage(reply);
    return true;
}

bool HandleStatsCommand(ChatHandler* handler)
{
    handler->PSendSysMessage("AW stats: reads {} {} fill_us {}", reads.load(std::memory_order_relaxed), TotalsText(),
                             fillMicros.load(std::memory_order_relaxed));
    return true;
}

namespace
{
// The table's side of a team, SIDE_COUNT for none
std::uint8_t SideIndex(TeamId side)
{
    return side == TEAM_ALLIANCE ? 0 : side == TEAM_HORDE ? 1 : DangerRules::SIDE_COUNT;
}

char const* GroundWord(DangerRules::Ground ground)
{
    switch (ground)
    {
        case DangerRules::Ground::Quiet:
            return "quiet";
        case DangerRules::Ground::Passable:
            return "passable";
        case DangerRules::Ground::Deadly:
            return "deadly";
    }
    return "?";
}

std::string FactsText(DangerRules::CellFacts const& facts)
{
    std::string text;
    static constexpr std::array<char const*, DangerRules::RANK_COUNT> RANKS{"normal", "elite", "rare", "boss"};
    for (std::size_t r = 0; r < DangerRules::RANK_COUNT; ++r)
        if (facts.count[r])
            text += Acore::StringFormat("{}{} {} x{}", text.empty() ? "" : ", ", RANKS[r],
                                        static_cast<unsigned>(facts.level[r]), static_cast<unsigned>(facts.count[r]));
    if (facts.guards)
        text += text.empty() ? "guards" : ", guards";
    return text.empty() ? "none" : text;
}

constexpr char const* DANGER_USAGE =
    "Usage: .playerbots aw danger [<player>] | <bot> check [expect=<same|near|apart|close|away|path|full|dead,..>] | "
    "<map> <x> <y> <z> <alliance|horde> <level 1..255>";
// `aw danger` times GroundAt over this many calls: the PerfMonitor row's count is known and the reply gives the mean
constexpr int DANGER_TIMED_CALLS = 1000;
}  // namespace

DangerRules::CellFacts GroundFactsAt(std::uint32_t mapId, float x, float y, float z, TeamId side)
{
    return DangerRules::ReadPlace(DangerRules::Published(), mapId, x, y, z, SideIndex(side)).read.facts;
}

DangerRules::Ground GroundAt(std::uint32_t mapId, float x, float y, float z, std::uint8_t level, TeamId side,
                             DangerRules::Condition condition)
{
    PerfMonitorOperation* pmo =
        PerfMonitor::IsEnabled() ? sPerfMonitor.start(PERF_MON_VALUE, "awareness ground", nullptr) : nullptr;
    DangerRules::Ground const ground = DangerRules::JudgeGround(GroundFactsAt(mapId, x, y, z, side), level, condition);
    if (pmo)
        pmo->finish();
    return ground;
}

bool HandleDangerCommand(ChatHandler* handler, std::string_view rest)
{
    std::vector<std::string_view> words = Words(rest);
    std::vector<std::string_view> expect;
    if (!words.empty() && words.back().substr(0, 7) == "expect=")
    {
        if (!ParseExpect(words.back().substr(7), expect,
                         {"same", "near", "apart", "close", "away", "path", "full", "dead"}))
        {
            handler->SendErrorMessage(DANGER_USAGE);
            return false;
        }
        words.pop_back();
    }
    bool const check = words.size() == 2 && words[1] == "check";
    if (!expect.empty() && !check)
    {
        handler->SendErrorMessage(DANGER_USAGE);
        return false;
    }
    if (check)
        words.pop_back();
    PlayerbotAI* botAI = nullptr;
    std::uint32_t mapId = 0;
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    std::uint32_t level = 0;
    TeamId side = TEAM_NEUTRAL;
    DangerRules::Condition now = DangerRules::SICK;
    std::string who = "place";
    bool const place = words.size() == 6;
    if (place)
    {
        Optional<std::uint32_t> const map = Acore::StringTo<std::uint32_t>(words[0]);
        Optional<float> const px = Acore::StringTo<float>(words[1]);
        Optional<float> const py = Acore::StringTo<float>(words[2]);
        Optional<float> const pz = Acore::StringTo<float>(words[3]);
        Optional<std::uint32_t> const lvl = Acore::StringTo<std::uint32_t>(words[5]);
        side = words[4] == "alliance" || words[4] == "0" ? TEAM_ALLIANCE
               : words[4] == "horde" || words[4] == "1"  ? TEAM_HORDE
                                                         : TEAM_NEUTRAL;
        if (!map || !px || !py || !pz || !lvl || *lvl < 1 || *lvl > 255 || side == TEAM_NEUTRAL)
        {
            handler->SendErrorMessage(DANGER_USAGE);
            return false;
        }
        mapId = *map;
        x = *px;
        y = *py;
        z = *pz;
        level = *lvl;
    }
    else if (words.size() <= 1)
    {
        WorldSession* session = handler->GetSession();
        if (!words.empty() && session && session->GetSecurity() < SEC_GAMEMASTER)
        {
            handler->SendErrorMessage("AW danger: naming a player is for game masters (it shows where that player is)");
            return false;
        }
        Player* player = words.empty() ? handler->getSelectedPlayerOrSelf()
                                       : ObjectAccessor::FindPlayerByName(std::string(words[0]));
        botAI = player && check ? GET_PLAYERBOT_AI(player) : nullptr;
        if (!player || !player->IsInWorld() || (check && !botAI))
        {
            handler->SendErrorMessage(DANGER_USAGE);
            return false;
        }
        mapId = player->GetMapId();
        x = player->GetPositionX();
        y = player->GetPositionY();
        z = player->GetPositionZ();
        level = player->GetLevel();
        side = player->GetTeamId();
        now.healthPct = static_cast<std::uint8_t>(std::clamp(std::lround(player->GetHealthPct()), 0L, 100L));
        now.sick = player->HasAura(Upkeep::SPELL_RESURRECTION_SICKNESS);
        who = player->GetName();
    }
    else
    {
        handler->SendErrorMessage(DANGER_USAGE);
        return false;
    }

    std::uint8_t const botLevel = static_cast<std::uint8_t>(level);
    DangerRules::Known const* known = DangerRules::Published();
    DangerRules::PlaceReading const reading = DangerRules::ReadPlace(known, mapId, x, y, z, SideIndex(side));
    std::string layers;
    for (std::uint8_t i = 0; i < reading.layerCount; ++i)
    {
        DangerRules::Layer const& layer = reading.layers[i];
        bool const read = i >= reading.read.firstRead && i < reading.read.firstRead + reading.read.readCount;
        layers += Acore::StringFormat("{}{} {}..{}{}", layers.empty() ? "" : ", ", i, layer.zLo, layer.zHi,
                                      read ? " read" : "");
    }
    auto const started = std::chrono::steady_clock::now();
    for (int call = 0; call < DANGER_TIMED_CALLS; ++call)
        GroundAt(mapId, x, y, z, botLevel, side);
    double const nanos = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - started).count() /
                         DANGER_TIMED_CALLS;
    DangerRules::CellFacts const& facts = reading.read.facts;
    std::string const asIs =
        place ? Acore::StringFormat("sick {}", GroundWord(DangerRules::JudgeGround(facts, botLevel, now)))
              : Acore::StringFormat("now {} (health {} %, sick {})",
                                    GroundWord(DangerRules::JudgeGround(facts, botLevel, now)),
                                    static_cast<unsigned>(now.healthPct), now.sick ? 1 : 0);
    std::string const cell = reading.cell ? Acore::StringFormat("cell {} {}", reading.cx, reading.cy) : "no cell";
    std::string const line = Acore::StringFormat(
        "AW danger {}: map {} {} z {:.1f} {} level {} | build {} | layers {} of {}{}{} | facts {} | deadlyBelow {} "
        "passableBelow {} | healthy {}, {} | ground {:.0f} ns per call over {} calls",
        who, mapId, cell, z,
        side == TEAM_ALLIANCE ? "alliance"
        : side == TEAM_HORDE  ? "horde"
                              : "neutral",
        level, known ? known->build : "none", static_cast<unsigned>(reading.read.readCount),
        static_cast<unsigned>(reading.layerCount), layers.empty() ? "" : ": ", layers, FactsText(facts),
        static_cast<unsigned>(DangerRules::DeadlyBelow(facts)),
        static_cast<unsigned>(DangerRules::PassableBelow(facts)),
        GroundWord(DangerRules::JudgeGround(facts, botLevel, DangerRules::HEALTHY)), asIs, nanos, DANGER_TIMED_CALLS);
    if (!check)
    {
        LOG_INFO("playerbots", "{}", line);
        handler->SendSysMessage(line);
        return true;
    }

    // The check: the same place judged by the picture's facts, both for the bot healthy (scenario aw-danger)
    Picture const& picture = Read(botAI, true);
    Player* bot = botAI->GetBot();
    // A patrol's way: the engine's waypoint path of a creature walking one, read by guid (no grid visit)
    std::array<bool, AwarenessRules::PICTURE_SIZE> pathReaches{};
    std::vector<float> points;
    for (std::size_t i = 0; i < picture.count; ++i)
    {
        Seen const& unit = picture.units[i];
        if (unit.type != UnitType::Creature || unit.side != Side::Enemy)
            continue;
        Creature* creature = ObjectAccessor::GetCreature(*bot, ObjectGuid(unit.guid));
        if (!creature || creature->GetMotionMaster()->GetCurrentMovementGeneratorType() != WAYPOINT_MOTION_TYPE)
            continue;
        WaypointPath const* path = sWaypointMgr->GetPath(creature->GetWaypointPath());
        if (!path)
            continue;
        points.clear();
        for (WaypointNode const& node : path->Nodes)
            points.insert(points.end(), {node.X, node.Y, node.Z});
        pathReaches[i] = DangerRules::PathReaches(points, reading.cx, reading.cy, z);
    }
    DangerRules::PictureFacts const seen = DangerRules::FactsOfPicture(picture, reading.cx, reading.cy, z, pathReaches);
    DangerRules::Ground const byFacts = DangerRules::JudgeGround(facts, botLevel, DangerRules::HEALTHY);
    DangerRules::Ground const byPicture = DangerRules::JudgeGround(seen.facts, botLevel, DangerRules::HEALTHY);
    DangerRules::Agreement const agreement = DangerRules::Agree(byFacts, byPicture);
    std::string units;
    for (std::size_t i = 0; i < picture.count; ++i)
    {
        Seen const& unit = picture.units[i];
        int const how = DangerRules::PictureCounts(unit, reading.cx, reading.cy, z, pathReaches[i]);
        if (!how)
            continue;
        units += Acore::StringFormat(
            "{}#{} lvl {} {} home {:.0f} yd dz {:+.0f}{}{}", units.empty() ? "" : ", ", unit.entry,
            std::clamp(int(picture.self.level) + int(unit.levelDiff), 1, 255), AwarenessRules::Word(unit.rank),
            DangerRules::SquareDistance(reading.cx, reading.cy, unit.homeX, unit.homeY), unit.homeZ - z,
            how == 2   ? " place"
            : how == 3 ? " path"
                       : "",
            unit.guard ? " guard" : "");
    }
    LOG_INFO("playerbots", "AW danger units {}: {}", who, units.empty() ? "none" : units);
    std::uint16_t const enemiesLeftOut = picture.leftOut[SideIndex(Side::Enemy)];
    std::uint32_t const dead = botAI->awareness ? botAI->awareness->deadSkipped : 0;
    static constexpr std::array<std::string_view, 3> AGREEMENTS{"same", "near", "apart"};
    std::vector<std::string_view> got{AGREEMENTS[static_cast<std::size_t>(agreement)]};
    if (agreement != DangerRules::Agreement::Apart)
        got.push_back("close");
    if (seen.byPlace)
        got.push_back("away");
    if (seen.byPath)
        got.push_back("path");
    if (enemiesLeftOut)
        got.push_back("full");
    if (dead)
        got.push_back("dead");
    std::string const checkLine = Acore::StringFormat(
        "AW danger check {}: facts {} picture {}: {} | picture facts {} by home {} by place {} by path {} | units {} "
        "left out e{} dead {} || {}",
        who, GroundWord(byFacts), GroundWord(byPicture), AGREEMENTS[static_cast<std::size_t>(agreement)],
        FactsText(seen.facts), static_cast<unsigned>(seen.byHome), static_cast<unsigned>(seen.byPlace),
        static_cast<unsigned>(seen.byPath), picture.count, enemiesLeftOut, dead, line);
    Reply(handler, bot, checkLine, expect, got);
    return true;
}

std::string MeterLine(bool reset)
{
    std::uint64_t const nowReads = reads.load(std::memory_order_relaxed);
    std::uint64_t const nowFills = fills.load(std::memory_order_relaxed);
    std::uint64_t const nowMicros = fillMicros.load(std::memory_order_relaxed);
    std::string line = Acore::StringFormat("AW meter: reads {} rebuilds {} fill_us {}", nowReads - meterReads,
                                           nowFills - meterFills, nowMicros - meterMicros);
    if (reset)
    {
        meterReads = nowReads;
        meterFills = nowFills;
        meterMicros = nowMicros;
    }
    return line;
}
// AW5, the record: the world log rows FightScene, FightEnemy and DeathScene, read off the picture
namespace
{
// Tuning value: how far an entering unit may have moved beyond an engine radius (the assist and flee calls) between
// the call and the engine tick that sees it
constexpr float ENTRY_MARGIN_YARDS = 2.0f;
// The units of the enter edge besides the first that get their row with the scene (the edge names one, rarely more)
constexpr std::size_t OPEN_ROWS = 8;

Store& OwnStore(PlayerbotAI* botAI)
{
    std::unique_ptr<Store>& own = botAI->awareness;
    if (!own)
        own = std::make_unique<Store>();
    return *own;
}

// One unit described as the fill describes it, without a grid visit
Seen DescribeOne(Player* bot, Store& store, Unit* unit)
{
    Worker const worker{bot, bot->GetGroup(), store};
    return worker.Describe(unit, unit->ToCreature());
}

bool InFight(Store const& store, std::uint64_t guid)
{
    for (std::uint8_t i = 0; i < store.fightEnemyCount; ++i)
        if (store.fightEnemies[i] == guid)
            return true;
    return false;
}

std::uint32_t RoseSeconds(Store const& store, std::uint32_t nowMs)
{
    if (!store.rose)
        return AwarenessRules::ROSE_NONE;
    return std::min<std::uint32_t>(getMSTimeDiff(store.roseMs, nowMs) / 1000, AwarenessRules::ROSE_NONE);
}

std::uint32_t Rounded(float value) { return static_cast<std::uint32_t>(std::max(0.0f, value) + 0.5f); }

// A creature fleeing for help (the engine's assistance movement)
bool SeekingHelp(Unit* unit)
{
    MovementGeneratorType const type = unit->GetMotionMaster()->GetCurrentMovementGeneratorType();
    return type == ASSISTANCE_MOTION_TYPE || type == ASSISTANCE_DISTRACT_MOTION_TYPE;
}

// Why a unit is in the fight (AwarenessRules::WhyInFight): from the last picture before it entered (the stored one,
// not rebuilt) and the fight's earlier enemies. The engine picks a call's helpers where the caller stood when it
// called and runs them in after a delay (CONFIG_CREATURE_FAMILY_ASSISTANCE_DELAY), while the caller chases the bot: an
// assist counts the two units' places in that picture as well as where they stand now (AW5 run 2: two of three joiners
// 6 yd from the pulled unit read "other" on the live places alone); a flee call, made at the helper, the live ones
AwarenessRules::Reason ReasonOf(Player* bot, Store const& store, Seen const& now, bool chosen, bool struckFirst)
{
    Picture const& last = store.picture;
    std::int16_t const slot = store.built ? AwarenessRules::SlotOf(last, now.guid) : AwarenessRules::NOT_IN_PICTURE;
    Seen const* was = slot >= 0 ? &last.units[slot] : nullptr;
    AwarenessRules::EntryFacts facts;
    facts.inLastPicture = was != nullptr;
    facts.lastBusy = was ? was->busy : Busy::Idle;
    facts.chosen = chosen;
    facts.struckFirst = struckFirst;
    // An evading creature's reach is the one it has at home (AW7): it would not attack now
    Seen const& used = was ? *was : now;
    facts.reached = used.busy != Busy::Evading &&
                    AwarenessRules::Covers(used, bot->GetPositionX(), bot->GetPositionY(), REACH_BOUND_YARDS);
    bool const free = !was || was->busy == Busy::Idle || was->busy == Busy::OnPath;
    if (now.type != UnitType::Creature || !free)
        return AwarenessRules::WhyInFight(facts);

    float const assist = AssistRadius();
    float const flee = sWorld->getFloatConfig(CONFIG_CREATURE_FAMILY_FLEE_ASSISTANCE_RADIUS);
    for (std::uint8_t i = 0; i < store.fightEnemyCount; ++i)
    {
        Unit* other = ObjectAccessor::GetUnit(*bot, ObjectGuid(store.fightEnemies[i]));
        if (!other || !other->IsAlive() || !other->IsCreature())
            continue;
        float const yards = AwarenessRules::PlanarDistance(now.x, now.y, other->GetPositionX(), other->GetPositionY());
        std::int16_t const otherSlot = AwarenessRules::SlotOf(last, store.fightEnemies[i]);
        float pictured = yards;
        if (was && otherSlot >= 0)
        {
            Seen const& caller = last.units[otherSlot];
            pictured = AwarenessRules::PlanarDistance(was->x, was->y, caller.x, caller.y);
        }
        facts.assisted = facts.assisted || (assist > 0.0f && std::min(yards, pictured) <= assist + ENTRY_MARGIN_YARDS);
        facts.called = facts.called || (flee > 0.0f && SeekingHelp(other) && yards <= flee + ENTRY_MARGIN_YARDS);
    }
    return AwarenessRules::WhyInFight(facts);
}

struct EnemyLine
{
    std::uint32_t entry = 0;
    char const* word = "?";
    std::uint64_t amount = 0;
};

// A unit that enters the fight: its row, and it joins the fight's enemies (false when they are full)
bool Enter(Player* bot, Store& store, Unit* unit, bool chosen, bool struckFirst, std::uint32_t nowMs, EnemyLine& line)
{
    if (store.fightEnemyCount >= FIGHT_ENEMIES_KEPT)
        return false;
    Seen const now = DescribeOne(bot, store, unit);
    AwarenessRules::Reason const reason = ReasonOf(bot, store, now, chosen, struckFirst);
    // The picture before it joined (not rebuilt): its kind before the chase the fight began
    std::int16_t const slot =
        store.built ? AwarenessRules::SlotOf(store.picture, now.guid) : AwarenessRules::NOT_IN_PICTURE;
    Seen const* pictured = slot >= 0 ? &store.picture.units[slot] : nullptr;
    store.fightEnemies[store.fightEnemyCount++] = now.guid;
    AwarenessRules::EnemyRow row;
    row.tenths = getMSTimeDiff(store.fightStartMs, nowMs) / 100;
    row.levelDiff = now.levelDiff;
    row.rank = now.rank;
    row.casts = now.casts;
    row.creatureType = now.creatureType;
    row.healthPct = Rounded(now.healthPct);
    row.type = now.type;
    row.motion = AwarenessRules::MotionAtJoin(now.motion, pictured, getMSTimeDiff(store.picture.builtMs, nowMs));
    line = EnemyLine{now.entry, AwarenessRules::Word(reason), AwarenessRules::EnemyAmount(row)};
    return true;
}

void Write(Player* bot, EnemyLine const& line)
{
    EventLog::Emit(bot, EventLog::EventType::FightEnemy, line.word, line.entry, static_cast<int64>(line.amount));
}
}  // namespace

void NoteAttack(PlayerbotAI* botAI, std::uint64_t targetGuid, std::string_view action)
{
    if (!botAI || !EventLog::Wanted(EventLog::EventType::FightScene))
        return;
    Store& store = OwnStore(botAI);
    store.notedCaller =
        AwarenessRules::KeptCaller(store.notedTarget, store.notedCaller, targetGuid, AwarenessRules::CallerOf(action));
    store.notedTarget = targetGuid;
}

void OnFightOpen(PlayerbotAI* botAI)
{
    Player* bot = botAI ? botAI->GetBot() : nullptr;
    if (!bot || !bot->IsInWorld() || !EventLog::Wanted(EventLog::EventType::FightScene))
        return;
    Store& store = OwnStore(botAI);
    std::uint32_t const nowMs = getMSTime();
    Unit* first = bot->GetCombatManager().GetAnyTarget();
    std::uint64_t const firstGuid = first ? first->GetGUID().GetRawValue() : 0;
    AwarenessRules::BeginFacts facts;
    facts.firstEnemy = firstGuid;
    facts.notedTarget = store.notedTarget;
    facts.notedCaller = store.notedCaller;
    Unit* victim = bot->GetVictim();
    facts.victim = victim ? victim->GetGUID().GetRawValue() : 0;
    facts.selection = bot->GetTarget().GetRawValue();
    AwarenessRules::Beginning const beginning = AwarenessRules::HowBegan(facts);
    store.notedTarget = 0;
    store.fightEnemyCount = 0;
    store.fightStartMs = nowMs;

    // The units in the fight now, judged against the picture before it (Look rebuilds it below)
    std::array<EnemyLine, OPEN_ROWS + 1> lines;
    std::size_t count = 0;
    bool const chosen = beginning.began == AwarenessRules::Began::Chosen;
    if (first && Enter(bot, store, first, chosen, !chosen, nowMs, lines[count]))
        ++count;
    CombatManager const& combat = bot->GetCombatManager();
    auto others = [&](auto const& refs)
    {
        for (auto const& [guid, ref] : refs)
            if (count < lines.size() && !InFight(store, guid.GetRawValue()) &&
                Enter(bot, store, ref->GetOther(bot), false, false, nowMs, lines[count]))
                ++count;
    };
    others(combat.GetPvECombatRefs());
    others(combat.GetPvPCombatRefs());

    Picture const& picture = Look(botAI);
    AwarenessRules::SceneCounts const counts = AwarenessRules::CountScene(picture, firstGuid);
    AwarenessRules::SceneRow row;
    row.level = picture.self.level;
    row.healthPct = Rounded(picture.self.healthPct);
    row.powerPct = Rounded(picture.self.powerPct);
    row.itemLevel = Rounded(picture.self.avgItemLevel);
    row.roseSeconds = RoseSeconds(store, nowMs);
    row.group = counts.group;
    row.guards = counts.guards;
    row.allies = counts.allies;
    row.nearby = counts.nearby;
    row.moving = bot->isMoving();
    // A held trip with no legs waits for a route: no leg under way
    HeldTrip const* trip = botAI->GetTravel().Trip();
    row.onLeg = trip && trip->CurrentLeg();
    EventLog::Emit(bot, EventLog::EventType::FightScene, AwarenessRules::Word(beginning), AwarenessRules::SceneId(row),
                   static_cast<int64>(AwarenessRules::SceneAmount(row)));
    for (std::size_t i = 0; i < count; ++i)
        Write(bot, lines[i]);
}

void OnFightClose(PlayerbotAI* botAI)
{
    if (botAI && botAI->awareness)
        botAI->awareness->notedTarget = 0;
}

void OnFightTick(PlayerbotAI* botAI)
{
    Store* store = botAI->awareness.get();
    Player* bot = botAI->GetBot();
    if (!store || !bot || store->fightEnemyCount >= FIGHT_ENEMIES_KEPT ||
        !EventLog::Wanted(EventLog::EventType::FightEnemy))
        return;
    // A tick that enters nobody compares the few combat references with the fight's few enemies and writes nothing
    CombatManager const& combat = bot->GetCombatManager();
    auto entered = [&](auto const& refs)
    {
        for (auto const& [guid, ref] : refs)
        {
            std::uint64_t const raw = guid.GetRawValue();
            EnemyLine line;
            if (!InFight(*store, raw) &&
                Enter(bot, *store, ref->GetOther(bot), raw == store->notedTarget, false, getMSTime(), line))
                Write(bot, line);
        }
    };
    entered(combat.GetPvECombatRefs());
    entered(combat.GetPvPCombatRefs());
}

namespace
{
// The rule's last step names the rise when it is at most this old: a Rise is carried out by the next action of the
// same tick or retried every tick, the healer's revive happens inside the step's own call
constexpr std::uint32_t RISE_ORDER_MS = 5000;

DeadMemo const& AskAndKeep(DeadMemo& memo)
{
    memo.last = AwarenessRules::NextOfDead(memo.facts);
    if (AwarenessRules::ChoosesHealer(memo.last) && !memo.healerChosen)
    {
        memo.healerChosen = true;
        memo.healerReason = memo.last.reason;
    }
    memo.lastMs = std::max<std::uint32_t>(getMSTime(), 1);
    // AW33b: a step that does not walk ends the walk's window, unless the walk stalled
    if (!AwarenessRules::WalkOfDead(memo.last.act))
        memo.walk = AwarenessRules::WatchPause(memo.walk, memo.lastMs);
    return memo;
}
}  // namespace

DeadMemo const& NextOfDead(PlayerbotAI* botAI, Corpse const* corpse)
{
    Player* bot = botAI->GetBot();
    Store& store = OwnStore(botAI);
    DeadMemo& memo = store.dead;
    std::uint32_t const nowMs = std::max<std::uint32_t>(getMSTime(), 1);
    // A new death starts a fresh memo; the corpse coming onto the ghost's map does not (AwarenessRules::DeathKey)
    std::uint32_t const corpseTime = corpse ? static_cast<std::uint32_t>(corpse->GetGhostTime()) : 0;
    std::uint32_t const death = AwarenessRules::DeathKey(store.died, memo.death, corpseTime);
    if (memo.death != death)
    {
        memo = DeadMemo{};
        memo.death = death;
        memo.startMs = nowMs;
    }
    AwarenessRules::DeadFacts& facts = memo.facts;
    facts = AwarenessRules::DeadFacts{};
    facts.inBattleground = bot->InBattleground();
    facts.released = bot->HasPlayerFlag(PLAYER_FLAGS_GHOST);
    facts.ownedByPlayer = botAI->HasGameClientMaster();
    facts.corpseHere = corpse != nullptr;
    facts.healerChosen = memo.healerChosen;
    facts.graveWithoutHealer = memo.graveWithoutHealer;
    facts.deathsInRow = std::max<std::uint32_t>(1, store.row);
    // The grave's facts stay at their defaults here (known, not at it): they decide only which step of the healer's
    // way or of the bound, which the spirit healer action asks again with them (NextOfDeadAtGrave)
    memo.holdRecheckMs = 0;
    // The seconds as a ghost: from the corpse when it lies here, else from the death edge or the memo's first ask
    std::int64_t const now = static_cast<std::int64_t>(time(nullptr));
    AwarenessRules::GhostClock clock;
    clock.corpse = corpse != nullptr;
    clock.sinceCorpse = corpse ? now - static_cast<std::int64_t>(corpse->GetGhostTime()) : 0;
    clock.edge = store.died != 0;
    clock.sinceEdge = now - static_cast<std::int64_t>(store.died);
    clock.sinceMemo = getMSTimeDiff(memo.startMs, nowMs) / 1000;
    clock.sinceLogin = GetMSTimeDiffToNow(botAI->rpgMemos.loginMs) / 1000;
    std::int64_t const ghostSeconds =
        TestConstraints::DeathAgeSeconds(AwarenessRules::GhostSecondsOf(clock), botAI->testConstraints);
    facts.ghostSeconds = static_cast<std::uint32_t>(std::clamp<std::int64_t>(ghostSeconds, 0, UINT32_MAX));
    if (!corpse)
        return AskAndKeep(memo);

    // AW38a: the corpse's ground for the bot healthy; the grave's as its revive leaves the bot, for a persistent bot
    // (the others keep upstream's choice of grave, GetGrave, and a quiet one)
    facts.corpseGround = GroundAt(corpse->GetMapId(), corpse->GetPositionX(), corpse->GetPositionY(),
                                  corpse->GetPositionZ(), bot->GetLevel(), bot->GetTeamId());
    if (botAI->KeepsPossessions())
        facts.graveGround = GraveGround(botAI, GraveOf(botAI));
    facts.corpseYards = bot->GetExactDist2d(corpse);
    facts.reclaimHere = corpse->IsWithinDistInMap(bot, CORPSE_RECLAIM_RADIUS, true);
    // AW33b: world PvP's hold of the ghost is a fact only on a PvP realm (where its fight rules ask it, AW44); on a
    // normal realm no enemy player's wait holds a ghost, and no hold's recheck sleeps past the bound
    if (!facts.ownedByPlayer && sWorld->IsPvPRealm())
    {
        WorldPvp::GhostPlan const plan = WorldPvp::PlanGhostFor(botAI, corpse);
        facts.playerHold = plan.step == WorldPvp::GhostStep::Corpse         ? AwarenessRules::PlayerHold::None
                           : plan.step == WorldPvp::GhostStep::SpiritHealer ? AwarenessRules::PlayerHold::Healer
                                                                            : AwarenessRules::PlayerHold::Wait;
        memo.holdRecheckMs = plan.recheckMs;
    }
    Player* leader = botAI->GetGroupLeader();
    if (leader && leader != bot)
    {
        facts.leaderAtCorpse = leader->IsAlive() && corpse->IsWithinDistInMap(leader, AwarenessRules::RISE_YARDS);
        facts.atLeader = bot->IsWithinDistInMap(leader, sPlayerbotAIConfig.SpellDistance);
        facts.leaderDown = !GET_PLAYERBOT_AI(leader) && leader->isDead() && leader->GetCorpse() &&
                           bot->IsWithinDistInMap(leader, sPlayerbotAIConfig.FarDistance);
    }
    // The ghost sees what stands near its corpse only (INTERFACE.md §3.1): its own place and the search around the
    // corpse within GHOST_LOOK_YARDS of it
    if (facts.released && facts.corpseYards <= AwarenessRules::GHOST_LOOK_YARDS)
    {
        facts.own = JudgePoint(botAI, *bot);
        facts.best = FindSafePoint(botAI, *corpse, AwarenessRules::RISE_YARDS - AwarenessRules::ARRIVE_YARDS);
        facts.atBest = AwarenessRules::AtBest(facts.best, bot->GetPositionX(), bot->GetPositionY());
        memo.unclearStartMs = AwarenessRules::UnclearStart(memo.unclearStartMs, facts.best, nowMs);
        facts.unclearSeconds = memo.unclearStartMs ? getMSTimeDiff(memo.unclearStartMs, nowMs) / 1000 : 0;
    }
    return AskAndKeep(memo);
}

DeadMemo const& NextOfDeadWalkFailed(PlayerbotAI* botAI)
{
    DeadMemo& memo = OwnStore(botAI).dead;
    memo.facts.corpseWalkFailed = true;
    memo.facts.healerChosen = memo.healerChosen;
    memo.facts.graveWithoutHealer = memo.graveWithoutHealer;
    return AskAndKeep(memo);
}

bool WalkStalled(PlayerbotAI* botAI, AwarenessRules::WalkKind kind, float x, float y)
{
    DeadMemo& memo = OwnStore(botAI).dead;
    Player const* bot = botAI->GetBot();
    std::uint32_t const nowMs = std::max<std::uint32_t>(getMSTime(), 1);
    memo.walk = AwarenessRules::WatchWalk(memo.walk, kind, x, y, bot->GetExactDist2d(x, y), nowMs);
    return AwarenessRules::WalkStalled(memo.walk, nowMs);
}

DeadMemo const& NextOfDeadAtGrave(PlayerbotAI* botAI, GraveFacts const& grave)
{
    DeadMemo& memo = OwnStore(botAI).dead;
    // AW33b: the searches at the grave, for the healer_wait row
    if (grave.looked)
        memo.healerFound = grave.healerInRange ? 1 : 0;
    memo.healerSearched = memo.healerSearched || grave.searched;
    memo.facts.graveKnown = grave.known;
    memo.facts.atGravePoint = grave.atGravePoint;
    memo.facts.healerInRange = grave.healerInRange;
    memo.facts.graveWalkFailed = grave.walkFailed;
    memo.facts.teleportSeen = grave.seen;
    memo.facts.healerChosen = memo.healerChosen;
    // AW30b: the ghost stood at its grave and a search near its point found no living spirit healer: this death's
    // healer's way is closed while its corpse lies on this map (AwarenessRules::NextOfDead, line 3)
    memo.graveWithoutHealer =
        AwarenessRules::GraveWithoutHealer(memo.graveWithoutHealer, grave.atGrave, grave.searched, grave.healerInRange);
    memo.facts.graveWithoutHealer = memo.graveWithoutHealer;
    return AskAndKeep(memo);
}

DeadMemo const* LastOfDead(PlayerbotAI* botAI) { return botAI->awareness ? &botAI->awareness->dead : nullptr; }

namespace
{
// AW38a: the sides each grave serves (bit 0 the Alliance, bit 1 the Horde), written once by LoadGraveSides before the
// world runs and only read afterwards
std::unordered_map<std::uint32_t, std::uint8_t> graveSides;

bool SideMayUse(std::uint32_t graveId, TeamId side)
{
    auto const it = graveSides.find(graveId);
    return side < TEAM_NEUTRAL && it != graveSides.end() && ((it->second >> side) & 1u) != 0;
}

// As the spirit healer's revive leaves the bot (ReviveAtGrave): half its health, sick where it pays for the revive at a
// level the sickness reaches (Player::ResurrectPlayer)
DangerRules::Condition ReviveCondition(Player* bot)
{
    bool const pays = !Upkeep::Granted(bot, UpkeepKind::Resurrect);
    std::int32_t const sickFrom = sWorld->getIntConfig(CONFIG_DEATH_SICKNESS_LEVEL);
    return {static_cast<std::uint8_t>(AwarenessRules::RISE_HEALTH_PCT),
            pays && static_cast<std::int32_t>(bot->GetLevel()) >= sickFrom};
}

// The nearest grave of the ghost's map its side may use whose ground is not deadly as the revive leaves it; 0 for none
std::uint32_t SurvivableGrave(Player* bot)
{
    DangerRules::Condition const condition = ReviveCondition(bot);
    std::uint32_t best = 0;
    float bestYards = 0.0f;
    for (auto const& [id, grave] : sGraveyard->GetGraveyardData())
    {
        if (grave.Map != bot->GetMapId() || !SideMayUse(id, bot->GetTeamId()))
            continue;
        float const yards = bot->GetExactDist2d(grave.x, grave.y);
        if ((best && yards >= bestYards) || GroundAt(grave.Map, grave.x, grave.y, grave.z, bot->GetLevel(),
                                                     bot->GetTeamId(), condition) == AwarenessRules::Ground::Deadly)
            continue;
        best = id;
        bestYards = yards;
    }
    return best;
}
}  // namespace

GraveyardStruct const* GraveOf(PlayerbotAI* botAI)
{
    Player* bot = botAI->GetBot();
    GraveyardStruct const* game = sGraveyard->GetClosestGraveyard(bot, bot->GetTeamId());
    DeadMemo& memo = OwnStore(botAI).dead;
    if (memo.facts.ghostSeconds < AwarenessRules::GHOST_BOUND_SECONDS)
        return game;
    if (!memo.boundGraveChosen)
    {
        memo.boundGraveChosen = true;
        memo.boundGrave = SurvivableGrave(bot);
    }
    GraveyardStruct const* bound = memo.boundGrave ? sGraveyard->GetGraveyard(memo.boundGrave) : nullptr;
    return bound ? bound : game;
}

AwarenessRules::Ground GraveGround(PlayerbotAI* botAI, GraveyardStruct const* grave)
{
    if (!grave)
        return AwarenessRules::Ground::Quiet;
    DeadMemo& memo = OwnStore(botAI).dead;
    if (memo.groundGrave != grave->ID)
    {
        Player* bot = botAI->GetBot();
        memo.groundGrave = grave->ID;
        memo.graveGround =
            GroundAt(grave->Map, grave->x, grave->y, grave->z, bot->GetLevel(), bot->GetTeamId(), ReviveCondition(bot));
    }
    return memo.graveGround;
}

void LoadGraveSides()
{
    std::uint32_t const started = getMSTime();
    graveSides.clear();
    std::size_t links = 0;
    if (QueryResult result = WorldDatabase.Query("SELECT ID, Faction FROM graveyard_zone"))
    {
        do
        {
            Field* fields = result->Fetch();
            std::uint32_t const faction = fields[1].Get<uint16>();
            // The core's links (Graveyard::LoadGraveyardZones): 0 serves both sides, a player faction its own
            std::uint8_t const sides = faction == 0          ? 3
                                       : faction == ALLIANCE ? 1u << TEAM_ALLIANCE
                                       : faction == HORDE    ? 1u << TEAM_HORDE
                                                             : 0;
            if (!sides)
                continue;
            graveSides[fields[0].Get<uint32>()] |= sides;
            ++links;
        } while (result->NextRow());
    }
    LOG_INFO("server.loading", ">> Loaded the sides of {} graves from {} graveyard links (awareness) in {} ms",
             graveSides.size(), links, GetMSTimeDiffToNow(started));
}

AwarenessRules::SelfRiseAct SelfRise(PlayerbotAI* botAI, std::uint32_t* bodySeconds)
{
    Store& store = OwnStore(botAI);
    Player* bot = botAI->GetBot();
    std::uint32_t const nowMs = std::max<std::uint32_t>(getMSTime(), 1);
    if (store.selfRiseAskMs != nowMs)
    {
        // The seconds since the death edge; a death without its stamp counts from the body's first ask
        if (!store.died && !store.selfRiseStartMs)
            store.selfRiseStartMs = nowMs;
        std::uint32_t const wall = static_cast<std::uint32_t>(time(nullptr));
        AwarenessRules::SelfRiseFacts& facts = store.selfRiseFacts;
        facts.inBattleground = bot->InBattleground() || bot->InArena();
        facts.ownedByPlayer = botAI->HasGameClientMaster();
        facts.bodySeconds =
            store.died ? wall - std::min(wall, store.died) : getMSTimeDiff(store.selfRiseStartMs, nowMs) / 1000;
        facts.own = JudgePoint(botAI, *bot);
        store.selfRiseAct = AwarenessRules::SelfRiseStep(facts);
        store.selfRiseAskMs = nowMs;
    }
    if (bodySeconds)
        *bodySeconds = store.selfRiseFacts.bodySeconds;
    return store.selfRiseAct;
}

void OrderSelfRise(PlayerbotAI* botAI) { OwnStore(botAI).selfRiseOrderedMs = std::max<std::uint32_t>(getMSTime(), 1); }

void ChooseHealer(PlayerbotAI* botAI) { OwnStore(botAI).dead.healerChosen = true; }

std::uint32_t DeathsInRow(PlayerbotAI* botAI)
{
    return std::max<std::uint32_t>(1, botAI->awareness ? botAI->awareness->row : 0);
}

bool RecoverFactsOf(PlayerbotAI* botAI, AwarenessRules::RecoverFacts& facts)
{
    Store const* store = botAI->awareness.get();
    Player* bot = botAI->GetBot();
    if (!store || !store->rose || !bot || !bot->IsAlive() || bot->InBattleground() || botAI->HasGameClientMaster())
        return false;
    facts.sinceRiseSeconds = GetMSTimeDiffToNow(store->roseMs) / 1000;
    facts.healthPct = static_cast<std::uint32_t>(bot->GetHealthPct());
    std::uint32_t const maxMana = bot->GetMaxPower(POWER_MANA);
    facts.usesMana = maxMana > 0;
    facts.powerPct = maxMana ? bot->GetPower(POWER_MANA) * 100 / maxMana : 100;
    facts.sick = bot->HasAura(Upkeep::SPELL_RESURRECTION_SICKNESS);
    return true;
}

void OnRise(Player* bot, std::string_view how)
{
    PlayerbotAI* botAI = bot ? GET_PLAYERBOT_AI(bot) : nullptr;
    if (!botAI)
        return;
    // AW9: every rise is the store's, a rule reads it (the row, the recovery)
    Store& store = OwnStore(botAI);
    // AW8: where the bot rose by the rule of the dead, from the return's memo, which ends here
    {
        DeadMemo& memo = store.dead;
        if (EventLog::Wanted(EventLog::EventType::Rise))
        {
            bool const ordered = memo.lastMs && GetMSTimeDiffToNow(memo.lastMs) <= RISE_ORDER_MS;
            // AW33b: a self-resurrection the self-rise rule ordered
            bool const self = store.selfRiseOrderedMs && GetMSTimeDiffToNow(store.selfRiseOrderedMs) <= RISE_ORDER_MS;
            AwarenessRules::RiseRow row;
            row.ghostSeconds = memo.lastMs ? memo.facts.ghostSeconds : 0;
            row.corpseYards = memo.lastMs && memo.facts.corpseHere
                                  ? std::min(Rounded(memo.facts.corpseYards), AwarenessRules::YARDS_NONE)
                                  : AwarenessRules::YARDS_NONE;
            row.deathsInRow = std::max<std::uint32_t>(1, store.row);
            row.reaching = JudgePoint(botAI, *bot).reaching;
            EventLog::Emit(bot, EventLog::EventType::Rise,
                           AwarenessRules::RiseWord(memo.last, ordered, how == EventLog::RESURRECT_CORPSE, self), 0,
                           static_cast<int64>(AwarenessRules::RiseAmount(row)));
        }
        memo = DeadMemo{};
    }
    store.selfRiseAskMs = 0;
    store.selfRiseStartMs = 0;
    store.selfRiseOrderedMs = 0;
    store.row = AwarenessRules::RowAtRise(store.row);
    store.died = 0;
    store.rose = true;
    store.roseMs = getMSTime();
    store.roseMap = bot->GetMapId();
    store.roseX = bot->GetPositionX();
    store.roseY = bot->GetPositionY();
}

void OnDeath(Player* bot, Unit* killer, std::uint32_t killerEntry)
{
    PlayerbotAI* botAI = bot ? GET_PLAYERBOT_AI(bot) : nullptr;
    if (!botAI || !bot->IsInWorld())
        return;
    Store& store = OwnStore(botAI);
    std::uint32_t const nowMs = getMSTime();
    // AW9: the row of deaths (INTERFACE.md §3.5) against the last rise, and the death's stamp (the return's key),
    // whether or not the DeathScene row is wanted. A battleground's death is not of the row
    if (!bot->InBattleground())
    {
        AwarenessRules::RowDeath death;
        death.rose = store.rose;
        death.sinceRiseSeconds = store.rose ? getMSTimeDiff(store.roseMs, nowMs) / 1000 : 0;
        death.sameMap = store.roseMap == bot->GetMapId();
        death.riseYards =
            AwarenessRules::PlanarDistance(store.roseX, store.roseY, bot->GetPositionX(), bot->GetPositionY());
        store.row = AwarenessRules::RowAfterDeath(store.row, death);
    }
    store.died = std::max<std::uint32_t>(static_cast<std::uint32_t>(time(nullptr)), 1);
    store.selfRiseAskMs = 0;
    store.selfRiseStartMs = 0;
    store.selfRiseOrderedMs = 0;
    if (!EventLog::Wanted(EventLog::EventType::DeathScene))
        return;
    Picture const& picture = Look(botAI);
    AwarenessRules::DeathRow row;
    if (killer && killer != bot)
    {
        Seen const seen = DescribeOne(bot, store, killer);
        row.killerLevelDiff = seen.levelDiff;
        row.killerRank = seen.rank;
        row.killerType = static_cast<std::uint32_t>(seen.type);
    }
    // The fight's enemies still alive (the core ends the bot's combat before this hook, so no link names it)
    bool killerCounted = false;
    for (std::uint8_t i = 0; i < store.fightEnemyCount; ++i)
    {
        Unit* unit = ObjectAccessor::GetUnit(*bot, ObjectGuid(store.fightEnemies[i]));
        if (unit && unit->IsAlive())
        {
            ++row.enemies;
            killerCounted = killerCounted || unit == killer;
        }
    }
    if (killer && killer != bot && !killerCounted && killer->IsAlive())
        ++row.enemies;
    row.roseSeconds = RoseSeconds(store, nowMs);
    if (store.rose && store.roseMap == bot->GetMapId())
        row.roseYards = std::min(
            Rounded(AwarenessRules::PlanarDistance(store.roseX, store.roseY, bot->GetPositionX(), bot->GetPositionY())),
            AwarenessRules::YARDS_NONE);
    AwarenessRules::Helper const helper =
        AwarenessRules::NearestHelper(picture, store.fightEnemies.data(), store.fightEnemyCount);
    if (helper.found)
        row.helperYards = std::min(Rounded(helper.yards), AwarenessRules::HELPER_NONE - 1);
    EventLog::Emit(bot, EventLog::EventType::DeathScene, AwarenessRules::Word(helper), killerEntry,
                   static_cast<int64>(AwarenessRules::DeathAmount(row)));
}

namespace
{
constexpr char const* WAY_USAGE =
    "Usage: .playerbots aw way <bot> <map> <x> <y> <z> [expect=<clear|least|routed|safe|nosafe,..>]";
constexpr char const* WAYS_USAGE = "Usage: .playerbots aw ways <alliance|horde> <level 1..80> | all";
constexpr char const* TERM_USAGE = "Usage: .playerbots aw term <off|on>";
// `aw ways`: each band's lines in the reply (Playerbots.log has every line); `all` asks these levels for both sides
constexpr std::size_t WAYS_LISTED = 30;
constexpr int WAYS_BAND_STEP = 5;
constexpr int WAYS_BAND_TOP = 80;
constexpr std::uint32_t WAYS_MONEY = 100000000;  // a purse no fare refuses
// The capitals `aw ways` starts from (TravelPlaces' names), by Snapshot::team, with the race whose start flight points
// a bot there has
struct CapitalName
{
    char const* name;
    std::uint32_t race;
};
constexpr std::array<CapitalName, 3> CAPITALS_ALLIANCE{
    {{"stormwind", RACE_HUMAN}, {"ironforge", RACE_DWARF}, {"darnassus", RACE_NIGHTELF}}};
constexpr std::array<CapitalName, 4> CAPITALS_HORDE{{{"orgrimmar", RACE_ORC},
                                                     {"undercity", RACE_UNDEAD_PLAYER},
                                                     {"thunderbluff", RACE_TAUREN},
                                                     {"silvermoon", RACE_BLOODELF}}};

struct Capital
{
    TravelPlaces::Place const* place = nullptr;
    std::uint32_t race = 0;
};

// The side's flight points with their zones' brackets, as the factory reads them (AddHistoryFlightNodes)
std::vector<StartingHistory::NodeInZone> TeamNodes(std::uint8_t team)
{
    std::vector<StartingHistory::NodeInZone> nodes;
    for (auto const& [dbGuid, info] : sTravelMgr.GetFlightMasters(team ? TEAM_HORDE : TEAM_ALLIANCE))
    {
        StartingHistory::NodeInZone spot;
        spot.node = info.taxiNodeId;
        spot.hasBracket = sTravelMgr.GetZoneBracket(info.zoneId, spot.bracket.low, spot.bracket.high);
        nodes.push_back(spot);
    }
    return nodes;
}

// The flight points the factory gives a bot of this race and level (PlayerbotFactory AddHistoryFlightNodes): its
// race's start nodes and every node of the side's zones whose bracket ends below the level
std::array<std::uint32_t, TravelRoute::TAXI_MASK_SIZE> HistoryMask(
    std::uint32_t race, std::uint8_t level, std::vector<StartingHistory::NodeInZone> const& teamNodes)
{
    PlayerTaxi raceStart;
    raceStart.InitTaxiNodesForLevel(race, CLASS_WARRIOR, level);
    std::vector<std::uint32_t> startNodes;
    for (std::uint32_t node = 1; node < sTaxiNodesStore.GetNumRows(); ++node)
        if (raceStart.IsTaximaskNodeKnown(node))
            startNodes.push_back(node);
    TravelRoute::Snapshot snapshot;
    for (std::uint32_t const node : StartingHistory::HistoryNodes(level, startNodes, teamNodes))
        snapshot.LearnTaxiNode(node);
    return snapshot.knownTaxi;
}

// The facts that make a link deadly, as `aw ways` prints them: guards, or each counted rank's letter, level and count
// (N31x1, E48x2+N40x3)
std::string FactsWord(DangerRules::CellFacts const& facts)
{
    if (facts.guards)
        return "guards";
    static constexpr std::array<char, 4> LETTERS{'N', 'E', 'R', 'B'};
    std::string word;
    for (std::size_t r = 0; r < DangerRules::RANK_COUNT && r < LETTERS.size(); ++r)
        if (facts.count[r])
            word += Acore::StringFormat("{}{}{}x{}", word.empty() ? "" : "+", LETTERS[r], int(facts.level[r]),
                                        int(facts.count[r]));
    return word.empty() ? "-" : word;
}

char const* VerdictWord(DangerRules::WayVerdict verdict)
{
    switch (verdict)
    {
        case DangerRules::WayVerdict::Clear:
            return "clear";
        case DangerRules::WayVerdict::LeastDeadly:
            return "least_deadly";
        case DangerRules::WayVerdict::Refused:
            return "no_safe_way";
    }
    return "?";
}

DangerRules::Place PlaceOf(TravelRoute::Point const& point) { return {point.mapId, point.x, point.y, point.z}; }

std::string AreaName(std::uint32_t id)
{
    AreaTableEntry const* const entry = sAreaTableStore.LookupEntry(id);
    return entry && entry->area_name[0] ? entry->area_name[0] : "?";
}

char const* SideWord(std::uint8_t team) { return team ? "horde" : "alliance"; }

// The route's legs and nodes: two answers alike take the same way
std::vector<std::uint32_t> RouteKey(TravelRoute::Result const& result)
{
    std::vector<std::uint32_t> key;
    for (TravelRoute::Leg const& leg : result.legs)
    {
        key.push_back(0x80000000u | static_cast<std::uint32_t>(leg.type));
        key.insert(key.end(), leg.nodes.begin(), leg.nodes.end());
    }
    return key;
}

// What a pass of searches cost, each search timed alone
struct PassTime
{
    double mean = 0.0;
    std::uint32_t p50 = 0;
    std::uint32_t p95 = 0;
    std::uint32_t max = 0;
};

PassTime TimeOf(std::vector<std::uint32_t> micros)
{
    PassTime time;
    if (micros.empty())
        return time;
    std::sort(micros.begin(), micros.end());
    double sum = 0.0;
    for (std::uint32_t const us : micros)
        sum += us;
    time.mean = sum / static_cast<double>(micros.size());
    time.p50 = micros[micros.size() / 2];
    time.p95 = micros[std::min(micros.size() - 1, micros.size() * 95 / 100)];
    time.max = micros.back();
    return time;
}

std::vector<TravelRoute::Result> SearchPass(TravelGraph const& graph, std::vector<TravelRoute::Request> const& requests,
                                            TravelRoute::Params const& params, std::vector<std::uint32_t>& micros)
{
    TravelRoute::StuckEdges const none{};
    std::vector<TravelRoute::Result> results;
    results.reserve(requests.size());
    micros.clear();
    micros.reserve(requests.size());
    for (TravelRoute::Request const& request : requests)
    {
        auto const started = std::chrono::steady_clock::now();
        results.push_back(TravelRoute::Search(graph, request, none, params));
        micros.push_back(static_cast<std::uint32_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - started).count()));
    }
    return results;
}

// One side and level of `aw ways`: its lines (the summary first) and what the whole command sums
struct BandOutcome
{
    std::vector<std::string> lines;
    std::size_t ownZones = 0;  // zones of the level's own bracket the band asks
    std::size_t ownCut = 0;    // of them cut off
    std::size_t searches = 0;
    std::vector<std::uint32_t> onMicros;
    std::vector<std::uint32_t> offMicros;
};

// A hub's zone against the level: its bracket holds the level, starts above it (at most ZONE_LEVELS_ABOVE, Plays)
// or ends below it
enum class ZoneKind : std::uint8_t
{
    Own,
    Ahead,
    Below
};
constexpr std::array<char const*, 3> ZONE_KIND_WORDS{"own", "ahead", "below"};

BandOutcome WaysBand(Planner::PlannerIndexes const& indexes, std::uint8_t team, std::uint8_t level)
{
    BandOutcome outcome;
    TravelGraph const& graph = TravelGraph::Instance();
    DangerRules::Known const* const known = DangerRules::Published();
    bool const termOn = DangerRules::TermOn();
    TravelRoute::Params const params = TravelRoute::ConfiguredParams();

    std::vector<Capital> capitals;
    auto const addCapital = [&capitals](CapitalName const& capital)
    {
        for (TravelPlaces::Place const& place : TravelPlaces::All())
            if (std::string_view(place.name) == capital.name)
                capitals.push_back({&place, capital.race});
    };
    if (team)
        for (CapitalName const& capital : CAPITALS_HORDE)
            addCapital(capital);
    else
        for (CapitalName const& capital : CAPITALS_ALLIANCE)
            addCapital(capital);
    std::vector<StartingHistory::NodeInZone> const teamNodes = TeamNodes(team);
    // The flight points of a bot from each capital at a level, built once per level
    auto const masksAt = [&capitals, &teamNodes](std::uint8_t at)
    {
        std::vector<std::array<std::uint32_t, TravelRoute::TAXI_MASK_SIZE>> masks;
        for (Capital const& capital : capitals)
            masks.push_back(HistoryMask(capital.race, at, teamNodes));
        return masks;
    };
    // A bot of the capital's race with the flight points the factory gives its level, no hearthstone and no flying
    // mount, with money for any fare: it flies where it knows both ends, rides ships, zeppelins, lifts and portals and
    // walks the ground
    auto const requestOf = [&capitals, team](std::size_t c, Planner::Hub const& hub, std::uint8_t at,
                                             std::array<std::uint32_t, TravelRoute::TAXI_MASK_SIZE> const& mask)
    {
        TravelRoute::Request request;
        request.snapshot.position = capitals[c].place->at;
        request.snapshot.team = team;
        request.snapshot.level = at;
        request.snapshot.money = WAYS_MONEY;
        request.snapshot.allowTransport = true;
        request.snapshot.knownTaxi = mask;
        request.goal = {hub.mapId, hub.x, hub.y, hub.z};
        return request;
    };

    // The hubs of the zones the level plays (SkillPathBridge::Plays), the side's own and the neutral ones
    struct HubAsk
    {
        std::size_t hub = 0;
        ZoneKind kind = ZoneKind::Own;
    };
    std::vector<HubAsk> hubs;
    std::size_t otherSide = 0;
    std::size_t noZone = 0;
    Planner::PlaceSide const enemy = team ? Planner::PlaceSide::Alliance : Planner::PlaceSide::Horde;
    for (std::size_t i = 0; i < indexes.hubs.size(); ++i)
    {
        Planner::Hub const& hub = indexes.hubs[i];
        if (hub.mapId != 0 && hub.mapId != 1 && hub.mapId != 530 && hub.mapId != 571)
            continue;
        if (hub.side == enemy)
        {
            ++otherSide;
            continue;
        }
        Planner::ZoneRecord const* const zone = indexes.FindZone(hub.zoneId);
        if (!zone)
        {
            ++noZone;
            continue;
        }
        bool const open = zone->side == Planner::ZoneSide::Contested || zone->side == Planner::ZoneSide::Sanctuary ||
                          (zone->side == Planner::ZoneSide::Alliance) == (team == 0);
        if (!SkillPathBridge::Plays({zone->zoneId, zone->low, zone->high, zone->mapId, open}, level))
            continue;
        ZoneKind const kind = zone->low > level    ? ZoneKind::Ahead
                              : zone->high < level ? ZoneKind::Below
                                                   : ZoneKind::Own;
        hubs.push_back({i, kind});
    }

    std::vector<TravelRoute::Request> requests;
    requests.reserve(hubs.size() * capitals.size());
    std::vector<std::array<std::uint32_t, TravelRoute::TAXI_MASK_SIZE>> const masks = masksAt(level);
    for (HubAsk const& ask : hubs)
        for (std::size_t c = 0; c < capitals.size(); ++c)
            requests.push_back(requestOf(c, indexes.hubs[ask.hub], level, masks[c]));
    outcome.searches = requests.size();

    // The pass as published, then the same requests without the term on this thread alone (DangerRules::Unpriced):
    // no other thread's search loses its term meanwhile
    std::vector<TravelRoute::Result> const on = SearchPass(graph, requests, params, outcome.onMicros);
    std::vector<TravelRoute::Result> off;
    {
        DangerRules::Unpriced const unpriced;
        off = SearchPass(graph, requests, params, outcome.offMicros);
    }

    struct ZoneTally
    {
        ZoneKind kind = ZoneKind::Own;
        std::size_t reached = 0;
        std::size_t noSafe = 0;
        std::vector<std::size_t> asks;  // its hubs, by index into `hubs`
    };
    std::map<std::uint32_t, ZoneTally> zones;
    std::vector<std::string> hubLines;
    std::vector<std::string> changedLines;
    std::size_t noSafeHubs = 0;
    std::size_t unreached = 0;
    std::size_t changed = 0;
    std::size_t okDiffers = 0;
    std::size_t const perHub = capitals.size();
    for (std::size_t h = 0; h < hubs.size(); ++h)
    {
        Planner::Hub const& hub = indexes.hubs[hubs[h].hub];
        DangerRules::Place const goal{hub.mapId, hub.x, hub.y, hub.z};
        std::size_t reached = 0;
        std::size_t safe = 0;
        std::size_t goalDeadly = 0;
        std::size_t best = perHub;
        DangerRules::DangerPart bestPart;
        for (std::size_t c = 0; c < perHub; ++c)
        {
            std::size_t const r = h * perHub + c;
            TravelRoute::Result const& result = on[r];
            DangerRules::Place const start = PlaceOf(capitals[c].place->at);
            if (result.ok != off[r].ok)
                ++okDiffers;
            if (!result.ok)
                continue;
            ++reached;
            DangerRules::DangerPart part;
            if (known)
                part = DangerRules::WayDanger(*known, DangerRules::WalkLinksOf(graph, result.legs), start, goal, team,
                                              level);
            if (part.goal == DangerRules::Ground::Deadly)
                ++goalDeadly;
            if (!DangerRules::Refuses(part))
                ++safe;
            else if (best == perHub || part.deadlyLinks < bestPart.deadlyLinks ||
                     (part.deadlyLinks == bestPart.deadlyLinks &&
                      result.totalSeconds < on[h * perHub + best].totalSeconds))
            {
                best = c;
                bestPart = part;
            }
            if (!off[r].ok || RouteKey(result) == RouteKey(off[r]))
                continue;
            ++changed;
            DangerRules::DangerPart offPart;
            if (known)
                offPart = DangerRules::WayDanger(*known, DangerRules::WalkLinksOf(graph, off[r].legs), start, goal,
                                                 team, level);
            float const plain = result.totalSeconds - part.penaltySeconds -
                                DangerRules::FIGHT_SECONDS * static_cast<float>(part.fights);
            changedLines.push_back(Acore::StringFormat(
                "AW ways {} {} changed {} -> hub #{} {} / {}: off {} deadly links {:.0f} yd {} fights {:.0f} s; on {} "
                "deadly links {:.0f} yd {} fights {:.0f} s plain ({:+.0f} s)",
                SideWord(team), level, capitals[c].place->name, hubs[h].hub, AreaName(hub.zoneId), AreaName(hub.areaId),
                offPart.deadlyLinks, offPart.deadlyYards, offPart.fights, off[r].totalSeconds, part.deadlyLinks,
                part.deadlyYards, part.fights, plain, plain - off[r].totalSeconds));
        }
        ZoneTally& zone = zones[hub.zoneId];
        zone.kind = hubs[h].kind;
        zone.asks.push_back(h);
        if (!reached)
        {
            ++unreached;
            continue;
        }
        ++zone.reached;
        if (safe)
            continue;
        ++noSafeHubs;
        ++zone.noSafe;
        TravelRoute::Result const& way = on[h * perHub + best];
        // Each deadly link of the best way: its yards, the yards of it deadly at the level and the facts that decide it
        std::string deadlyLinks;
        if (known)
            for (std::uint32_t const index : DangerRules::WalkLinksOf(graph, way.legs))
            {
                DangerRules::LinkDanger const* const link = known->LinkOf(index, team);
                if (!link || !DangerRules::TermOf(*link, level).deadly)
                    continue;
                deadlyLinks +=
                    Acore::StringFormat("{}#{} {}/{:.0f} {}", deadlyLinks.empty() ? "" : " ", index, link->yards,
                                        DangerRules::DeadlyYardsOfLink(*known, graph, index, team, level),
                                        FactsWord(DangerRules::DecidingFactsOfLink(*known, graph, index, team, level)));
            }
        hubLines.push_back(Acore::StringFormat(
            "AW ways {} {} hub #{} {} (zone {}, {}) / {}: map {} {:.0f} {:.0f} {:.0f}, no safe way from {} of {} (goal "
            "deadly {}); best from {}: {} deadly links {:.0f} yd [link yd/deadly yd facts: {}], {} fights, start {} "
            "goal {}, {:.0f} s (penalty {:.0f} s)",
            SideWord(team), level, hubs[h].hub, AreaName(hub.zoneId), hub.zoneId,
            ZONE_KIND_WORDS[static_cast<std::size_t>(hubs[h].kind)], AreaName(hub.areaId), hub.mapId, hub.x, hub.y,
            hub.z, reached, perHub, goalDeadly, capitals[best].place->name, bestPart.deadlyLinks, bestPart.deadlyYards,
            deadlyLinks, bestPart.fights, GroundWord(bestPart.start), GroundWord(bestPart.goal), way.totalSeconds,
            bestPart.penaltySeconds));
    }

    // The first level above the band's at which a hub of the zone has a safe way, its hubs asked from each capital
    // with that level's flight points; 0 when none has one up to the zone's top
    TravelRoute::StuckEdges const noStuck{};
    std::map<std::uint8_t, std::vector<std::array<std::uint32_t, TravelRoute::TAXI_MASK_SIZE>>> masksByLevel;
    auto const opensAt = [&](ZoneTally const& zone, std::uint32_t high)
    {
        for (std::uint32_t at = level + 1u; at <= std::min<std::uint32_t>(high, WAYS_BAND_TOP); ++at)
        {
            std::uint8_t const up = static_cast<std::uint8_t>(at);
            auto found = masksByLevel.find(up);
            if (found == masksByLevel.end())
                found = masksByLevel.emplace(up, masksAt(up)).first;
            for (std::size_t const h : zone.asks)
            {
                Planner::Hub const& hub = indexes.hubs[hubs[h].hub];
                for (std::size_t c = 0; c < capitals.size(); ++c)
                {
                    TravelRoute::Result const result =
                        TravelRoute::Search(graph, requestOf(c, hub, up, found->second[c]), noStuck, params);
                    if (result.ok && known &&
                        !DangerRules::Refuses(DangerRules::WayDanger(
                            *known, DangerRules::WalkLinksOf(graph, result.legs), PlaceOf(capitals[c].place->at),
                            {hub.mapId, hub.x, hub.y, hub.z}, team, up)))
                        return at;
                }
            }
        }
        return 0u;
    };

    // A zone is cut off when every hub of it the search reached has no safe way; one of the own bracket says from
    // which level it opens
    std::array<std::string, 3> cut;
    std::array<std::size_t, 3> kinds{};
    for (auto const& [zoneId, zone] : zones)
    {
        ++kinds[static_cast<std::size_t>(zone.kind)];
        if (!zone.reached || zone.noSafe != zone.reached)
            continue;
        std::string& list = cut[static_cast<std::size_t>(zone.kind)];
        list += Acore::StringFormat("{}{} ({})", list.empty() ? "" : ", ", AreaName(zoneId), zoneId);
        if (zone.kind != ZoneKind::Own)
            continue;
        ++outcome.ownCut;
        Planner::ZoneRecord const* const record = indexes.FindZone(zoneId);
        std::uint32_t const high = record ? record->high : level;
        std::uint32_t const opens = opensAt(zone, high);
        list += opens ? Acore::StringFormat(" from {}", opens) : Acore::StringFormat(" none to {}", high);
    }
    outcome.ownZones = kinds[static_cast<std::size_t>(ZoneKind::Own)];
    PassTime const onTime = TimeOf(outcome.onMicros);
    PassTime const offTime = TimeOf(outcome.offMicros);
    outcome.lines.push_back(Acore::StringFormat(
        "AW ways {} {}: {} hubs in {} zones (own {}, ahead {}, below {}; other side {}, no zone {}), {} capitals, {} "
        "searches a pass | no safe way: {} hubs; zones cut off: own [{}] ahead [{}] below [{}] | unreached {} hubs | "
        "changed by the term {} (ok differs {}) | search on mean {:.0f} p50 {} p95 {} max {} us, off mean {:.0f} p50 "
        "{} p95 {} max {} us | term {}, store {}",
        SideWord(team), level, hubs.size(), zones.size(), kinds[0], kinds[1], kinds[2], otherSide, noZone,
        capitals.size(), requests.size(), noSafeHubs, cut[0], cut[1], cut[2], unreached, changed, okDiffers,
        onTime.mean, onTime.p50, onTime.p95, onTime.max, offTime.mean, offTime.p50, offTime.p95, offTime.max,
        termOn ? "on" : "off", known ? known->build : "none"));
    outcome.lines.insert(outcome.lines.end(), hubLines.begin(), hubLines.end());
    outcome.lines.insert(outcome.lines.end(), changedLines.begin(), changedLines.end());
    return outcome;
}
}  // namespace

DangerRules::DangerPart WayOf(std::span<TravelRoute::Leg const> legs, TravelRoute::Point const& start,
                              TravelRoute::Point const& goal, TeamId side, std::uint8_t level)
{
    DangerRules::Known const* const known = DangerRules::Published();
    if (!known)
        return {};
    TravelGraph const& graph = TravelGraph::Instance();
    std::vector<std::uint32_t> const walks =
        graph.IsReady() ? DangerRules::WalkLinksOf(graph, legs) : std::vector<std::uint32_t>{};
    return DangerRules::WayDanger(*known, walks, PlaceOf(start), PlaceOf(goal), SideIndex(side), level);
}

bool WayJudgeDue(PlayerbotAI* botAI)
{
    HeldTrip const* const trip = botAI->GetTravel().Trip();
    if (!trip)
        return false;
    DangerRules::JudgedPair const last = botAI->awareness ? botAI->awareness->judged : DangerRules::JudgedPair{};
    return DangerRules::WayDue(true, trip->route.waiting, !trip->trip.legs.empty(), trip->tripId, trip->route.requestId,
                               last);
}

void JudgeTripWay(PlayerbotAI* botAI)
{
    HeldTrip const* const trip = botAI->GetTravel().Trip();
    if (!trip || trip->trip.legs.empty())
        return;
    std::unique_ptr<Store>& own = botAI->awareness;
    if (!own)
        own = std::make_unique<Store>();
    own->judged = {trip->tripId, trip->route.requestId};
    // `aw term off` (a lab's walk test) judges nothing: the route was searched without the term
    if (!DangerRules::TermOn())
        return;
    Player* const bot = botAI->GetBot();
    TravelRoute::Point const goal{trip->dest.GetMapId(), trip->dest.GetPositionX(), trip->dest.GetPositionY(),
                                  trip->dest.GetPositionZ()};
    std::uint8_t const level = static_cast<std::uint8_t>(std::min<std::uint32_t>(bot->GetLevel(), 255));
    DangerRules::DangerPart const part =
        WayOf(trip->trip.legs, trip->trip.legs.front().from, goal, bot->GetTeamId(), level);
    // Spared: a GM's order (`route go` names its place); the leaving's trip joins it with AW38b
    DangerRules::WayVerdict const verdict = DangerRules::JudgeWay(part, !trip->place.empty());
    char const* const choice = DangerRules::WayChoice(verdict);
    if (!choice)
        return;
    std::uint32_t const tripId = trip->tripId;
    char const* const goalWord = RpgStatus::TripGoal(botAI, trip->bindInn);
    DangerRules::WayEffects const effects = DangerRules::EffectsOf(verdict);
    for (std::uint8_t i = 0; i < effects.count; ++i)
        switch (effects.order[i])
        {
            case DangerRules::WayEffect::Record:
            {
                // W2: before the end, so the next search and pick for this goal read the entry
                FailedAttempts::Failure failure;
                failure.goal = TravelLegs::FailurePoint(goal);
                failure.spot = TravelLegs::FailurePoint(
                    {bot->GetMapId(), bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ()});
                botAI->GetTravel().NoteFailure(DangerRules::RECORD_WORD, failure, tripId);
                break;
            }
            case DangerRules::WayEffect::Row:
                // The value is the goal's ground (0 quiet, 1 passable, 2 deadly)
                WorldLog::Decision(bot, WorldLogFormat::DecisionKind::Way, choice, goalWord,
                                   static_cast<float>(part.goal),
                                   {{"deadly_yd", part.deadlyYards},
                                    {"deadly_links", static_cast<float>(part.deadlyLinks)},
                                    {"fights", static_cast<float>(part.fights)}},
                                   tripId);
                break;
            case DangerRules::WayEffect::End:
                // Last: the end releases the trip
                RpgStatus::End(botAI, StatusEnd::Outcome::Abandoned,
                               {.source = DangerRules::END_SOURCE, .text = "no safe way"});
                return;
        }
}

bool HandleWayCommand(ChatHandler* handler, PlayerbotAI* botAI, std::string_view rest)
{
    std::vector<std::string_view> words = Words(rest);
    std::vector<std::string_view> expect;
    if (!words.empty() && words.back().substr(0, 7) == "expect=")
    {
        if (!ParseExpect(words.back().substr(7), expect, {"clear", "least", "routed", "safe", "nosafe"}))
        {
            handler->SendErrorMessage(WAY_USAGE);
            return false;
        }
        words.pop_back();
    }
    bool const four = words.size() == 4;
    Optional<std::uint32_t> const map = four ? Acore::StringTo<std::uint32_t>(words[0]) : std::nullopt;
    Optional<float> const gx = four ? Acore::StringTo<float>(words[1]) : std::nullopt;
    Optional<float> const gy = four ? Acore::StringTo<float>(words[2]) : std::nullopt;
    Optional<float> const gz = four ? Acore::StringTo<float>(words[3]) : std::nullopt;
    TravelGraph const& graph = TravelGraph::Instance();
    if (!map || !gx || !gy || !gz || !graph.IsReady())
    {
        handler->SendErrorMessage(graph.IsReady() ? WAY_USAGE : "AW way: the travel graph is not ready");
        return false;
    }
    Player* bot = botAI->GetBot();
    // The request the bot's trip would make now (NewRpgTravelAction::RequestRoute): its snapshot, its constraints and
    // the live entries of its record of failed attempts, with the stuck edges flagged now
    TravelRoute::Snapshot snapshot = TravelRoute::FillSnapshot(bot, 0);
    snapshot.hasHearthstone = snapshot.hasHearthstone && TestConstraints::HearthAllowed(botAI->testConstraints);
    snapshot.allowTransport = true;
    if (!TestConstraints::TaxiAllowed(botAI->testConstraints))
        snapshot.knownTaxi.fill(0);
    if (!TestConstraints::OwnFlyingAllowed(botAI->testConstraints))
        snapshot.flyOutland = snapshot.flyNorthrend = false;
    TravelRoute::Request request{snapshot, {*map, *gx, *gy, *gz}};
    request.refused = FailedAttempts::LiveEntries(botAI->GetTravel().Failures(), getMSTime());
    request.freeRides = TestConstraints::RidesFree(botAI->testConstraints);
    static TravelRoute::StuckEdges const noStuck{};
    std::shared_ptr<TravelRoute::StuckEdges const> const stuck = TravelRoute::StuckBook::Instance().Current();
    TravelRoute::StuckEdges const& edges = stuck ? *stuck : noStuck;
    auto const started = std::chrono::steady_clock::now();
    TravelRoute::Result const result = TravelRoute::Search(graph, request, edges, TravelRoute::ConfiguredParams());
    auto const micros =
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - started).count();

    TeamId const side = bot->GetTeamId();
    std::uint8_t const level = static_cast<std::uint8_t>(std::min<std::uint32_t>(bot->GetLevel(), 255));
    std::span<TravelRoute::Leg const> const legs =
        result.ok ? std::span<TravelRoute::Leg const>(result.legs) : std::span<TravelRoute::Leg const>();
    DangerRules::DangerPart const part = WayOf(legs, snapshot.position, request.goal, side, level);
    DangerRules::WayVerdict const own = DangerRules::JudgeWay(part, false);
    DangerRules::WayVerdict const order = DangerRules::JudgeWay(part, true);
    bool const refuses = DangerRules::Refuses(part);
    std::size_t walks = 0;
    DangerRules::Known const* const known = DangerRules::Published();
    for (std::size_t i = 0; i < legs.size(); ++i)
    {
        TravelRoute::Leg const& leg = legs[i];
        std::vector<std::uint32_t> const links = DangerRules::WalkLinksOf(graph, legs.subspan(i, 1));
        walks += links.size();
        std::string deadly;
        for (std::uint32_t const index : links)
        {
            DangerRules::LinkDanger const* const link = known ? known->LinkOf(index, SideIndex(side)) : nullptr;
            if (!link || !DangerRules::TermOf(*link, level).deadly)
                continue;
            TravelGraphLink const& graphLink = graph.GetLink(index);
            deadly += Acore::StringFormat("{}{} -> {} {} yd ({:.0f} deadly)", deadly.empty() ? ", deadly: " : "; ",
                                          graph.GetName(graphLink.from), graph.GetName(graphLink.to), link->yards,
                                          DangerRules::DeadlyYardsOfLink(*known, graph, index, SideIndex(side), level));
        }
        LOG_INFO("playerbots",
                 "AW way {} leg {}/{}: {} {:.0f} s map {} {:.0f} {:.0f} {:.0f} -> {:.0f} {:.0f} {:.0f}, {} nodes, {} "
                 "walk links{}",
                 bot->GetName(), i + 1, legs.size(), TravelRoute::LegTypeWord(leg.type), leg.seconds, leg.from.mapId,
                 leg.from.x, leg.from.y, leg.from.z, leg.to.x, leg.to.y, leg.to.z, leg.nodes.size(), links.size(),
                 deadly);
        if (leg.type != TravelRoute::LegType::Walk || leg.nodes.empty())
            continue;
        std::string points;
        for (std::uint32_t const node : leg.nodes)
        {
            TravelGraphNode const& at = graph.GetNode(node);
            points += Acore::StringFormat("{}{:.0f},{:.0f}", points.empty() ? "" : " ", at.x, at.y);
        }
        LOG_INFO("playerbots", "AW way {} leg {} points: {}", bot->GetName(), i + 1, points);
    }
    std::string const line = Acore::StringFormat(
        "AW way {}: {} level {} map {} {:.0f} {:.0f} {:.0f} -> map {} {:.0f} {:.0f} {:.0f}: {}, {} legs, {:.0f} s "
        "(penalty {:.0f} s), {} walk links | deadly {} links {:.0f} yd, fights {}, unquiet {}, start {} goal {} | "
        "safe {}, own {}, order {} | term {} | search {} us",
        bot->GetName(), SideWord(SideIndex(side)), level, snapshot.position.mapId, snapshot.position.x,
        snapshot.position.y, snapshot.position.z, *map, *gx, *gy, *gz,
        result.ok ? "ok" : TravelRoute::FailureWord(result.failure), result.legs.size(), result.totalSeconds,
        part.penaltySeconds, walks, part.deadlyLinks, part.deadlyYards, part.fights, part.unquietLinks,
        GroundWord(part.start), GroundWord(part.goal), refuses ? "no" : "yes", VerdictWord(own), VerdictWord(order),
        DangerRules::TermOn() ? "on" : "off", micros);
    LOG_INFO("playerbots", "{}", line);
    if (expect.empty())
    {
        handler->SendSysMessage(line);
        return true;
    }
    std::string missed;
    for (std::string_view const word : expect)
    {
        bool const holds = !result.ok         ? false
                           : word == "clear"  ? own == DangerRules::WayVerdict::Clear
                           : word == "least"  ? own == DangerRules::WayVerdict::LeastDeadly
                           : word == "safe"   ? !refuses
                           : word == "nosafe" ? refuses
                                              : true;  // routed
        if (!holds)
            missed += Acore::StringFormat("{}{}", missed.empty() ? "" : ",", word);
    }
    handler->SendSysMessage(
        missed.empty() ? Acore::StringFormat("FIXTURE check {}: ok, {}", bot->GetName(), line)
                       : Acore::StringFormat("FIXTURE check {}: gap, want {}; {}", bot->GetName(), missed, line));
    return true;
}

bool HandleWaysCommand(ChatHandler* handler, std::string_view rest)
{
    std::vector<std::string_view> const words = Words(rest);
    bool const all = words.size() == 1 && words[0] == "all";
    Optional<std::uint32_t> const level = words.size() == 2 ? Acore::StringTo<std::uint32_t>(words[1]) : std::nullopt;
    std::uint8_t const team = words.size() == 2 && (words[0] == "horde" || words[0] == "1") ? 1 : 0;
    bool const sideGiven = words.size() == 2 && (team == 1 || words[0] == "alliance" || words[0] == "0");
    if (!all && (!sideGiven || !level || *level < 1 || *level > static_cast<std::uint32_t>(WAYS_BAND_TOP)))
    {
        handler->SendErrorMessage(WAYS_USAGE);
        return false;
    }
    std::shared_ptr<Planner::PlannerIndexes const> const indexes = Planner::Indexes();
    if (!indexes || !TravelGraph::Instance().IsReady())
    {
        handler->SendErrorMessage("AW ways: the planner's indexes or the travel graph are not ready");
        return false;
    }
    std::string const before = TravelRoute::RouteServiceStatus();
    auto const started = std::chrono::steady_clock::now();
    std::vector<std::string> reply;
    std::size_t bands = 0;
    std::size_t cutBands = 0;
    std::size_t bandsWithoutOwn = 0;
    std::size_t searches = 0;
    std::vector<std::uint32_t> onMicros;
    std::vector<std::uint32_t> offMicros;
    auto const band = [&](std::uint8_t side, std::uint8_t at)
    {
        BandOutcome const outcome = WaysBand(*indexes, side, at);
        for (std::string const& line : outcome.lines)
            LOG_INFO("playerbots", "{}", line);
        std::size_t const shown = all ? 1 : std::min(outcome.lines.size(), WAYS_LISTED + 1);
        reply.insert(reply.end(), outcome.lines.begin(), outcome.lines.begin() + shown);
        ++bands;
        cutBands += outcome.ownCut ? 1 : 0;
        bandsWithoutOwn += outcome.ownCut == outcome.ownZones ? 1 : 0;
        searches += outcome.searches;
        onMicros.insert(onMicros.end(), outcome.onMicros.begin(), outcome.onMicros.end());
        offMicros.insert(offMicros.end(), outcome.offMicros.begin(), outcome.offMicros.end());
    };
    if (all)
    {
        for (std::uint8_t side = 0; side < 2; ++side)
            for (int at = WAYS_BAND_STEP; at <= WAYS_BAND_TOP; at += WAYS_BAND_STEP)
                band(side, static_cast<std::uint8_t>(at));
    }
    else
        band(team, static_cast<std::uint8_t>(*level));
    auto const waited =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
    PassTime const onTime = TimeOf(std::move(onMicros));
    PassTime const offTime = TimeOf(std::move(offMicros));
    std::string const total = Acore::StringFormat(
        "AW ways {}: {} bands, {} with a zone of their own bracket cut off, bands without a zone of their own bracket "
        "safe: {} | {} searches a pass, on mean {:.0f} p50 {} p95 {} max {} us, off mean {:.0f} p50 {} p95 {} max {} "
        "us | the world thread waited {} ms",
        all ? "all" : "band", bands, cutBands, bandsWithoutOwn, searches, onTime.mean, onTime.p50, onTime.p95,
        onTime.max, offTime.mean, offTime.p50, offTime.p95, offTime.max, waited);
    LOG_INFO("playerbots", "{}", total);
    LOG_INFO("playerbots", "AW ways route service before: {}", before);
    LOG_INFO("playerbots", "AW ways route service after: {}", TravelRoute::RouteServiceStatus());
    for (std::string const& line : reply)
        handler->SendSysMessage(line);
    handler->SendSysMessage(total);
    return true;
}

bool HandleTermCommand(ChatHandler* handler, std::string_view rest)
{
    std::vector<std::string_view> const words = Words(rest);
    if (words.size() != 1 || (words[0] != "off" && words[0] != "on"))
    {
        handler->SendErrorMessage(TERM_USAGE);
        return false;
    }
    if (!DangerRules::SetTerm(words[0] == "on"))
    {
        handler->SendErrorMessage("AW term: the fold has not published its store yet");
        return false;
    }
    // A lab's switch (§4.5, AW.25): a night's Server.log must not hold this line
    if (words[0] == "off")
        LOG_WARN("server.loading",
                 "danger term off: bots are routed by time alone, through any ground, until `aw term on` (a lab's "
                 "switch, never main or lab5)");
    DangerRules::Known const* const known = DangerRules::Published();
    std::string const line = Acore::StringFormat("AW term {}: store {} with {} link entries of a graph of {} links",
                                                 words[0], known ? known->build : "none",
                                                 known ? known->links.size() : 0, known ? known->graphLinks : 0);
    LOG_INFO("playerbots", "{}", line);
    handler->SendSysMessage(line);
    return true;
}
}  // namespace Awareness
