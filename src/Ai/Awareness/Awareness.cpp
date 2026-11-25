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
#include <memory>
#include <string>
#include <vector>

#include "CellImpl.h"
#include "Chat.h"
#include "ChatHelper.h"
#include "Corpse.h"
#include "Creature.h"
#include "EventLog.h"
#include "EventLogFormat.h"
#include "EventWanted.h"
#include "GhostWait.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Group.h"
#include "Log.h"
#include "Map.h"
#include "MotionMaster.h"
#include "MoveSpline.h"
#include "ObjectAccessor.h"
#include "PathGenerator.h"
#include "PerfMonitor.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "StringConvert.h"
#include "StringFormat.h"
#include "TestConstraints.h"
#include "Timer.h"
#include "UpkeepRules.h"
#include "Util.h"
#include "World.h"

namespace Awareness
{
using AwarenessRules::Busy;
using AwarenessRules::Kind;
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

        if (creature && creature->IsInEvadeMode())
            seen.busy = Busy::Evading;
        else if (unit->IsInCombat())
            seen.busy = Busy::Fighting;
        else if (creature && creature->GetMotionMaster()->GetCurrentMovementGeneratorType() == WAYPOINT_MOTION_TYPE)
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
        "{} reaching {} nearest {} clearance {:.1f} top d{:+} {} {} {}", answer.clear ? "clear" : "reached",
        answer.reaching, name, answer.clearance, int(answer.topLevelDiff), AwarenessRules::Word(answer.topRank),
        answer.inView ? "inview" : "outview", AwarenessRules::Takeable(answer) ? "takeable" : "untakeable");
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
    "[expect=<clear|reached|inview|outview|takeable|untakeable,..>]";
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

bool HandleJudgeCommand(ChatHandler* handler, PlayerbotAI* botAI, std::string_view rest)
{
    std::vector<std::string_view> words = Words(rest);
    std::vector<std::string_view> expect;
    bool expectOk = true;
    if (!words.empty() && words.back().substr(0, 7) == "expect=")
    {
        expectOk = ParseExpect(words.back().substr(7), expect,
                               {"clear", "reached", "inview", "outview", "takeable", "untakeable"});
        words.pop_back();
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
    std::vector<std::string_view> const got = {answer.clear ? "clear" : "reached", answer.inView ? "inview" : "outview",
                                               AwarenessRules::Takeable(answer) ? "takeable" : "untakeable"};
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
    store.fightEnemies[store.fightEnemyCount++] = now.guid;
    AwarenessRules::EnemyRow row;
    row.tenths = getMSTimeDiff(store.fightStartMs, nowMs) / 100;
    row.levelDiff = now.levelDiff;
    row.rank = now.rank;
    row.casts = now.casts;
    row.creatureType = now.creatureType;
    row.healthPct = Rounded(now.healthPct);
    row.type = now.type;
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
    store.notedTarget = targetGuid;
    store.notedCaller = AwarenessRules::CallerOf(action);
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

    facts.corpseYards = bot->GetExactDist2d(corpse);
    facts.reclaimHere = corpse->IsWithinDistInMap(bot, CORPSE_RECLAIM_RADIUS, true);
    if (!facts.ownedByPlayer)
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

DeadMemo const& NextOfDeadAtGrave(PlayerbotAI* botAI, GraveFacts const& grave)
{
    DeadMemo& memo = OwnStore(botAI).dead;
    memo.facts.graveKnown = grave.known;
    memo.facts.atGrave = grave.atGrave;
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
            AwarenessRules::RiseRow row;
            row.ghostSeconds = memo.lastMs ? memo.facts.ghostSeconds : 0;
            row.corpseYards = memo.lastMs && memo.facts.corpseHere
                                  ? std::min(Rounded(memo.facts.corpseYards), AwarenessRules::YARDS_NONE)
                                  : AwarenessRules::YARDS_NONE;
            row.deathsInRow = std::max<std::uint32_t>(1, store.row);
            row.reaching = JudgePoint(botAI, *bot).reaching;
            EventLog::Emit(bot, EventLog::EventType::Rise,
                           AwarenessRules::RiseWord(memo.last, ordered, how == EventLog::RESURRECT_CORPSE), 0,
                           static_cast<int64>(AwarenessRules::RiseAmount(row)));
        }
        memo = DeadMemo{};
    }
    store.row = AwarenessRules::RowAtRise(store.row);
    store.died = 0;
    store.rose = true;
    store.roseMs = getMSTime();
    store.roseMap = bot->GetMapId();
    store.roseX = bot->GetPositionX();
    store.roseY = bot->GetPositionY();
}

void OnDeath(Player* bot, Unit* killer)
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
    EventLog::Emit(bot, EventLog::EventType::DeathScene, AwarenessRules::Word(helper),
                   killer && killer != bot ? killer->GetEntry() : 0,
                   static_cast<int64>(AwarenessRules::DeathAmount(row)));
}
}  // namespace Awareness
