/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_BRIDGEKNOWLEDGERULES_H
#define PLAYERBOTS_BRIDGEKNOWLEDGERULES_H

// LC18a: what a bot knows of the world's places (§3.3, LC.5) and the words of the `where` topic, Player-free. The
// reader in BridgeBotState.cpp turns the bot, its record and the planner indexes into the plain data below on the
// world thread; this header decides what the bot knows and words the answer. Only the reader, the order resolver and
// tests/bridge-knowledge-selftest.cpp include it.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "BridgeBotState.h"
#include "BridgeNames.h"
#include "BridgeTexts.h"
#include "PlannerIndexes.h"

namespace PlayerbotBridge::Knowledge
{
enum class Side : std::uint8_t
{
    Neither,  // a contested zone or a sanctuary
    Alliance,
    Horde
};

inline Side SideOf(Planner::ZoneSide side)
{
    if (side == Planner::ZoneSide::Alliance)
        return Side::Alliance;
    if (side == Planner::ZoneSide::Horde)
        return Side::Horde;
    return Side::Neither;
}

// A player's side from its TeamId (0 Alliance, 1 Horde)
inline Side SideOfTeam(std::uint32_t teamId) { return teamId == 0 ? Side::Alliance : Side::Horde; }

// The capitals' zone ids: Stormwind, Ironforge, Darnassus, the Exodar; Orgrimmar, Thunder Bluff, the Undercity,
// Silvermoon
constexpr std::array<std::uint32_t, 4> ALLIANCE_CAPITALS = {1519, 1537, 1657, 3557};
constexpr std::array<std::uint32_t, 4> HORDE_CAPITALS = {1637, 1638, 1497, 3487};

// A known flight point counts for a place when it lies on the place's map within the place's radius and this margin
// (a flight master stands within a few yards of its node)
constexpr float FLIGHT_POINT_MARGIN_YARDS = 50.0f;

// The upper bounds of the distance words' bands (BridgeTexts::Lookups::WHERE_DISTANCES); farther is the last word
constexpr std::array<float, 3> DISTANCE_BANDS_YARDS = {100.0f, 500.0f, 2000.0f};

// The most lines a `where` answer holds
constexpr std::size_t WHERE_SHOWN = 3;

struct MapPoint
{
    std::uint32_t mapId{0};
    float x{0.0f};
    float y{0.0f};
    float z{0.0f};
};

// What the reader knows of the bot. A stay is a zone of the record's living rows (RecordWords History's notion) or
// the zone the bot stands in; flight points are the positions of the taxi nodes it knows.
struct BotKnowledge
{
    Side side{Side::Alliance};
    std::uint32_t level{1};
    std::vector<std::uint32_t> stayedZones;
    std::vector<MapPoint> flightPoints;
};

// The place the rule judges: a town (a planner hub) or, for a spawn outside one, its own spot
struct PlaceFact
{
    std::uint32_t zoneId{0};
    Side side{Side::Neither};
    std::uint32_t lowLevel{0};  // where the zone's level range starts; 0 = the zone has none
    MapPoint at;
    float radius{0.0f};
};

inline bool IsCapitalOf(Side side, std::uint32_t zoneId)
{
    if (side == Side::Alliance)
        return std::find(ALLIANCE_CAPITALS.begin(), ALLIANCE_CAPITALS.end(), zoneId) != ALLIANCE_CAPITALS.end();
    if (side == Side::Horde)
        return std::find(HORDE_CAPITALS.begin(), HORDE_CAPITALS.end(), zoneId) != HORDE_CAPITALS.end();
    return false;
}

inline float Yards(MapPoint const& from, MapPoint const& to)
{
    float const dx = to.x - from.x;
    float const dy = to.y - from.y;
    float const dz = to.z - from.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

inline bool HasFlightPoint(BotKnowledge const& bot, PlaceFact const& place)
{
    for (MapPoint const& point : bot.flightPoints)
        if (point.mapId == place.at.mapId && Yards(point, place.at) <= place.radius + FLIGHT_POINT_MARGIN_YARDS)
            return true;
    return false;
}

// §3.3: a place is known when the bot's record shows a stay in its zone, or the bot has its flight point, or it
// belongs to the bot's side (or to neither) in a zone whose level range starts at or below the bot's level; its side's
// capitals are always known. A zone without a level range is known by the first three only (LC.5: never invent).
inline bool Knows(BotKnowledge const& bot, PlaceFact const& place)
{
    if (std::find(bot.stayedZones.begin(), bot.stayedZones.end(), place.zoneId) != bot.stayedZones.end())
        return true;
    if (HasFlightPoint(bot, place))
        return true;
    if (IsCapitalOf(bot.side, place.zoneId))
        return true;
    bool const sideAllows = place.side == Side::Neither || place.side == bot.side;
    return sideAllows && place.lowLevel != 0 && place.lowLevel <= bot.level;
}

// The rule's facts of a planner hub: its zone's side and level range, its centre and radius
inline PlaceFact PlaceOfHub(Planner::PlannerIndexes const& index, Planner::Hub const& hub)
{
    PlaceFact place;
    place.zoneId = hub.zoneId;
    place.at = {hub.mapId, hub.x, hub.y, hub.z};
    place.radius = hub.radius;
    if (Planner::ZoneRecord const* zone = index.FindZone(hub.zoneId))
    {
        place.side = SideOf(zone->side);
        place.lowLevel = zone->low;
    }
    return place;
}

// A service is known with its town; one outside a town is judged at its own spot
inline PlaceFact PlaceOfService(Planner::PlannerIndexes const& index, Planner::Service const& service)
{
    if (service.hub < index.hubs.size())
        return PlaceOfHub(index, index.hubs[service.hub]);
    PlaceFact place;
    place.zoneId = service.zoneId;
    place.at = {service.mapId, service.x, service.y, service.z};
    if (Planner::ZoneRecord const* zone = index.FindZone(service.zoneId))
    {
        place.side = SideOf(zone->side);
        place.lowLevel = zone->low;
    }
    return place;
}

static_assert(BridgeTexts::Lookups::WHERE_SERVICES.size() == Planner::SERVICE_KINDS,
              "a where service word per planner service kind");

// The service a question names in plain words, if it names one
inline std::optional<Planner::ServiceKind> AskedService(std::string_view text)
{
    std::string const key = NameKey(text);
    if (key.empty())
        return std::nullopt;
    for (std::size_t kind = 0; kind < Planner::SERVICE_KINDS; ++kind)
        for (char const* asked : BridgeTexts::Lookups::WHERE_SERVICES[kind].Asked)
            if (*asked && NameKey(asked) == key)
                return static_cast<Planner::ServiceKind>(kind);
    return std::nullopt;
}

inline char const* ServiceWhat(Planner::ServiceKind kind)
{
    return BridgeTexts::Lookups::WHERE_SERVICES[static_cast<std::size_t>(kind)].What;
}

inline char const* DistanceWord(float yards)
{
    for (std::size_t band = 0; band < DISTANCE_BANDS_YARDS.size(); ++band)
        if (yards < DISTANCE_BANDS_YARDS[band])
            return BridgeTexts::Lookups::WHERE_DISTANCES[band];
    return BridgeTexts::Lookups::WHERE_DISTANCES.back();
}

// The compass word from one point to another on a map: the game's x runs north, its y west
inline char const* DirectionWord(MapPoint const& from, MapPoint const& to)
{
    constexpr float SECTOR_RADIANS = 3.14159265f / 4.0f;
    float const bearing = std::atan2(from.y - to.y, to.x - from.x);  // clockwise from north
    int const sector = static_cast<int>(std::lround(bearing / SECTOR_RADIANS));
    return BridgeTexts::Lookups::WHERE_DIRECTIONS[static_cast<std::size_t>((sector + 8) % 8)];
}

// One thing a `where` question may mean: a person (a service spawn) or a town
struct WhereCandidate
{
    std::string name;
    std::string what;   // a service word (ServiceWhat) or WHERE_TOWN
    std::string place;  // the town or zone it is in
    MapPoint at;
    PlaceFact fact;  // what the rule judges: its town, or its own spot
};

// The row kinds (BotStateItem::code) of the where topic
enum class WhereRow : std::uint8_t
{
    Found,    // name = the thing's name, value = yards on the bot's map (0 on another map)
    Unknown,  // nothing the bot knows: one line, no name
    Pending   // the planner indexes are not built yet: one line
};

inline char const* RowWord(WhereRow row)
{
    namespace L = BridgeTexts::Lookups;
    switch (row)
    {
        case WhereRow::Found:
            return L::ROW_WHERE_FOUND;
        case WhereRow::Unknown:
            return L::ROW_WHERE_UNKNOWN;
        case WhereRow::Pending:
            return L::ROW_WHERE_PENDING;
    }
    return "";
}

// A line of its own: the unknown and pending lines
inline BotStateItem OneLine(WhereRow row, std::string text, std::string name = {})
{
    // the channel drops an entry without a name: the row's own word stands in, never the name asked
    if (name.empty())
        name = RowWord(row);
    return {std::move(name), 0, 0, static_cast<std::uint8_t>(row), std::move(text), RowWord(row)};
}

inline void WherePending(std::vector<BotStateItem>& items)
{
    items.clear();
    items.push_back(OneLine(WhereRow::Pending, BridgeTexts::Lookups::WHERE_PENDING));
}

inline std::string WhereText(WhereCandidate const& thing, MapPoint const& from)
{
    namespace L = BridgeTexts::Lookups;
    std::string text = thing.name;
    if (NameKey(thing.what) != NameKey(thing.name))
        text.append(L::WHERE_PART).append(thing.what);
    text.append(L::WHERE_IN).append(thing.place).append(L::WHERE_FAR_PART);
    if (thing.at.mapId != from.mapId)
        return text.append(L::WHERE_OTHER_CONTINENT).append(L::WHERE_END);
    return text.append(DistanceWord(Yards(from, thing.at)))
        .append(L::WHERE_TOWARDS)
        .append(DirectionWord(from, thing.at))
        .append(L::WHERE_END);
}

// The answer: the candidates the bot knows, those on its map nearest first, the others after them; at most
// WHERE_SHOWN lines. Nothing known: one line that does not repeat the name asked.
inline void WhereItems(BotKnowledge const& bot, MapPoint const& from, std::vector<WhereCandidate> candidates,
                       std::vector<BotStateItem>& items)
{
    items.clear();
    std::vector<std::pair<float, WhereCandidate const*>> known;
    for (WhereCandidate const& thing : candidates)
        if (Knows(bot, thing.fact))
            known.emplace_back(thing.at.mapId == from.mapId ? Yards(from, thing.at) : -1.0f, &thing);
    auto const order = [](auto const& a, auto const& b)
    {
        bool const aHere = a.first >= 0.0f;
        bool const bHere = b.first >= 0.0f;
        return aHere != bHere ? aHere : a.first < b.first;
    };
    std::stable_sort(known.begin(), known.end(), order);
    if (known.size() > WHERE_SHOWN)
        known.resize(WHERE_SHOWN);
    for (auto const& [yards, thing] : known)
        items.push_back({thing->name, yards >= 0.0f ? static_cast<std::uint32_t>(yards) : 0, 0,
                         static_cast<std::uint8_t>(WhereRow::Found), WhereText(*thing, from),
                         RowWord(WhereRow::Found)});
    if (items.empty())
        items.push_back(OneLine(WhereRow::Unknown, BridgeTexts::Lookups::WHERE_UNKNOWN));
}

// Whether a name asked (as NameKey) fits a name: the same, or one holding the other from MinPartialName letters on
// (BridgeNames.h ResolveName's rule)
inline bool NameFits(std::string const& key, std::string_view name)
{
    std::string const other = NameKey(name);
    if (key.empty() || other.empty())
        return false;
    return other == key || (key.size() >= MinPartialName && other.size() >= MinPartialName &&
                            (other.find(key) != std::string::npos || key.find(other) != std::string::npos));
}

// The towns as candidates: each hub under the name of its area and, once per zone (the hub nearest the bot), under
// the zone's name. nameOf(id) is the game's name of an area or zone id, empty for none.
template <typename NameOf>
std::vector<WhereCandidate> TownCandidates(Planner::PlannerIndexes const& index, MapPoint const& from,
                                           NameOf const& nameOf)
{
    std::vector<WhereCandidate> towns;
    std::vector<std::pair<std::uint32_t, std::size_t>> zoneAt;  // zone id -> its candidate in towns
    for (Planner::Hub const& hub : index.hubs)
    {
        std::string const zone = nameOf(hub.zoneId);
        if (zone.empty())
            continue;
        MapPoint const centre{hub.mapId, hub.x, hub.y, hub.z};
        PlaceFact const fact = PlaceOfHub(index, hub);
        std::string const area = nameOf(hub.areaId);
        if (!area.empty() && area != zone)
            towns.push_back({area, BridgeTexts::Lookups::WHERE_TOWN, zone, centre, fact});
        auto const seen =
            std::find_if(zoneAt.begin(), zoneAt.end(), [&hub](auto const& z) { return z.first == hub.zoneId; });
        if (seen == zoneAt.end())
        {
            zoneAt.emplace_back(hub.zoneId, towns.size());
            towns.push_back({zone, BridgeTexts::Lookups::WHERE_TOWN, zone, centre, fact});
        }
        else if (centre.mapId == from.mapId && (towns[seen->second].at.mapId != from.mapId ||
                                                Yards(from, centre) < Yards(from, towns[seen->second].at)))
            towns[seen->second] = {zone, BridgeTexts::Lookups::WHERE_TOWN, zone, centre, fact};
    }
    return towns;
}

// A town the bot knows, named by text (the same rule as a `where` question): its centre as "x,y,z,map", the order
// resolver's literal; nothing when none fits or none is known. An exact name wins over a part; then the nearer.
inline std::optional<std::string> TownSpot(BotKnowledge const& bot, std::vector<WhereCandidate> const& towns,
                                           MapPoint const& from, std::string_view text)
{
    std::string const key = NameKey(text);
    WhereCandidate const* best = nullptr;
    bool bestExact = false;
    float bestYards = 0.0f;
    for (WhereCandidate const& town : towns)
    {
        if (!NameFits(key, town.name) || !Knows(bot, town.fact))
            continue;
        bool const exact = NameKey(town.name) == key;
        float const yards = town.at.mapId == from.mapId ? Yards(from, town.at) : 1.0e9f;
        if (!best || (exact && !bestExact) || (exact == bestExact && yards < bestYards))
        {
            best = &town;
            bestExact = exact;
            bestYards = yards;
        }
    }
    if (!best)
        return std::nullopt;
    char spot[96];
    std::snprintf(spot, sizeof(spot), "%.2f,%.2f,%.2f,%u", best->at.x, best->at.y, best->at.z, best->at.mapId);
    return std::string(spot);
}
}  // namespace PlayerbotBridge::Knowledge

#endif
