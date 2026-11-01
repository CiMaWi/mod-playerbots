/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_COMPLEARNING_H
#define _PLAYERBOT_COMPLEARNING_H

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "Define.h"

// Comp learning for bot arena team formation (Playerbots.ArenaTeam.CompLearning.*, G6f). Pure: no core types, so the
// selftest (.playerbots arena comps selftest) and a standalone harness run the same code.
//
// The table is the hub's comp-strength.json (tools/comp-strength.js): per team-rating band, the win counts of each comp
// as the match records name it (sorted lower-case "class/spec" entries joined with "+"). A candidate's learned value
// for a team is the comp's shrunk win rate plus an upper-confidence-bound exploration bonus for comps with few games;
// the formation blends it into the partner score with a weight and picks by a softmax whose temperature follows the
// team's ladder percentile (strict at the top, forgiving at the bottom; never a sure pick).
namespace CompLearning
{
struct CompRow
{
    uint32 Games = 0;
    uint32 Wins = 0;
};

struct Band
{
    int32 Lo = 0;
    int32 Hi = 0;
    uint32 Games = 0;
    std::unordered_map<std::string, CompRow> Comps;
};

struct Table
{
    bool Loaded = false;
    float Prior = 8.0f;
    std::string GeneratedAt;
    uint32 Rows = 0;
    std::vector<Band> Bands;
};

// What the table says about one comp in one band (pooled over the specs of entries without one)
struct Lookup
{
    uint32 Games = 0;
    uint32 Wins = 0;
    bool Exact = true;
};

// The knobs at one ladder percentile
struct Params
{
    float Weight = 0.0f;
    float Explore = 0.0f;
    float Temperature = 0.1f;
};

inline float Lerp(float bottom, float top, float percentile)
{
    return bottom + (top - bottom) * std::clamp(percentile, 0.0f, 1.0f);
}

inline Params ParamsAt(float percentile, float weightBottom, float weightTop, float exploreBottom, float exploreTop,
                       float temperatureBottom, float temperatureTop)
{
    Params params;
    params.Weight = std::clamp(Lerp(weightBottom, weightTop, percentile), 0.0f, 1.0f);
    params.Explore = std::max(0.0f, Lerp(exploreBottom, exploreTop, percentile));
    params.Temperature = std::max(0.01f, Lerp(temperatureBottom, temperatureTop, percentile));
    return params;
}

// Lower case, sorted, joined with "+": the record's comp key
inline std::string CompKey(std::vector<std::string> entries)
{
    for (std::string& entry : entries)
    {
        std::transform(entry.begin(), entry.end(), entry.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    }

    std::sort(entries.begin(), entries.end());
    std::string key;
    for (std::string const& entry : entries)
        key += (key.empty() ? "" : "+") + entry;

    return key;
}

// Index of the band holding a team rating, or -1
inline int32 BandOf(Table const& table, uint32 rating)
{
    for (size_t i = 0; i < table.Bands.size(); ++i)
    {
        if (static_cast<int64>(rating) >= table.Bands[i].Lo && static_cast<int64>(rating) < table.Bands[i].Hi)
            return static_cast<int32>(i);
    }

    return -1;
}

// The band to learn from for a team rating: its own band, or when that has no games yet the nearest band that has
// some (the lower one on a tie); -1 when the table has no games at all
inline int32 DataBand(Table const& table, uint32 rating)
{
    int32 const own = BandOf(table, rating);
    if (own < 0)
        return -1;

    int32 const count = static_cast<int32>(table.Bands.size());
    for (int32 distance = 0; distance < count; ++distance)
    {
        for (int32 band : {own - distance, own + distance})
        {
            if (band >= 0 && band < count && table.Bands[band].Games)
                return band;
        }
    }

    return -1;
}

namespace Detail
{
// Splits a comp key into its entries
inline std::vector<std::string> SplitKey(std::string const& key)
{
    std::vector<std::string> parts;
    size_t start = 0;
    while (start <= key.size())
    {
        size_t const plus = key.find('+', start);
        size_t const end = plus == std::string::npos ? key.size() : plus;
        parts.push_back(key.substr(start, end - start));
        if (plus == std::string::npos)
            break;

        start = plus + 1;
    }

    return parts;
}

// An entry without a spec ("mage") matches every spec of its class ("mage/frost") and itself
inline bool EntryMatches(std::string const& pattern, std::string const& entry)
{
    if (pattern.find('/') != std::string::npos)
        return pattern == entry;

    return entry == pattern || (entry.size() > pattern.size() && entry.compare(0, pattern.size(), pattern) == 0 &&
                                entry[pattern.size()] == '/');
}

// True when the sorted entries can be matched one to one onto the patterns
inline bool KeyMatches(std::vector<std::string> const& patterns, std::vector<std::string> const& entries)
{
    if (patterns.size() != entries.size())
        return false;

    std::vector<bool> used(entries.size(), false);
    // Exact patterns first, so a wildcard never takes the entry an exact one needs
    for (int pass = 0; pass < 2; ++pass)
    {
        for (std::string const& pattern : patterns)
        {
            bool const exact = pattern.find('/') != std::string::npos;
            if (exact != (pass == 0))
                continue;

            bool found = false;
            for (size_t i = 0; i < entries.size() && !found; ++i)
            {
                if (!used[i] && EntryMatches(pattern, entries[i]))
                {
                    used[i] = true;
                    found = true;
                }
            }

            if (!found)
                return false;
        }
    }

    return true;
}
}  // namespace Detail

// The table's counts for a comp in a band. Entries without a spec pool every matching row (Exact false).
inline Lookup Find(Table const& table, int32 band, std::vector<std::string> const& entries)
{
    Lookup lookup;
    if (band < 0 || static_cast<size_t>(band) >= table.Bands.size())
        return lookup;

    Band const& rows = table.Bands[band];
    std::string const key = CompKey(entries);
    bool const exact = std::all_of(entries.begin(), entries.end(),
                                   [](std::string const& entry) { return entry.find('/') != std::string::npos; });
    if (exact)
    {
        auto const it = rows.Comps.find(key);
        if (it != rows.Comps.end())
        {
            lookup.Games = it->second.Games;
            lookup.Wins = it->second.Wins;
        }

        return lookup;
    }

    lookup.Exact = false;
    std::vector<std::string> const patterns = Detail::SplitKey(key);
    for (auto const& [compKey, row] : rows.Comps)
    {
        if (Detail::KeyMatches(patterns, Detail::SplitKey(compKey)))
        {
            lookup.Games += row.Games;
            lookup.Wins += row.Wins;
        }
    }

    return lookup;
}

// Shrunk win rate plus the exploration bonus, 0 .. 1. An untried comp counts as the prior mean (0.5) plus the full
// bonus; the bonus shrinks with the comp's games and grows slowly with the band's games (UCB1 shape).
inline float LearnedValue(Lookup const& lookup, float prior, uint32 bandGames, float explore)
{
    float const games = static_cast<float>(lookup.Games);
    float const shrunk = (static_cast<float>(lookup.Wins) + prior * 0.5f) / std::max(games + prior, 1.0f);
    float const bonus = explore * std::sqrt(std::log(static_cast<float>(bandGames) + 1.0f) / (games + 1.0f));
    return std::clamp(shrunk + bonus, 0.0f, 1.0f);
}

inline float Blend(float fit, float learned, float weight)
{
    float const w = std::clamp(weight, 0.0f, 1.0f);
    return fit * (1.0f - w) + learned * w;
}

// Softmax pick over scores; roll in [0, 1). Every option keeps a weight above 0, so no pick is ever certain.
inline size_t PickSoftmax(std::vector<float> const& scores, float temperature, float roll)
{
    if (scores.empty())
        return 0;

    float const best = *std::max_element(scores.begin(), scores.end());
    float const t = std::max(temperature, 0.01f);
    std::vector<float> weights;
    weights.reserve(scores.size());
    float total = 0.0f;
    for (float score : scores)
    {
        weights.push_back(std::exp((score - best) / t));
        total += weights.back();
    }

    float rest = std::clamp(roll, 0.0f, 1.0f) * total;
    for (size_t i = 0; i < weights.size(); ++i)
    {
        rest -= weights[i];
        if (rest < 0.0f)
            return i;
    }

    return weights.size() - 1;
}

// Index sets of size k from n items, in lexicographic order, at most cap of them
inline std::vector<std::vector<size_t>> Combinations(size_t n, size_t k, size_t cap)
{
    std::vector<std::vector<size_t>> sets;
    if (k > n || !cap)
        return sets;

    std::vector<size_t> current(k);
    for (size_t i = 0; i < k; ++i)
        current[i] = i;

    while (sets.size() < cap)
    {
        sets.push_back(current);
        if (!k)
            break;

        size_t i = k;
        while (i > 0 && current[i - 1] == n - k + i - 1)
            --i;

        if (!i)
            break;

        ++current[i - 1];
        for (size_t j = i; j < k; ++j)
            current[j] = current[j - 1] + 1;
    }

    return sets;
}

// The playing groups a candidate may form with a roster: every (teamSize - 1)-set of the members plus the candidate
struct GroupValue
{
    std::string Key;
    Lookup Counts;
    float Value = 0.5f;
};

// Mean learned value of the candidate's possible playing groups; false (value untouched) when the roster has fewer
// than teamSize - 1 members or the band is unknown
inline bool CandidateValue(Table const& table, int32 band, std::vector<std::string> const& members,
                           std::string const& candidate, uint32 teamSize, float explore, size_t maxGroups, float& value,
                           std::vector<GroupValue>* groups)
{
    if (!table.Loaded || band < 0 || static_cast<size_t>(band) >= table.Bands.size() || teamSize < 1 ||
        members.size() + 1 < teamSize)
        return false;

    uint32 const bandGames = table.Bands[band].Games;
    float sum = 0.0f;
    uint32 count = 0;
    for (std::vector<size_t> const& set : Combinations(members.size(), teamSize - 1, maxGroups))
    {
        std::vector<std::string> entries{candidate};
        for (size_t index : set)
            entries.push_back(members[index]);

        Lookup const lookup = Find(table, band, entries);
        float const learned = LearnedValue(lookup, table.Prior, bandGames, explore);
        sum += learned;
        ++count;
        if (groups)
            groups->push_back(GroupValue{CompKey(entries), lookup, learned});
    }

    if (!count)
        return false;

    value = sum / static_cast<float>(count);
    return true;
}

// Comp diversity of a set of weighted comp keys (a team's groups share its weight 1)
struct Diversity
{
    float Teams = 0.0f;
    uint32 Distinct = 0;
    float Effective = 0.0f;
    std::vector<std::pair<std::string, float>> Top;
};

inline Diversity Summarize(std::vector<std::pair<std::string, float>> const& weighted, size_t topCount)
{
    Diversity diversity;
    std::unordered_map<std::string, float> totals;
    for (auto const& [key, weight] : weighted)
    {
        totals[key] += weight;
        diversity.Teams += weight;
    }

    diversity.Distinct = static_cast<uint32>(totals.size());
    float entropy = 0.0f;
    for (auto const& [key, weight] : totals)
    {
        if (diversity.Teams > 0.0f && weight > 0.0f)
        {
            float const share = weight / diversity.Teams;
            entropy -= share * std::log(share);
        }

        diversity.Top.emplace_back(key, weight);
    }

    diversity.Effective = totals.empty() ? 0.0f : std::exp(entropy);
    std::sort(diversity.Top.begin(), diversity.Top.end(), [](auto const& a, auto const& b)
              { return a.second != b.second ? a.second > b.second : a.first < b.first; });
    if (diversity.Top.size() > topCount)
        diversity.Top.resize(topCount);

    return diversity;
}

// A tolerant reader for comp-strength.json: a small JSON parser (objects, arrays, strings, numbers, true/false/null);
// unknown keys are ignored, rows without comp, band or games are skipped.
namespace Json
{
struct Value
{
    enum class Kind
    {
        Null,
        Bool,
        Number,
        String,
        Array,
        Object
    };

    Kind Type = Kind::Null;
    bool Flag = false;
    double Number = 0.0;
    std::string Text;
    std::vector<Value> Items;
    std::vector<std::pair<std::string, Value>> Fields;

    Value const* Get(std::string const& key) const
    {
        for (auto const& [name, value] : Fields)
        {
            if (name == key)
                return &value;
        }

        return nullptr;
    }
};

class Parser
{
public:
    explicit Parser(std::string const& text) : _text(text) {}

    bool Parse(Value& out)
    {
        if (!ParseValue(out, 0))
            return false;

        SkipSpace();
        return _pos == _text.size();
    }

    size_t Position() const { return _pos; }

private:
    static constexpr uint32 MaxDepth = 32;

    void SkipSpace()
    {
        while (_pos < _text.size() && std::isspace(static_cast<unsigned char>(_text[_pos])))
            ++_pos;
    }

    bool Literal(char const* word)
    {
        size_t const length = std::char_traits<char>::length(word);
        if (_text.compare(_pos, length, word) != 0)
            return false;

        _pos += length;
        return true;
    }

    bool ParseString(std::string& out)
    {
        if (_pos >= _text.size() || _text[_pos] != '"')
            return false;

        ++_pos;
        while (_pos < _text.size())
        {
            char const c = _text[_pos++];
            if (c == '"')
                return true;

            if (c != '\\')
            {
                out += c;
                continue;
            }

            if (_pos >= _text.size())
                return false;

            char const escaped = _text[_pos++];
            switch (escaped)
            {
                case 'n':
                    out += '\n';
                    break;
                case 't':
                    out += '\t';
                    break;
                case 'r':
                    out += '\r';
                    break;
                case 'b':
                case 'f':
                    break;
                case 'u':
                    // Not needed by the table's keys: kept as '?'
                    if (_pos + 4 > _text.size())
                        return false;

                    _pos += 4;
                    out += '?';
                    break;
                default:
                    out += escaped;
                    break;
            }
        }

        return false;
    }

    bool ParseValue(Value& out, uint32 depth)
    {
        if (depth > MaxDepth)
            return false;

        SkipSpace();
        if (_pos >= _text.size())
            return false;

        char const c = _text[_pos];
        if (c == '{')
        {
            out.Type = Value::Kind::Object;
            ++_pos;
            SkipSpace();
            if (_pos < _text.size() && _text[_pos] == '}')
            {
                ++_pos;
                return true;
            }

            while (true)
            {
                SkipSpace();
                std::string name;
                if (!ParseString(name))
                    return false;

                SkipSpace();
                if (_pos >= _text.size() || _text[_pos] != ':')
                    return false;

                ++_pos;
                Value field;
                if (!ParseValue(field, depth + 1))
                    return false;

                out.Fields.emplace_back(std::move(name), std::move(field));
                SkipSpace();
                if (_pos < _text.size() && _text[_pos] == ',')
                {
                    ++_pos;
                    continue;
                }

                if (_pos < _text.size() && _text[_pos] == '}')
                {
                    ++_pos;
                    return true;
                }

                return false;
            }
        }

        if (c == '[')
        {
            out.Type = Value::Kind::Array;
            ++_pos;
            SkipSpace();
            if (_pos < _text.size() && _text[_pos] == ']')
            {
                ++_pos;
                return true;
            }

            while (true)
            {
                Value item;
                if (!ParseValue(item, depth + 1))
                    return false;

                out.Items.push_back(std::move(item));
                SkipSpace();
                if (_pos < _text.size() && _text[_pos] == ',')
                {
                    ++_pos;
                    continue;
                }

                if (_pos < _text.size() && _text[_pos] == ']')
                {
                    ++_pos;
                    return true;
                }

                return false;
            }
        }

        if (c == '"')
        {
            out.Type = Value::Kind::String;
            return ParseString(out.Text);
        }

        if (Literal("true") || Literal("false"))
        {
            out.Type = Value::Kind::Bool;
            out.Flag = _text[_pos - 1] == 'e' && _text.compare(_pos - 4, 4, "true") == 0;
            return true;
        }

        if (Literal("null"))
            return true;

        char const* start = _text.c_str() + _pos;
        char* end = nullptr;
        double const number = std::strtod(start, &end);
        if (end == start)
            return false;

        out.Type = Value::Kind::Number;
        out.Number = number;
        _pos += static_cast<size_t>(end - start);
        return true;
    }

    std::string const& _text;
    size_t _pos = 0;
};
}  // namespace Json

inline double NumberOr(Json::Value const* value, double fallback)
{
    return value && value->Type == Json::Value::Kind::Number ? value->Number : fallback;
}

// Parses comp-strength.json (format 1) into out; false with a reason (out untouched) when the text is not usable
inline bool ParseTable(std::string const& text, Table& out, std::string& error)
{
    Json::Value root;
    Json::Parser parser(text);
    if (!parser.Parse(root) || root.Type != Json::Value::Kind::Object)
    {
        error = "not a JSON object (stopped near byte " + std::to_string(parser.Position()) + ")";
        return false;
    }

    double const format = NumberOr(root.Get("format"), 0.0);
    if (format != 1.0)
    {
        error = "format " + std::to_string(static_cast<int32>(format)) + " is not 1";
        return false;
    }

    Json::Value const* bands = root.Get("bands");
    Json::Value const* comps = root.Get("comps");
    if (!bands || bands->Type != Json::Value::Kind::Array || bands->Items.empty() || !comps ||
        comps->Type != Json::Value::Kind::Array)
    {
        error = "bands or comps missing";
        return false;
    }

    Table table;
    table.Prior = static_cast<float>(std::max(0.0, NumberOr(root.Get("prior"), 8.0)));
    if (Json::Value const* generated = root.Get("generatedAt"))
        table.GeneratedAt = generated->Text;

    for (Json::Value const& item : bands->Items)
    {
        Band band;
        band.Lo = static_cast<int32>(NumberOr(item.Get("lo"), 0.0));
        band.Hi = static_cast<int32>(NumberOr(item.Get("hi"), 0.0));
        if (band.Hi <= band.Lo)
        {
            error = "a band with hi <= lo";
            return false;
        }

        table.Bands.push_back(std::move(band));
    }

    for (Json::Value const& item : comps->Items)
    {
        Json::Value const* comp = item.Get("comp");
        double const band = NumberOr(item.Get("band"), -1.0);
        double const games = NumberOr(item.Get("games"), -1.0);
        double const wins = NumberOr(item.Get("wins"), -1.0);
        if (!comp || comp->Type != Json::Value::Kind::String || comp->Text.empty() || band < 0.0 ||
            band >= static_cast<double>(table.Bands.size()) || games < 0.0 || wins < 0.0 || wins > games)
            continue;

        Band& target = table.Bands[static_cast<size_t>(band)];
        CompRow& row = target.Comps[CompKey(Detail::SplitKey(comp->Text))];
        row.Games += static_cast<uint32>(games);
        row.Wins += static_cast<uint32>(wins);
        target.Games += static_cast<uint32>(games);
        ++table.Rows;
    }

    table.Loaded = true;
    out = std::move(table);
    return true;
}

// The selftest's random source: a fixed-seed LCG, so a run is repeatable
class SelftestRandom
{
public:
    explicit SelftestRandom(uint64 seed) : _state(seed) {}

    float Next()
    {
        _state = _state * 6364136223846793005ULL + 1442695040888963407ULL;
        return static_cast<float>(_state >> 40) / 16777216.0f;
    }

private:
    uint64 _state;
};

// Self-checks of the pure code; appends one line per check to out, returns the failures
inline uint32 RunSelftest(std::vector<std::string>& out)
{
    uint32 failures = 0;
    auto check = [&](bool ok, std::string const& what)
    {
        out.push_back(std::string(ok ? "ok   " : "FAIL ") + what);
        if (!ok)
            ++failures;
    };
    auto fmt = [](char const* format, double a, double b = 0.0, double c = 0.0)
    {
        char buffer[160];
        std::snprintf(buffer, sizeof(buffer), format, a, b, c);
        return std::string(buffer);
    };

    // Parse: a sample in the file's layout, with an unknown key, escapes and a bad row
    std::string const sample =
        "{\n  \"format\": 1,\n  \"generatedAt\": \"2026-11-01T12:37:49Z\",\n"
        "  \"source\": {\"records\":623,\"note\":\"a \\\"quoted\\\" [note] {x}\",\"instances\":[\"arena1\"]},\n"
        "  \"prior\": 8,\n  \"bands\": [{\"lo\":0,\"hi\":500},{\"lo\":500,\"hi\":100000}],\n  \"comps\": [\n"
        "    {\"comp\":\"warlock/demo+paladin/prot\",\"band\":0,\"games\":40,\"wins\":30,\"shrunk\":0.71,\"rank\":1},\n"
        "    {\"comp\":\"mage/frost+priest/disc\",\"band\":0,\"games\":40,\"wins\":20,\"rank\":2},\n"
        "    {\"comp\":\"mage/fire+priest/disc\",\"band\":0,\"games\":20,\"wins\":10,\"rank\":3},\n"
        "    {\"comp\":\"rogue/sub+warrior/arms\",\"band\":0,\"games\":40,\"wins\":10,\"rank\":4},\n"
        "    {\"comp\":\"rogue/sub+warrior/arms\",\"band\":1,\"games\":5,\"wins\":4,\"rank\":1},\n"
        "    {\"comp\":\"bad\",\"band\":7,\"games\":5,\"wins\":4},\n"
        "    {\"band\":0,\"games\":5,\"wins\":4}\n  ]\n}\n";
    Table table;
    std::string error;
    bool const parsed = ParseTable(sample, table, error);
    check(parsed && table.Bands.size() == 2 && table.Rows == 5 && table.Prior == 8.0f &&
              table.GeneratedAt == "2026-11-01T12:37:49Z",
          "parse: 2 bands, 5 rows, prior 8 (bad rows skipped) " + error);
    check(parsed && table.Bands[0].Games == 140 && table.Bands[0].Comps.count("paladin/prot+warlock/demo") == 1,
          "parse: keys re-sorted, band games summed");
    Table untouched;
    check(!ParseTable("{\"format\":2,\"bands\":[],\"comps\":[]}", untouched, error) && !untouched.Loaded &&
              !ParseTable("{\"format\":1,", untouched, error),
          "parse: wrong format and cut file refused");

    // Keys, bands, lookups
    check(CompKey({"Warrior/Arms", "rogue/sub"}) == "rogue/sub+warrior/arms", "key: lower case, sorted");
    check(BandOf(table, 0) == 0 && BandOf(table, 499) == 0 && BandOf(table, 500) == 1 && BandOf(table, 3000) == 1,
          "band of rating");
    Table gap = table;
    gap.Bands[1].Comps.clear();
    gap.Bands[1].Games = 0;
    Table empty = gap;
    empty.Bands[0].Comps.clear();
    empty.Bands[0].Games = 0;
    check(DataBand(table, 3000) == 1 && DataBand(gap, 3000) == 0 && DataBand(empty, 3000) == -1,
          "data band: own band, else the nearest with games");
    Lookup const exact = Find(table, 0, {"priest/disc", "mage/frost"});
    Lookup const pooled = Find(table, 0, {"priest/disc", "mage"});
    Lookup const none = Find(table, 0, {"druid/resto", "mage/frost"});
    check(exact.Games == 40 && exact.Wins == 20 && exact.Exact, "find: exact row");
    check(pooled.Games == 60 && pooled.Wins == 30 && !pooled.Exact, "find: class-only entry pools its specs");
    check(none.Games == 0, "find: untried comp has no games");

    // Values: shrunk, bonus, blend with weight 0 leaves the fit unchanged (mode off)
    float const strong = LearnedValue(Find(table, 0, {"warlock/demo", "paladin/prot"}), 8.0f, 140, 0.0f);
    float const weak = LearnedValue(Find(table, 0, {"rogue/sub", "warrior/arms"}), 8.0f, 140, 0.0f);
    float const untried0 = LearnedValue(none, 8.0f, 140, 0.0f);
    float const untried1 = LearnedValue(none, 8.0f, 140, 0.2f);
    check(std::fabs(strong - 34.0f / 48.0f) < 1e-4f && std::fabs(weak - 14.0f / 48.0f) < 1e-4f &&
              std::fabs(untried0 - 0.5f) < 1e-4f,
          fmt("value: shrunk strong %.3f weak %.3f untried %.3f", strong, weak, untried0));
    check(untried1 > untried0 + 0.1f, fmt("value: exploration lifts the untried comp to %.3f", untried1));
    check(Blend(0.37f, 0.9f, 0.0f) == 0.37f, "blend: weight 0 keeps the fit");

    // Groups of a roster: 2v2 with two members gives two pairs; too few members gives no value
    std::vector<GroupValue> groups;
    float value = 0.0f;
    bool const grouped =
        CandidateValue(table, 0, {"paladin/prot", "warrior/arms"}, "warlock/demo", 2, 0.0f, 16, value, &groups);
    check(grouped && groups.size() == 2 && groups[0].Key == "paladin/prot+warlock/demo",
          fmt("groups: 2 pairs, mean %.3f", value));
    float unchanged = -1.0f;
    check(!CandidateValue(table, 0, {"paladin/prot"}, "warlock/demo", 3, 0.0f, 16, unchanged, nullptr) &&
              unchanged == -1.0f,
          "groups: a 3v3 roster of 1 has no group");
    check(Combinations(5, 2, 100).size() == 10 && Combinations(9, 4, 32).size() == 32 &&
              Combinations(1, 0, 4).size() == 1,
          "combinations: count and cap");

    // Picks: five candidates of equal fit, each making one comp with a paladin/prot captain
    std::vector<std::string> const candidates{"warlock/demo", "mage/frost", "mage/fire", "warrior/arms", "druid/resto"};
    Table picks;
    picks.Loaded = true;
    picks.Bands.push_back(Band{0, 100000, 0, {}});
    std::vector<std::pair<uint32, uint32>> const record{{60, 45}, {40, 20}, {20, 10}, {60, 15}, {0, 0}};
    for (size_t i = 0; i < candidates.size(); ++i)
    {
        picks.Bands[0].Comps[CompKey({"paladin/prot", candidates[i]})] = CompRow{record[i].first, record[i].second};
        picks.Bands[0].Games += record[i].first;
    }

    float const baseFit = 0.6f;
    auto simulate = [&](float percentile, uint64 seed)
    {
        Params const params = ParamsAt(percentile, 0.2f, 0.5f, 0.3f, 0.05f, 0.25f, 0.05f);
        std::vector<float> scores;
        for (std::string const& candidate : candidates)
        {
            float learned = 0.5f;
            CandidateValue(picks, 0, {"paladin/prot"}, candidate, 2, params.Explore, 16, learned, nullptr);
            scores.push_back(Blend(baseFit, learned, params.Weight));
        }

        SelftestRandom random(seed);
        std::vector<uint32> counts(candidates.size(), 0);
        for (uint32 i = 0; i < 4000; ++i)
            ++counts[PickSoftmax(scores, params.Temperature, random.Next())];

        return counts;
    };

    std::vector<uint32> const bottom = simulate(0.0f, 11);
    std::vector<uint32> const middle = simulate(0.5f, 12);
    std::vector<uint32> const top = simulate(1.0f, 13);
    auto share = [](std::vector<uint32> const& counts, size_t index)
    { return static_cast<float>(counts[index]) / 4000.0f; };
    for (auto const* counts : {&bottom, &middle, &top})
    {
        out.push_back(
            fmt("     picks at %.1f: ", counts == &bottom   ? 0.0
                                        : counts == &middle ? 0.5
                                                            : 1.0) +
            fmt("strong %.3f, frost %.3f, fire %.3f", share(*counts, 0), share(*counts, 1), share(*counts, 2)) +
            fmt(", weak %.3f, untried %.3f", share(*counts, 3), share(*counts, 4)));
    }

    check(share(bottom, 4) >= 0.10f && share(middle, 4) >= 0.05f && share(top, 4) >= 0.02f,
          fmt("untried comp picked sometimes (bottom %.3f, middle %.3f, top %.3f)", share(bottom, 4), share(middle, 4),
              share(top, 4)));
    check(share(top, 0) >= 0.5f && share(top, 0) > share(bottom, 0) + 0.2f,
          fmt("top mostly picks the strong comp (%.3f, bottom %.3f)", share(top, 0), share(bottom, 0)));
    bool someVariance = true;
    for (auto const* counts : {&bottom, &middle, &top})
        someVariance = someVariance && *std::max_element(counts->begin(), counts->end()) < 4000 * 9 / 10;

    check(someVariance && *std::min_element(bottom.begin(), bottom.end()) > 400 &&
              *std::min_element(top.begin(), top.end()) > 0,
          "no choice deterministic (best share below 90 % everywhere, every comp picked at the top)");
    check(share(bottom, 3) > share(top, 3),
          fmt("the bottom forgives the weak comp (%.3f > %.3f)", share(bottom, 3), share(top, 3)));
    return failures;
}
}  // namespace CompLearning

#endif
