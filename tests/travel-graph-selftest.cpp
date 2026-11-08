/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// Standalone check of the travel graph (src/Mgr/Travel/TravelGraph.h) on the shipped data: parses the INSERT rows of
// data/sql/playerbots/base/playerbots_travelnode*.sql and of the updates under data/sql/playerbots/updates/ into the
// same TravelGraphBuilder the server's loader feeds, then runs TravelGraph::Selftest (the counts, the per-node link
// ranges, the nearest-node grid and three routes) - the checks `playerbots route selftest` runs on the loaded graph.
// Usage: travel-graph-selftest [module root] (default: the current directory)
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <string>
#include <vector>

#include "TravelGraph.h"

namespace
{
// The values of one "(1, 'a, b', 2.5)," row, quotes removed; false when the line is no row
bool SplitRow(std::string const& line, std::vector<std::string>& values)
{
    values.clear();
    std::size_t const start = line.find_first_not_of(" \t");
    if (start == std::string::npos || line[start] != '(')
        return false;

    std::string value;
    bool quoted = false;
    for (std::size_t i = start + 1; i < line.size(); ++i)
    {
        char const c = line[i];
        if (quoted)
        {
            if (c == '\\' && i + 1 < line.size())
                value += line[++i];
            else if (c == '\'' && i + 1 < line.size() && line[i + 1] == '\'')
                value += line[++i];
            else if (c == '\'')
                quoted = false;
            else
                value += c;
        }
        else if (c == '\'')
            quoted = true;
        else if (c == ',' || c == ')')
        {
            values.push_back(value);
            value.clear();
            if (c == ')')
                return true;
        }
        else if (c != ' ')
            value += c;
    }
    return false;
}

// Calls onRow for each row inserted into table by the file; the rows follow an "INSERT INTO `table`" line
std::size_t ReadRows(std::filesystem::path const& file, std::string const& table,
                     std::function<void(std::vector<std::string> const&)> const& onRow)
{
    std::ifstream in(file);
    std::string const insert = "INSERT INTO `" + table + "`";
    std::string line;
    std::vector<std::string> values;
    bool inTable = false;
    std::size_t rows = 0;
    while (std::getline(in, line))
    {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.rfind("INSERT INTO", 0) == 0)
        {
            inTable = line.rfind(insert, 0) == 0;
            // A one-line insert carries its row after VALUES
            std::size_t const at = line.find("VALUES");
            if (inTable && at != std::string::npos && SplitRow(line.substr(at + 6), values))
            {
                onRow(values);
                ++rows;
            }
            continue;
        }
        if (inTable && line.rfind("VALUES", 0) == 0)
        {
            if (SplitRow(line.substr(6), values))
            {
                onRow(values);
                ++rows;
            }
            continue;
        }
        if (inTable && SplitRow(line, values))
        {
            onRow(values);
            ++rows;
        }
        else if (!line.empty() && line[0] != '(' && line[0] != ' ')
            inTable = false;
    }
    return rows;
}

std::uint32_t U(std::string const& value) { return static_cast<std::uint32_t>(std::stoul(value)); }
float F(std::string const& value) { return std::stof(value); }

TravelGraphBuilder::LinkRow Walk(std::uint32_t fromId, std::uint32_t toId, std::uint32_t type = 1)
{
    TravelGraphBuilder::LinkRow row;
    row.fromId = fromId;
    row.toId = toId;
    row.type = type;
    row.distance = 70.0f;
    return row;
}

// The builder's rules on a hand-made graph: bad links skipped, orphan points counted, the path length summed per map,
// out-of-order points and a duplicate node id refused
std::uint32_t BuilderChecks(std::vector<std::string>& failures)
{
    std::uint32_t checks = 0;
    auto const check = [&checks, &failures](bool ok, char const* what)
    {
        ++checks;
        if (!ok)
            failures.push_back(std::string("builder: ") + what);
    };

    std::string error;
    TravelGraphBuilder builder;
    builder.AddNode(20, "c", 0, 0.0f, 0.0f, 0.0f);
    builder.AddNode(10, "a", 0, 0.0f, 0.0f, 0.0f);
    builder.AddNode(15, "b", 0, 0.0f, 0.0f, 0.0f);
    builder.AddLink(Walk(10, 15));
    builder.AddLink(Walk(15, 20));
    builder.AddLink(Walk(10, 10));     // to itself
    builder.AddLink(Walk(10, 99));     // unknown node
    builder.AddLink(Walk(15, 10, 7));  // unknown type
    check(builder.EndLinks(error), "a small graph ends its links");
    builder.AddPoint(10, 15, 0, 0, 0.0f, 0.0f, 0.0f);
    builder.AddPoint(10, 15, 1, 0, 3.0f, 4.0f, 0.0f);   // 5 yd
    builder.AddPoint(10, 15, 2, 1, 50.0f, 0.0f, 0.0f);  // another map: no length
    builder.AddPoint(10, 15, 3, 1, 50.0f, 0.0f, 2.0f);  // 2 yd
    builder.AddPoint(30, 40, 0, 0, 0.0f, 0.0f, 0.0f);   // no such link
    TravelGraph graph;
    check(builder.Finish(graph, error), "a small graph finishes");
    graph.Publish(0, 0);
    TravelGraphStats const& stats = graph.Stats();
    check(stats.links == 2 && stats.skippedLinks == 3 && stats.orphanPoints == 1, "bad links skipped, orphans counted");
    check(graph.IndexOfId(10) == 0 && graph.IndexOfId(20) == 2 && graph.IndexOfId(11) == TravelGraph::NO_NODE,
          "nodes ordered by id");
    TravelGraphLink const& link = graph.LinksOf(0)[0];
    check(link.pathCount == 4 && std::fabs(link.pathLength - 7.0f) < 0.001f, "path length summed on one map");
    TravelGraphRoute const route = graph.SimpleRoute(0, 2);
    check(route.found && route.links.size() == 2 && std::fabs(route.seconds - 20.0f) < 0.001f, "a two-link route");
    check(!graph.SimpleRoute(2, 0).found, "no route against the links");

    TravelGraphBuilder disordered;
    disordered.AddNode(1, "a", 0, 0.0f, 0.0f, 0.0f);
    disordered.AddNode(2, "b", 0, 0.0f, 0.0f, 0.0f);
    disordered.AddLink(Walk(1, 2));
    disordered.AddLink(Walk(2, 1));
    disordered.EndLinks(error);
    disordered.AddPoint(1, 2, 0, 0, 0.0f, 0.0f, 0.0f);
    disordered.AddPoint(2, 1, 0, 0, 0.0f, 0.0f, 0.0f);
    disordered.AddPoint(1, 2, 1, 0, 0.0f, 0.0f, 0.0f);  // the link's points come in two groups
    TravelGraph refused;
    check(!disordered.Finish(refused, error), "points out of order refused");

    TravelGraphBuilder twice;
    twice.AddNode(1, "a", 0, 0.0f, 0.0f, 0.0f);
    twice.AddNode(1, "b", 0, 0.0f, 0.0f, 0.0f);
    twice.AddLink(Walk(1, 1));
    check(!twice.EndLinks(error), "a node id twice refused");
    return checks;
}
}  // namespace

int main(int argc, char** argv)
{
    std::filesystem::path const root = argc > 1 ? argv[1] : ".";
    std::filesystem::path const base = root / "data" / "sql" / "playerbots" / "base";
    std::filesystem::path const updatesDir = root / "data" / "sql" / "playerbots" / "updates";
    if (!std::filesystem::exists(base / "playerbots_travelnode_path.sql"))
    {
        std::cout << "travel-graph-selftest: no shipped graph under " << base.string() << "\n";
        return 2;
    }

    // The updates apply after the base files, in name (date) order. Their DELETEs are ignored: the only one today
    // (2025_03_03_00) removes rows of node 3780 that the base lacks, and a duplicate node id fails the build
    std::vector<std::filesystem::path> updates;
    for (auto const& entry : std::filesystem::directory_iterator(updatesDir))
        if (entry.path().extension() == ".sql")
            updates.push_back(entry.path());
    std::sort(updates.begin(), updates.end());

    auto const files = [&](char const* table)
    {
        std::vector<std::filesystem::path> list{base / (std::string(table) + ".sql")};
        list.insert(list.end(), updates.begin(), updates.end());
        return list;
    };

    auto const started = std::chrono::steady_clock::now();
    TravelGraphBuilder builder;
    std::size_t rows[3]{};
    for (auto const& file : files("playerbots_travelnode"))
        rows[0] += ReadRows(file, "playerbots_travelnode", [&builder](std::vector<std::string> const& v)
                            { builder.AddNode(U(v[0]), v[1], U(v[2]), F(v[3]), F(v[4]), F(v[5])); });
    for (auto const& file : files("playerbots_travelnode_link"))
        rows[1] += ReadRows(file, "playerbots_travelnode_link",
                            [&builder](std::vector<std::string> const& v)
                            {
                                TravelGraphBuilder::LinkRow row;
                                row.fromId = U(v[0]);
                                row.toId = U(v[1]);
                                row.type = U(v[2]);
                                row.object = U(v[3]);
                                row.distance = F(v[4]);
                                row.swim = F(v[5]);
                                row.extraCost = F(v[6]);
                                row.maxCreature[0] = U(v[8]);
                                row.maxCreature[1] = U(v[9]);
                                row.maxCreature[2] = U(v[10]);
                                builder.AddLink(row);
                            });

    std::string error;
    TravelGraph graph;
    bool built = builder.EndLinks(error);
    if (built)
    {
        for (auto const& file : files("playerbots_travelnode_path"))
            rows[2] += ReadRows(file, "playerbots_travelnode_path", [&builder](std::vector<std::string> const& v)
                                { builder.AddPoint(U(v[0]), U(v[1]), U(v[2]), U(v[3]), F(v[4]), F(v[5]), F(v[6])); });
        built = builder.Finish(graph, error);
    }
    auto const loadMs = static_cast<std::uint32_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count());
    if (!built)
    {
        std::cout << "travel-graph-selftest: build failed: " << error << "\n";
        return 1;
    }
    graph.Publish(loadMs, 0);

    TravelGraphStats const& stats = graph.Stats();
    std::cout << "rows read: " << rows[0] << " nodes, " << rows[1] << " links, " << rows[2] << " points\n";
    std::cout << "graph: " << stats.nodes << " nodes, " << stats.links << " links, " << stats.points << " points, "
              << static_cast<double>(stats.memoryBytes) / (1024.0 * 1024.0) << " MB, parsed in " << loadMs << " ms\n";

    std::vector<std::string> failures;
    std::vector<std::string> notes;
    auto const routeStart = std::chrono::steady_clock::now();
    std::uint32_t const checks = graph.Selftest(failures, notes) + BuilderChecks(failures);
    auto const selftestMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - routeStart).count();
    for (std::string const& note : notes)
        std::cout << "  " << note << "\n";
    for (std::string const& failure : failures)
        std::cout << "FAIL: " << failure << "\n";
    std::cout << "travel-graph-selftest: " << (checks - failures.size()) << "/" << checks << " passed (checks in "
              << selftestMs << " ms)\n";
    return failures.empty() ? 0 : 1;
}
