/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_TRAVELGRAPHSQL_H
#define _PLAYERBOT_TRAVELGRAPHSQL_H

// The offline selftests' travel graph: the INSERT rows of data/sql/playerbots/base/playerbots_travelnode*.sql and of
// the updates under data/sql/playerbots/updates/, or of a graph directory's files alone (a graph that replaces the
// three tables whole), fed into the TravelGraphBuilder the server's loader uses, plus the TaxiPath and TaxiNodes DBC
// files the server reads from its stores (tests/travel-graph-selftest.cpp, tests/route-rules-selftest.cpp)

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

#include "TravelGraph.h"

namespace TravelGraphSql
{
// The values of one "(1, 'a, b', 2.5)," row, quotes removed; false when the line is no row
inline bool SplitRow(std::string const& line, std::vector<std::string>& values)
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
inline std::size_t ReadRows(std::filesystem::path const& file, std::string const& table,
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

inline std::uint32_t U(std::string const& value) { return static_cast<std::uint32_t>(std::stoul(value)); }
inline float F(std::string const& value) { return std::stof(value); }

// The uint32 fields of a WDBC file's records; empty when the file is missing or malformed
inline std::vector<std::vector<std::uint32_t>> ReadDbc(std::filesystem::path const& file)
{
    std::ifstream in(file, std::ios::binary);
    char magic[4]{};
    std::uint32_t header[4]{};  // records, fields, record size, string block size
    in.read(magic, 4);
    in.read(reinterpret_cast<char*>(header), sizeof(header));
    if (!in || std::memcmp(magic, "WDBC", 4) != 0 || header[2] != header[1] * 4)
        return {};

    std::vector<std::vector<std::uint32_t>> records(header[0], std::vector<std::uint32_t>(header[1]));
    for (auto& record : records)
        in.read(reinterpret_cast<char*>(record.data()), static_cast<std::streamsize>(header[2]));
    return in ? records : std::vector<std::vector<std::uint32_t>>{};
}

struct LoadInfo
{
    std::size_t rows[3]{};  // nodes, links, points read
    std::size_t taxiNodes{0};
    std::size_t taxiPaths{0};
    std::uint32_t loadMs{0};
};

// The .sql files of dir in name (date) order
inline std::vector<std::filesystem::path> SqlFiles(std::filesystem::path const& dir)
{
    std::vector<std::filesystem::path> files;
    for (auto const& entry : std::filesystem::directory_iterator(dir))
        if (entry.path().extension() == ".sql")
            files.push_back(entry.path());
    std::sort(files.begin(), files.end());
    return files;
}

// Builds and publishes graph from the module at root, or from graphDir's files alone when it is given; dbcDir empty =
// no TaxiPath data (no flight is usable)
inline bool Load(std::filesystem::path const& root, std::filesystem::path const& dbcDir, TravelGraph& graph,
                 LoadInfo& info, std::string& error, std::filesystem::path const& graphDir = {})
{
    std::filesystem::path const base = root / "data" / "sql" / "playerbots" / "base";
    std::filesystem::path const updatesDir = root / "data" / "sql" / "playerbots" / "updates";
    if (graphDir.empty() && !std::filesystem::exists(base / "playerbots_travelnode_path.sql"))
    {
        error = "no shipped graph under " + base.string();
        return false;
    }
    if (!graphDir.empty() && !std::filesystem::is_directory(graphDir))
    {
        error = "no graph directory " + graphDir.string();
        return false;
    }

    // Updates apply after the base files, in name (date) order, with their DELETEs ignored: a delete of the shipped
    // updates removes rows the base lacks, and a duplicate node id fails the build. A graph directory's files replace
    // the three tables whole (each starts with a DELETE and numbers the nodes anew), so they are read alone
    std::vector<std::filesystem::path> const updates = graphDir.empty() ? SqlFiles(updatesDir) : SqlFiles(graphDir);
    auto const files = [&](char const* table)
    {
        std::vector<std::filesystem::path> list;
        if (graphDir.empty())
            list.push_back(base / (std::string(table) + ".sql"));
        list.insert(list.end(), updates.begin(), updates.end());
        return list;
    };

    auto const started = std::chrono::steady_clock::now();
    TravelGraphBuilder builder;
    for (auto const& file : files("playerbots_travelnode"))
        info.rows[0] += ReadRows(file, "playerbots_travelnode", [&builder](std::vector<std::string> const& v)
                                 { builder.AddNode(U(v[0]), v[1], U(v[2]), F(v[3]), F(v[4]), F(v[5])); });
    for (auto const& file : files("playerbots_travelnode_link"))
        info.rows[1] += ReadRows(file, "playerbots_travelnode_link",
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

    if (!dbcDir.empty())
    {
        // TaxiNodes.dbc: field 0 id, 22 MountCreatureID[0] (Horde), 23 MountCreatureID[1] (Alliance);
        // TaxiPath.dbc: id, from, to, price (DBCfmt.h TaxiNodesEntryfmt, TaxiPathEntryfmt)
        auto const nodes = ReadDbc(dbcDir / "TaxiNodes.dbc");
        auto const paths = ReadDbc(dbcDir / "TaxiPath.dbc");
        if (nodes.empty() || nodes.front().size() != 24 || paths.empty() || paths.front().size() != 4)
        {
            error = "no TaxiNodes.dbc / TaxiPath.dbc of the 3.3.5 layout under " + dbcDir.string();
            return false;
        }
        for (auto const& record : nodes)
            builder.AddTaxiNode(record[0], record[23] != 0, record[22] != 0);
        for (auto const& record : paths)
            builder.AddTaxiPath(record[0], record[1], record[2], record[3]);
        info.taxiNodes = nodes.size();
        info.taxiPaths = paths.size();
    }

    if (!builder.EndLinks(error))
        return false;
    for (auto const& file : files("playerbots_travelnode_path"))
        info.rows[2] += ReadRows(file, "playerbots_travelnode_path", [&builder](std::vector<std::string> const& v)
                                 { builder.AddPoint(U(v[0]), U(v[1]), U(v[2]), U(v[3]), F(v[4]), F(v[5]), F(v[6])); });
    if (!builder.Finish(graph, error))
        return false;

    info.loadMs = static_cast<std::uint32_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count());
    graph.Publish(info.loadMs, 0);
    return true;
}
}  // namespace TravelGraphSql

#endif
