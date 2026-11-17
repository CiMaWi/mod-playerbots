/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_CONFIGTABLE_H
#define _PLAYERBOT_CONFIGTABLE_H

#include <charconv>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

// One row per config value: the key under its family's prefix, the member it fills, its bounds and its kind. The
// default is the member's initializer in the family struct. Game-free: the walk takes its reader and its warning sink
// as arguments, so the server passes sConfigMgr and a selftest a fake.

enum class ConfigKind : uint8_t
{
    Tuning,        // a value the nights tune; conf.dist does not list it
    TuningListed,  // a tuning value a hub config or a test sets; conf.dist lists it
    Test,          // a test-only switch; listed
    Role,          // what an instance is (main, lab); listed
    NotYet,        // a switch kept until its feature is done; listed
    Retired        // a removed key that old configs may still set; ignored
};

enum class ConfigOutOfRange : uint8_t
{
    Clamp,       // the value is clamped into the bounds, with a warning
    KeepDefault  // the value is ignored and the default kept, with a warning
};

inline bool IsListedConfigKind(ConfigKind kind) { return kind != ConfigKind::Tuning && kind != ConfigKind::Retired; }

inline char const* ConfigKindName(ConfigKind kind)
{
    switch (kind)
    {
        case ConfigKind::Tuning:
            return "tuning";
        case ConfigKind::TuningListed:
            return "tuning, listed";
        case ConfigKind::Test:
            return "test";
        case ConfigKind::Role:
            return "role";
        case ConfigKind::NotYet:
            return "not yet";
        case ConfigKind::Retired:
            return "retired";
    }
    return "?";
}

// One element of an array member (Loading[3]): At returns the element of index Index
template <typename Struct, typename T>
struct ConfigElement
{
    T& (*At)(Struct& out, uint32_t index);
    uint32_t Index;
};

template <typename T>
struct IsConfigElement : std::false_type
{
};

template <typename Struct, typename T>
struct IsConfigElement<ConfigElement<Struct, T>> : std::true_type
{
};

// The member (or array element) of a row in out
template <typename Struct, typename Target>
decltype(auto) ConfigTargetRef(Struct& out, Target target)
{
    if constexpr (IsConfigElement<Target>::value)
        return target.At(out, target.Index);
    else
        return (out.*target);
}

// Its value, from a const struct (the defaults, a print): the accessor only reads here
template <typename Struct, typename Target>
auto ConfigTargetValue(Struct const& out, Target target)
{
    return ConfigTargetRef(const_cast<Struct&>(out), target);
}

template <typename Struct, typename Target>
using ConfigTargetType =
    std::remove_cvref_t<decltype(ConfigTargetRef(std::declval<Struct&>(), std::declval<Target>()))>;

// The type a value is read as: a uint8 as a uint32 and a double as a float, as every hand-written read of such a
// member did (the member held the float's value, not the text's double)
template <typename T>
using ConfigReadType =
    std::conditional_t<std::is_same_v<T, uint8_t>, uint32_t, std::conditional_t<std::is_same_v<T, double>, float, T>>;

template <typename Struct>
struct ConfigRow
{
    using Member = std::variant<uint32_t Struct::*, int32_t Struct::*, float Struct::*, bool Struct::*,
                                std::string Struct::*, uint8_t Struct::*, double Struct::*,
                                ConfigElement<Struct, float>, ConfigElement<Struct, uint32_t>>;
    // Fills the struct from the key's text; false keeps the default (with a warning). Only for string members.
    using Parser = bool (*)(std::string const& text, Struct& out);

    char const* Key;
    Member Target;
    // Bounds of a number; a read without bounds gets its type's range
    double Lower = -std::numeric_limits<double>::infinity();
    double Upper = std::numeric_limits<double>::infinity();
    ConfigKind Kind = ConfigKind::Tuning;
    ConfigOutOfRange OutOfRange = ConfigOutOfRange::Clamp;
    Parser Parse = nullptr;
};

template <typename Struct>
struct ConfigFamily
{
    char const* Prefix;  // "Playerbots.BankMail."
    std::span<ConfigRow<Struct> const> Rows;
    // A rule over the family's values, run after each walk (a class mask that drops unknown bits)
    void (*Finish)(Struct& out) = nullptr;
    // The defaults where they are not the struct's initializer (a tactic's default scope); null = Struct{}
    Struct const* Defaults = nullptr;
};

template <typename Struct>
Struct ConfigFamilyDefaults(ConfigFamily<Struct> const& family)
{
    return family.Defaults ? *family.Defaults : Struct{};
}

template <typename T>
std::string ConfigValueText(T const& value)
{
    if constexpr (std::is_same_v<T, std::string>)
        return value;
    else if constexpr (std::is_same_v<T, double>)
        return ConfigValueText(static_cast<float>(value));  // read as a float, printed as one
    else if constexpr (std::is_same_v<T, uint8_t>)
        return std::to_string(static_cast<uint32_t>(value));
    else if constexpr (std::is_same_v<T, bool>)
        return value ? "1" : "0";
    else if constexpr (std::is_floating_point_v<T>)
    {
        // the shortest text that reads back as the same number (0.5, not 0.500000)
        char buffer[64];
        auto const result = std::to_chars(buffer, buffer + sizeof(buffer), value);
        return std::string(buffer, result.ptr);
    }
    else
        return std::to_string(value);
}

inline std::string ConfigBoundText(double bound)
{
    if (bound < -1e18 || bound > 1e18)
        return bound < 0 ? "-inf" : "inf";
    if (bound == static_cast<double>(static_cast<int64_t>(bound)))
        return std::to_string(static_cast<int64_t>(bound));
    return ConfigValueText(bound);
}

// The row's current value in out, as text (for a print command)
template <typename Struct>
std::string ConfigRowValueText(ConfigRow<Struct> const& row, Struct const& out)
{
    return std::visit([&out](auto member) { return ConfigValueText(ConfigTargetValue(out, member)); }, row.Target);
}

// One line of a print command: "key = value (default D, bounds L to U, kind K)", with " *" after the value where it
// differs from the default (the struct's initializer). A retired row has no value and prints nothing.
template <typename Struct>
std::string ConfigRowLine(ConfigFamily<Struct> const& family, ConfigRow<Struct> const& row, Struct const& out)
{
    Struct const defaults = ConfigFamilyDefaults(family);
    std::string const value = ConfigRowValueText(row, out);
    std::string const defaultValue = ConfigRowValueText(row, defaults);
    bool const numeric = std::visit(
        [](auto member)
        {
            using T = ConfigTargetType<Struct, decltype(member)>;
            return !std::is_same_v<T, std::string> && !std::is_same_v<T, bool>;
        },
        row.Target);
    std::string line = std::string(family.Prefix) + row.Key + " = " + value;
    if (value != defaultValue)
        line += " *";
    line += " (default " + defaultValue;
    if (numeric)
        line += ", bounds " + ConfigBoundText(row.Lower) + " to " + ConfigBoundText(row.Upper);
    return line + ", " + ConfigKindName(row.Kind) + ")";
}

// Does the family know the key: it starts with the family's prefix and a row (a retired row too: old configs may still
// set it) or a code tail names the rest. A family named by a full key (no trailing dot) holds that key only: a longer
// key with the same start is a live sibling (Playerbots.PvpGear.Wear, Playerbots.PvpGear.WearBetweenMatches). A code
// tail is a key part that code outside the table still reads; one ending in a dot covers every key below it (Corr.).
template <typename Struct>
bool KnowsConfigKey(ConfigFamily<Struct> const& family, std::string const& key,
                    std::span<char const* const> codeTails = {})
{
    std::string const prefix = family.Prefix;
    bool const fullKey = !prefix.empty() && prefix.back() != '.';
    if (key.compare(0, prefix.size(), prefix) != 0 || (fullKey && key.size() != prefix.size()))
        return false;
    for (ConfigRow<Struct> const& row : family.Rows)
        if (key.compare(prefix.size(), std::string::npos, row.Key) == 0)
            return true;
    for (std::string const tail : codeTails)
    {
        bool const below = !tail.empty() && tail.back() == '.';
        if (below ? key.compare(prefix.size(), tail.size(), tail) == 0
                  : key.compare(prefix.size(), std::string::npos, tail) == 0)
            return true;
    }
    return false;
}

// The keys of the list under the family's prefix that the family does not know (KnowsConfigKey)
template <typename Struct>
std::vector<std::string> UnknownConfigKeys(ConfigFamily<Struct> const& family, std::vector<std::string> const& keys,
                                           std::span<char const* const> codeTails = {})
{
    std::vector<std::string> unknown;
    std::string const prefix = family.Prefix;
    bool const fullKey = !prefix.empty() && prefix.back() != '.';
    for (std::string const& key : keys)
    {
        if (key.compare(0, prefix.size(), prefix) != 0 || (fullKey && key.size() != prefix.size()))
            continue;
        if (!KnowsConfigKey(family, key, codeTails))
            unknown.push_back(key);
    }
    return unknown;
}

// Reads every row into out, the member's initializer (or the family's Defaults) as the default: read(key,
// defaultValue, warnIfMissing) returns the value of defaultValue's type (uint32_t, int32_t, float, bool or
// std::string: ConfigReadType), warn(text) takes one warning. Only the family's own members are set, so several
// families (Hold, Focus, FocusTarget) may fill one struct; then the family's Finish runs. Returns the number of
// warnings.
template <typename Struct, typename Reader, typename Warn>
uint32_t LoadConfigFamily(ConfigFamily<Struct> const& family, Struct& out, Reader&& read, Warn&& warn)
{
    Struct const defaults = ConfigFamilyDefaults(family);
    uint32_t warnings = 0;
    for (ConfigRow<Struct> const& row : family.Rows)
    {
        if (row.Kind == ConfigKind::Retired)
            continue;

        std::string const key = std::string(family.Prefix) + row.Key;
        bool const warnIfMissing = IsListedConfigKind(row.Kind);
        std::visit(
            [&](auto member)
            {
                using T = ConfigTargetType<Struct, decltype(member)>;
                using R = ConfigReadType<T>;
                R const fallback = static_cast<R>(ConfigTargetValue(defaults, member));
                R value = read(key, fallback, warnIfMissing);
                if constexpr (std::is_same_v<T, std::string>)
                {
                    if (row.Parse && !row.Parse(value, out))
                    {
                        warn(key + " = '" + value + "' is not valid, keeping '" + fallback + "'");
                        ++warnings;
                        value = fallback;
                        row.Parse(fallback, out);  // the members the parser fills take the default too
                    }
                }
                else if constexpr (!std::is_same_v<T, bool>)
                {
                    // a uint8 also stays inside its type's range
                    double const typeUpper = std::is_same_v<T, uint8_t> ? 255.0 : row.Upper;
                    double const upper = row.Upper < typeUpper ? row.Upper : typeUpper;
                    double const number = static_cast<double>(value);
                    // written so that a NaN is outside (a plain < and > pass it)
                    if (!(number >= row.Lower && number <= upper))
                    {
                        R const kept = row.OutOfRange == ConfigOutOfRange::KeepDefault ? fallback
                                       : number < row.Lower                            ? static_cast<R>(row.Lower)
                                                                                       : static_cast<R>(upper);
                        warn(key + " = " + ConfigValueText(value) + " is outside " + ConfigBoundText(row.Lower) +
                             " to " + ConfigBoundText(upper) + ", using " + ConfigValueText(kept));
                        ++warnings;
                        value = kept;
                    }
                }
                ConfigTargetRef(out, member) = static_cast<T>(value);
            },
            row.Target);
    }
    if (family.Finish)
        family.Finish(out);
    return warnings;
}

#endif
