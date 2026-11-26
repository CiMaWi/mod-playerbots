/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_GATHERRATERULES_H
#define _PLAYERBOT_GATHERRATERULES_H

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

#include "GatherRateTypes.h"

// Minutes per take as bots get them (professions plan WPC13b, DECISIONS WPC.13 = B). Per zone and way (herbalism,
// mining, creatures) the minutes and takes of every circuit span that ran, one without a take too; a span cut at once
// (part 4's rule: at most CUT_MS with no stop resolved) and one of mixed ways are none. Observations fade, so a zone
// that was crowded recovers. A reader's estimate per take is (prior x PRIOR_TAKES + observed minutes) / (PRIOR_TAKES +
// observed takes), the prior its own static minutes per take; it reads "observed" once the faded takes reach
// PRIOR_TAKES. In memory per server run, std only: the map threads add spans to a Book under the live code's lock, the
// world thread fades it and publishes a Snapshot, the planner reads one snapshot per computation through a View. An
// empty View is the static rule.
namespace GatherRate
{
// Tuning values [E]
constexpr double PRIOR_TAKES = 10.0;              // the static prior's weight in takes, and the observed threshold
constexpr double FADE_HALF_LIFE_MINUTES = 120.0;  // an observation's weight halves in this time
constexpr double FORGOTTEN = 0.05;                // a tally faded below this in minutes and takes is dropped
constexpr std::uint32_t CUT_MS = 60000;           // a span this short with no stop resolved was cut at once
constexpr double MS_PER_MINUTE = 60000.0;
constexpr double DEFAULT_RATE_MINUTES = 1.0;  // the DefaultRate control's made-up rate

constexpr std::uint16_t SKILL_HERBALISM = 182;
constexpr std::uint16_t SKILL_MINING = 186;

// Negative controls of the selftest; None in the server
enum class Fault : std::uint8_t
{
    None,
    DefaultRate,      // a zone without observations reads a made-up rate
    TakesLeftOut,     // the observed takes are left out of the weight
    ZoneIgnored,      // one tally per way for every zone
    CutCounted,       // a span cut at once is observed
    TakelessSkipped,  // a span without a take is not observed
    NoFade            // observations never fade
};

// A circuit's way by its stops (part 4's rule, hub tools/lib/wlog-professions.js measureCtes): creatures when every
// stop is one, herbalism when every stop is a herb node, mining when none is a herb node or a creature
inline Way WayOf(std::uint32_t stops, std::uint32_t herbStops, std::uint32_t creatureStops)
{
    if (!stops)
        return Way::None;
    if (creatureStops)
        return creatureStops == stops ? Way::Creatures : Way::None;
    if (herbStops >= stops)
        return Way::Herbalism;
    return herbStops ? Way::None : Way::Mining;
}

// A node row's or a node stop's way by the skill that gathers it
inline Way WayOfSkill(std::uint16_t skill)
{
    return skill == SKILL_HERBALISM ? Way::Herbalism : skill == SKILL_MINING ? Way::Mining : Way::None;
}

inline bool CutAtOnce(Span const& span) { return span.milliseconds <= CUT_MS && !span.resolved; }

struct Tally
{
    double minutes{0.0};
    double takes{0.0};
};

struct Estimate
{
    double minutes{0.0};  // per take
    bool observed{false};
};

inline std::uint64_t KeyOf(std::uint32_t zone, Way way, Fault fault = Fault::None)
{
    std::uint64_t const place = fault == Fault::ZoneIgnored ? 0 : zone;
    return (place << 8) | static_cast<std::uint8_t>(way);
}

// A reader's estimate from a tally and its own prior; a prior not above 0 is kept
inline Estimate EstimateOf(Tally const& tally, double prior, Fault fault = Fault::None)
{
    if (!(prior > 0.0))
        return {prior, false};
    double const takes = fault == Fault::TakesLeftOut ? 0.0 : tally.takes;
    return {(prior * PRIOR_TAKES + tally.minutes) / (PRIOR_TAKES + takes), tally.takes >= PRIOR_TAKES};
}

using Tallies = std::unordered_map<std::uint64_t, Tally>;

struct Snapshot
{
    Tallies tallies;
    Fault fault{Fault::None};

    [[nodiscard]] Tally const* Find(std::uint32_t zone, Way way) const
    {
        auto const found = tallies.find(KeyOf(zone, way, fault));
        return found == tallies.end() ? nullptr : &found->second;
    }
};

// One estimate a reader asked for and its answer (the skill path's reuse, professions WPC14b)
struct Read
{
    std::uint32_t zone{0};
    Way way{Way::None};
    double prior{0.0};
    Estimate answer;
};

// What the planner reads: one published snapshot; null is the static rule
struct View
{
    std::shared_ptr<Snapshot const> snapshot;
    std::vector<Read>* reads{nullptr};  // every estimate asked is added here; null: none kept

    [[nodiscard]] Estimate Of(std::uint32_t zone, Way way, double prior) const
    {
        Estimate const answer = Answer(zone, way, prior);
        if (reads)
            reads->push_back({zone, way, prior, answer});
        return answer;
    }

    // The estimate, not added to the reads
    [[nodiscard]] Estimate Answer(std::uint32_t zone, Way way, double prior) const
    {
        if (!snapshot || way == Way::None || !zone)
            return {prior, false};
        if (Tally const* tally = snapshot->Find(zone, way))
            return EstimateOf(*tally, prior, snapshot->fault);
        if (snapshot->fault == Fault::DefaultRate)
            return EstimateOf({DEFAULT_RATE_MINUTES * PRIOR_TAKES, PRIOR_TAKES}, prior);
        return {prior, false};
    }

    // The estimate over the prior: what a reader's static minutes are multiplied by
    [[nodiscard]] double Factor(std::uint32_t zone, Way way, double prior) const
    {
        return prior > 0.0 ? Of(zone, way, prior).minutes / prior : 1.0;
    }
};

// The tally the map threads add spans to and the world thread fades and publishes
struct Book
{
    Tallies tallies;
    Fault fault{Fault::None};

    // False: the span is no observation (mixed ways, no zone, cut at once)
    bool Observe(Span const& span)
    {
        if (span.way == Way::None || !span.zone || (CutAtOnce(span) && fault != Fault::CutCounted) ||
            (!span.takes && fault == Fault::TakelessSkipped))
            return false;
        Tally& tally = tallies[KeyOf(span.zone, span.way, fault)];
        tally.minutes += span.milliseconds / MS_PER_MINUTE;
        tally.takes += span.takes;
        return true;
    }

    // The minutes since the last fade: every tally keeps half its weight per FADE_HALF_LIFE_MINUTES
    void Fade(double minutes)
    {
        if (!(minutes > 0.0) || fault == Fault::NoFade)
            return;
        double const keep = std::exp2(-minutes / FADE_HALF_LIFE_MINUTES);
        for (auto at = tallies.begin(); at != tallies.end();)
        {
            at->second.minutes *= keep;
            at->second.takes *= keep;
            if (at->second.minutes < FORGOTTEN && at->second.takes < FORGOTTEN)
                at = tallies.erase(at);
            else
                ++at;
        }
    }

    [[nodiscard]] std::shared_ptr<Snapshot const> Publish() const
    {
        auto snapshot = std::make_shared<Snapshot>();
        snapshot->tallies = tallies;
        snapshot->fault = fault;
        return snapshot;
    }
};
}  // namespace GatherRate

#endif
