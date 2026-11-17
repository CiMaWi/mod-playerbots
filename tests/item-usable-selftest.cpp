/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// Item use plan WI15a: the pure parts of the bot's usable list (ItemUsableRules.h). The kind mask a trigger's gate
// reads, the health loss ring, the ready filter, and a seeded search that builds lists from the dump's records as the
// list does, refreshes ready by lane 1's CheapCheck and checks every offer of the free part against the same check:
// U1 the offer passes CheapCheck, U2 a cooling entry is never offered, U3 never the PvP trinket, U4 never a consumed
// entry. Run: item-usable-selftest <module root> [seed]

#include <array>
#include <cmath>
#include <cstdint>
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

std::vector<DumpRow> dump;

bool Chance(std::mt19937_64& rng, double p) { return std::uniform_real_distribution<double>(0.0, 1.0)(rng) < p; }

float Uniform(std::mt19937_64& rng, float lo, float hi) { return std::uniform_real_distribution<float>(lo, hi)(rng); }

float Dist(float ax, float ay, float az, float bx, float by, float bz)
{
    float const dx = ax - bx;
    float const dy = ay - by;
    float const dz = az - bz;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

UsableEntry Of(Kind kind, bool consumed = false)
{
    UsableEntry entry;
    entry.kind = kind;
    entry.consumed = consumed;
    entry.count = 1;
    entry.ready = true;
    return entry;
}

void KindMasks()
{
    std::vector<UsableEntry> const list = {Of(Kind::Boost), Of(Kind::Heal, true), Of(Kind::DamageOne)};
    KindMask const mask = MaskOf(list);
    Check(mask == (KindBit(Kind::Boost) | KindBit(Kind::Heal) | KindBit(Kind::DamageOne)), "the mask has the kinds");
    for (std::size_t n = 0; n < NEED_COUNT; ++n)
    {
        Need const need = static_cast<Need>(n);
        bool any = false;
        for (UsableEntry const& entry : list)
            any = any || entry.kind == NeedKind(need);
        Check(HasEntryFor(mask, need) == any, "the gate agrees with NeedKind for " + std::string(NeedWord(need)));
    }
    Check(MaskOf(std::vector<UsableEntry>{}) == 0, "an empty list has no kind");
    Check(KindsOf(NeedBit(Need::Speed)) == KindBit(Kind::Move), "speed reads move entries");
    Check(KindsOf(NeedBit(Need::Helper) | NeedBit(Need::Boost)) == (KindBit(Kind::Summon) | KindBit(Kind::Boost)),
          "the needs' kinds are their NeedKinds");
    std::vector<UsableEntry> const free = KeepFree(list);
    Check(free.size() == 2 && MaskOf(free) == (KindBit(Kind::Boost) | KindBit(Kind::DamageOne)),
          "the free part keeps the entries with consumed false");
}

void Ring()
{
    HealthRing ring;
    Check(ring.LossPerSecond() == 0.0f, "an empty ring loses nothing");
    ring.Push(1000, 1000);
    Check(ring.LossPerSecond() == 0.0f, "one sample loses nothing");
    ring.Push(2000, 900);
    ring.Push(3000, 800);
    Check(std::fabs(ring.LossPerSecond() - 100.0f) < 0.01f, "200 health in 2 s is 100/s");
    ring.Clear();
    ring.Push(0, 2000);
    ring.Push(6000, 500);
    ring.Push(7000, 400);
    Check(std::fabs(ring.LossPerSecond() - 100.0f) < 0.01f, "a sample older than the window is not read");
    ring.Clear();
    ring.Push(0, 500);
    ring.Push(1000, 800);
    Check(ring.LossPerSecond() == 0.0f, "a gain is no loss");
    ring.Clear();
    for (std::uint32_t i = 0; i < 20; ++i)
        ring.Push(i * 500, 10000 - i * 50);  // 100/s, past the ring's size
    Check(std::fabs(ring.LossPerSecond() - 100.0f) < 0.01f, "the ring wraps and keeps the rate");
}

void ReadyCauses()
{
    Check(ReadyCause(Cause::None), "no cause is ready");
    for (Cause cause : {Cause::NoEffect, Cause::NoItem, Cause::NotEquipped, Cause::Level, Cause::Skill, Cause::Class,
                        Cause::CantUse, Cause::NotReady, Cause::Potion, Cause::NoCharges, Cause::Busy, Cause::Dead,
                        Cause::NoControl, Cause::InFight, Cause::NotInFight, Cause::Arena, Cause::Place, Cause::Form})
        Check(!ReadyCause(cause), "not ready: " + std::string(CauseWord(cause)));
    for (Cause cause : {Cause::NoTarget, Cause::BadTarget, Cause::Full, Cause::Range, Cause::TooClose, Cause::Sight})
        Check(ReadyCause(cause), "a target cause leaves the entry ready: " + std::string(CauseWord(cause)));
}

// One drawn entry: its record and the bot's view of it, as UsableOf and the ready refresh see them
struct Drawn
{
    Record record;
    BotView view;
};

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
            v.friendly = true;
            v.healthFull = s.healthPct >= 100.0f;
            break;
        case TargetKind::Unit:
            for (std::size_t i = 0; i < s.unitCount; ++i)
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

Request RequestOf(Record const& record, Target const& target)
{
    Request request;
    request.item.entry = record.itemEntry;
    request.target = target;
    request.caller = Caller::Fight;
    request.spellId = record.spellId;
    return request;
}

Situation Draw(std::mt19937_64& rng)
{
    Situation s;
    s.level = 80;
    s.inFight = Chance(rng, 0.75);
    s.fightEnded = !s.inFight && Chance(rng, 0.3);
    s.healthPct = Uniform(rng, 5.0f, 100.0f);
    s.usesMana = Chance(rng, 0.6);
    s.manaPct = s.usesMana ? Uniform(rng, 0.0f, 100.0f) : 100.0f;
    s.maxHealth = 20000;
    s.healthLossPerSecond = Chance(rng, 0.5) ? Uniform(rng, 0.0f, 800.0f) : 0.0f;
    s.bursts = Chance(rng, 0.5);
    s.holdsBurst = s.bursts && Chance(rng, 0.2);
    s.inArena = Chance(rng, 0.1);
    s.potionDrunk = Chance(rng, 0.2);
    s.rooted = Chance(rng, 0.1);
    s.dispelMask = Chance(rng, 0.15) ? 2u : 0u;
    s.cornered = Chance(rng, 0.1);
    std::uint8_t const enemies = s.inFight ? static_cast<std::uint8_t>(1 + rng() % 5) : 0;
    for (std::uint8_t i = 0; i < enemies; ++i)
    {
        NearUnit& u = s.units[s.unitCount++];
        u.guid = 100 + i;
        u.x = Uniform(rng, -40.0f, 40.0f);
        u.y = Uniform(rng, -40.0f, 40.0f);
        u.level = 80;
        u.healthPct = Uniform(rng, 1.0f, 100.0f);
        u.health = 10000;
        u.inFight = true;
        u.onBot = Chance(rng, 0.6);
        u.controlled = Chance(rng, 0.1);
    }
    if (enemies)
        s.target = 0;
    std::uint8_t const friends = static_cast<std::uint8_t>(rng() % 3);
    for (std::uint8_t i = 0; i < friends; ++i)
    {
        NearUnit& u = s.units[s.unitCount++];
        u.guid = 200 + i;
        u.x = Uniform(rng, -30.0f, 30.0f);
        u.y = Uniform(rng, -30.0f, 30.0f);
        u.friendly = true;
        u.healthPct = Chance(rng, 0.15) ? 0.0f : Uniform(rng, 10.0f, 100.0f);
        u.inFight = s.inFight;
        if (u.healthPct <= 0.0f)
            ++s.groupDead;
    }
    s.groupSize = static_cast<std::uint8_t>(1 + friends);
    return s;
}

void Search(std::uint64_t seed)
{
    std::vector<std::size_t> pool;
    for (std::size_t i = 0; i < dump.size(); ++i)
    {
        Record const& r = dump[i].record;
        Kind const kind = Classify(r).kind;
        bool const read = kind == Kind::Heal || kind == Kind::Mana || kind == Kind::Protect || kind == Kind::Dispel ||
                          kind == Kind::Move || kind == Kind::Boost || kind == Kind::DamageOne ||
                          kind == Kind::DamageArea || kind == Kind::Control || kind == Kind::Summon ||
                          kind == Kind::Resurrect;
        if (read || PvpTrinketSpell(r.spellId))
            pool.push_back(i);
    }
    Check(pool.size() > 500, "the pool has records the offer reads");

    std::mt19937_64 rng(seed);
    std::uint32_t situations = 0, offers = 0, u1 = 0, u2 = 0, u3 = 0, u4 = 0, cooledListed = 0, pvpListed = 0,
                  keepWrong = 0;
    std::array<std::uint32_t, NEED_COUNT> perNeed{};
    for (int round = 0; round < 40000; ++round)
    {
        Situation const s = Draw(rng);
        ++situations;
        std::vector<UsableEntry> list;
        std::vector<Drawn> drawn;
        std::size_t const size = 1 + rng() % 10;
        for (std::size_t k = 0; k < size; ++k)
        {
            std::size_t const row = pool[rng() % pool.size()];
            Record const& record = dump[row].record;
            Classified const classified = Classify(record);
            if (!Listed(classified, false))
                continue;
            Drawn d;
            d.record = record;
            d.view.level = 80;
            d.view.classBit = 0xFFFFFFFFu;
            d.view.raceBit = 0xFFFFFFFFu;
            d.view.skillValue = record.needs.skillRank;
            d.view.held = Chance(rng, 0.05) ? 0 : static_cast<std::uint32_t>(1 + rng() % 5);
            d.view.equipped = true;
            d.view.cooldownLeftMs = Chance(rng, 0.25) ? 1 + static_cast<std::uint32_t>(rng() % 60000) : 0;
            d.view.chargesLeft = static_cast<std::int32_t>(d.view.held);
            d.view.potionDrunk = s.potionDrunk;
            d.view.inFight = s.inFight;
            d.view.inArena = s.inArena;
            d.view.outdoors = !record.needs.indoors;
            d.view.x = s.x;
            d.view.y = s.y;
            d.view.z = s.z;
            ItemRef const ref{record.itemEntry, 0, record.inventoryType ? std::uint8_t(13) : SLOT_BAGS};
            UsableEntry entry = EntryOf(record, classified, static_cast<std::uint32_t>(row), ref, d.view.held, false);
            // the ready refresh: CheapCheck on the bot itself, busy left to the action's Check
            Target self;
            self.kind = TargetKind::Self;
            entry.ready = entry.count > 0 &&
                          ReadyCause(CheapCheck(record, RequestOf(record, self), d.view, TargetViewOf(s, self)));
            if (d.view.cooldownLeftMs && entry.ready)
                ++cooledListed;
            if (entry.pvpTrinket)
                ++pvpListed;
            list.push_back(entry);
            drawn.push_back(d);
        }

        // the fight offer's list: the free part, its views kept beside it
        std::vector<UsableEntry> free;
        std::vector<Drawn> freeDrawn;
        for (std::size_t k = 0; k < list.size(); ++k)
            if (!list[k].consumed)
            {
                free.push_back(list[k]);
                freeDrawn.push_back(drawn[k]);
            }
        if (free.size() != KeepFree(list).size())
            ++keepWrong;

        Offer const o = MakeOffer(s, free);
        if (!o.any)
            continue;
        if (o.usable >= free.size())
        {
            ++u1;
            continue;
        }
        ++offers;
        ++perNeed[static_cast<std::size_t>(o.need)];
        UsableEntry const& e = free[o.usable];
        Drawn const& d = freeDrawn[o.usable];
        if (CheapCheck(d.record, RequestOf(d.record, o.target), d.view, TargetViewOf(s, o.target)) != Cause::None)
        {
            ++u1;
            if (u1 <= 5)
                std::cout << "U1: " << NeedWord(o.need) << " " << dump[e.record].name << " refused "
                          << CauseWord(
                                 CheapCheck(d.record, RequestOf(d.record, o.target), d.view, TargetViewOf(s, o.target)))
                          << "\n";
        }
        if (d.view.cooldownLeftMs)
            ++u2;
        if (e.pvpTrinket)
            ++u3;
        if (e.consumed || o.consumed)
            ++u4;
    }

    std::cout << "search: " << situations << " situations, " << offers << " offers; U1 " << u1 << " U2 " << u2 << " U3 "
              << u3 << " U4 " << u4 << "; listed cooling-ready " << cooledListed << ", pvp " << pvpListed << "\n";
    Check(offers > 1000, "the search offers");
    for (Need need : {Need::Heal, Need::Mana, Need::Protect, Need::Boost, Need::DamageOne, Need::DamageArea})
        Check(perNeed[static_cast<std::size_t>(need)] > 0, "the search offers " + std::string(NeedWord(need)));
    Check(keepWrong == 0, "KeepFree keeps the free entries");
    Check(pvpListed > 0, "the search lists the PvP trinket");
    Check(cooledListed == 0, "a cooling entry is never ready");
    Check(u1 == 0, "U1 the offer passes CheapCheck");
    Check(u2 == 0, "U2 a cooling entry is never offered");
    Check(u3 == 0, "U3 never the PvP trinket");
    Check(u4 == 0, "U4 the free list offers nothing consumed");
}
}  // namespace

int main(int argc, char** argv)
{
    std::string const root = argc > 1 ? argv[1] : ".";
    std::uint64_t const seed = argc > 2 ? std::stoull(argv[2]) : 20261117;

    KindMasks();
    Ring();
    ReadyCauses();

    std::ifstream in(root + "/tests/fixtures/item-use-dump.txt", std::ios::binary);
    Check(static_cast<bool>(in), "the dump opens");
    std::string line;
    std::getline(in, line);
    std::getline(in, line);
    while (std::getline(in, line))
        if (std::optional<DumpRow> row = ParseDumpLine(line))
            dump.push_back(*row);
    Check(dump.size() > 7000, "the dump loads");
    Search(seed);

    std::cout << passed << " passed, " << failed << " failed\n";
    return failed == 0 ? 0 : 1;
}
