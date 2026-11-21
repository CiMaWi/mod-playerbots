/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_PROFESSIONRULES_H
#define _PLAYERBOT_PROFESSIONRULES_H

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#include "TraitRules.h"

// The Player-free rules of a bot's professions (professions plan WR2; WR.2, WR.3, WR.7): the main pairs open to a
// class, the person rolled once from guid and version, the secondary professions, and the person a bot's held skills
// make. Header-only so tests/profession-rules-selftest.cpp checks the same code the server runs.
namespace ProfessionRules
{
// SkillLine ids, the core's SkillType values
enum class Skill : uint16_t
{
    None = 0,
    FirstAid = 129,
    Blacksmithing = 164,
    Leatherworking = 165,
    Alchemy = 171,
    Herbalism = 182,
    Cooking = 185,
    Mining = 186,
    Tailoring = 197,
    Engineering = 202,
    Enchanting = 333,
    Fishing = 356,
    Skinning = 393,
    Jewelcrafting = 755,
    Inscription = 773
};

enum class Kind : uint8_t
{
    Gatherer,
    Craft,
    Secondary
};

struct ProfessionInfo
{
    Skill Which;
    std::string_view Name;  // the GM command's word
    Kind Type;
};

constexpr ProfessionInfo PROFESSIONS[] = {
    {Skill::Mining, "mining", Kind::Gatherer},
    {Skill::Herbalism, "herbalism", Kind::Gatherer},
    {Skill::Skinning, "skinning", Kind::Gatherer},
    {Skill::Blacksmithing, "blacksmithing", Kind::Craft},
    {Skill::Leatherworking, "leatherworking", Kind::Craft},
    {Skill::Tailoring, "tailoring", Kind::Craft},
    {Skill::Engineering, "engineering", Kind::Craft},
    {Skill::Jewelcrafting, "jewelcrafting", Kind::Craft},
    {Skill::Alchemy, "alchemy", Kind::Craft},
    {Skill::Inscription, "inscription", Kind::Craft},
    {Skill::Enchanting, "enchanting", Kind::Craft},
    {Skill::FirstAid, "firstaid", Kind::Secondary},
    {Skill::Fishing, "fishing", Kind::Secondary},
    {Skill::Cooking, "cooking", Kind::Secondary},
};
constexpr std::size_t PROFESSION_COUNT = 14;
static_assert(std::size(PROFESSIONS) == PROFESSION_COUNT, "one row per profession");

// Class ids
constexpr uint8_t WARRIOR = 1;
constexpr uint8_t PALADIN = 2;
constexpr uint8_t HUNTER = 3;
constexpr uint8_t ROGUE = 4;
constexpr uint8_t PRIEST = 5;
constexpr uint8_t DEATH_KNIGHT = 6;
constexpr uint8_t SHAMAN = 7;
constexpr uint8_t MAGE = 8;
constexpr uint8_t WARLOCK = 9;
constexpr uint8_t DRUID = 11;

constexpr uint32_t ClassBit(uint8_t cls) { return cls >= 1 && cls <= 32 ? 1u << (cls - 1) : 0u; }

constexpr uint32_t ALL_CLASSES = ClassBit(WARRIOR) | ClassBit(PALADIN) | ClassBit(HUNTER) | ClassBit(ROGUE) |
                                 ClassBit(PRIEST) | ClassBit(DEATH_KNIGHT) | ClassBit(SHAMAN) | ClassBit(MAGE) |
                                 ClassBit(WARLOCK) | ClassBit(DRUID);

// A craft, the classes it serves and the gatherer that feeds it (WR.2): the armour crafts follow what the class wears
// as it levels; the gatherer None marks tailoring and enchanting, which pair with each other or with any gatherer
struct Craft
{
    Skill Which;
    uint32_t Classes;
    Skill Gatherer;
};

constexpr Craft CRAFTS[] = {
    {Skill::Blacksmithing, ClassBit(WARRIOR) | ClassBit(PALADIN) | ClassBit(DEATH_KNIGHT), Skill::Mining},
    {Skill::Leatherworking, ClassBit(HUNTER) | ClassBit(SHAMAN) | ClassBit(ROGUE) | ClassBit(DRUID), Skill::Skinning},
    {Skill::Tailoring, ClassBit(PRIEST) | ClassBit(MAGE) | ClassBit(WARLOCK), Skill::None},
    {Skill::Engineering, ALL_CLASSES, Skill::Mining},
    {Skill::Jewelcrafting, ALL_CLASSES, Skill::Mining},
    {Skill::Alchemy, ALL_CLASSES, Skill::Herbalism},
    {Skill::Inscription, ALL_CLASSES, Skill::Herbalism},
    {Skill::Enchanting, ALL_CLASSES, Skill::None},
};

constexpr Skill GATHERERS[] = {Skill::Mining, Skill::Herbalism, Skill::Skinning};
constexpr std::size_t GATHERER_COUNT = std::size(GATHERERS);

constexpr std::size_t CRAFT_COUNT = std::size(CRAFTS);
constexpr std::size_t CLASS_COUNT = 10;
constexpr std::size_t CELL_COUNT = CLASS_COUNT * CRAFT_COUNT;
constexpr std::array<uint8_t, CLASS_COUNT> CLASS_IDS = {WARRIOR,      PALADIN, HUNTER, ROGUE,   PRIEST,
                                                        DEATH_KNIGHT, SHAMAN,  MAGE,   WARLOCK, DRUID};

// The slot of a class in the tables (-1: not a player class), the index of a craft in CRAFTS (CRAFT_COUNT: none)
constexpr int ClassSlot(uint8_t cls)
{
    for (std::size_t slot = 0; slot < CLASS_COUNT; ++slot)
        if (CLASS_IDS[slot] == cls)
            return static_cast<int>(slot);

    return -1;
}

constexpr std::size_t CraftIndex(Skill skill)
{
    for (std::size_t index = 0; index < CRAFT_COUNT; ++index)
        if (CRAFTS[index].Which == skill)
            return index;

    return CRAFT_COUNT;
}

// The cell of a class slot and a craft index in the flat tables (biases, chances)
constexpr std::size_t Cell(std::size_t slot, std::size_t craft) { return slot * CRAFT_COUNT + craft; }

// The look-ahead of the craft rule and of the skill path (WR.2, WR10): how many levels ahead a bot counts what it will
// soon use. About 15 levels at level 1, falling in a line to 5 from level 40 [E]
constexpr uint32_t SOON_LEVELS_FIRST = 15;
constexpr uint32_t SOON_LEVELS_LAST = 5;
constexpr uint32_t SOON_LEVELS_UNTIL = 40;

constexpr uint32_t SoonLevels(uint32_t level)
{
    if (level >= SOON_LEVELS_UNTIL)
        return SOON_LEVELS_LAST;

    uint32_t const from = level < 1 ? 1 : level;
    return SOON_LEVELS_LAST +
           (SOON_LEVELS_UNTIL - from) * (SOON_LEVELS_FIRST - SOON_LEVELS_LAST) / (SOON_LEVELS_UNTIL - 1);
}

constexpr std::size_t MAX_MAINS = 2;

// The secondary professions as bits of BotProfessions::Secondary
constexpr uint8_t SECONDARY_FIRST_AID = 1;
constexpr uint8_t SECONDARY_FISHING = 2;
constexpr uint8_t SECONDARY_COOKING = 4;

// Own salt, so the profession draws are independent of the trait draws of the same guid and version
constexpr uint64_t SEED_SALT = 0x5EC0DA9F17B0C4A3ULL;
// A bot of strongly low drive keeps to one main profession, the first of its pair [E]
constexpr int ONE_MAIN_DRIVE = -TraitRules::STRONG;
// Percent chances of a secondary profession [E]: fishing and cooking follow the interests of the same name
constexpr double SECONDARY_WITHOUT_INTEREST_PERCENT = 10.0;
// First aid mostly to the classes that cannot heal themselves; never to a tailor (it would eat the tailor's cloth)
constexpr double FIRST_AID_PERCENT = 60.0;
constexpr double FIRST_AID_SELF_HEALER_PERCENT = 15.0;
constexpr uint32_t SELF_HEALERS = ClassBit(PALADIN) | ClassBit(PRIEST) | ClassBit(SHAMAN) | ClassBit(DRUID);

// How the person came about; stored as its number
enum class Origin : uint8_t
{
    Rolled,  // rolled from guid and version
    Held,    // the professions the bot held when it got its person (WR.7), wholly or in part
    Set,     // set by the GM command (labs)
    Chosen   // the skill path's second choice (WR11): a second gatherer stored in the craft's place
};

struct BotProfessions
{
    std::array<Skill, MAX_MAINS> Mains{};  // learning order: the gatherer first; None = no profession
    uint8_t Secondary = 0;                 // SECONDARY_* bits
    uint32_t Version = 1;                  // the roll counter (1 = first roll, a reroll adds one)
    Origin From = Origin::Rolled;
};

// Main professions in learning order: the gatherer first
struct Pair
{
    Skill First;
    Skill Second;
};

constexpr ProfessionInfo const* Find(Skill skill)
{
    for (ProfessionInfo const& info : PROFESSIONS)
        if (info.Which == skill)
            return &info;

    return nullptr;
}

inline bool Is(Skill skill, Kind kind)
{
    ProfessionInfo const* info = Find(skill);
    return info && info->Type == kind;
}

inline bool IsMain(Skill skill) { return Is(skill, Kind::Gatherer) || Is(skill, Kind::Craft); }

constexpr std::string_view Name(Skill skill)
{
    ProfessionInfo const* info = Find(skill);
    return info ? info->Name : std::string_view("none");
}

// The profession of a command word; false when there is none
inline bool FindByName(std::string_view name, Skill& skill)
{
    for (ProfessionInfo const& info : PROFESSIONS)
        if (info.Name == name)
        {
            skill = info.Which;
            return true;
        }

    return false;
}

inline uint8_t SecondaryBit(Skill skill)
{
    switch (skill)
    {
        case Skill::FirstAid:
            return SECONDARY_FIRST_AID;
        case Skill::Fishing:
            return SECONDARY_FISHING;
        case Skill::Cooking:
            return SECONDARY_COOKING;
        default:
            return 0;
    }
}

inline Craft const* FindCraft(Skill skill)
{
    for (Craft const& craft : CRAFTS)
        if (craft.Which == skill)
            return &craft;

    return nullptr;
}

constexpr bool Serves(Craft const& craft, uint8_t cls) { return (craft.Classes & ClassBit(cls)) != 0; }

inline bool HasMain(BotProfessions const& person, Skill skill)
{
    for (Skill const main : person.Mains)
        if (main == skill && skill != Skill::None)
            return true;

    return false;
}

// A main or secondary profession of the person
inline bool Has(BotProfessions const& person, Skill skill)
{
    uint8_t const bit = SecondaryBit(skill);
    return bit ? (person.Secondary & bit) != 0 : HasMain(person, skill);
}

inline std::size_t MainCount(BotProfessions const& person)
{
    std::size_t count = 0;
    for (Skill const main : person.Mains)
        count += main != Skill::None ? 1 : 0;

    return count;
}

// The tools a profession's casts ask for (WR.12, PLAN 3.4), each profession's complete list from the data (the
// recipe index's tool and totem-category columns, the world DB's item templates and spawned vendors, 2026-11-20): the
// item, the totem category it covers (0: asked for as the item itself) and whether a vendor sells it. A vendor's tool
// is a need of the profession (UpkeepNeedRules) and given with a profession history; a made one (the spanner, the
// micro-adjustor, the stone, the rods) is a step of the skill path (SkillPathRules)
struct ToolRow
{
    Skill Which;
    uint32_t Item;
    uint32_t Category;
    bool Vendor;
};

constexpr ToolRow TOOLS[] = {
    {Skill::Mining, 2901, 165, true},        // Mining Pick
    {Skill::Blacksmithing, 5956, 162, true}, // Blacksmith Hammer
    {Skill::Engineering, 5956, 162, true},   // Blacksmith Hammer
    {Skill::Engineering, 6219, 14, false},   // Arclight Spanner
    {Skill::Engineering, 10498, 15, false},  // Gyromatic Micro-Adjustor
    {Skill::Jewelcrafting, 20815, 0, true},  // Jeweler's Kit
    {Skill::Jewelcrafting, 20824, 0, true},  // Simple Grinder
    {Skill::Inscription, 39505, 121, true},  // Virtuoso Inking Set
    {Skill::Alchemy, 9149, 12, false},       // Philosopher's Stone
    {Skill::Enchanting, 6218, 6, false},     // Runed Copper Rod
    {Skill::Enchanting, 6339, 7, false},     // Runed Silver Rod
    {Skill::Enchanting, 11130, 8, false},    // Runed Golden Rod
    {Skill::Enchanting, 11145, 9, false},    // Runed Truesilver Rod
    {Skill::Enchanting, 16207, 10, false},   // Runed Arcanite Rod
    {Skill::Enchanting, 22461, 41, false},   // Runed Fel Iron Rod
    {Skill::Enchanting, 22462, 62, false},   // Runed Adamantite Rod
    {Skill::Enchanting, 22463, 63, false},   // Runed Eternium Rod
    {Skill::Enchanting, 44452, 190, false},  // Runed Titanium Rod
    {Skill::Skinning, 7005, 166, true},      // Skinning Knife
    {Skill::Fishing, 6256, 0, true},         // Fishing Pole
};

// The pairs open to a class (WR.2): each craft it serves with its gatherer; tailoring and enchanting with each other
// or with any gatherer. Two gatherers are never a pair. Roll picks a craft first (the fitted chances) and its partner
// from these
inline std::vector<Pair> OpenPairs(uint8_t cls)
{
    std::vector<Pair> pairs;
    for (Craft const& craft : CRAFTS)
        if (Serves(craft, cls) && craft.Gatherer != Skill::None)
            pairs.push_back({craft.Gatherer, craft.Which});

    for (Craft const& craft : CRAFTS)
    {
        if (!Serves(craft, cls) || craft.Gatherer != Skill::None)
            continue;

        for (Skill const gatherer : GATHERERS)
            pairs.push_back({gatherer, craft.Which});
    }

    for (std::size_t i = 0; i < std::size(CRAFTS); ++i)
        for (std::size_t j = i + 1; j < std::size(CRAFTS); ++j)
            if (CRAFTS[i].Gatherer == Skill::None && CRAFTS[j].Gatherer == Skill::None && Serves(CRAFTS[i], cls) &&
                Serves(CRAFTS[j], cls))
                pairs.push_back({CRAFTS[i].Which, CRAFTS[j].Which});

    return pairs;
}

// The partners a craft without a gatherer (tailoring, enchanting) draws from, evenly: every gatherer, then the other
// such craft the class serves. A craft with a gatherer has none to draw: its partner is the gatherer
struct PartnerOptions
{
    std::array<Skill, GATHERER_COUNT + 2> Skills{};
    std::size_t Count = 0;
};

inline PartnerOptions PartnersOf(std::size_t craftIndex, uint8_t cls)
{
    PartnerOptions options;
    if (craftIndex >= CRAFT_COUNT || CRAFTS[craftIndex].Gatherer != Skill::None)
        return options;

    for (Skill const gatherer : GATHERERS)
        options.Skills[options.Count++] = gatherer;

    for (std::size_t other = 0; other < CRAFT_COUNT; ++other)
        if (other != craftIndex && CRAFTS[other].Gatherer == Skill::None && Serves(CRAFTS[other], cls))
            options.Skills[options.Count++] = CRAFTS[other].Which;

    return options;
}

// The global craft shares and the class biases (WR.2), the inputs of the fit. A share is a weight: the table is
// normalised to 100 %. A bias is 0 where the class does not consider the craft (the table of CRAFTS, not an option),
// 1 neutral, above 1 a lean
struct Settings
{
    std::array<float, CRAFT_COUNT> Share{};
    std::array<float, CELL_COUNT> Bias{};  // Cell(class slot, craft index)
};

constexpr float DEFAULT_SHARE = 100.0f / CRAFT_COUNT;  // equal [E]
constexpr float HUNTER_ENGINEERING_BIAS = 2.0f;        // [E]

inline Settings DefaultSettings()
{
    Settings settings;
    settings.Share.fill(DEFAULT_SHARE);
    for (std::size_t slot = 0; slot < CLASS_COUNT; ++slot)
        for (std::size_t craft = 0; craft < CRAFT_COUNT; ++craft)
        {
            bool const lean = CLASS_IDS[slot] == HUNTER && CRAFTS[craft].Which == Skill::Engineering;
            float const neutral = lean ? HUNTER_ENGINEERING_BIAS : 1.0f;
            settings.Bias[Cell(slot, craft)] = Serves(CRAFTS[craft], CLASS_IDS[slot]) ? neutral : 0.0f;
        }

    return settings;
}

// The chance that a class's roll picks each craft first: per class slot, the served crafts sum to 1
using ChanceTable = std::array<double, CELL_COUNT>;

// The fit: the iteration bound, the stop when no share is further than the tolerance from its target, and the gap from
// which a share counts as one no class can reach: it stays short of its target by more than that (shares as fractions
// of one, so 0.0005 is half a point of a hundred). The crafts that take up what it leaves overshoot their targets
constexpr uint32_t FIT_MAX_ITERATIONS = 5000;
constexpr double FIT_TOLERANCE = 1e-9;
constexpr double FIT_STEP = 0.8;  // the exponent of one update (1 = a plain proportional step; below it settles surely)
constexpr double FIT_UNREACHABLE_GAP = 0.0005;
constexpr double FIT_FACTOR_MIN = 1e-9;
constexpr double FIT_FACTOR_MAX = 1e9;

struct FitResult
{
    ChanceTable Chance{};
    std::array<double, CRAFT_COUNT> Target{};   // the normalised global shares
    std::array<double, CRAFT_COUNT> Reached{};  // what the roster holds under Chance
    uint32_t Iterations = 0;
    bool Converged = false;
    uint32_t Unreachable = 0;  // bit per craft index: a share no class can reach
};

// What a class that rolls each craft first with the chances holds: the rolled craft, and the other craft of a
// tailor-and-enchanter pair (counted from both sides: the pair is reached from either craft)
inline void AddHoldings(std::size_t slot, ChanceTable const& chance, std::array<double, CRAFT_COUNT>& holdings)
{
    uint8_t const cls = CLASS_IDS[slot];
    for (std::size_t craft = 0; craft < CRAFT_COUNT; ++craft)
    {
        double const rolled = chance[Cell(slot, craft)];
        holdings[craft] += rolled;
        PartnerOptions const options = PartnersOf(craft, cls);
        for (std::size_t i = 0; i < options.Count; ++i)
        {
            std::size_t const other = CraftIndex(options.Skills[i]);
            if (other < CRAFT_COUNT)
                holdings[other] += rolled / options.Count;
        }
    }
}

// Iterative proportional fitting: each class's chance for a craft is its bias times one factor of the craft, scaled to
// sum to 1 over the class's served crafts; the factors are moved until the roster (every class weighs the same, as
// Population keeps the roster even by class) holds the global shares. The roster's share of a craft is its share of
// all crafts held (a tailor-and-enchanter holds two). The fit is deterministic; a share no class can reach stays
// short and is named in Unreachable
inline FitResult FitChances(Settings const& settings)
{
    FitResult result;
    double total = 0.0;
    for (float const share : settings.Share)
        total += share > 0.0f ? share : 0.0f;

    for (std::size_t craft = 0; craft < CRAFT_COUNT; ++craft)
        result.Target[craft] =
            total > 0.0 ? (settings.Share[craft] > 0.0f ? settings.Share[craft] : 0.0f) / total : 1.0 / CRAFT_COUNT;

    // The effective biases: zero outside a class's table; a class whose served biases are all zero takes them as 1
    std::array<double, CELL_COUNT> bias{};
    for (std::size_t slot = 0; slot < CLASS_COUNT; ++slot)
    {
        double sum = 0.0;
        for (std::size_t craft = 0; craft < CRAFT_COUNT; ++craft)
        {
            float const set = settings.Bias[Cell(slot, craft)];
            double const value = Serves(CRAFTS[craft], CLASS_IDS[slot]) && set > 0.0f ? set : 0.0;
            bias[Cell(slot, craft)] = value;
            sum += value;
        }

        if (sum <= 0.0)
            for (std::size_t craft = 0; craft < CRAFT_COUNT; ++craft)
                bias[Cell(slot, craft)] = Serves(CRAFTS[craft], CLASS_IDS[slot]) ? 1.0 : 0.0;
    }

    std::array<double, CRAFT_COUNT> factor;
    factor.fill(1.0);
    auto const chances = [&]()
    {
        ChanceTable chance{};
        for (std::size_t slot = 0; slot < CLASS_COUNT; ++slot)
        {
            double sum = 0.0;
            for (std::size_t craft = 0; craft < CRAFT_COUNT; ++craft)
                sum += bias[Cell(slot, craft)] * factor[craft];

            if (sum > 0.0)
                for (std::size_t craft = 0; craft < CRAFT_COUNT; ++craft)
                    chance[Cell(slot, craft)] = bias[Cell(slot, craft)] * factor[craft] / sum;
        }

        return chance;
    };
    auto const reached = [&](ChanceTable const& chance)
    {
        std::array<double, CRAFT_COUNT> holdings{};
        for (std::size_t slot = 0; slot < CLASS_COUNT; ++slot)
            AddHoldings(slot, chance, holdings);

        double held = 0.0;
        for (double const count : holdings)
            held += count;

        if (held > 0.0)
            for (double& count : holdings)
                count /= held;

        return holdings;
    };

    for (uint32_t iteration = 0; iteration < FIT_MAX_ITERATIONS; ++iteration)
    {
        result.Iterations = iteration;
        result.Chance = chances();
        result.Reached = reached(result.Chance);
        double gap = 0.0;
        for (std::size_t craft = 0; craft < CRAFT_COUNT; ++craft)
            gap = (std::max)(gap, std::fabs(result.Reached[craft] - result.Target[craft]));

        if (gap <= FIT_TOLERANCE)
        {
            result.Converged = true;
            break;
        }

        for (std::size_t craft = 0; craft < CRAFT_COUNT; ++craft)
        {
            if (result.Target[craft] <= 0.0)
                factor[craft] = 0.0;
            else if (result.Reached[craft] > 0.0)
            {
                double const step = std::pow(result.Target[craft] / result.Reached[craft], FIT_STEP);
                factor[craft] = std::clamp(factor[craft] * step, FIT_FACTOR_MIN, FIT_FACTOR_MAX);
            }
        }
    }

    if (!result.Converged)
    {
        result.Chance = chances();
        result.Reached = reached(result.Chance);
    }

    for (std::size_t craft = 0; craft < CRAFT_COUNT; ++craft)
        if (result.Target[craft] - result.Reached[craft] > FIT_UNREACHABLE_GAP)
            result.Unreachable |= 1u << craft;

    return result;
}

inline bool HasInterest(TraitRules::BotTraits const& traits, TraitRules::Interest interest)
{
    uint8_t const stored = static_cast<uint8_t>(static_cast<uint8_t>(interest) + 1);
    for (uint8_t const held : traits.Interests)
        if (held == stored)
            return true;

    return false;
}

// The craft a class's roll picks first: by the fitted chances of the class's served crafts (an empty row: evenly among
// them); CRAFT_COUNT when the class serves none
inline std::size_t PickCraft(ChanceTable const& chances, uint8_t cls, double roll)
{
    int const slot = ClassSlot(cls);
    if (slot < 0)
        return CRAFT_COUNT;

    double sum = 0.0;
    std::size_t served = 0;
    std::size_t last = CRAFT_COUNT;
    for (std::size_t craft = 0; craft < CRAFT_COUNT; ++craft)
        if (Serves(CRAFTS[craft], cls))
        {
            sum += chances[Cell(slot, craft)];
            ++served;
            last = craft;
        }

    if (!served)
        return CRAFT_COUNT;

    double const point = std::clamp(roll, 0.0, 1.0) * (sum > 0.0 ? sum : static_cast<double>(served));
    double reached = 0.0;
    for (std::size_t craft = 0; craft < CRAFT_COUNT; ++craft)
        if (Serves(CRAFTS[craft], cls))
        {
            double const weight = sum > 0.0 ? chances[Cell(slot, craft)] : 1.0;
            reached += weight;
            if (weight > 0.0 && point < reached)
                return craft;
        }

    return last;
}

// The pair of a rolled craft: the craft's gatherer, or for a craft without one the partner drawn evenly (the rolled
// craft first when the partner is the other such craft, so a bot with one main keeps what it rolled)
inline Pair PairOfCraft(std::size_t craftIndex, uint8_t cls, double partnerRoll)
{
    Craft const& craft = CRAFTS[craftIndex];
    if (craft.Gatherer != Skill::None)
        return {craft.Gatherer, craft.Which};

    PartnerOptions const options = PartnersOf(craftIndex, cls);
    std::size_t const index =
        (std::min)(static_cast<std::size_t>(std::clamp(partnerRoll, 0.0, 1.0) * options.Count), options.Count - 1);
    Skill const partner = options.Skills[index];
    return Is(partner, Kind::Gatherer) ? Pair{partner, craft.Which} : Pair{craft.Which, partner};
}

// Deterministic: the same guid, version, class, traits and chances give the same person on every build. The craft is
// rolled first (the fitted chances of the class), the pair follows from it; two gatherers are never rolled (WR.2)
inline BotProfessions Roll(uint32_t guid, uint32_t version, uint8_t cls, TraitRules::BotTraits const& traits,
                           ChanceTable const& chances)
{
    uint64_t state = ((static_cast<uint64_t>(guid) << 32) | version) ^ SEED_SALT;
    // Every draw is taken in this order whatever the outcome, so one rule's change leaves the others' draws alone
    double const craftRoll = TraitRules::Uniform(state);
    double const fishingRoll = TraitRules::Uniform(state) * 100.0;
    double const cookingRoll = TraitRules::Uniform(state) * 100.0;
    double const firstAidRoll = TraitRules::Uniform(state) * 100.0;
    double const partnerRoll = TraitRules::Uniform(state);

    BotProfessions person;
    person.Version = version;
    std::size_t const craft = PickCraft(chances, cls, craftRoll);
    if (craft < CRAFT_COUNT)
    {
        Pair const pair = PairOfCraft(craft, cls, partnerRoll);
        person.Mains[0] = pair.First;
        if (traits.Values[TraitRules::Index(TraitRules::Trait::Drive)] > ONE_MAIN_DRIVE)
            person.Mains[1] = pair.Second;
    }

    double const fishing =
        HasInterest(traits, TraitRules::Interest::Fishing) ? 100.0 : SECONDARY_WITHOUT_INTEREST_PERCENT;
    double const cooking =
        HasInterest(traits, TraitRules::Interest::Cooking) ? 100.0 : SECONDARY_WITHOUT_INTEREST_PERCENT;
    double const firstAid = (SELF_HEALERS & ClassBit(cls)) ? FIRST_AID_SELF_HEALER_PERCENT : FIRST_AID_PERCENT;
    if (fishingRoll < fishing)
        person.Secondary |= SECONDARY_FISHING;

    if (cookingRoll < cooking)
        person.Secondary |= SECONDARY_COOKING;

    if (firstAidRoll < firstAid && !HasMain(person, Skill::Tailoring))
        person.Secondary |= SECONDARY_FIRST_AID;

    return person;
}

// The person of a bot that gets one (WR.7): the main professions it holds, whatever they are (gatherers first, at most
// two; more stay held but are not the person), else the rolled ones; the same for the secondary professions
inline BotProfessions Adopt(BotProfessions const& rolled, std::vector<Skill> const& held)
{
    BotProfessions person = rolled;
    std::array<Skill, MAX_MAINS> mains{};
    std::size_t count = 0;
    for (Kind const kind : {Kind::Gatherer, Kind::Craft})
        for (ProfessionInfo const& info : PROFESSIONS)
            if (info.Type == kind && count < MAX_MAINS)
                for (Skill const skill : held)
                    if (skill == info.Which)
                    {
                        mains[count++] = skill;
                        break;
                    }

    uint8_t secondary = 0;
    for (Skill const skill : held)
        secondary |= SecondaryBit(skill);

    if (count)
    {
        person.Mains = mains;
        person.From = Origin::Held;
    }

    if (secondary)
    {
        person.Secondary = secondary;
        person.From = Origin::Held;
    }
    // The rolled secondaries were drawn for the rolled mains: a held tailor takes no first aid it does not hold
    else if (HasMain(person, Skill::Tailoring))
        person.Secondary &= static_cast<uint8_t>(~SECONDARY_FIRST_AID);

    return person;
}

// The first broken rule of a rolled person, empty when it keeps them all: at most two different main professions, every
// craft serves the class and comes with its gatherer (tailoring and enchanting excepted), no tailor with first aid
inline std::string Violation(BotProfessions const& person, uint8_t cls)
{
    if (person.Mains[0] != Skill::None && person.Mains[0] == person.Mains[1])
        return "the same main profession twice";

    for (Skill const main : person.Mains)
    {
        if (main == Skill::None)
            continue;

        if (!IsMain(main))
            return std::string(Name(main)) + " is no main profession";

        Craft const* craft = FindCraft(main);
        if (!craft)
            continue;

        if (!Serves(*craft, cls))
            return std::string(Name(main)) + " does not serve the class";

        if (craft->Gatherer != Skill::None && !HasMain(person, craft->Gatherer))
            return std::string(Name(main)) + " without " + std::string(Name(craft->Gatherer));
    }

    if (person.Mains[0] == Skill::None && person.Mains[1] != Skill::None)
        return "a second main profession without a first";

    if (HasMain(person, Skill::Tailoring) && (person.Secondary & SECONDARY_FIRST_AID))
        return "a tailor with first aid";

    return {};
}

// "mining + blacksmithing; fishing, cooking"
inline std::string Describe(BotProfessions const& person)
{
    std::string text;
    for (Skill const main : person.Mains)
        if (main != Skill::None)
            text += (text.empty() ? "" : " + ") + std::string(Name(main));

    if (text.empty())
        text = "no main profession";

    std::string secondary;
    for (ProfessionInfo const& info : PROFESSIONS)
        if (info.Type == Kind::Secondary && (person.Secondary & SecondaryBit(info.Which)))
            secondary += (secondary.empty() ? "" : ", ") + std::string(info.Name);

    return text + "; " + (secondary.empty() ? "no secondary profession" : secondary);
}

inline std::string_view OriginWord(Origin origin)
{
    switch (origin)
    {
        case Origin::Rolled:
            return "rolled";
        case Origin::Held:
            return "held";
        case Origin::Set:
            return "set";
        case Origin::Chosen:
            return "chosen";
        default:
            return "other";
    }
}
}  // namespace ProfessionRules

#endif
