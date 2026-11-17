/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_ITEMUSEPACKET_H
#define _PLAYERBOT_ITEMUSEPACKET_H

#include <cstdint>
#include <cstring>
#include <vector>

// The bytes of CMSG_USE_ITEM in the order the game's handler reads them (WorldSession::HandleUseItemOpcode, then
// SpellCastTargets::Read): the item's place, the cast counter, the spell, the item's guid, the glyph index and the
// cast flags, then one target mask and its fields. Std only: ItemUse::BuildPacket sends these bytes and the parity
// selftest compares them with the packets of the old builders (item-use plan WI5)
namespace ItemUse
{
// TARGET_FLAG_* of the core's SharedDefines.h (ItemUse.cpp asserts that they agree)
constexpr std::uint32_t PACKET_TARGET_NONE = 0x0;
constexpr std::uint32_t PACKET_TARGET_UNIT = 0x2;
constexpr std::uint32_t PACKET_TARGET_ITEM = 0x10;
constexpr std::uint32_t PACKET_TARGET_DEST = 0x40;
constexpr std::uint32_t PACKET_TARGET_GAMEOBJECT = 0x800;
constexpr std::uint8_t PACKET_CAST_COUNT = 1;  // the client's cast counter; the handler only echoes it

struct PacketFields
{
    std::uint8_t bag{0};
    std::uint8_t slot{0};
    std::uint32_t spellId{0};
    std::uint64_t itemGuid{0};
    std::uint32_t targetMask{PACKET_TARGET_NONE};  // one of the masks above
    std::uint64_t targetGuid{0};                   // UNIT, ITEM, GAMEOBJECT
    float x{0.0f};                                 // DEST: a spot with no transport
    float y{0.0f};
    float z{0.0f};
};

namespace Packet
{
inline void PutU32(std::vector<std::uint8_t>& out, std::uint32_t value)
{
    for (int i = 0; i < 4; ++i)
        out.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
}

inline void PutU64(std::vector<std::uint8_t>& out, std::uint64_t value)
{
    for (int i = 0; i < 8; ++i)
        out.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
}

inline void PutFloat(std::vector<std::uint8_t>& out, float value)
{
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    PutU32(out, bits);
}

// A packed guid (ObjectGuid::WriteAsPacked): a mask byte with one bit per non-zero byte, then those bytes
inline void PutPacked(std::vector<std::uint8_t>& out, std::uint64_t guid)
{
    std::size_t const mask = out.size();
    out.push_back(0);
    for (int i = 0; i < 8; ++i)
    {
        std::uint8_t const byte = static_cast<std::uint8_t>(guid >> (8 * i));
        if (!byte)
            continue;
        out[mask] |= static_cast<std::uint8_t>(1u << i);
        out.push_back(byte);
    }
}
}  // namespace Packet

// Little endian, as ByteBuffer writes on the server's machines; nothing after the target's fields (the handler
// reads no more)
inline std::vector<std::uint8_t> UsePacketBytes(PacketFields const& fields)
{
    std::vector<std::uint8_t> out;
    out.reserve(1 + 1 + 1 + 4 + 8 + 4 + 1 + 4 + 9 + 12);
    out.push_back(fields.bag);
    out.push_back(fields.slot);
    out.push_back(PACKET_CAST_COUNT);
    Packet::PutU32(out, fields.spellId);
    Packet::PutU64(out, fields.itemGuid);
    Packet::PutU32(out, 0);  // glyph index
    out.push_back(0);        // cast flags
    Packet::PutU32(out, fields.targetMask);
    switch (fields.targetMask)
    {
        case PACKET_TARGET_DEST:
            Packet::PutPacked(out, 0);
            Packet::PutFloat(out, fields.x);
            Packet::PutFloat(out, fields.y);
            Packet::PutFloat(out, fields.z);
            break;
        case PACKET_TARGET_UNIT:
        case PACKET_TARGET_ITEM:
        case PACKET_TARGET_GAMEOBJECT:
            Packet::PutPacked(out, fields.targetGuid);
            break;
        default:
            break;
    }
    return out;
}
}  // namespace ItemUse

#endif
