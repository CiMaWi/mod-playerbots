/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "ItemUseCommands.h"

#include <array>
#include <cstdint>
#include <fstream>
#include <map>
#include <span>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "Chat.h"
#include "Config.h"
#include "GitRevision.h"
#include "ItemUseFacts.h"
#include "ItemUseIndex.h"
#include "ItemUseKinds.h"

namespace ItemUse
{
namespace
{
// A console reply prints at most this many lines of a list; the rest is counted
constexpr std::size_t LINES_SHOWN = 200;

std::string Trim(std::string text)
{
    std::size_t const first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
        return {};
    return text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
}

// The dump goes into the server's logs folder under a bare file name: players may run the command, so the argument
// never names a path elsewhere (an empty result: refused)
std::string DumpPath(std::string const& argument)
{
    std::string const name = argument.empty() ? "item-use-dump.txt" : argument;
    if (name.find_first_of("/\\:") != std::string::npos || name.find("..") != std::string::npos)
        return {};
    std::string dir = sConfigMgr->GetOption<std::string>("LogsDir", "", false);
    if (!dir.empty() && dir.back() != '/' && dir.back() != '\\')
        dir += '/';
    return dir + name;
}
}  // namespace

bool HandleCensusCommand(ChatHandler* handler, char const* args)
{
    ItemUseIndex const& index = ItemUseIndex::Get();
    ItemUseIndex::Stats const& stats = index.GetStats();
    bool const listUnclassified = Trim(args ? args : "").find("unclassified") != std::string::npos;

    std::map<std::pair<int, int>, std::uint32_t> classes;
    std::array<std::uint32_t, static_cast<std::size_t>(Shape::Count)> shapes{};
    std::array<std::uint32_t, static_cast<std::size_t>(Kind::Count)> kinds{};
    std::uint32_t cut = 0;
    std::vector<std::pair<std::uint32_t, Classified>> unclassified;
    std::span<Record const> const all = index.All();
    for (std::uint32_t id = 0; id < all.size(); ++id)
    {
        Record const& record = all[id];
        if (record.source == Source::ItemSpell)
            ++classes[{record.itemClass, record.itemSubClass}];
        ++shapes[static_cast<std::size_t>(record.shape)];
        Classified const result = Classify(record);
        ++kinds[static_cast<std::size_t>(result.kind)];
        cut += record.effectsCut ? 1 : 0;
        if (result.kind == Kind::Unclassified)
            unclassified.emplace_back(id, result);
    }

    if (listUnclassified)
    {
        handler->PSendSysMessage("ITEM census unclassified: {} records", unclassified.size());
        for (std::size_t i = 0; i < unclassified.size() && i < LINES_SHOWN; ++i)
        {
            Record const& record = all[unclassified[i].first];
            bool const item = record.source == Source::ItemSpell;
            handler->PSendSysMessage("ITEM unclassified {} {} {} {}: {}", item ? record.itemEntry : record.enchantId,
                                     item ? "entry" : "enchant", record.spellId, index.NameOf(unclassified[i].first),
                                     unclassified[i].second.why);
        }
        if (unclassified.size() > LINES_SHOWN)
            handler->PSendSysMessage("ITEM unclassified: {} more (see the dump)", unclassified.size() - LINES_SHOWN);
        return true;
    }

    handler->PSendSysMessage("ITEM census: records {} items {} enchants {} cut {} no_spell {} build_ms {}", all.size(),
                             stats.itemRecords, stats.enchantRecords, cut, stats.missingSpells, stats.buildMs);
    std::size_t lines = 0;
    for (auto const& [key, count] : classes)
    {
        if (++lines > LINES_SHOWN)
            break;
        handler->PSendSysMessage("ITEM census class {} sub {}: {}", key.first, key.second, count);
    }
    for (std::size_t shape = 0; shape < shapes.size(); ++shape)
        handler->PSendSysMessage("ITEM census shape {}: {}", ShapeWord(static_cast<Shape>(shape)), shapes[shape]);
    for (std::size_t kind = 0; kind < kinds.size(); ++kind)
        handler->PSendSysMessage("ITEM census kind {}: {}", KindWord(static_cast<Kind>(kind)), kinds[kind]);
    return true;
}

bool HandleInfoCommand(ChatHandler* handler, char const* args)
{
    std::istringstream in(args ? args : "");
    std::uint32_t entry = 0;
    std::string word;
    in >> entry >> word;
    if (!entry)
    {
        handler->SendErrorMessage("Usage: playerbots item info <entry> [enchant]");
        return false;
    }
    bool const enchant = word == "enchant";
    ItemUseIndex const& index = ItemUseIndex::Get();
    std::span<Record const> const records = enchant ? index.OfEnchant(entry) : index.OfItem(entry);
    if (records.empty())
    {
        handler->PSendSysMessage("ITEM info: {} has no use effect", entry);
        return true;
    }
    for (Record const& record : records)
    {
        Classified const result = Classify(record);
        handler->PSendSysMessage("ITEM info {} {}: kind {} {}", entry, enchant ? "enchant" : "entry",
                                 KindWord(result.kind), result.why);
        handler->PSendSysMessage("{}", FormatDumpLine(record, index.NameOf(index.IdOf(record))));
    }
    return true;
}

bool HandleDumpCommand(ChatHandler* handler, char const* args)
{
    ItemUseIndex const& index = ItemUseIndex::Get();
    std::string const path = DumpPath(Trim(args ? args : ""));
    if (path.empty())
    {
        handler->SendErrorMessage("ITEM dump: give a bare file name; the dump goes into the logs folder");
        return false;
    }

    std::string text = DumpHeader("unknown", GitRevision::GetHash(), index.All().size());
    text += '\n';
    text += DumpFieldNames();
    text += '\n';
    for (std::uint32_t id = 0; id < index.All().size(); ++id)
    {
        text += FormatDumpLine(index.All()[id], index.NameOf(id));
        text += '\n';
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    out.flush();
    if (!out)
    {
        handler->SendErrorMessage("ITEM dump: cannot write {}", path);
        return false;
    }
    handler->PSendSysMessage("ITEM dump: {} records to {}", index.All().size(), path);
    return true;
}
}  // namespace ItemUse
