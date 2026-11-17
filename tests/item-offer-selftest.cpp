/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// Item use plan WI14: the offer (PLAN 3.5) and the aim of an area item (3.7). Hand cases per need, a seeded search
// over random situations and lists that counts the invariants O1-O11, A1-A4 and G1-G4 (WI18, the gadgets: a rooting
// item never on a casting target, no helper below OVERWHELMED_DANGER, no resurrection while fighting, no repair with a
// vendor near or in a fight) (each must stay 0, the search must not
// be empty and must offer every need), and the printed choices for sample situations. O3 asks lane 1's real
// CheapCheck (ItemUseRules.h, since WI15a) over views built from the situation. Run: item-offer-selftest <module root>
// [seed] [--choices]

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <optional>
#include <random>
#include <string>
#include <vector>

#include "ItemOfferRules.h"
#include "ItemUsableRules.h"
#include "ItemUseFacts.h"
#include "ItemUseRules.h"

using namespace ItemUse;

namespace
{
int passed = 0;
int failed = 0;

void Check(bool ok, std::string const& what)
{
    if (ok)
        ++passed;
    else
    {
        ++failed;
        std::cout << "FAIL: " << what << "\n";
    }
}

constexpr std::uint32_t PVP_TRINKET = 42292;
constexpr std::uint32_t PVP_TRINKET_OTHER = 65547;

// One usable effect with its record (lane 1's CheapCheck reads the Record; fight and notInArena may be redrawn)
struct Item
{
    UsableEntry entry;
    Record record;
    FightNeed fight{FightNeed::Any};
    bool notInArena{false};
    bool cooling{false};
    std::string name;
};

float Dist(float ax, float ay, float az, float bx, float by, float bz)
{
    float const dx = ax - bx;
    float const dy = ay - by;
    float const dz = az - bz;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// ---- Lane 1's CheapCheck over views of the situation ----
// The record the offer saw: the item's, with the entry's target facts and the drawn fight and arena needs
Record RecordOf(Item const& item)
{
    Record record = item.record;
    UsableEntry const& e = item.entry;
    record.shape = e.shape;
    record.radiusYards = e.radiusYards;
    record.rangeYards = e.rangeYards;
    record.minRangeYards = e.minRangeYards;
    record.rangeFriendYards = e.rangeFriendYards;
    record.consumed = e.consumed;
    record.potion = e.potion;
    record.needs.fight = item.fight;
    record.needs.notInArena = item.notInArena;
    return record;
}

// The bot as the search draws it: it holds the item, may use it (level 80, every class and race, the skill), and a
// cooling item has its cooldown left
BotView BotViewOf(Item const& item, Record const& record, Situation const& s)
{
    BotView v;
    v.level = 80;
    v.classBit = 0xFFFFFFFFu;
    v.raceBit = 0xFFFFFFFFu;
    v.skillValue = record.needs.skillRank;
    v.held = item.entry.count;
    v.equipped = true;
    v.cooldownLeftMs = item.cooling ? 1000 : 0;
    v.chargesLeft = static_cast<std::int32_t>(item.entry.count);
    v.potionDrunk = s.potionDrunk;
    v.inFight = s.inFight;
    v.inArena = s.inArena;
    v.outdoors = !record.needs.indoors;
    v.x = s.x;
    v.y = s.y;
    v.z = s.z;
    return v;
}

TargetView TargetViewOf(Situation const& s, Target const& t)
{
    TargetView v;
    v.kind = t.kind;
    switch (t.kind)
    {
        case TargetKind::None:
        case TargetKind::Self:
            v.found = true;
            v.isBot = true;
            v.alive = s.healthPct > 0.0f;
            v.friendly = true;
            v.healthFull = s.healthPct >= 100.0f;
            break;
        case TargetKind::Unit:
            for (std::size_t i = 0; i < s.unitCount && i < MAX_NEAR; ++i)
                if (s.units[i].guid == t.guid)
                {
                    NearUnit const& u = s.units[i];
                    v.found = true;
                    v.alive = u.healthPct > 0.0f;
                    v.friendly = u.friendly;
                    v.hostile = !u.friendly;
                    v.healthFull = u.healthPct >= 100.0f;
                    v.yards = Dist(s.x, s.y, s.z, u.x, u.y, u.z);
                }
            break;
        case TargetKind::Spot:
            v.found = true;
            v.yards = Dist(s.x, s.y, s.z, t.x, t.y, t.z);
            break;
        default:
            break;
    }
    return v;
}

Cause RealCheck(Item const& item, Situation const& s, Target const& t)
{
    Record const record = RecordOf(item);
    Request request;
    request.item.entry = item.entry.item.entry;
    request.target = t;
    request.caller = Caller::Fight;
    request.spellId = record.spellId;
    return CheapCheck(record, request, BotViewOf(item, record, s), TargetViewOf(s, t));
}

// ---- Independent recomputation of the aim's facts (A1, A2, covers) ----
bool IndepSelfCentred(UsableEntry const& e)
{
    return e.radiusYards > 0.0f &&
           (e.shape == Shape::Self || e.shape == Shape::None || (e.shape == Shape::Ground && e.rangeYards <= 0.0f));
}

std::uint8_t IndepCovers(Situation const& s, float x, float y, float z, float radius)
{
    std::uint8_t n = 0;
    for (std::size_t i = 0; i < s.unitCount; ++i)
    {
        NearUnit const& u = s.units[i];
        if (!u.friendly && u.inFight && u.healthPct > 0.0f && !u.controlled && Dist(x, y, z, u.x, u.y, u.z) <= radius)
            ++n;
    }
    return n;
}

bool IndepForbidden(Situation const& s, float x, float y, float z, float radius, bool selfCentred)
{
    if (selfCentred && !(s.healthPct >= RECOIL_HEALTH_PCT))
        return true;
    for (std::size_t i = 0; i < s.unitCount; ++i)
    {
        NearUnit const& u = s.units[i];
        if (u.friendly || !(u.healthPct > 0.0f) || Dist(x, y, z, u.x, u.y, u.z) > radius)
            continue;
        if (!u.inFight || u.controlled)
            return true;
    }
    return false;
}

bool SameTarget(Target const& a, Target const& b)
{
    return a.kind == b.kind && a.guid == b.guid && a.x == b.x && a.y == b.y && a.z == b.z && a.dead == b.dead;
}

bool SameOffer(Offer const& a, Offer const& b)
{
    return a.any == b.any && a.need == b.need && a.usable == b.usable && SameTarget(a.target, b.target) &&
           a.consumed == b.consumed && a.verdict.spend == b.verdict.spend && a.verdict.reason == b.verdict.reason &&
           a.verdict.savesCopper == b.verdict.savesCopper && a.verdict.costCopper == b.verdict.costCopper &&
           a.verdict.danger == b.verdict.danger;
}

bool SameSpots(AimSpots const& a, AimSpots const& b)
{
    if (a.count != b.count)
        return false;
    for (std::size_t i = 0; i < a.count; ++i)
    {
        AimSpot const& p = a.spots[i];
        AimSpot const& q = b.spots[i];
        if (p.x != q.x || p.y != q.y || p.z != q.z || p.covers != q.covers || p.forbidden != q.forbidden ||
            p.inRange != q.inRange || p.inSight != q.inSight || p.unit != q.unit)
            return false;
    }
    return true;
}

// ---- Building entries ----
std::vector<DumpRow> dump;

bool IsPvpTrinket(Record const& r) { return r.spellId == PVP_TRINKET || r.spellId == PVP_TRINKET_OTHER; }

Item FromRecord(Record const& r, std::string const& name, std::uint32_t id)
{
    Classified const c = Classify(r);
    Item item;
    item.record = r;
    UsableEntry& e = item.entry;
    e.record = id;
    e.item.entry = r.itemEntry;
    e.kind = c.kind;
    e.sub = c.sub;
    e.rank = c.rank;
    e.count = 1;
    e.consumed = r.consumed;
    e.potion = r.potion;
    e.replaceCopper = r.replaceCopper;
    e.ready = true;
    e.shape = r.shape;
    e.radiusYards = r.radiusYards;
    e.rangeYards = r.rangeYards;
    e.minRangeYards = r.minRangeYards;
    e.rangeFriendYards = r.rangeFriendYards;
    e.pvpTrinket = IsPvpTrinket(r);
    e.channeled = r.channeled;
    e.stopsCast = c.kind == Kind::Control && StopsCast(r);
    item.fight = r.needs.fight;
    item.notInArena = r.needs.notInArena;
    item.name = name;
    return item;
}

std::optional<Item> Real(std::string const& name)
{
    for (std::size_t i = 0; i < dump.size(); ++i)
        if (dump[i].name == name)
            return FromRecord(dump[i].record, name, static_cast<std::uint32_t>(i));
    std::cout << "NOTE: no record named " << name << " in the dump\n";
    return std::nullopt;
}

std::optional<Item> RealOf(Kind kind, Sub sub, Shape shape)  // the first dump record of a kind, sub and shape
{
    for (std::size_t i = 0; i < dump.size(); ++i)
    {
        Classified const c = Classify(dump[i].record);
        if (c.kind == kind && c.sub == static_cast<std::uint8_t>(sub) && dump[i].record.shape == shape &&
            !dump[i].record.needs.notInArena && dump[i].record.needs.fight == FightNeed::Any)
            return FromRecord(dump[i].record, dump[i].name, static_cast<std::uint32_t>(i));
    }
    return std::nullopt;
}

Item Synth(std::string const& name, Kind kind, Sub sub, Shape shape, std::int32_t rank, bool consumed,
           std::uint32_t price, float range = 0.0f, float radius = 0.0f)
{
    Item item;
    UsableEntry& e = item.entry;
    e.kind = kind;
    e.sub = static_cast<std::uint8_t>(sub);
    e.shape = shape;
    e.rank = rank;
    e.consumed = consumed;
    e.replaceCopper = consumed ? price : 0;
    e.count = 1;
    e.ready = true;
    e.rangeYards = range;
    e.radiusYards = radius;
    e.rangeFriendYards = 30.0f;
    item.record.spellId = 1;  // a use effect; the rest of the record follows the entry (RecordOf)
    item.record.life = kind == Kind::Resurrect ? Life::Dead : Life::Alive;
    item.name = "synthetic " + name;
    return item;
}

// ---- Situations ----
NearUnit Enemy(std::uint64_t guid, float x, float y, bool onBot, std::uint8_t level = 60, Rank rank = Rank::Normal)
{
    NearUnit u;
    u.guid = guid;
    u.x = x;
    u.y = y;
    u.level = level;
    u.rank = rank;
    u.inFight = true;
    u.onBot = onBot;
    u.health = 4000;
    return u;
}

Situation Base()
{
    Situation s;
    s.level = 60;
    s.maxHealth = 4000;
    s.x = 100.0f;
    s.y = 100.0f;
    s.inFight = true;
    s.earnCopperPerMinute = 3000;  // 30 silver a minute, a level 60 at work
    s.repairCopper = 3000;
    return s;
}

void Add(Situation& s, NearUnit const& u)
{
    if (s.unitCount < MAX_NEAR)
        s.units[s.unitCount++] = u;
}

std::vector<UsableEntry> Entries(std::vector<Item> const& items)
{
    std::vector<UsableEntry> list;
    for (Item const& i : items)
        list.push_back(i.entry);
    return list;
}

Offer Run(Situation const& s, std::vector<Item> const& items)
{
    std::vector<UsableEntry> const list = Entries(items);
    return MakeOffer(s, std::span<UsableEntry const>(list.data(), list.size()));
}

std::string NeedList(NeedMask mask)
{
    std::string out;
    for (std::size_t n = 0; n < NEED_COUNT; ++n)
        if (mask & NeedBit(static_cast<Need>(n)))
            out += (out.empty() ? "" : ",") + std::string(NeedWord(static_cast<Need>(n)));
    return out.empty() ? "-" : out;
}

std::string Fmt(float v)
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.0f", static_cast<double>(v));
    return buf;
}

std::string Fmt2(float v)
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.2f", static_cast<double>(v));
    return buf;
}

// The printed choice: the situation, the wanted needs, every candidate weighed, the choice, and an aim's spots
bool printing = false;

void Print(std::string const& title, Situation const& s, std::vector<Item> const& items)
{
    if (!printing)
        return;
    float const danger = Danger(s);
    NeedMask const wanted = WantedNeeds(s);
    std::uint8_t onBot = 0;
    std::uint8_t enemies = 0;
    for (std::size_t i = 0; i < s.unitCount; ++i)
    {
        NearUnit const& u = s.units[i];
        if (!u.friendly && u.inFight && u.healthPct > 0.0f)
        {
            ++enemies;
            if (u.onBot)
                ++onBot;
        }
    }
    std::cout << "### " << title << "\n";
    std::cout << "situation: health " << Fmt(s.healthPct) << "% mana " << (s.usesMana ? Fmt(s.manaPct) + "%" : "-")
              << " fight " << (s.inFight ? (s.fightEnded ? "ended" : "yes") : "no") << " enemies " << int(enemies)
              << " on bot " << int(onBot) << " danger " << Fmt2(danger) << (s.bursts ? " bursts" : "")
              << (s.holdsBurst ? " holds_burst" : "") << (s.flees ? " flees" : "") << (s.chases ? " chases" : "")
              << (s.cornered ? " cornered" : "") << (s.inArena ? " arena" : "")
              << (s.stake == Stake::Player ? " stake_player" : "") << (s.potionDrunk ? " potion_drunk" : "") << " earn "
              << s.earnCopperPerMinute << " c/min\n";
    std::cout << "wanted: " << NeedList(wanted) << "\n";
    std::vector<UsableEntry> const list = Entries(items);
    std::span<UsableEntry const> const span(list.data(), list.size());
    for (std::size_t i = 0; i < items.size(); ++i)
    {
        UsableEntry const& e = items[i].entry;
        std::cout << "  [" << i << "] " << items[i].name << ": " << KindWord(e.kind) << "/" << SubWord(e.sub)
                  << " shape " << ShapeWord(e.shape) << " rank " << e.rank << (e.consumed ? " consumed" : " free")
                  << (e.potion ? " potion" : "") << " price " << e.replaceCopper << (e.ready ? "" : " not_ready")
                  << (e.pvpTrinket ? " pvp_trinket" : "");
        bool any = false;
        for (std::size_t n = 0; n < NEED_COUNT; ++n)
        {
            Need const need = static_cast<Need>(n);
            if (!(wanted & NeedBit(need)) || !EntryAnswers(e, need))
                continue;
            any = true;
            Target t;
            std::uint8_t hits = 1;
            bool const aimed = AnswerTarget(s, e, need, t, hits);
            std::cout << " | " << NeedWord(need) << (aimed ? "" : " no_target");
            if (aimed && e.consumed)
            {
                SpendVerdict const v = Spend(SpendCaseFor(s, e, danger, hits, false));
                std::cout << " " << (v.spend ? "spend:" : "keep:") << SpendReasonWord(v.reason) << " saves "
                          << Fmt(v.savesCopper) << " cost " << Fmt(v.costCopper);
            }
            if (aimed && need == Need::DamageArea)
                std::cout << " hits " << int(hits);
        }
        if (!any)
            std::cout << " | answers no wanted need";
        std::cout << "\n";
    }
    Offer const o = MakeOffer(s, span);
    if (o.any)
        std::cout << "choice: [" << int(o.usable) << "] " << items[o.usable].name << " for " << NeedWord(o.need)
                  << (o.consumed
                          ? std::string(" (consumed, spend:") + std::string(SpendReasonWord(o.verdict.reason)) + ")"
                          : std::string(" (free)"))
                  << "\n";
    else if (o.consumed)
        std::cout << "choice: none; " << SpendDecision(list[o.usable].kind, o.verdict) << " ("
                  << SpendReasonWord(o.verdict.reason) << ") for " << NeedWord(o.need) << "\n";
    else
        std::cout << "choice: none\n";
    for (std::size_t i = 0; i < items.size(); ++i)
    {
        UsableEntry const& e = items[i].entry;
        if (!(wanted & NeedBit(Need::DamageArea)) || !EntryAnswers(e, Need::DamageArea))
            continue;
        AimSpots const spots = AimCandidates(s, e);
        std::int8_t const pick = AimPick(spots);
        std::cout << "  spots of [" << i << "] (radius " << Fmt(e.radiusYards) << ", range " << Fmt(e.rangeYards)
                  << "): idx x y covers forbidden inRange picked\n";
        for (std::size_t k = 0; k < spots.count; ++k)
        {
            AimSpot const& p = spots.spots[k];
            std::cout << "    " << k << " " << Fmt2(p.x - s.x) << " " << Fmt2(p.y - s.y) << " " << int(p.covers) << " "
                      << (p.forbidden ? "yes" : "no") << " " << (p.inRange ? "yes" : "no") << " "
                      << (static_cast<std::int8_t>(k) == pick ? "PICK" : "") << "\n";
        }
    }
    std::cout << "\n";
}

// ---- Counters of the search ----
struct Counters
{
    std::uint64_t situations{0};
    std::uint64_t lists{0};
    std::uint64_t offers{0};
    std::uint64_t spotsPicked{0};
    std::uint64_t aims{0};
    std::array<std::uint64_t, NEED_COUNT> perNeed{};
    std::uint64_t consumedOffers{0};
    std::uint64_t potionOffers{0};
    std::uint64_t keeps{0};
    std::uint64_t o1{0}, o2{0}, o3{0}, o4{0}, o5{0}, o6{0}, o7{0}, o8{0}, a1{0}, a2{0}, coversWrong{0}, forbidWrong{0};
    std::uint64_t o9{0}, o10{0}, o11{0}, channelOffers{0}, arenaSituations{0};
    // WI17: A3 a pick out of sight or a lazy sight order that picks otherwise, A4 an area use that pulls a unit
    // outside the fight, worthWrong an area offer below AREA_MIN_HITS without a strong enemy covered
    std::uint64_t a3{0}, a4{0}, worthWrong{0}, sightQuestions{0}, areaOffers{0};
    // WI18: G1 root_caster, G2 helper_trivial, G3 res_in_fight, G4 repair_vendor; casterStops: control offers on a
    // casting target (the search must draw them)
    std::uint64_t g1{0}, g2{0}, g3{0}, g4{0}, casterStops{0};
};

void CheckAim(Situation const& s, UsableEntry const& e, AimSpots spots, std::mt19937_64& rng, Counters& c)
{
    bool const selfCentred = IndepSelfCentred(e);
    for (std::size_t k = 0; k < spots.count; ++k)
    {
        AimSpot const& p = spots.spots[k];
        if (p.covers != IndepCovers(s, p.x, p.y, p.z, e.radiusYards))
            ++c.coversWrong;
        if (p.forbidden != IndepForbidden(s, p.x, p.y, p.z, e.radiusYards, selfCentred))
            ++c.forbidWrong;
    }
    // the harness sets sight (WI17: through the situation's bits, as the sampler does), and the sampler's lazy order
    // must reach the same pick asking only what NextSightCheck asks
    std::uint32_t truth = 0;
    for (std::size_t k = 0; k < 32; ++k)
        if ((rng() % 5) != 0)
            truth |= std::uint32_t(1) << k;
    Situation sighted = s;
    sighted.aimSight = truth;
    spots = AimCandidates(sighted, e);
    ++c.aims;
    std::int8_t const pick = AimPick(spots);
    std::uint32_t known = 0;
    std::uint32_t seen = 0;
    for (std::int8_t k = NextSightCheck(spots, selfCentred, known, seen); k >= 0;
         k = NextSightCheck(spots, selfCentred, known, seen))
    {
        std::uint32_t const bit = std::uint32_t(1) << k;
        if ((known & bit) != 0)
        {
            ++c.a3;  // asked twice: the order would not end
            break;
        }
        known |= bit;
        seen |= truth & bit;
        ++c.sightQuestions;
    }
    Situation lazy = s;
    lazy.aimSight = seen;
    if (AimPick(AimCandidates(lazy, e)) != pick)
        ++c.a3;
    if (pick >= 0 && !selfCentred && (truth & (std::uint32_t(1) << pick)) == 0)
        ++c.a3;
    if (pick < 0)
    {
        for (std::size_t k = 0; k < spots.count; ++k)
        {
            AimSpot const& p = spots.spots[k];
            if (!p.forbidden && p.inRange && p.inSight && p.covers > 0)
                ++c.a2;
        }
        return;
    }
    ++c.spotsPicked;
    AimSpot const& chosen = spots.spots[static_cast<std::size_t>(pick)];
    if (IndepForbidden(s, chosen.x, chosen.y, chosen.z, e.radiusYards, selfCentred) || !chosen.inRange ||
        !chosen.inSight)
        ++c.a1;
    std::uint8_t const covers = IndepCovers(s, chosen.x, chosen.y, chosen.z, e.radiusYards);
    for (std::size_t k = 0; k < spots.count; ++k)
    {
        AimSpot const& p = spots.spots[k];
        if (p.forbidden || !p.inRange || !p.inSight)
            continue;
        if (IndepCovers(s, p.x, p.y, p.z, e.radiusYards) > covers)
            ++c.a2;
    }
}

void CheckOffer(Situation const& s, std::vector<Item> const& items, Counters& c)
{
    std::vector<UsableEntry> const list = Entries(items);
    std::span<UsableEntry const> const span(list.data(), list.size());
    Offer const o = MakeOffer(s, span);
    Situation const sCopy = s;
    std::vector<UsableEntry> const listCopy = list;
    Offer const again = MakeOffer(sCopy, std::span<UsableEntry const>(listCopy.data(), listCopy.size()));
    if (!SameOffer(o, again))
        ++c.o6;
    NeedMask const wanted = WantedNeeds(s);
    if (wanted != WantedNeeds(sCopy))
        ++c.o6;
    if (!o.any)
    {
        if (o.consumed)
            ++c.keeps;
        return;
    }
    ++c.offers;
    ++c.perNeed[static_cast<std::size_t>(o.need)];
    if (!(wanted & NeedBit(o.need)))
        ++c.o1;
    if (o.usable >= items.size())
    {
        ++c.o3;
        return;
    }
    Item const& item = items[o.usable];
    UsableEntry const& e = item.entry;
    if (e.consumed != o.consumed)
        ++c.o3;
    if (RealCheck(item, s, o.target) != Cause::None)
        ++c.o3;
    if (e.pvpTrinket)
        ++c.o5;
    if (e.potion && s.potionDrunk)
        ++c.o7;
    if (o.need == Need::Boost && s.holdsBurst)
        ++c.o8;
    // G1-G4 (WI18)
    if (o.need == Need::Control && o.target.kind == TargetKind::Unit)
        for (std::size_t i = 0; i < s.unitCount; ++i)
            if (s.units[i].guid == o.target.guid && s.units[i].casts)
            {
                ++c.casterStops;
                if (!e.stopsCast)
                    ++c.g1;
            }
    if (o.need == Need::Helper && Danger(s) < OVERWHELMED_DANGER)
        ++c.g2;
    if (o.need == Need::Resurrect && s.inFight && !s.fightEnded)
        ++c.g3;
    if (o.need == Need::Repair && (s.vendorNear || s.inFight))
        ++c.g4;
    // O9 (WI16): no consumed heal in an arena (the PvP tactics' potion gate)
    if (s.inArena && e.consumed && e.kind == Kind::Heal)
        ++c.o9;
    // O10 (WI16): a channel only while nothing hits the bot, no held foe stands in melee reach and it stands; a
    // channelled heal not while bandaged
    if (e.channeled)
    {
        ++c.channelOffers;
        bool hit = false;
        for (std::size_t i = 0; i < s.unitCount; ++i)
        {
            NearUnit const& u = s.units[i];
            bool const foe = !u.friendly && u.inFight && u.healthPct > 0.0f;
            float const dx = u.x - s.x;
            float const dy = u.y - s.y;
            float const dz = u.z - s.z;
            bool const reach = std::sqrt(dx * dx + dy * dy + dz * dz) <= 5.0f;
            hit = hit || (foe && u.onBot && !u.controlled) || (foe && u.controlled && reach);
        }
        if (hit || s.moving || (e.kind == Kind::Heal && s.recentlyBandaged))
            ++c.o10;
    }
    if (o.consumed)
    {
        ++c.consumedOffers;
        if (e.potion)
            ++c.potionOffers;
    }
    if (o.need == Need::DamageArea && o.target.kind == TargetKind::Spot &&
        IndepForbidden(s, o.target.x, o.target.y, o.target.z, e.radiusYards, IndepSelfCentred(e)))
        ++c.a1;
    // A4 (WI17): the area the use covers (the spot, the unit it is aimed at, or the bot) holds no unit outside the
    // fight; and it is worth it: AREA_MIN_HITS covered, or one strong enemy
    if (o.need == Need::DamageArea)
    {
        ++c.areaOffers;
        float x = s.x;
        float y = s.y;
        float z = s.z;
        if (o.target.kind == TargetKind::Spot)
        {
            x = o.target.x;
            y = o.target.y;
            z = o.target.z;
        }
        else if (o.target.kind == TargetKind::Unit)
            for (std::size_t i = 0; i < s.unitCount; ++i)
                if (s.units[i].guid == o.target.guid)
                {
                    x = s.units[i].x;
                    y = s.units[i].y;
                    z = s.units[i].z;
                }
        bool strong = false;
        for (std::size_t i = 0; i < s.unitCount; ++i)
        {
            NearUnit const& u = s.units[i];
            if (u.friendly || !(u.healthPct > 0.0f) || Dist(x, y, z, u.x, u.y, u.z) > e.radiusYards)
                continue;
            if (!u.inFight)
                ++c.a4;
            else if (!u.controlled && u.rank != Rank::Normal && u.rank != Rank::Player)
                strong = true;
        }
        if (!(IndepCovers(s, x, y, z, e.radiusYards) >= AREA_MIN_HITS || strong))
            ++c.worthWrong;
    }
    float const danger = Danger(s);
    for (std::size_t j = 0; j < items.size(); ++j)
    {
        if (j == o.usable)
            continue;
        UsableEntry const& other = items[j].entry;
        if (!EntryUsable(s, other) || !EntryAnswers(other, o.need))
            continue;
        Target t;
        std::uint8_t hits = 1;
        if (!AnswerTarget(s, other, o.need, t, hits))
            continue;
        // O2: a free ready answer of the same need beats every consumed one
        if (o.consumed && !other.consumed)
            ++c.o2;
        // O4: a stronger ready one of the same class that would be allowed (WI16: a consumed class is potion or not)
        if (other.consumed == e.consumed && (!e.consumed || other.potion == e.potion) && other.rank > e.rank &&
            (!other.consumed || Spend(SpendCaseFor(s, other, danger, hits, false)).spend))
            ++c.o4;
        // O11 (WI16, the chain): a potion while a non-potion consumed answer of the need would be spent
        if (o.consumed && e.potion && other.consumed && !other.potion &&
            Spend(SpendCaseFor(s, other, danger, hits, false)).spend)
            ++c.o11;
    }
}

// ---- The generator ----
float Uniform(std::mt19937_64& rng, float lo, float hi) { return std::uniform_real_distribution<float>(lo, hi)(rng); }

bool Chance(std::mt19937_64& rng, double p) { return std::bernoulli_distribution(p)(rng); }

Situation GenSituation(std::mt19937_64& rng)
{
    Situation s;
    s.level = static_cast<std::uint8_t>(10 + rng() % 71);
    s.role = static_cast<Role>(rng() % 5);
    s.healthPct = Chance(rng, 0.5) ? Uniform(rng, 1.0f, 100.0f) : Uniform(rng, 1.0f, 60.0f);
    s.usesMana = Chance(rng, 0.5);
    s.manaPct = Uniform(rng, 0.0f, 100.0f);
    s.maxHealth = static_cast<std::uint32_t>(500 + rng() % 8000);
    s.healthLossPerSecond = Chance(rng, 0.5) ? Uniform(rng, 0.0f, 300.0f) : 0.0f;
    s.x = Uniform(rng, -5000.0f, 5000.0f);
    s.y = Uniform(rng, -5000.0f, 5000.0f);
    s.z = Uniform(rng, -100.0f, 300.0f);
    s.inFight = Chance(rng, 0.75);
    s.fightEnded = Chance(rng, 0.15);
    s.bursts = Chance(rng, 0.3);
    s.holdsBurst = Chance(rng, 0.3);
    s.flees = Chance(rng, 0.1);
    s.chases = Chance(rng, 0.1);
    s.cornered = Chance(rng, 0.1);
    s.rooted = Chance(rng, 0.08);
    s.snared = Chance(rng, 0.08);
    s.inArena = Chance(rng, 0.1);
    s.stake = static_cast<Stake>(rng() % 3);
    s.groupSize = static_cast<std::uint8_t>(1 + rng() % 5);
    s.groupHurt = static_cast<std::uint8_t>(rng() % 3);
    s.dispelMask = Chance(rng, 0.15) ? static_cast<std::uint32_t>(1 + rng() % 31) : 0;
    s.gearBroken = Chance(rng, 0.15);
    s.vendorNear = Chance(rng, 0.3);
    s.potionDrunk = Chance(rng, 0.3);
    s.thrift = Uniform(rng, -1.0f, 1.0f);
    s.daring = Uniform(rng, -1.0f, 1.0f);
    s.earnCopperPerMinute = Chance(rng, 0.2) ? 1 : static_cast<std::uint32_t>(rng() % 3000);
    s.repairCopper = static_cast<std::uint32_t>(rng() % 6000);
    s.control = Chance(rng, 0.05);
    s.moving = Chance(rng, 0.2);
    s.recentlyBandaged = Chance(rng, 0.15);

    std::size_t const units = rng() % (MAX_NEAR + 1);
    std::size_t const clusters = 1 + rng() % 3;
    std::array<std::array<float, 3>, 3> centres{};
    for (std::size_t k = 0; k < clusters; ++k)
        centres[k] = {s.x + Uniform(rng, -35.0f, 35.0f), s.y + Uniform(rng, -35.0f, 35.0f),
                      s.z + Uniform(rng, -3.0f, 3.0f)};
    for (std::size_t i = 0; i < units; ++i)
    {
        NearUnit u;
        u.guid = 1000 + i;
        bool const spread = Chance(rng, 0.3);
        std::array<float, 3> const& c = centres[rng() % clusters];
        float const r = spread ? 30.0f : 4.0f;
        u.x = (spread ? s.x : c[0]) + Uniform(rng, -r, r);
        u.y = (spread ? s.y : c[1]) + Uniform(rng, -r, r);
        u.z = (spread ? s.z : c[2]) + Uniform(rng, -2.0f, 2.0f);
        u.level = static_cast<std::uint8_t>(std::clamp<int>(s.level + static_cast<int>(rng() % 9) - 4, 1, 83));
        u.rank = static_cast<Rank>(rng() % 6);
        u.friendly = Chance(rng, 0.2);
        u.inFight = u.friendly ? Chance(rng, 0.5) : Chance(rng, 0.8);
        u.onBot = !u.friendly && u.inFight && Chance(rng, 0.5);
        u.controlled = !u.friendly && Chance(rng, 0.12);
        u.healthPct = Chance(rng, 0.08) ? 0.0f : Uniform(rng, 1.0f, 100.0f);
        u.flees = Chance(rng, 0.05);
        u.casts = Chance(rng, 0.2);
        s.units[s.unitCount++] = u;
    }
    // WI17: bystanders inside the clusters (not in the fight, some of them controlled), where the aim must step
    // around them; and a partial sight of the candidate spots, as the sampler leaves it
    for (std::size_t k = 0; k < clusters && s.unitCount < MAX_NEAR; ++k)
    {
        if (!Chance(rng, 0.35))
            continue;
        NearUnit u;
        u.guid = 2000 + k;
        u.x = centres[k][0] + Uniform(rng, -3.0f, 3.0f);
        u.y = centres[k][1] + Uniform(rng, -3.0f, 3.0f);
        u.z = centres[k][2] + Uniform(rng, -1.0f, 1.0f);
        u.level = s.level;
        u.rank = static_cast<Rank>(rng() % 5);
        u.controlled = Chance(rng, 0.3);
        u.healthPct = Uniform(rng, 20.0f, 100.0f);
        s.units[s.unitCount++] = u;
    }
    if (Chance(rng, 0.3))
        s.aimSight = static_cast<std::uint32_t>(rng());
    if (s.unitCount > 0 && Chance(rng, 0.8))
        s.target = static_cast<std::int8_t>(rng() % s.unitCount);
    s.groupDead = 0;
    for (std::size_t i = 0; i < s.unitCount; ++i)
        if (s.units[i].friendly && !(s.units[i].healthPct > 0.0f))
            ++s.groupDead;
    if (Chance(rng, 0.05))
        ++s.groupDead;  // a dead member out of sight
    return s;
}

std::vector<std::size_t> realPool;  // dump rows worth drawing: a kind the offer reads, or the PvP trinket

void Readiness(Item& item, Situation const& s, std::mt19937_64& rng)
{
    item.cooling = Chance(rng, 0.15);
    bool const fightOk =
        !(item.fight == FightNeed::Fight && !s.inFight) && !(item.fight == FightNeed::Peace && s.inFight);
    bool const arenaOk = !(item.notInArena && s.inArena);
    // ready folds cooldown, the fight need and the arena; a drunk potion and an empty stack are left for the offer
    item.entry.ready = !item.cooling && fightOk && arenaOk;
    item.entry.count = Chance(rng, 0.08) ? 0 : static_cast<std::uint32_t>(1 + rng() % 20);
}

std::vector<Item> GenList(std::mt19937_64& rng, Situation const& s)
{
    std::vector<Item> items;
    std::size_t const n = rng() % 13;
    for (std::size_t i = 0; i < n; ++i)
    {
        Item item;
        if (!realPool.empty() && Chance(rng, 0.45))
        {
            std::size_t const row = realPool[rng() % realPool.size()];
            item = FromRecord(dump[row].record, dump[row].name, static_cast<std::uint32_t>(row));
        }
        else
        {
            Kind const kind = static_cast<Kind>(rng() % static_cast<std::size_t>(Kind::Count));
            Sub sub = Sub::None;
            if (kind == Kind::Move)
                sub = static_cast<Sub>(1 + rng() % 3);
            else if (kind == Kind::Summon)
                sub = std::array<Sub, 5>{Sub::Helper, Sub::Decoy, Sub::Repair, Sub::Object, Sub::None}[rng() % 5];
            else if (kind == Kind::Mana)
                sub = std::array<Sub, 4>{Sub::Mana, Sub::Mana, Sub::Rage, Sub::Energy}[rng() % 4];
            Shape const shape = static_cast<Shape>(rng() % static_cast<std::size_t>(Shape::Count));
            bool const consumed = Chance(rng, 0.6);
            float const range = Chance(rng, 0.3) ? 0.0f : Uniform(rng, 5.0f, 40.0f);
            float const radius = (kind == Kind::DamageArea || Chance(rng, 0.1)) ? Uniform(rng, 3.0f, 12.0f) : 0.0f;
            item = Synth(std::string(KindWord(kind)), kind, sub, shape, static_cast<std::int32_t>(rng() % 5000),
                         consumed, static_cast<std::uint32_t>(rng() % 30000), range, radius);
            item.entry.minRangeYards = Chance(rng, 0.1) ? Uniform(rng, 0.0f, 8.0f) : 0.0f;
            item.entry.potion = consumed && (kind == Kind::Heal || kind == Kind::Mana) && Chance(rng, 0.6);
            item.entry.channeled = !item.entry.potion && Chance(rng, kind == Kind::Heal ? 0.4 : 0.05);
            item.record.channeled = item.entry.channeled;
            item.entry.record = static_cast<std::uint32_t>(100000 + i);
            item.entry.stopsCast = kind == Kind::Control && Chance(rng, 0.5);
            item.fight = static_cast<FightNeed>(rng() % 3);
            item.notInArena = Chance(rng, 0.2);
            if (Chance(rng, 0.03))
                item.entry.pvpTrinket = true;
        }
        Readiness(item, s, rng);
        items.push_back(item);
    }
    // duplicates: a second entry of an existing kind with another rank and price
    if (!items.empty() && Chance(rng, 0.4))
    {
        Item copy = items[rng() % items.size()];
        copy.entry.rank += static_cast<std::int32_t>(rng() % 200) - 100;
        copy.entry.replaceCopper = copy.entry.consumed ? static_cast<std::uint32_t>(rng() % 30000) : 0;
        copy.name += " (dup)";
        Readiness(copy, s, rng);
        items.push_back(copy);
    }
    return items;
}

// ---- Hand cases ----
// WI18: the gadgets' hand cases with their real records
void GadgetCases(Item const& net)
{
    std::optional<Item> const tidal = Real("Tidal Charm");
    std::optional<Item> const sand = Real("Slumber Sand");
    std::optional<Item> const dummy = Real("Target Dummy");
    std::optional<Item> const dragonling = Real("Arcanite Dragonling");
    std::optional<Item> const cables = Real("Goblin Jumper Cables");
    std::optional<Item> const jeeves = Real("Jeeves");
    std::optional<Item> const bot74a = Real("Field Repair Bot 74A");
    Check(tidal && sand && dummy && dragonling && cables && jeeves && bot74a, "the gadget cases' real records exist");
    if (!tidal || !sand || !dummy || !dragonling || !cables || !jeeves || !bot74a)
        return;
    Check(!net.entry.stopsCast && tidal->entry.stopsCast && sand->entry.stopsCast,
          "the net roots (no stop of a cast), the charm and the sand stun");
    Check(dragonling->entry.sub == static_cast<std::uint8_t>(Sub::Helper) &&
              dummy->entry.sub == static_cast<std::uint8_t>(Sub::Decoy) &&
              jeeves->entry.sub == static_cast<std::uint8_t>(Sub::Repair) &&
              bot74a->entry.sub == static_cast<std::uint8_t>(Sub::Repair),
          "the gadgets' roles");

    // a level-80 bot (1000 c a minute) and one enemy that flees: the net saves the chase (RUNNER_MINUTES)
    Situation runner = Base();
    runner.level = 80;
    runner.earnCopperPerMinute = 1000;
    NearUnit runs = Enemy(1, 112.0f, 100.0f, false, 80);
    runs.flees = true;
    Add(runner, runs);
    runner.target = 0;
    std::vector<Item> items = {net};
    Offer o = Run(runner, items);
    Check(o.any && o.need == Need::Control && o.consumed && o.target.guid == 1 && o.verdict.spend &&
              o.verdict.reason == SpendReason::Time,
          "control: the net on a lone runner at level 80, spent for the chase (time)");
    Print("control: a lone runner, a level-80 bot with the net", runner, items);
    runner.earnCopperPerMinute = 100;  // a level-20 band's rate: the chase is worth less than the net
    o = Run(runner, items);
    Check(!o.any && o.consumed && o.verdict.reason == SpendReason::NotWorth,
          "control: the net on a lone runner at a low earning rate: kept (not worth)");
    // WI18b (NeedAt): a bursting bot with a boost trinket and the net: the net while the runner runs, the boost once
    // it stands; a caster keeps the enum order (lab12 r3-r4: a warrior's boost came first and the runner escaped)
    runner.earnCopperPerMinute = 1000;
    runner.bursts = true;
    Item const boost = Synth("damage trinket", Kind::Boost, Sub::None, Shape::Self, 100, false, 0);
    items = {boost, net};
    o = Run(runner, items);
    Check(o.any && o.need == Need::Control && o.usable == 1 && o.target.guid == 1,
          "control before boost: the net on the runner while it runs, the boost trinket ready");
    runner.units[0].flees = false;
    o = Run(runner, items);
    Check(o.any && o.need == Need::Boost && o.usable == 0, "control before boost: the runner stands: the boost");
    runner.units[0].casts = true;
    o = Run(runner, items);
    Check(o.any && o.need == Need::Boost && o.usable == 0, "control before boost: a caster keeps the enum order");
    runner.units[0].casts = false;
    runner.units[0].flees = true;
    runner.bursts = false;
    items = {net};
    Situation walker = runner;
    walker.units[0].flees = false;
    walker.units[0].onBot = true;
    Check((WantedNeeds(walker) & NeedBit(Need::Control)) == 0, "control: one ordinary enemy on the bot: none wanted");

    // one enemy that casts: a stun answers, a root never
    Situation caster = Base();
    NearUnit casts = Enemy(1, 110.0f, 100.0f, true);
    casts.casts = true;
    Add(caster, casts);
    caster.target = 0;
    Check((WantedNeeds(caster) & NeedBit(Need::Control)) != 0, "control: a casting enemy wants control");
    items = {net};
    o = Run(caster, items);
    Check(!o.any && !o.consumed, "control: the net never on a caster");
    items = {net, *sand};
    o = Run(caster, items);
    Check(o.any && o.need == Need::Control && o.usable == 1 && o.target.guid == 1, "control: the sand on a caster");
    Print("control: one caster, the net and the sand", caster, items);
    items = {net, *sand, *tidal};
    o = Run(caster, items);
    Check(o.any && o.usable == 2 && !o.consumed, "control: the free charm first");
    // two on the bot, one of them casting: the caster is the one held, whatever the target
    Situation two = Base();
    Add(two, Enemy(1, 104.0f, 100.0f, true));
    NearUnit castsToo = Enemy(2, 108.0f, 100.0f, true);
    castsToo.casts = true;
    Add(two, castsToo);
    two.target = 1;
    items = {*tidal};
    o = Run(two, items);
    Check(o.any && o.need == Need::Control && o.target.guid == 2, "control: the caster among two, even the target");

    // a helper in danger, none in a fight nearly won
    Situation bad = Base();
    bad.healthPct = 35.0f;
    bad.healthLossPerSecond = 400.0f;
    for (int i = 0; i < 4; ++i)
        Add(bad, Enemy(1 + i, 104.0f, 100.0f + i, true, 62, Rank::Elite));
    bad.target = 0;
    items = {*dragonling};
    o = Run(bad, items);
    Check(o.any && o.need == Need::Helper && !o.consumed, "helper: the dragonling when overwhelmed");
    Situation won = Base();
    for (int i = 0; i < 2; ++i)
    {
        NearUnit dying = Enemy(1 + i, 104.0f, 100.0f + i, true);
        dying.healthPct = 5.0f;
        Add(won, dying);
    }
    won.target = 0;
    o = Run(won, items);
    Check((WantedNeeds(won) & NeedBit(Need::Helper)) == 0 && !o.any, "helper: none in a fight nearly won");

    // a decoy when overwhelmed: the consumed dummy spent, at the nearest enemy's place within its range
    items = {*dummy};
    o = Run(bad, items);
    Check(o.any && o.need == Need::Decoy && o.consumed && o.verdict.spend && o.target.kind == TargetKind::Spot &&
              o.target.x == 104.0f && o.target.y == 100.0f,
          "decoy: the dummy at the nearest enemy's place");
    Print("decoy: four elites on the bot, the target dummy", bad, items);
    Situation far = bad;
    for (std::size_t i = 0; i < far.unitCount; ++i)
        far.units[i].x = 125.0f;  // beyond the dummy's range (10): the bot's own spot
    o = Run(far, items);
    Check(o.any && o.need == Need::Decoy && o.target.kind == TargetKind::Spot && o.target.x == far.x &&
              o.target.y == far.y,
          "decoy: the enemies beyond its range: at the bot's own spot");
    Situation corner = Base();
    NearUnit faint = Enemy(1, 104.0f, 100.0f, true, 50);
    faint.healthPct = 3.0f;
    Add(corner, faint);
    corner.target = 0;
    corner.cornered = true;
    o = Run(corner, items);
    Check(!o.any && o.consumed && !o.verdict.spend, "decoy: cornered by one nearly dead enemy: the dummy kept");

    // the cables on a dead group member in reach after the fight; not out of reach; not while fighting
    Situation after = Base();
    after.inFight = false;
    after.fightEnded = true;
    NearUnit corpse;
    corpse.guid = 77;
    corpse.x = 104.0f;
    corpse.y = 100.0f;
    corpse.friendly = true;
    corpse.healthPct = 0.0f;
    Add(after, corpse);
    after.groupDead = 1;
    items = {*cables};
    o = Run(after, items);
    Check(o.any && o.need == Need::Resurrect && o.target.guid == 77 && o.target.dead,
          "cables: a dead mate 4 yd off after the fight");
    Situation away = after;
    away.units[0].x = 107.0f;
    o = Run(away, items);
    Check(!o.any, "cables: the mate 7 yd off, beyond the reach: none (no walk to the corpse)");
    Situation during = after;
    during.inFight = true;
    during.fightEnded = false;
    Add(during, Enemy(5, 103.0f, 100.0f, true));
    o = Run(during, items);
    Check((WantedNeeds(during) & NeedBit(Need::Resurrect)) == 0 && o.need != Need::Resurrect,
          "cables: none while the fight lasts");

    // repair: Jeeves out of a fight with broken gear; the consumed 74A kept (out of a fight it saves no death)
    Situation broken = Base();
    broken.inFight = false;
    broken.gearBroken = true;
    items = {*jeeves};
    o = Run(broken, items);
    Check(o.any && o.need == Need::Repair && !o.consumed, "repair: Jeeves with broken gear and no vendor");
    items = {*bot74a};
    o = Run(broken, items);
    Check(!o.any && o.consumed && o.verdict.reason == SpendReason::Won, "repair: the 74A kept out of a fight (won)");
    broken.vendorNear = true;
    items = {*jeeves};
    o = Run(broken, items);
    Check(!o.any, "repair: Jeeves not with a vendor near");
}

void HandCases()
{
    std::optional<Item> const potion = Real("Major Healing Potion");
    std::optional<Item> const manaPotion = Real("Major Mana Potion");
    std::optional<Item> const grenade = Real("Thorium Grenade");
    std::optional<Item> const bomb = Real("Hi-Explosive Bomb");
    std::optional<Item> const sapper = Real("Goblin Sapper Charge");
    std::optional<Item> const net = Real("Netherweave Net");
    std::optional<Item> const xtreme = Real("Rocket Boots Xtreme");
    if (xtreme)
        std::cout << "NOTE: Rocket Boots Xtreme classify as " << KindWord(xtreme->entry.kind)
                  << " (WI12): the speed case takes the dump's first move/speed self record\n";
    std::optional<Item> const boots = RealOf(Kind::Move, Sub::Speed, Shape::Self);
    std::optional<Item> const cables = Real("Goblin Jumper Cables");
    std::optional<Item> const bandage = Real("Heavy Netherweave Bandage");
    Check(potion && manaPotion && grenade && bomb && sapper && net, "the hand cases' real records exist");
    if (!potion || !manaPotion || !grenade || !bomb || !sapper || !net)
        return;
    Check(potion->entry.kind == Kind::Heal && potion->entry.potion && potion->entry.consumed,
          "Major Healing Potion is a consumed heal potion");

    // 1. Heal
    {
        Situation s = Base();
        s.healthPct = 30.0f;
        s.healthLossPerSecond = 150.0f;
        for (int i = 0; i < 3; ++i)
            Add(s, Enemy(1 + i, 103.0f, 100.0f + i, true));
        s.target = 0;
        Item stone = Synth("healthstone-like (free)", Kind::Heal, Sub::None, Shape::Self, 1000, false, 0);
        std::vector<Item> items = {*potion, stone};
        Offer o = Run(s, items);
        Check(o.any && o.usable == 1 && !o.consumed && o.need == Need::Heal, "heal: the free one wins over the potion");
        Print("heal: three on the bot at 30%, a potion and a free healthstone-like", s, items);
        items = {*potion};
        o = Run(s, items);
        Check(o.any && o.consumed && o.verdict.spend && o.verdict.reason == SpendReason::Death,
              "heal: danger high spends the potion (death)");
        Print("heal: three on the bot at 30%, only the potion", s, items);
        Situation easy = Base();
        easy.healthPct = 30.0f;
        NearUnit weak = Enemy(1, 103.0f, 100.0f, true, 50);
        weak.healthPct = 3.0f;
        Add(easy, weak);
        easy.target = 0;
        o = Run(easy, items);
        Check(!o.any && o.consumed && !o.verdict.spend &&
                  (o.verdict.reason == SpendReason::NotWorth || o.verdict.reason == SpendReason::NoDanger),
              "heal: a weak nearly dead enemy: keep");
        Print("heal: a trivial fight at 30% (grey enemy at 3%)", easy, items);
    }
    // 1b. Heal a friend (WI15b): the bot whole, a hurt group member near
    {
        Situation s = Base();
        s.healthPct = 100.0f;
        Add(s, Enemy(1, 103.0f, 100.0f, false));
        NearUnit mate;
        mate.guid = 50;
        mate.x = 108.0f;
        mate.y = 100.0f;
        mate.level = 60;
        mate.friendly = true;
        mate.inFight = true;
        mate.healthPct = 35.0f;
        mate.health = 1400;
        Add(s, mate);
        NearUnit whole = mate;
        whole.guid = 51;
        whole.y = 103.0f;
        whole.healthPct = 50.0f;
        Add(s, whole);
        s.target = 0;
        Item crystals = Synth("friend heal (free)", Kind::Heal, Sub::None, Shape::Friend, 1000, false, 0);
        Item stone = Synth("self heal (free, stronger)", Kind::Heal, Sub::None, Shape::Self, 2000, false, 0);
        std::vector<Item> items = {stone, crystals};
        Offer o = Run(s, items);
        Check(o.any && o.need == Need::Heal && o.usable == 1 && o.target.kind == TargetKind::Unit &&
                  o.target.guid == 50 && !o.target.dead,
              "heal friend: a whole bot heals the most hurt friend with the friend-shaped item, not the self one");
        Print("heal friend: the bot whole, a mate at 35% and one at 50%", s, items);
        Situation far = s;
        far.units[1].x = 140.0f;
        far.units[2].x = 140.0f;
        o = Run(far, items);
        Check(!o.any, "heal friend: hurt friends beyond the item's friend reach get none");
        Situation self = s;
        self.healthPct = 40.0f;
        o = Run(self, items);
        Check(o.any && o.need == Need::Heal && o.usable == 0 && o.target.kind == TargetKind::Self,
              "heal friend: a hurt bot heals itself first");
        Situation fine = s;
        fine.units[1].healthPct = 90.0f;
        fine.units[2].healthPct = 90.0f;
        Check(!(WantedNeeds(fine) & NeedBit(Need::Heal)),
              "heal friend: no heal wanted when the bot and its friends are whole");
        NearUnit foe = mate;
        foe.guid = 52;
        foe.friendly = false;
        Situation hostile = fine;
        Add(hostile, foe);
        Check(!(WantedNeeds(hostile) & NeedBit(Need::Heal)), "heal friend: a hurt enemy wants no heal");
    }
    // 2. Mana
    {
        Situation s = Base();
        s.role = Role::Caster;
        s.usesMana = true;
        s.manaPct = 20.0f;
        s.healthLossPerSecond = 200.0f;
        s.healthPct = 50.0f;
        Add(s, Enemy(1, 110.0f, 100.0f, true, 61, Rank::Elite));
        Add(s, Enemy(2, 111.0f, 100.0f, true, 61, Rank::Elite));
        s.target = 0;
        std::vector<Item> items = {*manaPotion};
        Offer o = Run(s, items);
        Check(o.any && o.need == Need::Mana && o.consumed, "mana: the caster at 20% drinks the mana potion");
        Print("mana: a caster out of mana against two elites", s, items);
        s.potionDrunk = true;
        o = Run(s, items);
        // a drunk potion is not usable (ready folds it; EntryUsable too), so it is not weighed
        Check(!o.any && !o.verdict.spend, "mana: after this fight's potion, none");
        Print("mana: the same caster after this fight's potion", s, items);
        s.potionDrunk = false;
        s.healthPct = 25.0f;
        items = {*manaPotion, *potion};
        o = Run(s, items);
        float const danger = Danger(s);
        std::array<SpendCase, 2> cases = {SpendCaseFor(s, potion->entry, danger, 1, false),
                                          SpendCaseFor(s, manaPotion->entry, danger, 1, false)};
        std::size_t const best = BestPotionNeed(std::span<SpendCase const>(cases.data(), cases.size()));
        Check(o.any && o.consumed && o.usable < items.size() && items[o.usable].entry.potion,
              "mana and heal: one potion is offered");
        Check(best < 2 && o.usable == (best == 0 ? 1 : 0), "mana and heal: the potion where it saves most");
        Print("heal and mana both wanted, two potions: one potion", s, items);
    }
    // 3. Protect, Dispel, Speed, Boost
    {
        Situation s = Base();
        s.healthPct = 50.0f;
        s.healthLossPerSecond = 100.0f;
        Add(s, Enemy(1, 103.0f, 100.0f, true));
        s.target = 0;
        Item shield = Synth("protection trinket", Kind::Protect, Sub::None, Shape::Self, 900, false, 0);
        std::vector<Item> items = {shield};
        Offer o = Run(s, items);
        Check(o.any && o.need == Need::Protect, "protect: losing health at 50%: the protection");
        // WI16: a flight at full health from one foe asks for the protection too (plan 3.8)
        s.healthPct = 100.0f;
        s.healthLossPerSecond = 0.0f;
        o = Run(s, items);
        Check(!o.any, "protect: one foe at full health, no flight: none");
        s.flees = true;
        o = Run(s, items);
        Check(o.any && o.need == Need::Protect, "protect: a flight from one foe at full health: the protection");
        s.flees = false;
        s.healthPct = 50.0f;
        s.healthLossPerSecond = 100.0f;
        s.dispelMask = 8;
        Item cure = Synth("dispel item", Kind::Dispel, Sub::None, Shape::Self, 1, false, 0);
        items = {cure};
        o = Run(s, items);
        Check(o.any && o.need == Need::Dispel, "dispel: an effect to remove: the dispel item");
        s.dispelMask = 0;
        s.flees = true;
        Item mount = Synth("mount-like", Kind::Mount, Sub::None, Shape::Self, 100, false, 0);
        Item fall = Synth("slow fall", Kind::Move, Sub::Fall, Shape::Self, 100, false, 0);
        items = {mount, fall};
        if (boots)
            items.push_back(*boots);
        o = Run(s, items);
        Check(boots && o.any && o.need == Need::Speed && o.usable == 2,
              "speed: fleeing: the speed item, no mount or fall");
        Print("a flight begins: a speed item, a mount-like and a slow fall", s, items);
        s.flees = false;
        s.bursts = true;
        Item boost = Synth("damage trinket", Kind::Boost, Sub::None, Shape::Self, 100, false, 0);
        items = {boost};
        o = Run(s, items);
        Check(o.any && o.need == Need::Boost, "boost: bursting: the trinket");
        s.holdsBurst = true;
        s.stake = Stake::Player;
        s.units[0].rank = Rank::Player;
        o = Run(s, items);
        Check(!o.any, "boost: against a player while the tactics hold the burst: none");
        Print("a PvP burst held: the damage trinket waits", s, items);
    }
    // 4. DamageOne
    {
        Situation s = Base();
        Add(s, Enemy(1, 110.0f, 100.0f, true));
        s.target = 0;
        std::vector<Item> items = {*net};
        Offer o = Run(s, items);
        Check(!o.any || o.need != Need::DamageOne, "damage_one: a net is no damage");
        Item freeDmg = Synth("free damage", Kind::DamageOne, Sub::None, Shape::Enemy, 100, false, 0, 30.0f);
        Item usedDmg = Synth("consumed damage", Kind::DamageOne, Sub::None, Shape::Enemy, 500, true, 500, 30.0f);
        items = {usedDmg, freeDmg};
        o = Run(s, items);
        Check(o.any && o.usable == 1 && o.need == Need::DamageOne, "damage_one: free beats consumed");
        Item strong = Synth("strong free damage", Kind::DamageOne, Sub::None, Shape::Enemy, 900, false, 0, 30.0f);
        items = {freeDmg, strong};
        o = Run(s, items);
        Check(o.any && o.usable == 1, "damage_one: the stronger ready one");
        Print("a normal pull: two free damage items", s, items);
    }
    // 5. DamageArea
    {
        Situation s = Base();
        s.healthPct = 60.0f;
        s.healthLossPerSecond = 200.0f;
        for (int i = 0; i < 5; ++i)
            Add(s, Enemy(1 + i, 120.0f + static_cast<float>(i % 2), 100.0f + static_cast<float>(i) * 0.8f, i < 3));
        s.target = 0;
        std::vector<Item> items = {*grenade, *bomb};
        Offer o = Run(s, items);
        AimSpots spots = AimCandidates(s, grenade->entry);
        std::int8_t pick = AimPick(spots);
        Check(pick >= 0 && spots.spots[static_cast<std::size_t>(pick)].covers == 5, "area: the spot covers all five");
        Check(o.any && o.need == Need::DamageArea && o.target.kind == TargetKind::Spot,
              "area: a ground item at a spot");
        Print("a pack for the area item: five enemies in a cluster", s, items);

        Situation by = s;
        NearUnit stranger = Enemy(50, 121.0f, 103.0f, false);
        stranger.inFight = false;
        Add(by, stranger);
        spots = AimCandidates(by, grenade->entry);
        pick = AimPick(spots);
        Check(pick < 0 ||
                  !IndepForbidden(by, spots.spots[static_cast<std::size_t>(pick)].x,
                                  spots.spots[static_cast<std::size_t>(pick)].y,
                                  spots.spots[static_cast<std::size_t>(pick)].z, grenade->entry.radiusYards, false),
              "area: a bystander in the cluster: the pick moves or none");
        Print("area: a bystander inside the cluster", by, items);

        Situation cc = s;
        cc.units[2].controlled = true;
        spots = AimCandidates(cc, grenade->entry);
        bool allForbidden = true;
        for (std::size_t k = 0; k < spots.count; ++k)
            if (Dist(spots.spots[k].x, spots.spots[k].y, spots.spots[k].z, cc.units[2].x, cc.units[2].y,
                     cc.units[2].z) <= grenade->entry.radiusYards &&
                !spots.spots[k].forbidden)
                allForbidden = false;
        Check(allForbidden, "area: a spot holding a controlled enemy is forbidden");

        Situation two = Base();
        for (int i = 0; i < 2; ++i)
            Add(two, Enemy(1 + i, 120.0f, 80.0f + i, true));
        for (int i = 0; i < 4; ++i)
            Add(two, Enemy(10 + i, 115.0f, 120.0f + i, true));
        two.target = 0;
        spots = AimCandidates(two, grenade->entry);
        pick = AimPick(spots);
        Check(pick >= 0 && spots.spots[static_cast<std::size_t>(pick)].covers == 4, "area: two clusters: the larger");
        Print("area: two clusters, two and four", two, items);

        Situation far = Base();
        for (int i = 0; i < 4; ++i)
            Add(far, Enemy(1 + i, 180.0f, 100.0f + i, true));
        far.target = 0;
        spots = AimCandidates(far, grenade->entry);
        Check(AimPick(spots) < 0, "area: out of range: none");

        spots = AimCandidates(two, grenade->entry);
        pick = AimPick(spots);
        if (pick >= 0)
        {
            spots.spots[static_cast<std::size_t>(pick)].inSight = false;
            std::int8_t const second = AimPick(spots);
            Check(second >= 0 && second != pick && spots.spots[static_cast<std::size_t>(second)].covers <= 4,
                  "area: the best out of sight: the second best");
        }

        Situation near = Base();
        near.healthPct = 80.0f;
        for (int i = 0; i < 3; ++i)
            Add(near, Enemy(1 + i, 101.0f, 100.0f + i, true));
        near.target = 0;
        spots = AimCandidates(near, sapper->entry);
        Check(spots.count == 1 && spots.spots[0].x == near.x && spots.spots[0].covers == 3 && !spots.spots[0].forbidden,
              "area: the sapper's only candidate is the bot");
        items = {*sapper};
        Print("area: a bot-centred charge with three around the bot", near, items);
        near.healthPct = 20.0f;
        spots = AimCandidates(near, sapper->entry);
        Check(spots.count == 1 && spots.spots[0].forbidden,
              "area: below the recoil bound the bot-centred spot is forbidden");
        Offer charge = Run(near, items);
        Check(!(charge.any && charge.need == Need::DamageArea),
              "area: below the recoil bound the charge is not offered");
        near.healthPct = 80.0f;
        near.aimSight = 0;
        Target t;
        std::uint8_t hits = 0;
        Check(AnswerTarget(near, sapper->entry, Need::DamageArea, t, hits) && hits == 3 && t.kind == TargetKind::Self,
              "area: a bot-centred charge needs no sight of a spot");

        // WI17: no candidate in sight: nothing thrown; the lazy sight order asks the best spot first and stops there
        Situation blind = s;
        blind.aimSight = 0;
        items = {*grenade, *bomb};
        Check(!AnswerTarget(blind, grenade->entry, Need::DamageArea, t, hits), "area: no spot in sight: no aim");
        Offer const none = Run(blind, items);
        Check(!(none.any && none.need == Need::DamageArea), "area: no spot in sight: none");
        AimItem const thrown = AimItemOf(grenade->entry);
        spots = AimCandidates(s, thrown);
        std::int8_t const first = NextSightCheck(spots, false, 0, 0);
        Check(first >= 0 && first == AimPick(spots), "area: the first sight question is the best spot");
        std::uint32_t const firstBit = first >= 0 ? std::uint32_t(1) << first : 0;
        Check(NextSightCheck(spots, false, firstBit, firstBit) < 0, "area: the best spot in sight settles the order");
        std::int8_t const next = NextSightCheck(spots, false, firstBit, 0);
        Check(next >= 0 && next != first, "area: the best spot out of sight: the next best is asked");

        // WI17: one elite alone is worth an area item, one ordinary enemy or one player is not
        Situation elite = Base();
        Add(elite, Enemy(1, 110.0f, 100.0f, true, 60, Rank::Elite));
        elite.target = 0;
        Check((WantedNeeds(elite) & NeedBit(Need::DamageArea)) != 0, "area: one elite wants an area item");
        Check(AnswerTarget(elite, grenade->entry, Need::DamageArea, t, hits) && hits == 1 && t.kind == TargetKind::Spot,
              "area: one elite: the grenade at its spot, one hit");
        Print("area: one elite alone", elite, items);
        Situation single = elite;
        single.units[0].rank = Rank::Normal;
        Check((WantedNeeds(single) & NeedBit(Need::DamageArea)) == 0, "area: one ordinary enemy wants none");
        Check(!AnswerTarget(single, grenade->entry, Need::DamageArea, t, hits), "area: one ordinary enemy: no aim");
        single.units[0].rank = Rank::Player;
        Check((WantedNeeds(single) & NeedBit(Need::DamageArea)) == 0, "area: one player wants none");
        Situation guarded = elite;
        guarded.units[0].controlled = true;
        Check((WantedNeeds(guarded) & NeedBit(Need::DamageArea)) == 0, "area: one controlled elite wants none");
    }
    // 6. Control, Helper, Decoy, Resurrect, Repair
    {
        Situation s = Base();
        Add(s, Enemy(1, 105.0f, 100.0f, true));
        Add(s, Enemy(2, 106.0f, 100.0f, true));
        s.target = 0;
        std::vector<Item> items = {*net};
        Offer o = Run(s, items);
        Check(o.any && o.need == Need::Control && o.target.guid == 2, "control: the net on the one not targeted");
        Situation one = Base();
        Add(one, Enemy(1, 105.0f, 100.0f, true));
        one.units[0].controlled = true;
        one.target = 0;
        one.cornered = true;
        o = Run(one, items);
        Check(!o.any, "control: the target already controlled: no net");

        Situation bad = Base();
        bad.healthPct = 35.0f;
        bad.healthLossPerSecond = 400.0f;
        for (int i = 0; i < 4; ++i)
            Add(bad, Enemy(1 + i, 102.0f, 100.0f + i, true, 62, Rank::Elite));
        bad.target = 0;
        Item helper = Synth("helper (role synthetic)", Kind::Summon, Sub::Helper, Shape::None, 100, false, 0);
        Item decoy = Synth("decoy (role synthetic)", Kind::Summon, Sub::Decoy, Shape::Ground, 100, false, 0, 30.0f);
        items = {decoy, helper};
        o = Run(bad, items);
        Check(o.any && (o.need == Need::Helper || o.need == Need::Decoy) &&
                  (WantedNeeds(bad) & NeedBit(Need::Helper)) != 0,
              "helper: overwhelmed: a helper or decoy");
        Check(o.need == Need::Helper && o.usable == 1, "helper: the helper comes first");
        Print("a bad fight: four elites on the bot, helper and decoy", bad, items);

        Situation after = Base();
        after.inFight = false;
        after.fightEnded = true;
        NearUnit corpse;
        corpse.guid = 77;
        corpse.x = 105.0f;
        corpse.y = 100.0f;
        corpse.friendly = true;
        corpse.healthPct = 0.0f;
        Add(after, corpse);
        after.groupDead = 1;
        Item res = cables ? *cables : Synth("resurrect", Kind::Resurrect, Sub::None, Shape::Friend, 1, true, 1000);
        items = {res};
        o = Run(after, items);
        Check(o.any && o.need == Need::Resurrect && o.target.guid == 77 && o.target.dead,
              "resurrect: a dead group member after the fight");
        Print("a corpse in the group after the fight", after, items);

        Situation broken = Base();
        broken.inFight = false;
        broken.gearBroken = true;
        Item jeeves = Synth("repair bot (role synthetic)", Kind::Summon, Sub::Repair, Shape::None, 1, false, 0);
        items = {jeeves};
        o = Run(broken, items);
        Check(o.any && o.need == Need::Repair, "repair: broken gear, out of fight, no vendor");
        broken.vendorNear = true;
        o = Run(broken, items);
        Check(!o.any, "repair: a vendor near: none");
        broken.vendorNear = false;
        broken.inFight = true;
        Add(broken, Enemy(5, 104.0f, 100.0f, true));
        o = Run(broken, items);
        Check((WantedNeeds(broken) & NeedBit(Need::Repair)) == 0 && o.need != Need::Repair, "repair: in a fight: none");
    }
    // 6b. The gadgets (WI18): control by what it stops, the chase's time, the decoy's spot, cables and repair bots
    GadgetCases(*net);
    // 7. The PvP trinket, the arena
    {
        std::optional<std::size_t> trinketRow;
        for (std::size_t i = 0; i < dump.size() && !trinketRow; ++i)
            if (IsPvpTrinket(dump[i].record))
                trinketRow = i;
        Check(trinketRow.has_value(), "the dump holds a PvP trinket");
        if (trinketRow)
        {
            Item trinket = FromRecord(dump[*trinketRow].record, dump[*trinketRow].name, 1);
            Situation s = Base();
            s.dispelMask = 0xFF;
            s.rooted = true;
            s.flees = true;
            s.cornered = true;
            s.inArena = true;
            s.stake = Stake::Player;
            Add(s, Enemy(1, 105.0f, 100.0f, true));
            s.target = 0;
            std::vector<Item> items = {trinket};
            Offer o = Run(s, items);
            Check(!o.any, "the PvP trinket is never offered");
            Item arenaOnly = Synth("not in arena", Kind::Dispel, Sub::None, Shape::Self, 1, false, 0);
            arenaOnly.notInArena = true;
            arenaOnly.entry.ready = false;
            items = {trinket, arenaOnly};
            o = Run(s, items);
            Check(!o.any, "arena: a ready==false notInArena entry is not offered");
        }
    }
    // 9. WI16: the chain, the arena, the channel
    {
        std::optional<Item> const stoneReal = Real("Healthstone");
        std::optional<Item> const greater = Real("Greater Healing Potion");
        std::optional<Item> const lesserMana = Real("Lesser Mana Potion");
        Check(stoneReal && greater && lesserMana, "the WI16 hand cases' real records exist");
        if (stoneReal && greater && lesserMana)
        {
            Check(stoneReal->entry.consumed && !stoneReal->entry.potion && stoneReal->entry.kind == Kind::Heal,
                  "Healthstone is a consumed heal, not a potion");
            Situation s = Base();
            s.healthPct = 25.0f;
            s.healthLossPerSecond = 150.0f;
            for (int i = 0; i < 2; ++i)
                Add(s, Enemy(1 + i, 103.0f, 100.0f + i, true, 63));
            s.target = 0;
            std::vector<Item> items = {*greater, *stoneReal};
            Offer o = Run(s, items);
            Check(o.any && o.consumed && o.usable == 1,
                  "chain: the healthstone before the stronger potion (the potion is the fight's one)");
            Print("chain: 25% against two, a Greater Healing Potion and a Healthstone", s, items);
            items[1].entry.ready = false;  // the stone's cooldown
            o = Run(s, items);
            Check(o.any && o.consumed && o.usable == 0, "chain: the potion once the stone is cooling");
            s.potionDrunk = true;
            o = Run(s, items);
            Check(!o.any, "chain: no second potion in the fight");
            s.potionDrunk = false;
            items[1].entry.ready = true;

            Situation arena = s;
            arena.inArena = true;
            arena.stake = Stake::Player;
            arena.usesMana = true;
            arena.manaPct = 10.0f;
            o = Run(arena, items);
            Check(!o.any || o.need != Need::Heal, "arena: no consumed heal (the tactics' potion gate)");
            std::vector<Item> withMana = {*greater, *stoneReal, *lesserMana};
            o = Run(arena, withMana);
            Check(o.any && o.need == Need::Mana && o.usable == 2, "arena: the mana potion stays the offer's");
        }
        if (bandage)
        {
            Check(bandage->entry.channeled && bandage->entry.consumed && bandage->entry.kind == Kind::Heal,
                  "a bandage is a channelled consumed heal");
            Situation hit = Base();
            hit.healthPct = 40.0f;
            Add(hit, Enemy(1, 103.0f, 100.0f, true));
            hit.target = 0;
            std::vector<Item> items = {*bandage};
            Check(!Run(hit, items).any, "bandage: none while an enemy hits the bot");
            // the enemy is held; the bot lost health to it before (the spend rule needs a danger: WI16b's live case)
            Situation held = hit;
            held.units[0].controlled = true;
            held.healthPct = 25.0f;
            held.healthLossPerSecond = 300.0f;  // a bandage at 1.2 gold is worth it near death
            Check(!Run(held, items).any, "bandage: none while a held enemy stands in melee reach (a root still hits)");
            held.units[0].x = 112.0f;  // the bot stepped away from its rooted foe
            Offer o = Run(held, items);
            Check(o.any && o.need == Need::Heal && o.usable == 0 && o.target.kind == TargetKind::Self,
                  "bandage: the bot bandages itself while its enemy is held");
            held.moving = true;
            Check(!Run(held, items).any, "bandage: none while the bot moves");
            held.moving = false;
            held.recentlyBandaged = true;
            Check(!Run(held, items).any, "bandage: none while Recently Bandaged is on the bot");
        }
    }
    // 8. Empty list, unofferable kinds
    {
        Situation s = Base();
        s.healthPct = 20.0f;
        s.usesMana = true;
        s.manaPct = 5.0f;
        s.dispelMask = 1;
        s.flees = true;
        s.bursts = true;
        s.healthLossPerSecond = 300.0f;
        for (int i = 0; i < 4; ++i)
            Add(s, Enemy(1 + i, 103.0f, 100.0f + i, true));
        s.target = 0;
        std::vector<Item> none;
        Check(!Run(s, none).any, "an empty list: none");
        std::vector<Item> items = {Synth("unclassified", Kind::Unclassified, Sub::None, Shape::Self, 1, false, 0),
                                   Synth("mount", Kind::Mount, Sub::None, Shape::None, 1, false, 0),
                                   Synth("learn", Kind::Learn, Sub::Recipe, Shape::None, 1, true, 100)};
        Check(!Run(s, items).any, "unclassified, mount and learn are never offered");
    }
    // Sample choices: earning rate
    {
        Situation s = Base();
        s.healthPct = 40.0f;
        s.healthLossPerSecond = 80.0f;
        Add(s, Enemy(1, 103.0f, 100.0f, true, 61));
        s.target = 0;
        s.earnCopperPerMinute = 1;
        s.repairCopper = 0;
        std::vector<Item> items = {*potion};
        Print("a bot earning 1 copper a minute at 40% against one", s, items);
        s.earnCopperPerMinute = 1000000;
        Print("a bot earning 100 gold a minute at 40% against one", s, items);
        if (bandage)
        {
            Situation after = Base();
            after.inFight = false;
            after.fightEnded = true;
            after.healthPct = 50.0f;
            std::vector<Item> b = {*bandage, *potion};
            Print("after the fight at 50%: a bandage and a potion", after, b);
        }
    }
}
}  // namespace

int main(int argc, char** argv)
{
    std::string const root = argc > 1 ? argv[1] : ".";
    std::uint64_t seed = 20261117;
    for (int i = 2; i < argc; ++i)
    {
        std::string const arg = argv[i];
        if (arg == "--choices")
            printing = true;
        else
            seed = std::stoull(arg);
    }

    std::ifstream in(root + "/tests/fixtures/item-use-dump.txt", std::ios::binary);
    Check(static_cast<bool>(in), "the dump opens");
    std::string line;
    std::getline(in, line);
    std::getline(in, line);
    while (std::getline(in, line))
        if (std::optional<DumpRow> row = ParseDumpLine(line))
            dump.push_back(*row);
    Check(dump.size() > 7000, "the dump loads");
    for (std::size_t i = 0; i < dump.size(); ++i)
    {
        Kind const kind = Classify(dump[i].record).kind;
        bool wanted = IsPvpTrinket(dump[i].record);
        for (std::size_t n = 0; n < NEED_COUNT && !wanted; ++n)
            wanted = NeedKind(static_cast<Need>(n)) == kind;
        if (wanted || i % 50 == 0)
            realPool.push_back(i);
    }

    // Words
    Check(NeedWord(Need::Heal) == "heal" && NeedWord(Need::Repair) == "repair" &&
              NeedWord(Need::DamageArea) == "damage_area",
          "need words");
    Check(NeedKind(Need::Speed) == Kind::Move && NeedKind(Need::Helper) == Kind::Summon &&
              NeedKind(Need::Decoy) == Kind::Summon && NeedKind(Need::Repair) == Kind::Summon,
          "need kinds");

    HandCases();

    // The search
    std::cout << "search seed " << seed << "\n";
    std::mt19937_64 rng(seed);
    Counters c;
    constexpr std::size_t ROUNDS = 60000;
    for (std::size_t round = 0; round < ROUNDS; ++round)
    {
        Situation const s = GenSituation(rng);
        ++c.situations;
        if (s.inArena)
            ++c.arenaSituations;
        std::vector<Item> const items = GenList(rng, s);
        ++c.lists;
        CheckOffer(s, items, c);
        for (Item const& item : items)
        {
            UsableEntry const& e = item.entry;
            if (!(e.radiusYards > 0.0f))
                continue;
            AimSpots const spots = AimCandidates(s, e);
            if (!SameSpots(spots, AimCandidates(Situation(s), UsableEntry(e))))
                ++c.o6;
            CheckAim(s, e, spots, rng, c);
        }
        // a real area record through the Record overload
        if (!realPool.empty())
        {
            Record const& r = dump[realPool[rng() % realPool.size()]].record;
            if (r.radiusYards > 0.0f)
            {
                AimSpots const spots = AimCandidates(s, r);
                if (!SameSpots(spots, AimCandidates(s, r)))
                    ++c.o6;
                Item const item = FromRecord(r, "", 0);
                if (!SameSpots(spots, AimCandidates(s, item.entry)))
                    ++c.o6;
                CheckAim(s, item.entry, spots, rng, c);
            }
        }
    }
    std::cout << "search: " << c.situations << " situations, " << c.lists << " lists, " << c.offers << " offers ("
              << c.consumedOffers << " consumed, " << c.potionOffers << " potions), " << c.keeps << " keeps, " << c.aims
              << " aims, " << c.spotsPicked << " spots picked\n";
    std::cout << "offers per need:";
    for (std::size_t n = 0; n < NEED_COUNT; ++n)
        std::cout << " " << NeedWord(static_cast<Need>(n)) << "=" << c.perNeed[n];
    std::cout << "\n";
    std::cout << "violations: O1 no_need=" << c.o1 << " O2 consumed_over_free=" << c.o2 << " O3 refused_offer=" << c.o3
              << " O4 weaker=" << c.o4 << " O5 pvp_trinket=" << c.o5 << " O6 not_same=" << c.o6
              << " O7 potion_twice=" << c.o7 << " O8 held_burst=" << c.o8 << " A1 forbidden_spot=" << c.a1
              << " A2 fewer=" << c.a2 << " covers_wrong=" << c.coversWrong << " forbidden_wrong=" << c.forbidWrong
              << " O9 arena_heal=" << c.o9 << " O10 channel_held=" << c.o10 << " O11 potion_before_stone=" << c.o11
              << " (channel offers " << c.channelOffers << ")\n";
    std::cout << "aim: A3 sighted=" << c.a3 << " A4 new_pull=" << c.a4 << " area_worth=" << c.worthWrong
              << " (sight questions " << c.sightQuestions << ", area offers " << c.areaOffers << ")\n";
    Check(c.situations > 0 && c.offers > 0 && c.spotsPicked > 0 && c.consumedOffers > 0 && c.potionOffers > 0 &&
              c.keeps > 0,
          "the search is not empty");
    for (std::size_t n = 0; n < NEED_COUNT; ++n)
        Check(c.perNeed[n] > 0, "the search offers " + std::string(NeedWord(static_cast<Need>(n))));
    Check(c.o1 == 0, "O1 no_need");
    Check(c.o2 == 0, "O2 consumed_over_free");
    Check(c.o3 == 0, "O3 refused_offer");
    Check(c.o4 == 0, "O4 weaker");
    Check(c.o5 == 0, "O5 pvp_trinket");
    Check(c.o6 == 0, "O6 not_same");
    Check(c.o7 == 0, "O7 potion_twice");
    Check(c.o8 == 0, "O8 held_burst");
    Check(c.a1 == 0, "A1 forbidden_spot");
    Check(c.a2 == 0, "A2 fewer");
    Check(c.coversWrong == 0, "covers equal an independent count");
    Check(c.forbidWrong == 0, "forbidden equals an independent check");
    Check(c.channelOffers > 0 && c.arenaSituations > 0, "the search offers channels and draws arenas");
    Check(c.o9 == 0, "O9 arena_heal");
    Check(c.o10 == 0, "O10 channel_held");
    Check(c.o11 == 0, "O11 potion_before_stone");
    Check(c.a3 == 0, "A3 sighted");
    Check(c.a4 == 0, "A4 new_pull");
    Check(c.worthWrong == 0, "area offers are worth it");
    Check(c.sightQuestions > 0 && c.areaOffers > 0, "the search asks sight and offers area items");
    std::cout << "gadgets: G1 root_caster=" << c.g1 << " G2 helper_trivial=" << c.g2 << " G3 res_in_fight=" << c.g3
              << " G4 repair_vendor=" << c.g4 << " (control offers on a caster " << c.casterStops << ")\n";
    Check(c.casterStops > 0, "the search offers control on casters");
    Check(c.g1 == 0, "G1 root_caster");
    Check(c.g2 == 0, "G2 helper_trivial");
    Check(c.g3 == 0, "G3 res_in_fight");
    Check(c.g4 == 0, "G4 repair_vendor");

    std::cout << passed << " passed, " << failed << " failed\n";
    return failed == 0 ? 0 : 1;
}
