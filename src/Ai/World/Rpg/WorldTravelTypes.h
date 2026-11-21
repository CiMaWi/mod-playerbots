/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_WORLDTRAVELTYPES_H
#define _PLAYERBOT_WORLDTRAVELTYPES_H

#include <cstdint>

// World travel's plain types (WorldTravelRules.h): a spot.
namespace WorldTravel
{
struct SightPoint
{
    uint32_t MapId = 0;
    float X = 0.0f;
    float Y = 0.0f;
    float Z = 0.0f;
};
}  // namespace WorldTravel

#endif
