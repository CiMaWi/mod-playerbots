/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// Offline proof of what a bot knows of the world's places and of the `where` answer (living-conversation LC18a,
// BridgeKnowledgeRules.h): each clause of the rule, the words, the line's form, then a search for a counterexample.
// The search builds a synthetic world through the planner index's own build (towns of both sides, contested zones,
// sanctuaries, capitals, zones without a level range, on four maps), runs the answer for every town and every sample
// bot (side x level x flight points x stays) and checks each named town against an oracle that is a hand-written
// table, not the rule. Brackets are set for the boundaries the grid of levels hits, not the game's.
// Build (Git Bash, module root, inside vcvars64): clang-cl /std:c++20 /EHsc /W4 /WX /I src/Bot /I src/Ai/World/Rpg
// tests/bridge-knowledge-selftest.cpp

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

#include "BridgeKnowledgeRules.h"

namespace
{
using namespace PlayerbotBridge;
using namespace PlayerbotBridge::Knowledge;
namespace L = BridgeTexts::Lookups;

int passed = 0;
int failed = 0;

void Check(bool pass, char const* name)
{
    std::printf("%s %s\n", pass ? "PASS" : "FAIL", name);
    (pass ? passed : failed)++;
}

bool HasDigit(std::string_view text)
{
    for (char const c : text)
        if (c >= '0' && c <= '9')
            return true;
    return false;
}

bool IsRowWord(std::string_view word)
{
    if (word.empty() || word.size() > 64)
        return false;
    for (char const c : word)
        if (!((c >= 'a' && c <= 'z') || c == '_'))
            return false;
    return true;
}

bool Holds(std::string_view text, std::string_view part) { return text.find(part) != std::string_view::npos; }

PlaceFact Place(std::uint32_t zone, Side side, std::uint32_t low, MapPoint at = {0, 100.0f, 100.0f, 0.0f},
                float radius = 40.0f)
{
    return {zone, side, low, at, radius};
}

BotKnowledge Bot(Side side, std::uint32_t level) { return {side, level, {}, {}}; }

void RuleChecks()
{
    BotKnowledge const ally = Bot(Side::Alliance, 12);
    Check(Knows(ally, Place(40, Side::Alliance, 10)), "own side, range starts below the level: known");
    Check(Knows(ally, Place(40, Side::Alliance, 12)), "own side, range starts at the level: known");
    Check(!Knows(ally, Place(40, Side::Alliance, 13)), "own side, range starts above the level: unknown");
    Check(Knows(ally, Place(33, Side::Neither, 12)), "neither side, range starts at the level: known");
    Check(!Knows(ally, Place(17, Side::Horde, 10)), "the other side, range below the level: unknown");
    Check(!Knows(ally, Place(36, Side::Alliance, 0)), "a zone without a level range: unknown by level");
    Check(!Knows(Bot(Side::Alliance, 80), Place(4395, Side::Neither, 0)), "a sanctuary without a range: unknown");

    BotKnowledge stayed = ally;
    stayed.stayedZones = {17};
    Check(Knows(stayed, Place(17, Side::Horde, 10)), "a stay in the zone: known on the other side");
    Check(!Knows(stayed, Place(14, Side::Horde, 5)), "a stay elsewhere: the other side stays unknown");

    BotKnowledge flown = ally;
    flown.flightPoints = {{0, 180.0f, 100.0f, 0.0f}};
    Check(Knows(flown, Place(17, Side::Horde, 60)), "a flight point within the radius and margin: known");
    flown.flightPoints = {{0, 191.0f, 100.0f, 0.0f}};
    Check(!Knows(flown, Place(17, Side::Horde, 60)), "a flight point beyond the radius and margin: unknown");
    flown.flightPoints = {{1, 100.0f, 100.0f, 0.0f}};
    Check(!Knows(flown, Place(17, Side::Horde, 60)), "a flight point on another map: unknown");

    Check(Knows(Bot(Side::Alliance, 1), Place(1519, Side::Alliance, 0)), "own capital at level one: known");
    Check(Knows(Bot(Side::Alliance, 1), Place(1657, Side::Alliance, 10)), "own capital above its range: known");
    Check(Knows(Bot(Side::Horde, 1), Place(3487, Side::Horde, 0)), "a horde capital to the horde: known");
    Check(!Knows(Bot(Side::Horde, 80), Place(1519, Side::Alliance, 0)), "the other side's capital: unknown");
    Check(SideOfTeam(0) == Side::Alliance && SideOfTeam(1) == Side::Horde, "team ids to sides");
    Check(
        SideOf(Planner::ZoneSide::Sanctuary) == Side::Neither && SideOf(Planner::ZoneSide::Contested) == Side::Neither,
        "sanctuaries and contested zones belong to neither side");
}

void WordChecks()
{
    Check(std::string_view(DistanceWord(99.0f)) == "close by" && std::string_view(DistanceWord(100.0f)) == "near" &&
              std::string_view(DistanceWord(1999.0f)) == "far" && std::string_view(DistanceWord(2000.0f)) == "very far",
          "distance bands");
    MapPoint const at{0, 0.0f, 0.0f, 0.0f};
    Check(std::string_view(DirectionWord(at, {0, 50.0f, 0.0f, 0.0f})) == "north" &&
              std::string_view(DirectionWord(at, {0, 0.0f, -50.0f, 0.0f})) == "east" &&
              std::string_view(DirectionWord(at, {0, -50.0f, 0.0f, 0.0f})) == "south" &&
              std::string_view(DirectionWord(at, {0, 0.0f, 50.0f, 0.0f})) == "west" &&
              std::string_view(DirectionWord(at, {0, 50.0f, -50.0f, 0.0f})) == "north-east" &&
              std::string_view(DirectionWord(at, {0, -50.0f, 50.0f, 0.0f})) == "south-west",
          "compass words: the game's x runs north, its y west");

    Check(AskedService("Inn") == Planner::ServiceKind::Innkeeper &&
              AskedService("Flight Master") == Planner::ServiceKind::FlightMaster &&
              AskedService("auction-house") == Planner::ServiceKind::Auctioneer &&
              AskedService("bank") == Planner::ServiceKind::Banker &&
              AskedService("repairs") == Planner::ServiceKind::Repairer &&
              AskedService("mailbox") == Planner::ServiceKind::Mailbox &&
              AskedService("quest giver") == Planner::ServiceKind::QuestGiver &&
              AskedService("trainer") == Planner::ServiceKind::Trainer &&
              AskedService("vendor") == Planner::ServiceKind::Vendor,
          "the plan's service words name their kinds");
    Check(!AskedService("Goldshire") && !AskedService("") && !AskedService("?"), "a name that is no service word");
    bool roundTrip = true;
    bool clean = true;
    for (std::size_t kind = 0; kind < Planner::SERVICE_KINDS; ++kind)
    {
        for (char const* asked : L::WHERE_SERVICES[kind].Asked)
            if (*asked)
                roundTrip &= AskedService(asked) == static_cast<Planner::ServiceKind>(kind);
        clean &= *L::WHERE_SERVICES[kind].What && !HasDigit(L::WHERE_SERVICES[kind].What);
    }
    Check(roundTrip, "every asked word names its own kind, none two");
    Check(std::string_view(ServiceWhat(Planner::ServiceKind::Innkeeper)) == "innkeeper" &&
              std::string_view(ServiceWhat(Planner::ServiceKind::FlightMaster)) == "flight master" &&
              std::string_view(ServiceWhat(Planner::ServiceKind::QuestGiver)) == "quest giver",
          "the service words follow the planner's kinds");

    for (char const* text :
         {L::WHERE_SENTENCE, L::WHERE_UNKNOWN, L::WHERE_PENDING, L::WHERE_TOWN, L::WHERE_OTHER_CONTINENT})
        clean &= !HasDigit(text);
    for (char const* word : L::WHERE_DISTANCES)
        clean &= *word && !HasDigit(word);
    for (char const* word : L::WHERE_DIRECTIONS)
        clean &= *word && !HasDigit(word);
    Check(clean, "no where text holds a digit");
    Check(IsRowWord(RowWord(WhereRow::Found)) && IsRowWord(RowWord(WhereRow::Unknown)) &&
              IsRowWord(RowWord(WhereRow::Pending)) && std::string_view(RowWord(WhereRow::Found)) != "unknown",
          "row words of [a-z_], one per kind of line");
}

void AnswerChecks()
{
    BotKnowledge const bot = Bot(Side::Alliance, 12);
    MapPoint const from{0, 0.0f, 0.0f, 0.0f};
    PlaceFact const goldshire = Place(12, Side::Alliance, 5);
    PlaceFact const crossroads = Place(17, Side::Horde, 10, {1, 0.0f, 0.0f, 0.0f});
    std::vector<WhereCandidate> const things = {
        {"Innkeeper Farley", "innkeeper", "Goldshire", {0, 300.0f, -300.0f, 0.0f}, goldshire},
        {"Mailbox", "mailbox", "Goldshire", {0, 50.0f, 0.0f, 0.0f}, goldshire},
        {"Innkeeper Boorand", "innkeeper", "the Crossroads", {1, 10.0f, 0.0f, 0.0f}, crossroads},
        {"Innkeeper Allison", "innkeeper", "Stormwind", {0, 3000.0f, 0.0f, 0.0f}, Place(1519, Side::Alliance, 0)},
        {"Innkeeper Heather", "innkeeper", "Kharanos", {1, 0.0f, 0.0f, 0.0f}, Place(1, Side::Alliance, 5)},
        {"Innkeeper Firebrew", "innkeeper", "Ironforge", {0, -900.0f, 0.0f, 0.0f}, Place(1537, Side::Alliance, 0)}};

    std::vector<BotStateItem> items;
    WhereItems(bot, from, {things[0]}, items);
    Check(items.size() == 1 && items[0].text == "Innkeeper Farley, innkeeper in Goldshire; near, to the north-east." &&
              items[0].name == "Innkeeper Farley" && items[0].word == "found" && items[0].value == 424,
          "a found line: name, what, town, how far, direction");
    WhereItems(bot, from, {things[1]}, items);
    Check(items.size() == 1 && items[0].text == "Mailbox in Goldshire; close by, to the north.",
          "a thing named by what it is is not named twice");
    WhereItems(bot, from, {things[4]}, items);
    Check(items.size() == 1 && items[0].text == "Innkeeper Heather, innkeeper in Kharanos; on another continent.",
          "another continent: no direction");
    WhereItems(bot, from, {things[2]}, items);
    Check(items.size() == 1 && items[0].word == "unknown" && items[0].name.empty() &&
              items[0].text == L::WHERE_UNKNOWN && !Holds(items[0].text, "Boorand") &&
              !Holds(items[0].text, "Crossroads"),
          "the other side's town: one unknown line without the name");
    WhereItems(bot, from, {}, items);
    Check(items.size() == 1 && items[0].word == "unknown", "no candidate: one unknown line");
    WhereItems(bot, from, things, items);
    Check(items.size() == WHERE_SHOWN && items[0].name == "Mailbox" && items[1].name == "Innkeeper Farley" &&
              items[2].name == "Innkeeper Firebrew",
          "known things on the bot's map nearest first, cut to three, the other side left out");
    std::vector<WhereCandidate> const far = {things[4], things[3]};
    WhereItems(bot, from, far, items);
    Check(items.size() == 2 && items[0].name == "Innkeeper Allison" && items[1].name == "Innkeeper Heather",
          "other maps after the bot's own");
    WherePending(items);
    Check(items.size() == 1 && items[0].word == "pending" && items[0].text == L::WHERE_PENDING,
          "not ready: one pending line");

    Planner::GatherInput input;
    input.zones.push_back({12, 5, 12, Planner::ZoneSide::Alliance, 0});
    input.zones.push_back({17, 10, 25, Planner::ZoneSide::Horde, 1});
    input.spawns.push_back({1, 500.0f, 500.0f, 0.0f, 7, Planner::NpcFlag::VENDOR, 0, 17, 380, 0, false});
    Planner::PlannerIndexes const index = Planner::PlannerIndexes::Build(input);
    PlaceFact const lone = PlaceOfService(index, index.services.front());
    Check(index.hubs.empty() && lone.zoneId == 17 && lone.side == Side::Horde && lone.lowLevel == 10 &&
              lone.at.mapId == 1 && lone.radius == 0.0f,
          "a service outside a town is judged at its own spot");
}

// The oracle: a town of the synthetic world and the lowest level each side knows it at by level or capital, set by
// hand from the plan's rule (NEVER: not by level)
constexpr std::uint32_t NEVER = 1000;

struct Town
{
    char const* name;
    char const* zoneName;
    std::uint32_t zone;
    Planner::ZoneSide side;
    std::uint32_t low;
    std::uint32_t map;
    std::uint32_t minAlliance;
    std::uint32_t minHorde;
};

using ZS = Planner::ZoneSide;
constexpr Town TOWNS[] = {
    {"Stormwind", "Stormwind City", 1519, ZS::Alliance, 0, 0, 0, NEVER},
    {"Ironforge", "Ironforge City", 1537, ZS::Alliance, 0, 0, 0, NEVER},
    {"Darnassus", "Darnassus City", 1657, ZS::Alliance, 10, 1, 0, NEVER},
    {"Exodar", "Exodar City", 3557, ZS::Alliance, 0, 530, 0, NEVER},
    {"Orgrimmar", "Orgrimmar City", 1637, ZS::Horde, 0, 1, NEVER, 0},
    {"Thunder Bluff", "Thunder Bluff City", 1638, ZS::Horde, 0, 1, NEVER, 0},
    {"Undercity", "Undercity Ruins", 1497, ZS::Horde, 10, 0, NEVER, 0},
    {"Silvermoon", "Silvermoon City", 3487, ZS::Horde, 0, 530, NEVER, 0},
    {"Goldshire", "Elwynn Forest", 12, ZS::Alliance, 5, 0, 5, NEVER},
    {"Razor Hill", "Durotar", 14, ZS::Horde, 5, 1, NEVER, 5},
    {"Sentinel Hill", "Westfall", 40, ZS::Alliance, 10, 0, 10, NEVER},
    {"Crossroads", "The Barrens", 17, ZS::Horde, 10, 1, NEVER, 10},
    {"Bloodhoof Village", "Mulgore", 215, ZS::Horde, 1, 1, NEVER, 1},
    {"Booty Bay", "Stranglethorn Vale", 33, ZS::Contested, 32, 0, 32, 32},
    {"Darkshire", "Duskwood", 10, ZS::Contested, 19, 0, 19, 19},
    {"Nighthaven", "Moonglade", 493, ZS::Sanctuary, 15, 1, 15, 15},
    {"Strahnbrad", "Alterac Mountains", 36, ZS::Alliance, 0, 0, NEVER, NEVER},
    {"Karazhan Gate", "Deadwind Pass", 41, ZS::Contested, 0, 0, NEVER, NEVER},
    {"Honor Hold", "Hellfire Peninsula", 3483, ZS::Contested, 58, 530, 58, 58},
    {"Telredor", "Zangarmarsh", 3521, ZS::Contested, 61, 530, 61, 61},
    {"Shattrath", "Terokkar", 3703, ZS::Sanctuary, 0, 530, NEVER, NEVER},
    {"Valiance Keep", "Borean Tundra", 3537, ZS::Alliance, 71, 571, 71, NEVER},
    {"Vengeance Landing", "Howling Fjord", 495, ZS::Horde, 71, 571, NEVER, 71},
    {"Dalaran", "Crystalsong", 4395, ZS::Sanctuary, 0, 571, NEVER, NEVER},
    {"Bouldercrag", "Storm Peaks", 67, ZS::Contested, 80, 571, 80, 80},
};
constexpr std::size_t TOWN_COUNT = sizeof(TOWNS) / sizeof(TOWNS[0]);
constexpr std::uint32_t LEVELS[] = {1, 5, 10, 11, 20, 30, 40, 59, 60, 61, 70, 71, 80};
constexpr float TOWN_SPACING_YARDS = 2500.0f;
constexpr float FLIGHT_X = 30.0f;  // the flight master's spot from the town's first spawn
constexpr float FLIGHT_Y = 10.0f;

struct World
{
    Planner::PlannerIndexes index;
    std::vector<std::size_t> hubOf;  // the town's hub
    std::vector<MapPoint> centre;    // the town's first spawn
    std::vector<std::size_t> innOf;  // the town's innkeeper, into index.services
};

World Build()
{
    Planner::GatherInput input;
    std::uint32_t entry = 100;
    std::vector<std::uint32_t> perMap(1000, 0);
    World world;
    for (std::size_t town = 0; town < TOWN_COUNT; ++town)
    {
        Town const& row = TOWNS[town];
        input.zones.push_back({row.zone, row.low, row.low ? row.low + 10 : 0, row.side, row.map});
        std::uint32_t const slot = perMap[row.map]++;
        float const x = TOWN_SPACING_YARDS * static_cast<float>(slot);
        float const y = slot % 2 ? 1200.0f : -800.0f;
        std::uint32_t const area = 5000 + static_cast<std::uint32_t>(town);
        world.centre.push_back({row.map, x, y, 10.0f});
        auto add = [&](float dx, float dy, std::uint32_t flags, bool mailbox = false)
        { input.spawns.push_back({row.map, x + dx, y + dy, 10.0f, ++entry, flags, 0, row.zone, area, 0, mailbox}); };
        add(0.0f, 0.0f, Planner::NpcFlag::INNKEEPER);
        add(FLIGHT_X, FLIGHT_Y, Planner::NpcFlag::FLIGHTMASTER);
        add(-10.0f, 5.0f, Planner::NpcFlag::VENDOR);
        add(0.0f, -15.0f, Planner::NpcFlag::REPAIR);
        add(5.0f, 20.0f, 0, true);
    }
    world.index = Planner::PlannerIndexes::Build(std::move(input));
    world.hubOf.assign(TOWN_COUNT, TOWN_COUNT);
    world.innOf.assign(TOWN_COUNT, 0);
    for (std::size_t hub = 0; hub < world.index.hubs.size(); ++hub)
        world.hubOf[world.index.hubs[hub].areaId - 5000] = hub;
    for (std::size_t service = 0; service < world.index.services.size(); ++service)
        if (world.index.services[service].npcFlags & Planner::NpcFlag::INNKEEPER)
            world.innOf[world.index.services[service].areaId - 5000] = service;
    return world;
}

// The sample bots: side x level x flight points (none, each town's, all) x stays (none, each town's zone, all)
struct Sample
{
    BotKnowledge bot;
    std::size_t flight;  // TOWN_COUNT = none, TOWN_COUNT + 1 = all
    std::size_t stay;
    MapPoint from;
};

bool OracleKnows(Sample const& sample, std::size_t town)
{
    Town const& row = TOWNS[town];
    if (sample.stay == TOWN_COUNT + 1 || sample.stay == town)
        return true;
    if (sample.flight == TOWN_COUNT + 1 || sample.flight == town)
        return true;
    std::uint32_t const least = sample.bot.side == Side::Alliance ? row.minAlliance : row.minHorde;
    return sample.bot.level >= least;
}

// The one town a line names (by the town names of the table), TOWN_COUNT for none or more than one
std::size_t NamedTown(std::string_view text)
{
    std::size_t found = TOWN_COUNT;
    for (std::size_t town = 0; town < TOWN_COUNT; ++town)
        if (Holds(text, TOWNS[town].name))
        {
            if (found != TOWN_COUNT)
                return TOWN_COUNT;
            found = town;
        }
    return found;
}

float PlainYards(MapPoint const& a, MapPoint const& b) { return std::hypot(a.x - b.x, a.y - b.y); }

struct Search
{
    std::uint64_t pairs = 0;
    std::uint64_t lists = 0;
    std::uint64_t knownPairs = 0;
    std::uint64_t counterexamples = 0;

    void Report(char const* what, Sample const& sample, std::size_t town)
    {
        if (++counterexamples <= 5)
            std::printf("COUNTEREXAMPLE %s: side %d level %u flight %zu stay %zu town %s\n", what,
                        static_cast<int>(sample.bot.side), sample.bot.level, sample.flight, sample.stay,
                        town < TOWN_COUNT ? TOWNS[town].name : "-");
    }
};

// One town asked alone: a known town gives one line that names it with a distance and a direction, an unknown one
// gives one unknown line without its name
void AskTown(World const& world, Sample const& sample, std::size_t town, Search& search)
{
    Planner::Hub const& hub = world.index.hubs[world.hubOf[town]];
    WhereCandidate const thing{TOWNS[town].name,
                               L::WHERE_TOWN,
                               TOWNS[town].zoneName,
                               {hub.mapId, hub.x, hub.y, hub.z},
                               PlaceOfHub(world.index, hub)};
    std::vector<BotStateItem> items;
    WhereItems(sample.bot, sample.from, {thing}, items);
    ++search.pairs;
    bool const known = OracleKnows(sample, town);
    search.knownPairs += known;
    if (items.size() != 1 || HasDigit(items[0].text))
        return search.Report("not one clean line", sample, town);
    BotStateItem const& line = items[0];
    if (known)
    {
        bool direction = hub.mapId != sample.from.mapId;
        for (char const* word : L::WHERE_DIRECTIONS)
            direction |= Holds(line.text, word);
        if (line.word != L::ROW_WHERE_FOUND || NamedTown(line.text) != town || !direction)
            search.Report("a known town not named", sample, town);
    }
    else if (line.word != L::ROW_WHERE_UNKNOWN || !line.name.empty() || Holds(line.text, TOWNS[town].name) ||
             Holds(line.text, TOWNS[town].zoneName))
        search.Report("an unknown town named", sample, town);
}

// Every town or every innkeeper at once: each line names a town the oracle knows, the nearest known ones on the bot's
// map first, as many as the answer holds
void AskAll(World const& world, Sample const& sample, bool inns, Search& search)
{
    std::vector<WhereCandidate> things;
    for (std::size_t town = 0; town < TOWN_COUNT; ++town)
    {
        if (inns)
        {
            Planner::Service const& inn = world.index.services[world.innOf[town]];
            things.push_back({std::string("Innkeeper of ") + TOWNS[town].name,
                              ServiceWhat(Planner::ServiceKind::Innkeeper),
                              TOWNS[town].name,
                              {inn.mapId, inn.x, inn.y, inn.z},
                              PlaceOfService(world.index, inn)});
        }
        else
        {
            Planner::Hub const& hub = world.index.hubs[world.hubOf[town]];
            things.push_back({TOWNS[town].name,
                              L::WHERE_TOWN,
                              TOWNS[town].zoneName,
                              {hub.mapId, hub.x, hub.y, hub.z},
                              PlaceOfHub(world.index, hub)});
        }
    }
    std::vector<std::pair<float, std::size_t>> expected;
    for (std::size_t town = 0; town < TOWN_COUNT; ++town)
        if (OracleKnows(sample, town))
            expected.emplace_back(things[town].at.mapId == sample.from.mapId ? PlainYards(sample.from, things[town].at)
                                                                             : 1.0e9f + static_cast<float>(town),
                                  town);
    std::sort(expected.begin(), expected.end());
    if (expected.size() > WHERE_SHOWN)
        expected.resize(WHERE_SHOWN);

    std::vector<BotStateItem> items;
    WhereItems(sample.bot, sample.from, things, items);
    ++search.lists;
    if (expected.empty())
    {
        if (items.size() != 1 || items[0].word != L::ROW_WHERE_UNKNOWN || NamedTown(items[0].text) != TOWN_COUNT)
            search.Report(inns ? "inns: unknown answer names a town" : "towns: unknown answer names a town", sample,
                          TOWN_COUNT);
        return;
    }
    if (items.size() != expected.size())
        return search.Report(inns ? "inns: wrong count" : "towns: wrong count", sample, TOWN_COUNT);
    for (std::size_t line = 0; line < items.size(); ++line)
    {
        std::size_t const named = NamedTown(items[line].text);
        if (named == TOWN_COUNT || !OracleKnows(sample, named))
            search.Report(inns ? "inns: an unknown town named" : "towns: an unknown town named", sample, named);
        else if (expected[line].second != named)
            search.Report(inns ? "inns: not nearest first" : "towns: not nearest first", sample, named);
    }
}

void CounterexampleSearch()
{
    World const world = Build();
    bool everyTown = world.index.hubs.size() == TOWN_COUNT;
    for (std::size_t town = 0; town < TOWN_COUNT; ++town)
        everyTown &= world.hubOf[town] < TOWN_COUNT &&
                     (world.index.services[world.innOf[town]].npcFlags & Planner::NpcFlag::INNKEEPER);
    Check(everyTown, "the synthetic world: a hub and an innkeeper per town");
    if (!everyTown)
        return;

    Search search;
    std::size_t sampleIndex = 0;
    for (Side const side : {Side::Alliance, Side::Horde})
        for (std::uint32_t const level : LEVELS)
            for (std::size_t flight = 0; flight <= TOWN_COUNT + 1; ++flight)
                for (std::size_t stay = 0; stay <= TOWN_COUNT + 1; ++stay)
                {
                    Sample sample{Bot(side, level), flight, stay, world.centre[(sampleIndex++ * 7) % TOWN_COUNT]};
                    sample.from.x += 200.0f;
                    sample.from.y -= 300.0f;
                    for (std::size_t town = 0; town < TOWN_COUNT; ++town)
                    {
                        MapPoint const& at = world.centre[town];
                        if (flight == town || flight == TOWN_COUNT + 1)
                            sample.bot.flightPoints.push_back({at.mapId, at.x + FLIGHT_X, at.y + FLIGHT_Y, at.z});
                        if (stay == town || stay == TOWN_COUNT + 1)
                            sample.bot.stayedZones.push_back(TOWNS[town].zone);
                    }
                    for (std::size_t town = 0; town < TOWN_COUNT; ++town)
                        AskTown(world, sample, town, search);
                    AskAll(world, sample, false, search);
                    AskAll(world, sample, true, search);
                }
    std::printf("pairs tried: %llu (bot x town, %llu of them known), lists tried: %llu, counterexamples: %llu\n",
                static_cast<unsigned long long>(search.pairs), static_cast<unsigned long long>(search.knownPairs),
                static_cast<unsigned long long>(search.lists), static_cast<unsigned long long>(search.counterexamples));
    Check(search.knownPairs > 0 && search.knownPairs < search.pairs, "the search meets known and unknown towns");
    Check(search.counterexamples == 0, "no answer names a town the rule calls unknown, none leaves a known one out");
}
}  // namespace

// LC18a-2: the towns as the reader and the order resolver see them: a candidate under each hub's area name and once
// under its zone's name; a town named is a literal "x,y,z,map" only when the rule lets the bot know it
void TownChecks()
{
    World const world = Build();
    auto const nameOf = [](std::uint32_t id)
    {
        for (std::size_t town = 0; town < TOWN_COUNT; ++town)
        {
            if (id == TOWNS[town].zone)
                return std::string(TOWNS[town].zoneName);
            if (id == 5000 + town)
                return std::string(TOWNS[town].name);
        }
        return std::string();
    };
    MapPoint const from{0, 0.0f, 0.0f, 0.0f};
    std::vector<WhereCandidate> const towns = TownCandidates(world.index, from, nameOf);
    Check(towns.size() == 2 * TOWN_COUNT, "a candidate under the area name and one under the zone name per town");
    BotKnowledge const ally = Bot(Side::Alliance, 1);
    Planner::Hub const& stormwind = world.index.hubs[world.hubOf[0]];
    char expected[96];
    std::snprintf(expected, sizeof(expected), "%.2f,%.2f,%.2f,%u", stormwind.x, stormwind.y, stormwind.z,
                  stormwind.mapId);
    Check(TownSpot(ally, towns, from, "Stormwind") == std::string(expected),
          "a known town by its area name: its literal");
    Check(TownSpot(ally, towns, from, "stormwind city") == std::string(expected), "a known town by its zone name");
    Check(TownSpot(ally, towns, from, "Stormw") == std::string(expected), "a part of the name, from three letters");
    Check(!TownSpot(ally, towns, from, "Orgrimmar"), "the other side's capital: no literal");
    Check(!TownSpot(ally, towns, from, "Nowhere") && !TownSpot(ally, towns, from, "") &&
              !TownSpot(ally, towns, from, "St"),
          "an unmatched, empty or too short name: no literal");
    BotKnowledge stayed = ally;
    stayed.stayedZones = {1637};
    Check(TownSpot(stayed, towns, from, "Orgrimmar").has_value(), "a stay in the zone makes its town known");
    for (WhereCandidate const& town : towns)
        Check(town.what == L::WHERE_TOWN && !town.place.empty() && town.fact.zoneId != 0, "a town candidate is whole");
}

int main()
{
    RuleChecks();
    WordChecks();
    AnswerChecks();
    TownChecks();
    CounterexampleSearch();
    std::printf("%d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
