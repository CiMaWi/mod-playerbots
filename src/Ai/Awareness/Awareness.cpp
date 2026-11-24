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
#include "Creature.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Group.h"
#include "Log.h"
#include "MotionMaster.h"
#include "ObjectAccessor.h"
#include "PerfMonitor.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "StringConvert.h"
#include "StringFormat.h"
#include "Timer.h"
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
        if (!bot->CanSeeOrDetect(unit))
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
        if (creature && !pet && seen.side == Side::Enemy && !creature->IsInEvadeMode() &&
            creature->HasReactState(REACT_AGGRESSIVE))
            // The engine's proximity aggro (Creature::CanStartAttack): the template's detection range, not the
            // fixed 20 of GetAttackDistance
            seen.reachYards = creature->GetAggroRange(bot) + creature->m_CombatDistance;

        if (creature && creature->IsInEvadeMode())
            seen.busy = Busy::Evading;
        else if (unit->IsInCombat())
            seen.busy = Busy::Fighting;
        else if (creature && creature->GetMotionMaster()->GetCurrentMovementGeneratorType() == WAYPOINT_MOTION_TYPE)
            seen.busy = Busy::OnPath;
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
            "AW unit {} {} | {} {} | {} | d{:+} {} | hp {:.0f}% | {:.1f} yd | reach {:.1f} | {} -> {} | {}", i,
            NameOf(bot, unit), AwarenessRules::Word(unit.side), AwarenessRules::Word(unit.kind),
            AwarenessRules::Word(unit.type), int(unit.levelDiff), AwarenessRules::Word(unit.rank), unit.healthPct,
            AwarenessRules::DistanceToDisc(unit, self.x, self.y), unit.reachYards, AwarenessRules::Word(unit.busy),
            Whom(unit), unit.inSight ? "sight" : "no sight"));
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
}  // namespace

Picture const& Look(PlayerbotAI* botAI)
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
    if (!store.built || !AwarenessRules::IsFresh(getMSTimeDiff(store.picture.builtMs, nowMs), bot->IsInCombat()))
        Fill(botAI, bot, store, nowMs);
    return store.picture;
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
}  // namespace Awareness
