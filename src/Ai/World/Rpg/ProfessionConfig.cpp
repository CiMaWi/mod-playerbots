/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "ProfessionConfig.h"

#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include "ConfigTable.h"
#include "ProfessionRules.h"

namespace
{
using Row = ConfigRow<ProfessionConfig>;
using ProfessionRules::CELL_COUNT;
using ProfessionRules::CLASS_COUNT;
using ProfessionRules::CLASS_IDS;
using ProfessionRules::CRAFT_COUNT;
using ProfessionRules::CRAFTS;

static_assert(std::tuple_size_v<decltype(ProfessionConfig::CraftShare)> == CRAFT_COUNT, "one share per craft");
static_assert(std::tuple_size_v<decltype(ProfessionConfig::ClassBias)> == CELL_COUNT, "one bias per class and craft");
static_assert(std::is_same_v<decltype(ProfessionConfig::Chance), ProfessionRules::ChanceTable>,
              "the config holds the table the roll reads");
static_assert(std::is_same_v<decltype(ProfessionConfig::CraftShare), decltype(ProfessionRules::Settings::Share)> &&
                  std::is_same_v<decltype(ProfessionConfig::ClassBias), decltype(ProfessionRules::Settings::Bias)>,
              "the config holds the settings the fit reads");

// The key words, in the order of CRAFTS and CLASS_IDS
constexpr std::array<char const*, CRAFT_COUNT> CraftWords = {"Blacksmithing", "Leatherworking", "Tailoring",
                                                             "Engineering",   "Jewelcrafting",  "Alchemy",
                                                             "Inscription",   "Enchanting"};
constexpr std::array<char const*, CLASS_COUNT> ClassWords = {"Warrior",     "Paladin", "Hunter", "Rogue",   "Priest",
                                                             "DeathKnight", "Shaman",  "Mage",   "Warlock", "Druid"};

constexpr bool SameWord(std::string_view key, std::string_view name)
{
    if (key.size() != name.size())
        return false;

    for (std::size_t i = 0; i < key.size(); ++i)
    {
        char const lower = key[i] >= 'A' && key[i] <= 'Z' ? static_cast<char>(key[i] - 'A' + 'a') : key[i];
        if (lower != name[i])
            return false;
    }

    return true;
}

constexpr bool CraftWordsMatch()
{
    for (std::size_t craft = 0; craft < CRAFT_COUNT; ++craft)
        if (!SameWord(CraftWords[craft], ProfessionRules::Name(CRAFTS[craft].Which)))
            return false;

    return true;
}

static_assert(CraftWordsMatch(), "the craft key words follow CRAFTS");

// The cells of the table a class considers: a bias key each; a cell outside the table is no option
constexpr std::size_t ServedCellCount()
{
    std::size_t count = 0;
    for (std::size_t slot = 0; slot < CLASS_COUNT; ++slot)
        for (std::size_t craft = 0; craft < CRAFT_COUNT; ++craft)
            count += ProfessionRules::Serves(CRAFTS[craft], CLASS_IDS[slot]) ? 1 : 0;

    return count;
}

// The flat cell of the n-th served cell
constexpr std::size_t ServedCell(std::size_t n)
{
    for (std::size_t slot = 0; slot < CLASS_COUNT; ++slot)
        for (std::size_t craft = 0; craft < CRAFT_COUNT; ++craft)
            if (ProfessionRules::Serves(CRAFTS[craft], CLASS_IDS[slot]) && n-- == 0)
                return ProfessionRules::Cell(slot, craft);

    return 0;
}

constexpr std::size_t PlainRowCount = 3;
constexpr std::size_t KeyCount = CRAFT_COUNT + ServedCellCount();
constexpr std::size_t RowCount = PlainRowCount + KeyCount;

struct KeyText
{
    std::array<char, 48> Text{};
};

constexpr void Append(KeyText& key, std::size_t& at, std::string_view word)
{
    for (char const letter : word)
        key.Text[at++] = letter;
}

// "Share.Engineering" for the first CRAFT_COUNT keys, then "Bias.Hunter.Engineering" for each served cell
constexpr std::array<KeyText, KeyCount> MakeKeys()
{
    std::array<KeyText, KeyCount> keys{};
    for (std::size_t craft = 0; craft < CRAFT_COUNT; ++craft)
    {
        std::size_t at = 0;
        Append(keys[craft], at, "Share.");
        Append(keys[craft], at, CraftWords[craft]);
    }

    for (std::size_t n = 0; n < ServedCellCount(); ++n)
    {
        std::size_t const cell = ServedCell(n);
        std::size_t at = 0;
        KeyText& key = keys[CRAFT_COUNT + n];
        Append(key, at, "Bias.");
        Append(key, at, ClassWords[cell / CRAFT_COUNT]);
        Append(key, at, ".");
        Append(key, at, CraftWords[cell % CRAFT_COUNT]);
    }

    return keys;
}

constexpr std::array<KeyText, KeyCount> Keys = MakeKeys();

float& ShareAt(ProfessionConfig& out, uint32_t index) { return out.CraftShare[index]; }
float& BiasAt(ProfessionConfig& out, uint32_t index) { return out.ClassBias[index]; }

constexpr Row RowAt(std::size_t index)
{
    switch (index)
    {
        case 0:
            return {"Learn.GathererLevel", &ProfessionConfig::GathererLevel, 1, 80};
        case 1:
            return {"Craft.VisitDeadlineSeconds", &ProfessionConfig::CraftVisitDeadlineSeconds, 60, 3600};
        case 2:
            return {"VisitStepSeconds", &ProfessionConfig::VisitStepSeconds, 1, 60};
        default:
            break;
    }

    std::size_t const key = index - PlainRowCount;
    if (key < CRAFT_COUNT)
        return Row{Keys[key].Text.data(), ConfigElement<ProfessionConfig, float>{&ShareAt, static_cast<uint32_t>(key)},
                   0.0, 100.0, ConfigKind::TuningListed};

    return Row{Keys[key].Text.data(),
               ConfigElement<ProfessionConfig, float>{&BiasAt, static_cast<uint32_t>(ServedCell(key - CRAFT_COUNT))},
               0.1, 10.0, ConfigKind::TuningListed};
}

template <std::size_t... Index>
constexpr std::array<Row, RowCount> MakeRows(std::index_sequence<Index...>)
{
    return {{RowAt(Index)...}};
}

constexpr std::array<Row, RowCount> ProfessionRows = MakeRows(std::make_index_sequence<RowCount>{});

// The fitted chances of the loaded shares and biases (ProfessionRules::FitChances); the cells outside a class's table
// are zero whatever the walk left there
void FinishProfessions(ProfessionConfig& out)
{
    for (std::size_t slot = 0; slot < CLASS_COUNT; ++slot)
        for (std::size_t craft = 0; craft < CRAFT_COUNT; ++craft)
            if (!ProfessionRules::Serves(CRAFTS[craft], CLASS_IDS[slot]))
                out.ClassBias[ProfessionRules::Cell(slot, craft)] = 0.0f;

    ProfessionRules::Settings settings;
    settings.Share = out.CraftShare;
    settings.Bias = out.ClassBias;
    ProfessionRules::FitResult const fit = ProfessionRules::FitChances(settings);
    out.Chance = fit.Chance;
    out.UnreachableCrafts = fit.Unreachable;
}

ProfessionConfig MakeDefaults()
{
    ProfessionConfig config;
    ProfessionRules::Settings const settings = ProfessionRules::DefaultSettings();
    config.CraftShare = settings.Share;
    config.ClassBias = settings.Bias;
    FinishProfessions(config);
    return config;
}

ProfessionConfig const ProfessionDefaults = MakeDefaults();
}  // namespace

ConfigFamily<ProfessionConfig> const ProfessionFamily{"Playerbots.Professions.", ProfessionRows, &FinishProfessions,
                                                      &ProfessionDefaults};

ProfessionConfig& ProfessionConfigStorage()
{
    static ProfessionConfig values;
    return values;
}

std::string ProfessionUnreachableCrafts(ProfessionConfig const& config)
{
    std::string words;
    for (std::size_t craft = 0; craft < CRAFT_COUNT; ++craft)
        if (config.UnreachableCrafts & (1u << craft))
            words += (words.empty() ? "" : ", ") + std::string(ProfessionRules::Name(CRAFTS[craft].Which));

    return words;
}
