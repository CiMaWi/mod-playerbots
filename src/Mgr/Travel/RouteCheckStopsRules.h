/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_ROUTECHECKSTOPSRULES_H
#define PLAYERBOTS_ROUTECHECKSTOPSRULES_H

// The pure rules of `route check stops` (navigation plan NV31, §3.8 5): where a vessel stands still for boarding and
// its pose there. Templates over the engine's containers (TransportAnimation::Path and ::Rotations, KeyFrameVec), so
// the selftest feeds them plain structs with the same member names. Engine copies are marked with their source.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <vector>

namespace RouteCheckStopsRules
{
constexpr double PI = 3.14159265358979323846;  // M_PI

// Position::NormalizeOrientation (Position.h)
inline float NormalizeOrientation(float o)
{
    if (o < 0)
    {
        float mod = o * -1;
        mod = std::fmod(mod, 2.0f * static_cast<float>(PI));
        mod = -mod + 2.0f * static_cast<float>(PI);
        return mod;
    }
    return std::fmod(o, 2.0f * static_cast<float>(PI));
}

// a moving MotionTransport's heading from its spline's derivative (Transport.cpp:357-358)
inline float HeadingFromDirection(float dx, float dy)
{
    return NormalizeOrientation(static_cast<float>(std::atan2(dy, dx) + PI));
}

// the smaller angle between two headings, radians in [0, pi]
inline float AngleDelta(float a, float b)
{
    float const full = 2.0f * static_cast<float>(PI);
    float d = std::fmod(std::fabs(a - b), full);
    return std::min(d, full - d);
}

// Type 15. A stop is a stop frame; stops are numbered in key-frame order. The frame a stop is arrived from is the one
// before it, the last for the first: the engine always teleports from the last frame to the first
// (TransportMgr.cpp, "last to first is always teleport"), and a teleport sets the next frame's InitialOrientation.
inline std::size_t ArrivingFrame(std::size_t frameCount, std::size_t stopFrame)
{
    return stopFrame ? stopFrame - 1 : frameCount - 1;
}

// the stop a MotionTransport stands at by its timer (GetPathProgress() % period), as the engine waits in Update:
// a stop frame with ArriveTime <= timer < DepartureTime; -1 while it moves. frameIndex gets the frame.
template <class Frames>
int StopAt(Frames const& frames, std::uint32_t timer, std::size_t& frameIndex)
{
    int stop = 0;
    for (std::size_t i = 0; i < frames.size(); ++i)
    {
        if (!frames[i].IsStopFrame())
            continue;
        if (frames[i].ArriveTime <= timer && timer < frames[i].DepartureTime)
        {
            frameIndex = i;
            return stop;
        }
        ++stop;
    }
    return -1;
}

// Type 11. TransportAnimation::GetAnimNode (TransportMgr.cpp), returning false where the engine asserts.
template <class Path>
bool AnimNode(Path const& path, std::uint32_t time, typename Path::mapped_type& curr, typename Path::mapped_type& next,
              float& percPos)
{
    if (path.empty())
        return false;

    for (auto itr = path.rbegin(); itr != path.rend(); ++itr)
        if (time >= itr->first)
        {
            if (itr == path.rbegin())
                return false;
            curr = itr->second;
            --itr;
            next = itr->second;
            percPos = float(time - curr->TimeSeg) / float(next->TimeSeg - curr->TimeSeg);
            return true;
        }

    return false;
}

struct Turn
{
    float x;
    float y;
    float z;
    float w;
};

// TransportAnimation::GetAnimRotation (TransportMgr.cpp)
template <class Rotations>
void AnimRotation(Rotations const& rotations, std::uint32_t totalTime, std::uint32_t time, Turn& curr, Turn& next,
                  float& percRot)
{
    if (rotations.empty())
    {
        curr = {0.0f, 0.0f, 0.0f, 1.0f};
        next = {0.0f, 0.0f, 0.0f, 1.0f};
        percRot = 0.0f;
        return;
    }

    for (auto itr = rotations.rbegin(); itr != rotations.rend(); ++itr)
        if (time >= itr->first)
        {
            std::uint32_t currSeg = itr->second->TimeSeg, nextSeg;
            curr = {itr->second->X, itr->second->Y, itr->second->Z, itr->second->W};
            if (itr != rotations.rbegin())
            {
                --itr;
                next = {itr->second->X, itr->second->Y, itr->second->Z, itr->second->W};
                nextSeg = itr->second->TimeSeg;
            }
            else
            {
                auto const& first = rotations.begin()->second;
                next = {first->X, first->Y, first->Z, first->W};
                nextSeg = totalTime;
            }
            percRot = float(time - currSeg) / float(nextSeg - currSeg);
            return;
        }

    curr = {0.0f, 0.0f, 0.0f, 1.0f};
    next = {0.0f, 0.0f, 0.0f, 1.0f};
    percRot = 0.0f;
}

struct Pose
{
    float x;
    float y;
    float z;
    float o;
};

// What a type-11 car needs besides its animation: the spawn's position and heading (the stationary position) and the
// z and w of its path rotation (gameobject_addon.parent_rotation, else the spawn's rotation; Transport.cpp:928-932).
struct Spawn
{
    float x;
    float y;
    float z;
    float o;
    float pathRotationZ;
    float pathRotationW;
};

// StaticTransport::RelocateToProgress (Transport.cpp:1074-1119), the same float arithmetic in the same order; false
// where the engine leaves the car where it was (no node for the progress)
template <class Path, class Rotations>
bool PoseAtProgress(Path const& path, Rotations const& rotations, std::uint32_t totalTime, Spawn const& spawn,
                    std::uint32_t progress, Pose& pose)
{
    typename Path::mapped_type curr = nullptr;
    typename Path::mapped_type next = nullptr;
    float percPos;
    if (!AnimNode(path, progress, curr, next, percPos))
        return false;

    float x = curr->X;
    float y = curr->Y;
    float z = curr->Z;
    x += percPos * (next->X - curr->X);
    y += percPos * (next->Y - curr->Y);
    z += percPos * (next->Z - curr->Z);

    float sign = spawn.pathRotationZ >= 0.0f ? 1.0f : -1.0f;
    float pathRotAngle = sign * 2.0f * std::acos(spawn.pathRotationW);
    float cs = std::cos(pathRotAngle), sn = std::sin(pathRotAngle);
    float nx = x * cs - y * sn;
    float ny = x * sn + y * cs;
    x = nx;
    y = ny;

    x += spawn.x;
    y += spawn.y;
    z += spawn.z;

    Turn currRot, nextRot;
    float percRot;
    AnimRotation(rotations, totalTime, progress, currRot, nextRot, percRot);
    float signCurr = currRot.z >= 0.0f ? 1.0f : -1.0f;
    float oriRotAngleCurr = signCurr * 2.0f * std::acos(currRot.w);
    float signNext = nextRot.z >= 0.0f ? 1.0f : -1.0f;
    float oriRotAngleNext = signNext * 2.0f * std::acos(nextRot.w);
    float oriRotAngle = oriRotAngleCurr + percRot * (oriRotAngleNext - oriRotAngleCurr);

    pose = {x, y, z, NormalizeOrientation(spawn.o + oriRotAngle)};
    return true;
}

// A type-11 stop: a span of progress (ms) in which the car stands. With a pause time (gameobject_template
// transport.pauseAtTime) it stands at progress 0 and at the pause time until its state changes (StaticTransport::
// Update), two spans of length 0. Without one the progress loops over the period (the last key's time) and a stop is
// a run of consecutive keys at one position; a run that ends the period joins the one that starts it when the two
// keys hold one position (wrap: end < start). Numbered in path order: the stop the period starts in first.
struct Span
{
    std::uint32_t start;
    std::uint32_t end;
    bool wrap;
};

template <class Path>
std::vector<Span> StopSpans(Path const& path, std::uint32_t period, std::uint32_t pauseTime)
{
    std::vector<Span> spans;
    if (pauseTime)
    {
        spans.push_back({0, 0, false});
        spans.push_back({pauseTime, pauseTime, false});
        return spans;
    }
    if (path.size() < 2)
        return spans;

    auto const same = [](auto const* a, auto const* b) { return a->X == b->X && a->Y == b->Y && a->Z == b->Z; };
    auto first = path.begin();
    auto last = path.begin();
    for (auto next = std::next(path.begin()); next != path.end(); ++next)
    {
        auto const prev = std::prev(next);
        last = next;
        if (!same(prev->second, next->second))
            continue;
        if (!spans.empty() && spans.back().end == prev->first)
            spans.back().end = next->first;
        else
            spans.push_back({prev->first, next->first, false});
    }

    if (spans.size() >= 2 && first->first == 0 && spans.front().start == 0 && spans.back().end == last->first &&
        last->first == period && same(first->second, last->second))
    {
        Span const joined{spans.back().start, spans.front().end, true};
        spans.pop_back();
        spans.front() = joined;
    }
    return spans;
}

// the last progress of a span the engine evaluates: the period itself never is (the progress loops below it)
inline std::uint32_t SpanEndProgress(Span const& span, std::uint32_t period)
{
    if (span.wrap)
        return span.end;
    return period && span.end >= period ? period - 1 : span.end;
}
}  // namespace RouteCheckStopsRules

#endif
