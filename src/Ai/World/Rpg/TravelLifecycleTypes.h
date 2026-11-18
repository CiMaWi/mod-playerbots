/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_TRAVEL_LIFECYCLE_TYPES_H
#define PLAYERBOTS_TRAVEL_LIFECYCLE_TYPES_H

#include <cstdint>

// A transport passenger's plain types (TravelLifecycle.h): points, the dock pose, the motion a rider is
// given and its outcome, the probe cadence. The rules that maintain them stay in TravelLifecycle.h.
namespace TravelLifecycle
{
struct Point
{
    float X = 0.0f;
    float Y = 0.0f;
    float Z = 0.0f;
};

struct DockPose
{
    Point Position;
    float Orientation = 0.0f;
};

enum class MotionKind : std::uint8_t
{
    Board,
    Exit
};
enum class MotionState : std::uint8_t
{
    None,
    Moving,
    Reached,
    Cancelled
};
enum class Outcome : std::uint8_t
{
    Pending,
    Boarded,
    Exited,
    Cancelled,
    Lost
};
enum class Contact : std::uint8_t
{
    Unavailable,
    OffSurface,
    AboveDeck,
    Feet
};
enum class DockState : std::uint8_t
{
    Unavailable,
    Still,
    Departed
};

struct PassengerMotion
{
    MotionKind Kind = MotionKind::Board;
    std::uint64_t Transport = 0;
    std::uint32_t Entry = 0;
    std::uint32_t Map = 0;
    std::uint32_t Spline = 0;
    Point Endpoint;
    Outcome Result = Outcome::Pending;
    bool CompletionSampled = false;
    DockPose Dock;
    bool Jump = false;  // WT70c: a jump by a stored row: never stopped in the air when the vessel leaves
};

struct Cadence
{
    std::uint32_t LastProbeMs = 0;
    std::uint32_t Map = 0;
    bool Sampled = false;
    bool Revalidate = false;
    std::uint8_t OffSurface = 0;  // the rider's samples off its deck in a row (OFF_SURFACE_SAMPLES)
};
}  // namespace TravelLifecycle

#endif
