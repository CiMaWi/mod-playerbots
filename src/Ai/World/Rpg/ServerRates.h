/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_SERVERRATES_H
#define _PLAYERBOT_SERVERRATES_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

// The server's own rates (professions plan WR29, DECISIONS WR.28): the skill-gain steps, the colour chances and the
// mining and skinning steps, the item drop rate per quality, the reference rate and amount, the group amount and the
// money rate. Read once where the recipe, fishing and gather node indexes load (Planner::ReadServerRates, from the
// core's CONFIG_SKILL_GAIN_*, CONFIG_SKILL_CHANCE_* and RATE_DROP_*), carried into the pure rules by this struct; its
// defaults are the stock values, so a rule without the server's values computes as an unconfigured core does.
// Std only: any thread.
namespace Recipes
{
constexpr std::size_t ITEM_QUALITIES = 7;  // poor to artifact: the qualities the core scales (below heirloom)
constexpr std::uint16_t SKILL_MINING = 186;
constexpr std::uint16_t SKILL_SKINNING = 393;

struct ServerRates
{
    std::uint32_t craftStep{1};   // SkillGain.Crafting: the points a craft's skill-up gives
    std::uint32_t gatherStep{1};  // SkillGain.Gathering: the points of a node's, a skin's or a catch's skill-up
    int orange{1000};             // SkillChance.* x 10: per mille
    int yellow{750};
    int green{250};
    int grey{0};
    std::uint32_t miningSteps{0};  // SkillChance.MiningSteps: the chance halves every this many points; 0: never
    std::uint32_t skinningSteps{0};
    std::array<float, ITEM_QUALITIES> quality{1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f};  // Rate.Drop.Item.<quality>
    float referenced{1.0f};        // Rate.Drop.Item.Referenced: a reference row's chance
    float referencedAmount{1.0f};  // Rate.Drop.Item.ReferencedAmount: a reference's runs per max count
    float groupAmount{1.0f};       // Rate.Drop.Item.GroupAmount: a top-level group's draws
    float money{1.0f};             // Rate.Drop.Money

    // The drop rate of an item's quality (an heirloom or an unknown quality is not scaled, as LootStoreItem::Roll)
    [[nodiscard]] float QualityRate(std::uint8_t itemQuality) const
    {
        return itemQuality < ITEM_QUALITIES ? quality[itemQuality] : 1.0f;
    }

    // The points one skill-up gives: a craft's or a gathering skill's (herbalism, mining, skinning, fishing)
    [[nodiscard]] std::uint32_t Step(bool craft) const { return craft ? craftStep : gatherStep; }

    // The chance steps of a gathering skill line (mining and skinning only)
    [[nodiscard]] std::uint32_t ChanceSteps(std::uint16_t skill) const
    {
        return skill == SKILL_MINING ? miningSteps : skill == SKILL_SKINNING ? skinningSteps : 0;
    }

    bool operator==(ServerRates const& other) const
    {
        return craftStep == other.craftStep && gatherStep == other.gatherStep && orange == other.orange &&
               yellow == other.yellow && green == other.green && grey == other.grey &&
               miningSteps == other.miningSteps && skinningSteps == other.skinningSteps && quality == other.quality &&
               referenced == other.referenced && referencedAmount == other.referencedAmount &&
               groupAmount == other.groupAmount && money == other.money;
    }
};

// The rates as one line of `key=value` words: the recipe index's startup line and its dump header (ParseRates reads
// it back)
inline std::string RatesText(ServerRates const& rates)
{
    std::ostringstream out;
    out << std::setprecision(6) << "gain.crafting=" << rates.craftStep << " gain.gathering=" << rates.gatherStep
        << " chance=" << rates.orange << '/' << rates.yellow << '/' << rates.green << '/' << rates.grey
        << " steps.mining=" << rates.miningSteps << " steps.skinning=" << rates.skinningSteps << " drop.quality=";
    for (std::size_t i = 0; i < ITEM_QUALITIES; ++i)
        out << (i ? "/" : "") << rates.quality[i];
    out << " drop.referenced=" << rates.referenced << " drop.referencedAmount=" << rates.referencedAmount
        << " drop.groupAmount=" << rates.groupAmount << " drop.money=" << rates.money;
    return out.str();
}

// RatesText back; false when a word is missing or malformed
inline bool ParseRates(std::string const& text, ServerRates& rates)
{
    auto const value = [&text](std::string const& key, std::string& out)
    {
        std::size_t const at = text.find(key + "=");
        if (at == std::string::npos)
            return false;
        std::size_t const start = at + key.size() + 1;
        out = text.substr(start, text.find(' ', start) - start);
        return !out.empty();
    };
    auto const split = [](std::string const& text)
    {
        std::vector<std::string> parts;
        std::string part;
        std::istringstream in(text);
        while (std::getline(in, part, '/'))
            parts.push_back(part);
        return parts;
    };
    ServerRates read;
    std::string word;
    try
    {
        if (!value("gain.crafting", word))
            return false;
        read.craftStep = static_cast<std::uint32_t>(std::stoul(word));
        if (!value("gain.gathering", word))
            return false;
        read.gatherStep = static_cast<std::uint32_t>(std::stoul(word));
        if (!value("chance", word))
            return false;
        std::vector<std::string> const chances = split(word);
        if (chances.size() != 4)
            return false;
        read.orange = std::stoi(chances[0]);
        read.yellow = std::stoi(chances[1]);
        read.green = std::stoi(chances[2]);
        read.grey = std::stoi(chances[3]);
        if (!value("steps.mining", word))
            return false;
        read.miningSteps = static_cast<std::uint32_t>(std::stoul(word));
        if (!value("steps.skinning", word))
            return false;
        read.skinningSteps = static_cast<std::uint32_t>(std::stoul(word));
        if (!value("drop.quality", word))
            return false;
        std::vector<std::string> const qualities = split(word);
        if (qualities.size() != ITEM_QUALITIES)
            return false;
        for (std::size_t i = 0; i < ITEM_QUALITIES; ++i)
            read.quality[i] = std::stof(qualities[i]);
        if (!value("drop.referenced", word))
            return false;
        read.referenced = std::stof(word);
        if (!value("drop.referencedAmount", word))
            return false;
        read.referencedAmount = std::stof(word);
        if (!value("drop.groupAmount", word))
            return false;
        read.groupAmount = std::stof(word);
        if (!value("drop.money", word))
            return false;
        read.money = std::stof(word);
    }
    catch (...)
    {
        return false;
    }
    rates = read;
    return true;
}
}  // namespace Recipes

#endif
