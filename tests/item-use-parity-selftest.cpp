/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// Item use plan WI5b: the packets of the old item builders, recorded before WI5 points them at the framework.
// tests/fixtures/item-use-old-packets.txt holds the CMSG_USE_ITEM bytes that UseItemAction::UseItem,
// UseTrinketAction::UseTrinket and PlayerbotAI::ImbueItem sent on lab9 (module a37f89f5 with a temporary trace),
// plus synthetic lines for the branches the block C scenarios do not reach. Each old writer is transcribed below in
// its field order; the test rebuilds every line's bytes from its inputs. Run: item-use-parity-selftest <module root>

#include <cstdint>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <vector>

namespace
{
int passed = 0;
int failed = 0;

void Check(bool ok, std::string const& what)
{
    if (ok)
        ++passed;
    else
    {
        ++failed;
        std::cout << "FAIL: " << what << "\n";
    }
}

// TARGET_FLAG_* of SharedDefines.h (the core): the masks the old writers send
constexpr std::uint32_t FLAG_NONE = 0x0;
constexpr std::uint32_t FLAG_UNIT = 0x2;
constexpr std::uint32_t FLAG_ITEM = 0x10;
constexpr std::uint32_t FLAG_GAMEOBJECT = 0x800;
constexpr std::uint32_t FLAG_TRADE_ITEM = 0x1000;
constexpr std::uint64_t TRADE_SLOT_NONTRADED = 6;  // TradeSlots in TradeData.h

// ByteBuffer's order on this machine: little endian; a packed guid is a mask byte, then the guid's non-zero bytes
struct Bytes
{
    std::vector<std::uint8_t> data;

    void U8(std::uint8_t value) { data.push_back(value); }

    void U32(std::uint32_t value)
    {
        for (int i = 0; i < 4; ++i)
            data.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
    }

    void U64(std::uint64_t value)
    {
        for (int i = 0; i < 8; ++i)
            data.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
    }

    void Packed(std::uint64_t guid)
    {
        std::size_t const mask = data.size();
        data.push_back(0);
        for (int i = 0; i < 8; ++i)
        {
            std::uint8_t const byte = static_cast<std::uint8_t>(guid >> (8 * i));
            if (byte)
            {
                data[mask] |= static_cast<std::uint8_t>(1u << i);
                data.push_back(byte);
            }
        }
    }
};

std::string Hex(std::vector<std::uint8_t> const& data)
{
    static char const* const digits = "0123456789abcdef";
    std::string out;
    for (std::uint8_t const byte : data)
    {
        out += digits[byte >> 4];
        out += digits[byte & 0xF];
    }
    return out;
}

// One target write of the old UseItem, in the order it appended them: a mask and the target's packed guid
// (GAMEOBJECT, ITEM, UNIT), the mask alone (NONE), or the trade slot (TRADE_ITEM)
struct Segment
{
    std::uint32_t flag{0};
    std::uint64_t guid{0};
    bool trade{false};
};

struct Line
{
    std::string origin;  // recorded (lab9 trace) or synthetic (the transcription's own bytes)
    std::string site;    // UseItem, UseTrinket, ImbueItem
    std::uint32_t entry{0};
    std::uint32_t bag{0};
    std::uint32_t slot{0};
    std::uint32_t castCount{0};
    std::uint32_t spell{0};
    std::uint64_t item{0};
    std::vector<Segment> segments;
    std::string tailKind{"none"};  // none, pack (the bot's packed guid), raw (a unit's guid as 8 bytes)
    std::uint64_t tailGuid{0};
    std::string hex;
    std::string text;
};

bool ParseLine(std::string const& text, Line& line)
{
    std::map<std::string, std::string> fields;
    std::size_t pos = 0;
    std::vector<std::string> words;
    while (pos < text.size())
    {
        std::size_t const end = text.find(' ', pos);
        words.push_back(text.substr(pos, end == std::string::npos ? std::string::npos : end - pos));
        if (end == std::string::npos)
            break;
        pos = end + 1;
    }
    if (words.size() < 3)
        return false;
    line.origin = words[0];
    line.site = words[1];
    for (std::size_t i = 2; i < words.size(); ++i)
    {
        std::size_t const eq = words[i].find('=');
        if (eq == std::string::npos)
            return false;
        fields[words[i].substr(0, eq)] = words[i].substr(eq + 1);
    }
    for (char const* key : {"entry", "bag", "slot", "cc", "spell", "item", "segs", "tail", "hex"})
        if (!fields.count(key))
            return false;
    line.entry = static_cast<std::uint32_t>(std::stoul(fields["entry"]));
    line.bag = static_cast<std::uint32_t>(std::stoul(fields["bag"]));
    line.slot = static_cast<std::uint32_t>(std::stoul(fields["slot"]));
    line.castCount = static_cast<std::uint32_t>(std::stoul(fields["cc"]));
    line.spell = static_cast<std::uint32_t>(std::stoul(fields["spell"]));
    line.item = std::stoull(fields["item"], nullptr, 16);
    std::string const& segs = fields["segs"];
    pos = 0;
    while (pos < segs.size())
    {
        std::size_t const end = segs.find(';', pos);
        if (end == std::string::npos)
            return false;
        std::string const seg = segs.substr(pos, end - pos);
        std::size_t const colon = seg.find(':');
        if (colon == std::string::npos)
            return false;
        Segment segment;
        segment.flag = static_cast<std::uint32_t>(std::stoul(seg.substr(0, colon)));
        std::string const guid = seg.substr(colon + 1);
        segment.trade = guid == "t";
        segment.guid = guid.empty() || segment.trade ? 0 : std::stoull(guid, nullptr, 16);
        line.segments.push_back(segment);
        pos = end + 1;
    }
    std::string const& tail = fields["tail"];
    std::size_t const colon = tail.find(':');
    line.tailKind = tail.substr(0, colon);
    if (colon != std::string::npos)
        line.tailGuid = std::stoull(tail.substr(colon + 1), nullptr, 16);
    line.hex = fields["hex"];
    line.text = text;
    return !line.segments.empty();
}

// The header all three writers share: bag, slot, cast count, spell, item guid, glyph index 0, cast flags 0
void Header(Bytes& out, Line const& line, std::uint8_t castCount)
{
    out.U8(static_cast<std::uint8_t>(line.bag));
    out.U8(static_cast<std::uint8_t>(line.slot));
    out.U8(castCount);
    out.U32(line.spell);
    out.U64(line.item);
    out.U32(0);
    out.U8(0);
}

// UseItemAction::UseItem (UseItemAction.cpp:232-394 at a37f89f5): cast_count 1; each selected target appends its
// mask and packed guid (game object :251-252, item :273-274, master's unit :290, unit :299, item for spell :358-359);
// the trade branch :351 writes the mask, a byte 1 and the packed guid TRADE_SLOT_NONTRADED; no target :373 the mask
// NONE, then the unit's raw guid :378 or the bot's packed guid :389
std::vector<std::uint8_t> OldUseItem(Line const& line)
{
    Bytes out;
    Header(out, line, 1);
    for (Segment const& segment : line.segments)
    {
        out.U32(segment.flag);
        if (segment.trade)
        {
            out.U8(1);
            out.Packed(TRADE_SLOT_NONTRADED);
        }
        else if (segment.flag != FLAG_NONE)
            out.Packed(segment.guid);
    }
    if (line.tailKind == "raw")
        out.U64(line.tailGuid);
    else if (line.tailKind == "pack")
        out.Packed(line.tailGuid);
    return out.data;
}

// UseTrinketAction::UseTrinket (GenericSpellActions.cpp:656-660 at a37f89f5): cast_count 1, the mask NONE, then the
// bot's packed guid
std::vector<std::uint8_t> OldUseTrinket(Line const& line)
{
    Bytes out;
    Header(out, line, 1);
    out.U32(FLAG_NONE);
    out.Packed(line.tailGuid);
    return out.data;
}

// PlayerbotAI::ImbueItem(item, flag, guid) (PlayerbotAI.cpp:6611-6624 at a37f89f5): cast_count 0, the mask, and the
// packed guid only for UNIT, ITEM or GAMEOBJECT
std::vector<std::uint8_t> OldImbueItem(Line const& line)
{
    Bytes out;
    Header(out, line, 0);
    std::uint32_t const flag = line.segments.front().flag;
    out.U32(flag);
    if (flag & (FLAG_UNIT | FLAG_ITEM | FLAG_GAMEOBJECT))
        out.Packed(line.segments.front().guid);
    return out.data;
}

std::vector<std::uint8_t> OldBytes(Line const& line)
{
    if (line.site == "UseItem")
        return OldUseItem(line);
    if (line.site == "UseTrinket")
        return OldUseTrinket(line);
    return OldImbueItem(line);
}

// What each writer's constants say about a line: its cast count, its tail, its one mask
bool ShapeFits(Line const& line)
{
    if (line.site == "UseItem")
    {
        for (Segment const& segment : line.segments)
            if (segment.trade != (segment.flag == FLAG_TRADE_ITEM))
                return false;
        return line.castCount == 1 && (line.segments.back().flag != FLAG_NONE || line.tailKind != "none");
    }
    if (line.site == "UseTrinket")
        return line.castCount == 1 && line.segments.size() == 1 && line.segments[0].flag == FLAG_NONE &&
               line.tailKind == "pack";
    if (line.site == "ImbueItem")
        return line.castCount == 0 && line.segments.size() == 1 && line.tailKind == "none";
    return false;
}
}  // namespace

int main(int argc, char** argv)
{
    bool const emit = argc > 2 && std::string(argv[2]) == "--emit";
    std::string const root = argc > 1 ? argv[1] : ".";
    std::ifstream in(root + "/tests/fixtures/item-use-old-packets.txt", std::ios::binary);
    Check(static_cast<bool>(in), "the fixture opens");
    if (!in)
    {
        std::cout << passed << " passed, " << failed << " failed\n";
        return 1;
    }

    std::map<std::string, int> recorded;
    std::map<std::string, int> synthetic;
    std::string text;
    while (std::getline(in, text))
    {
        if (!text.empty() && text.back() == '\r')
            text.pop_back();
        if (text.empty() || text[0] == '#')
            continue;
        Line line;
        bool const parsed = ParseLine(text, line);
        Check(parsed, "parses: " + text);
        if (!parsed)
            continue;
        Check(line.origin == "recorded" || line.origin == "synthetic", "origin is recorded or synthetic: " + text);
        Check(ShapeFits(line), "the writer's constants fit: " + text);
        std::string const bytes = Hex(OldBytes(line));
        if (emit)
            std::cout << "EMIT " << line.site << " " << line.entry << " hex=" << bytes << "\n";
        Check(bytes == line.hex, "the transcription gives the line's bytes: " + text + "\n  got " + bytes);
        ++(line.origin == "recorded" ? recorded : synthetic)[line.site];
    }

    int recordedLines = 0;
    for (auto const& [site, count] : recorded)
    {
        std::cout << "recorded " << site << ": " << count << "\n";
        recordedLines += count;
    }
    for (auto const& [site, count] : synthetic)
        std::cout << "synthetic " << site << ": " << count << "\n";
    Check(recorded.count("UseItem") && recorded.count("UseTrinket") && recorded.count("ImbueItem"),
          "every old writer has a recorded line");
    Check(recordedLines >= 5, "at least five recorded lines");

    std::cout << passed << " passed, " << failed << " failed\n";
    return failed ? 1 : 0;
}
