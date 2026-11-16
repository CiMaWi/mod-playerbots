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

template <typename Struct>
struct ConfigRow
{
    using Member =
        std::variant<uint32_t Struct::*, int32_t Struct::*, float Struct::*, bool Struct::*, std::string Struct::*>;
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
};

template <typename T>
std::string ConfigValueText(T const& value)
{
    if constexpr (std::is_same_v<T, std::string>)
        return value;
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
    return std::visit([&out](auto member) { return ConfigValueText(out.*member); }, row.Target);
}

// One line of a print command: "key = value (default D, bounds L to U, kind K)", with " *" after the value where it
// differs from the default (the struct's initializer). A retired row has no value and prints nothing.
template <typename Struct>
std::string ConfigRowLine(ConfigFamily<Struct> const& family, ConfigRow<Struct> const& row, Struct const& out)
{
    Struct const defaults{};
    std::string const value = ConfigRowValueText(row, out);
    std::string const defaultValue = ConfigRowValueText(row, defaults);
    bool const numeric = std::visit(
        [](auto member)
        {
            using T = std::remove_reference_t<decltype(std::declval<Struct>().*member)>;
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

// The keys of the list that start with the family's prefix and that no row of the family knows. A retired row knows its
// key: old configs may still set it.
template <typename Struct>
std::vector<std::string> UnknownConfigKeys(ConfigFamily<Struct> const& family, std::vector<std::string> const& keys)
{
    std::vector<std::string> unknown;
    std::string const prefix = family.Prefix;
    for (std::string const& key : keys)
    {
        if (key.compare(0, prefix.size(), prefix) != 0)
            continue;
        bool known = false;
        for (ConfigRow<Struct> const& row : family.Rows)
            if (key.compare(prefix.size(), std::string::npos, row.Key) == 0)
            {
                known = true;
                break;
            }
        if (!known)
            unknown.push_back(key);
    }
    return unknown;
}

// Resets out to the struct's initializers, then reads every row: read(key, defaultValue, warnIfMissing) returns the
// value of defaultValue's type (uint32_t, int32_t, float, bool or std::string), warn(text) takes one warning.
// Returns the number of warnings.
template <typename Struct, typename Reader, typename Warn>
uint32_t LoadConfigFamily(ConfigFamily<Struct> const& family, Struct& out, Reader&& read, Warn&& warn)
{
    out = Struct{};
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
                using T = std::remove_reference_t<decltype(out.*member)>;
                T const fallback = out.*member;
                T value = read(key, fallback, warnIfMissing);
                if constexpr (std::is_same_v<T, std::string>)
                {
                    if (row.Parse && !row.Parse(value, out))
                    {
                        warn(key + " = '" + value + "' is not valid, keeping '" + fallback + "'");
                        ++warnings;
                        value = fallback;
                    }
                }
                else if constexpr (!std::is_same_v<T, bool>)
                {
                    double const number = static_cast<double>(value);
                    if (number < row.Lower || number > row.Upper)
                    {
                        T const kept = row.OutOfRange == ConfigOutOfRange::KeepDefault ? fallback
                                       : number < row.Lower                            ? static_cast<T>(row.Lower)
                                                                                       : static_cast<T>(row.Upper);
                        warn(key + " = " + ConfigValueText(value) + " is outside " + ConfigBoundText(row.Lower) +
                             " to " + ConfigBoundText(row.Upper) + ", using " + ConfigValueText(kept));
                        ++warnings;
                        value = kept;
                    }
                }
                out.*member = value;
            },
            row.Target);
    }
    return warnings;
}

#endif
