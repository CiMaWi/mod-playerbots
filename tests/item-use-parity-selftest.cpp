/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// Item use plan WI5b and WI6: the packets of the old item builders, recorded before WI5 points them at the framework.
// tests/fixtures/item-use-old-packets.txt holds the CMSG_USE_ITEM bytes that UseItemAction::UseItem,
// UseTrinketAction::UseTrinket and PlayerbotAI::ImbueItem sent on lab9 (module a37f89f5 with a temporary trace),
// plus synthetic lines for the branches the block C scenarios do not reach. Each old writer is transcribed below in
// its field order; the test rebuilds every line's bytes from its inputs (WI5b-1). Then the framework's writer
// (ItemUsePacket.h, which ItemUse::BuildPacket sends) gets the same inputs, with the spell the item use index picks
// (tests/fixtures/item-use-dump.txt), and must give the bytes the game's handler read from the old packet (WI5b-2).
// WI6 adds the three packets the raid scripts built by hand (RocketPack, Flare: a spot; TkWorn: the unit mask and the
// bot's own packed guid), synthetic lines transcribed from their builders before the scripts were changed.
// Run: item-use-parity-selftest <module root>

#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#include "ItemUseFacts.h"
#include "ItemUsePacket.h"

using namespace ItemUse;

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
constexpr std::uint32_t FLAG_DEST = 0x40;
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

    void Float(float value)
    {
        std::uint32_t bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        U32(bits);
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
    std::string site;    // UseItem, UseTrinket, ImbueItem, RocketPack, Flare, TkWorn
    std::uint32_t entry{0};
    std::uint32_t bag{0};
    std::uint32_t slot{0};
    std::uint32_t castCount{0};
    std::uint32_t spell{0};
    std::uint64_t item{0};
    std::vector<Segment> segments;
    std::string tailKind{"none"};  // none, pack (the bot's packed guid), raw (a unit's guid as 8 bytes)
    std::uint64_t tailGuid{0};
    float pos[3]{0.0f, 0.0f, 0.0f};  // the spot of a DEST use (pos=x,y,z)
    bool hasPos{false};
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
    if (fields.count("pos"))
    {
        std::string const& pos = fields["pos"];
        std::size_t const c1 = pos.find(',');
        std::size_t const c2 = c1 == std::string::npos ? c1 : pos.find(',', c1 + 1);
        if (c2 == std::string::npos)
            return false;
        line.pos[0] = std::stof(pos.substr(0, c1));
        line.pos[1] = std::stof(pos.substr(c1 + 1, c2 - c1 - 1));
        line.pos[2] = std::stof(pos.substr(c2 + 1));
        line.hasPos = true;
    }
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

// UseTrinketAction::UseTrinket (GenericSpellActions.cpp:656-660 at a37f89f5) and PvpTrinketAction::Execute
// (CcTacticStrategy.cpp:2302-2311 at d8e2d244, spell 42292): cast_count 1, the mask NONE, then the bot's packed guid
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

// The rocket pack (ICCActions_GSB.cpp UseRocketPack at the WIL1 merge) and the flares (ICCActions_SG.cpp
// TryDropTombFlares): cast_count 1, the mask DEST_LOCATION, an empty packed guid (appendPackGUID(0)), x, y, z
std::vector<std::uint8_t> OldSpotPacket(Line const& line)
{
    Bytes out;
    Header(out, line, 1);
    out.U32(FLAG_DEST);
    out.Packed(0);
    out.Float(line.pos[0]);
    out.Float(line.pos[1]);
    out.Float(line.pos[2]);
    return out.data;
}

// The Kael'thas weapons (TKActions.cpp UseEquippedItemWithPacket): cast_count 1, the mask UNIT and the bot's own
// packed guid (bot->GetPackGUID())
std::vector<std::uint8_t> OldTkWorn(Line const& line)
{
    Bytes out;
    Header(out, line, 1);
    out.U32(FLAG_UNIT);
    out.Packed(line.segments.front().guid);
    return out.data;
}

std::vector<std::uint8_t> OldBytes(Line const& line)
{
    if (line.site == "RocketPack" || line.site == "Flare")
        return OldSpotPacket(line);
    if (line.site == "TkWorn")
        return OldTkWorn(line);
    if (line.site == "UseItem")
        return OldUseItem(line);
    if (line.site == "UseTrinket" || line.site == "PvpTrinket")
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
    if (line.site == "UseTrinket" || line.site == "PvpTrinket")
        return line.castCount == 1 && line.segments.size() == 1 && line.segments[0].flag == FLAG_NONE &&
               line.tailKind == "pack";
    if (line.site == "ImbueItem")
        return line.castCount == 0 && line.segments.size() == 1 && line.tailKind == "none";
    if (line.site == "RocketPack" || line.site == "Flare")
        return line.castCount == 1 && line.segments.size() == 1 && line.segments[0].flag == FLAG_DEST &&
               line.tailKind == "none" && line.hasPos;
    if (line.site == "TkWorn")
        return line.castCount == 1 && line.segments.size() == 1 && line.segments[0].flag == FLAG_UNIT &&
               line.segments[0].guid != 0 && line.tailKind == "none";
    return false;
}
// The handler reads the header (20 bytes), the mask and the mask's own fields (SpellCastTargets::Read): the old
// builders' tails after them were never read. Known and accepted differences, each counted:
// tail      the old UseItem and UseTrinket appended a guid after the target (the bot's packed guid or a unit's raw
//           guid); the framework writes nothing the handler does not read
// castCount ImbueItem sent cast_count 0, the others 1; the handler only echoes it to the client; the framework 1
// spell     the old UseItem took the last spell slot, the framework the index's record (the engine checks only that
//           the spell exists); printed per entry
// selfUnit  the Kael'thas weapons went as UNIT with the bot's own packed guid; their records are self uses, which the
//           framework sends as NONE: the handler reads no unit for a spell with no target of its own, and the
//           spell's caster is the bot either way (WI6)
// unit      a consumable on a unit (a soulstone on a friend) went as NONE with the unit's guid in the unread tail;
//           the framework names the unit (UNIT) when the record's shape takes one
struct Differences
{
    int compared{0};
    int equal{0};
    int tail{0};
    int castCount{0};
    int spell{0};
    int unit{0};
    int selfUnit{0};
    int outside{0};   // the trade slot: stays outside the framework
    int noRecord{0};  // the index has no record of the entry
};

using Records = std::map<std::uint32_t, std::vector<Record>>;

Records LoadDump(std::string const& root)
{
    Records records;
    std::ifstream in(root + "/tests/fixtures/item-use-dump.txt", std::ios::binary);
    std::string text;
    while (std::getline(in, text))
    {
        if (text.empty() || text[0] == '#')
            continue;
        if (std::optional<DumpRow> row = ParseDumpLine(text))
            if (row->record.source == Source::ItemSpell)
                records[row->record.itemEntry].push_back(row->record);
    }
    return records;
}

bool NamesUnit(Shape shape) { return shape != Shape::None && shape != Shape::Self; }

// The framework's fields for the old line's use: the same item, its record's spell (the trinket: the spell the old
// action picked, Request::spellId), the target the handler read
PacketFields FrameworkFields(Line const& line, Record const* record, bool& unitNamed)
{
    PacketFields fields;
    fields.bag = static_cast<std::uint8_t>(line.bag);
    fields.slot = static_cast<std::uint8_t>(line.slot);
    fields.itemGuid = line.item;
    fields.spellId = line.site == "UseTrinket" || line.site == "PvpTrinket" || !record ? line.spell : record->spellId;
    Segment const& first = line.segments.front();
    unitNamed = false;
    if (first.flag == FLAG_DEST)
    {
        fields.targetMask = PACKET_TARGET_DEST;
        fields.x = line.pos[0];
        fields.y = line.pos[1];
        fields.z = line.pos[2];
        return fields;
    }
    if (first.flag == FLAG_NONE)
    {
        if (line.tailKind == "raw" && record && NamesUnit(record->shape))
        {
            fields.targetMask = PACKET_TARGET_UNIT;
            fields.targetGuid = line.tailGuid;
            unitNamed = true;
        }
    }
    else if (first.flag != FLAG_UNIT || !record || NamesUnit(record->shape))
    {
        fields.targetMask = first.flag;
        fields.targetGuid = first.guid;
    }
    return fields;
}

bool Compare(Line const& line, std::vector<std::uint8_t> const& old, std::vector<std::uint8_t> const& framework,
             bool unitNamed, Differences& counts, bool count)
{
    if (framework.size() > old.size() && !unitNamed)
        return false;
    bool spellDiffers = false;
    // a self use the old builder sent as UNIT with the bot's guid: the framework's NONE is equal in effect
    bool const selfUnit = line.site == "TkWorn" && framework.size() == 24 && old.size() > 24 && old[20] == FLAG_UNIT &&
                          framework[20] == FLAG_NONE;
    std::size_t const compared = unitNamed || selfUnit ? 20 : framework.size();
    for (std::size_t i = 0; i < compared; ++i)
    {
        if (framework[i] == old[i])
            continue;
        if (i == 2 && line.site == "ImbueItem" && old[i] == 0 && framework[i] == PACKET_CAST_COUNT)
            continue;
        if (i >= 3 && i < 7)
        {
            spellDiffers = true;
            continue;
        }
        return false;
    }
    if (unitNamed)
    {
        // NONE and the unit's raw guid became UNIT and its packed guid
        std::vector<std::uint8_t> named(framework.begin(), framework.begin() + 20);
        Packet::PutU32(named, PACKET_TARGET_UNIT);
        Packet::PutPacked(named, line.tailGuid);
        if (named != framework)
            return false;
    }
    if (selfUnit)
    {
        // the whole mask is NONE and nothing follows it
        for (std::size_t i = 20; i < framework.size(); ++i)
            if (framework[i] != 0)
                return false;
    }
    if (!count)
        return true;
    ++counts.compared;
    counts.selfUnit += selfUnit ? 1 : 0;
    bool const equal =
        !selfUnit && !unitNamed && !spellDiffers && old.size() == framework.size() && old[2] == framework[2];
    counts.equal += equal ? 1 : 0;
    counts.tail += !unitNamed && !selfUnit && old.size() > framework.size() ? 1 : 0;
    counts.castCount += old[2] != framework[2] ? 1 : 0;
    counts.unit += unitNamed ? 1 : 0;
    if (spellDiffers)
    {
        ++counts.spell;
        std::cout << "spell differs: " << line.site << " " << line.entry << " old " << line.spell << "\n";
    }
    return true;
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

    Records const records = LoadDump(root);
    Check(!records.empty(), "the item use dump gives records");
    Differences counts;
    bool mutationChecked = false;
    bool spotMutationChecked = false;
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

        // the framework, for the same use
        if (line.segments.front().trade)
        {
            ++counts.outside;
            continue;
        }
        auto const found = records.find(line.entry);
        Record const* record = found == records.end() ? nullptr : &found->second.front();
        counts.noRecord += record ? 0 : 1;
        bool unitNamed = false;
        std::vector<std::uint8_t> const old = OldBytes(line);
        std::vector<std::uint8_t> const framework = UsePacketBytes(FrameworkFields(line, record, unitNamed));
        Check(Compare(line, old, framework, unitNamed, counts, true),
              "the framework sends what the handler read: " + text + "\n  framework " + Hex(framework));
        if (!mutationChecked)
        {
            // a flipped byte of the framework's output must be found
            std::vector<std::uint8_t> mutated = framework;
            mutated[10] ^= 0x01;
            Check(!Compare(line, old, mutated, unitNamed, counts, false), "a flipped byte fails the comparison");
            std::cout << "mutation: byte 10 flipped, the comparison fails\n";
            mutationChecked = true;
        }
        if (!spotMutationChecked && line.hasPos)
        {
            // a flipped byte of the spot's x must be found, and the two kinds of spot users each have a line
            std::vector<std::uint8_t> mutated = framework;
            mutated[26] ^= 0x01;
            Check(!Compare(line, old, mutated, unitNamed, counts, false),
                  "a flipped coordinate byte fails the comparison");
            Check(framework.size() == 20 + 4 + 1 + 12 && old.size() == framework.size(),
                  "a spot packet is byte for byte");
            std::cout << "mutation: spot byte 26 flipped, the comparison fails\n";
            spotMutationChecked = true;
        }
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
    Check(synthetic.count("RocketPack") && synthetic.count("Flare") && synthetic.count("TkWorn"),
          "the three raid script packets have lines");
    Check(synthetic.count("PvpTrinket") > 0, "the PvP trinket's packet has a line");
    Check(recordedLines >= 5, "at least five recorded lines");
    std::cout << "framework vs old: " << counts.compared << " compared, " << counts.equal << " byte for byte, "
              << counts.tail << " without the unread tail, " << counts.castCount << " cast count 1 for 0, "
              << counts.spell << " the record's spell, " << counts.unit << " the unit named, " << counts.selfUnit
              << " self for unit, " << counts.outside << " trade slot outside, " << counts.noRecord
              << " entries without a record\n";
    Check(counts.compared > 0 && counts.outside >= 1, "every line compared but the trade slot's");

    // a spot, which no old builder sent: the destination with an empty transport guid, then x, y, z
    PacketFields spot;
    spot.targetMask = PACKET_TARGET_DEST;
    spot.x = 1.0f;
    std::vector<std::uint8_t> const spotBytes = UsePacketBytes(spot);
    Check(spotBytes.size() == 20 + 4 + 1 + 12 && spotBytes[24] == 0 && spotBytes[28] == 0x3f,
          "a spot: mask, empty packed guid, three floats");

    std::cout << passed << " passed, " << failed << " failed\n";
    return failed ? 1 : 0;
}
