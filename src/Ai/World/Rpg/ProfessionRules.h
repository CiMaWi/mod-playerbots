/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_PROFESSIONRULES_H
#define _PLAYERBOT_PROFESSIONRULES_H

#include <algorithm>
#include <array>
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
    Set      // set by the GM command (labs)
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

inline ProfessionInfo const* Find(Skill skill)
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

inline std::string_view Name(Skill skill)
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

inline bool Serves(Craft const& craft, uint8_t cls) { return (craft.Classes & ClassBit(cls)) != 0; }

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

// The pairs open to a class (WR.2): each craft it serves with its gatherer; tailoring and enchanting with each other
// or with any gatherer; two gatherers. The roll picks among them evenly (WR.3)
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

    for (std::size_t i = 0; i < std::size(GATHERERS); ++i)
        for (std::size_t j = i + 1; j < std::size(GATHERERS); ++j)
            pairs.push_back({GATHERERS[i], GATHERERS[j]});

    return pairs;
}

inline bool HasInterest(TraitRules::BotTraits const& traits, TraitRules::Interest interest)
{
    uint8_t const stored = static_cast<uint8_t>(static_cast<uint8_t>(interest) + 1);
    for (uint8_t const held : traits.Interests)
        if (held == stored)
            return true;

    return false;
}

// Deterministic: the same guid, version, class and traits give the same person on every build
inline BotProfessions Roll(uint32_t guid, uint32_t version, uint8_t cls, TraitRules::BotTraits const& traits)
{
    uint64_t state = ((static_cast<uint64_t>(guid) << 32) | version) ^ SEED_SALT;
    // Every draw is taken in this order whatever the outcome, so one rule's change leaves the others' draws alone
    double const pairRoll = TraitRules::Uniform(state);
    double const fishingRoll = TraitRules::Uniform(state) * 100.0;
    double const cookingRoll = TraitRules::Uniform(state) * 100.0;
    double const firstAidRoll = TraitRules::Uniform(state) * 100.0;

    BotProfessions person;
    person.Version = version;
    std::vector<Pair> const pairs = OpenPairs(cls);
    if (!pairs.empty())
    {
        std::size_t const index = std::min(static_cast<std::size_t>(pairRoll * pairs.size()), pairs.size() - 1);
        person.Mains[0] = pairs[index].First;
        if (traits.Values[TraitRules::Index(TraitRules::Trait::Drive)] > ONE_MAIN_DRIVE)
            person.Mains[1] = pairs[index].Second;
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
        default:
            return "other";
    }
}
}  // namespace ProfessionRules

#endif
