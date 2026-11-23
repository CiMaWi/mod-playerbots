/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// Offline selftest of `route check stops`' rules (RouteCheckStopsRules.h, navigation plan NV31): the stop spans of a
// time-keyed animation (runs of one position, the wrap included; pause-time stops at 0 and at the pause), the type-11
// pose bit-equal to a verbatim copy of StaticTransport::RelocateToProgress, heading B equal to the engine's formula
// for a moving MotionTransport, the frame a stop is arrived from and the stop a timer stands at.

#define _USE_MATH_DEFINES

#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#include "RouteCheckStopsRules.h"

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

// TransportAnimationEntry and TransportRotationEntry (DBCStructure.h)
struct AnimEntry
{
    std::uint32_t TransportEntry;
    std::uint32_t TimeSeg;
    float X;
    float Y;
    float Z;
};

struct RotationEntry
{
    std::uint32_t TransportEntry;
    std::uint32_t TimeSeg;
    float X;
    float Y;
    float Z;
    float W;
};

using PathMap = std::map<std::uint32_t, AnimEntry const*>;
using RotationMap = std::map<std::uint32_t, RotationEntry const*>;

PathMap MakePath(std::vector<AnimEntry> const& entries)
{
    PathMap path;
    for (AnimEntry const& entry : entries)
        path[entry.TimeSeg] = &entry;
    return path;
}

// ---- the engine, verbatim (G3D's Vector3 and Quat reduced to their float members) ----

struct Vector3
{
    float x, y, z;
    Vector3(float ax, float ay, float az) : x(ax), y(ay), z(az) {}
    Vector3& operator+=(Vector3 const& v)
    {
        x += v.x;
        y += v.y;
        z += v.z;
        return *this;
    }
};

struct Quat
{
    float x, y, z, w;
    Quat() : x(0.0f), y(0.0f), z(0.0f), w(1.0f) {}
    Quat(float ax, float ay, float az, float aw) : x(ax), y(ay), z(az), w(aw) {}
};

float EngineNormalizeOrientation(float o)
{
    if (o < 0)
    {
        float mod = o * -1;
        mod = std::fmod(mod, 2.0f * static_cast<float>(M_PI));
        mod = -mod + 2.0f * static_cast<float>(M_PI);
        return mod;
    }
    return std::fmod(o, 2.0f * static_cast<float>(M_PI));
}

bool EngineGetAnimNode(PathMap const& Path, std::uint32_t time, AnimEntry const*& curr, AnimEntry const*& next,
                       float& percPos)
{
    if (Path.empty())
        return false;

    for (PathMap::const_reverse_iterator itr = Path.rbegin(); itr != Path.rend(); ++itr)
        if (time >= itr->first)
        {
            curr = itr->second;
            if (itr == Path.rbegin())
                return false;  // ASSERT in the engine
            --itr;
            next = itr->second;
            percPos = float(time - curr->TimeSeg) / float(next->TimeSeg - curr->TimeSeg);
            return true;
        }

    return false;
}

void EngineGetAnimRotation(RotationMap const& Rotations, std::uint32_t TotalTime, std::uint32_t time, Quat& curr,
                           Quat& next, float& percRot)
{
    if (Rotations.empty())
    {
        curr = Quat(0.0f, 0.0f, 0.0f, 1.0f);
        next = Quat(0.0f, 0.0f, 0.0f, 1.0f);
        percRot = 0.0f;
        return;
    }

    for (RotationMap::const_reverse_iterator itr = Rotations.rbegin(); itr != Rotations.rend(); ++itr)
        if (time >= itr->first)
        {
            std::uint32_t currSeg = itr->second->TimeSeg, nextSeg;
            curr = Quat(itr->second->X, itr->second->Y, itr->second->Z, itr->second->W);
            if (itr != Rotations.rbegin())
            {
                --itr;
                next = Quat(itr->second->X, itr->second->Y, itr->second->Z, itr->second->W);
                nextSeg = itr->second->TimeSeg;
            }
            else
            {
                next = Quat(Rotations.begin()->second->X, Rotations.begin()->second->Y, Rotations.begin()->second->Z,
                            Rotations.begin()->second->W);
                nextSeg = TotalTime;
            }
            percRot = float(time - currSeg) / float(nextSeg - currSeg);
            return;
        }

    curr = Quat(0.0f, 0.0f, 0.0f, 1.0f);
    next = Quat(0.0f, 0.0f, 0.0f, 1.0f);
    percRot = 0.0f;
}

// StaticTransport::RelocateToProgress; GetFloatValue(GAMEOBJECT_PARENTROTATION + 2/3) and GetStationary*() are
// parameters, UpdatePosition writes the result
bool EngineRelocateToProgress(PathMap const& Path, RotationMap const& Rotations, std::uint32_t TotalTime,
                              float parentRotation2, float parentRotation3, float stationaryX, float stationaryY,
                              float stationaryZ, float stationaryO, std::uint32_t progress,
                              RouteCheckStopsRules::Pose& result)
{
    AnimEntry const *curr = nullptr, *next = nullptr;
    float percPos;
    if (EngineGetAnimNode(Path, progress, curr, next, percPos))
    {
        Vector3 pos = Vector3(curr->X, curr->Y, curr->Z);

        pos += Vector3(percPos * (next->X - curr->X), percPos * (next->Y - curr->Y), percPos * (next->Z - curr->Z));

        float sign = parentRotation2 >= 0.0f ? 1.0f : -1.0f;
        float pathRotAngle = sign * 2.0f * acos(parentRotation3);
        float cs = cos(pathRotAngle), sn = std::sin(pathRotAngle);
        float nx = pos.x * cs - pos.y * sn;
        float ny = pos.x * sn + pos.y * cs;
        pos.x = nx;
        pos.y = ny;

        pos += Vector3(stationaryX, stationaryY, stationaryZ);

        Quat currRot, nextRot;
        float percRot;
        EngineGetAnimRotation(Rotations, TotalTime, progress, currRot, nextRot, percRot);
        float signCurr = currRot.z >= 0.0f ? 1.0f : -1.0f;
        float oriRotAngleCurr = signCurr * 2.0f * acos(currRot.w);
        float signNext = nextRot.z >= 0.0f ? 1.0f : -1.0f;
        float oriRotAngleNext = signNext * 2.0f * acos(nextRot.w);
        float oriRotAngle = oriRotAngleCurr + percRot * (oriRotAngleNext - oriRotAngleCurr);

        result = {pos.x, pos.y, pos.z, EngineNormalizeOrientation(stationaryO + oriRotAngle)};
        return true;
    }
    return false;
}

// ---- checks ----

bool SameBits(float a, float b) { return std::memcmp(&a, &b, sizeof(float)) == 0; }

std::string SpansText(std::vector<RouteCheckStopsRules::Span> const& spans)
{
    std::string text;
    for (RouteCheckStopsRules::Span const& span : spans)
        text += "[" + std::to_string(span.start) + "," + std::to_string(span.end) + (span.wrap ? ",wrap" : "") + "]";
    return text;
}

void CheckSpans(std::string const& name, std::vector<AnimEntry> const& entries, std::uint32_t pause,
                std::string const& want)
{
    PathMap const path = MakePath(entries);
    std::uint32_t const period = path.empty() ? 0 : path.rbegin()->first;
    std::string const got = SpansText(RouteCheckStopsRules::StopSpans(path, period, pause));
    Check(got == want, name + ": spans " + got + ", want " + want);
}

void SpanChecks()
{
    // two stops inside the period, the last key elsewhere: no wrap
    CheckSpans("inside",
               {{1, 0, 1.0f, 2.0f, 3.0f},
                {1, 1000, 1.0f, 2.0f, 3.0f},
                {1, 3000, 5.0f, 2.0f, 3.0f},
                {1, 4000, 5.0f, 2.0f, 3.0f},
                {1, 6000, 9.0f, 2.0f, 3.0f},
                {1, 8000, 1.5f, 2.0f, 3.0f}},
               0, "[0,1000][3000,4000]");
    // a run of three keys is one stop
    CheckSpans("run",
               {{1, 0, 0.0f, 0.0f, 0.0f},
                {1, 1000, 0.0f, 0.0f, 9.0f},
                {1, 2000, 0.0f, 0.0f, 9.0f},
                {1, 3000, 0.0f, 0.0f, 9.0f},
                {1, 4000, 0.0f, 0.0f, 0.0f}},
               0, "[1000,3000]");
    // the run that ends the period joins the one that starts it, and is stop 0
    CheckSpans("wrap",
               {{1, 0, 4.0f, 4.0f, 4.0f},
                {1, 1000, 4.0f, 4.0f, 4.0f},
                {1, 2000, 4.0f, 4.0f, 20.0f},
                {1, 3000, 4.0f, 4.0f, 20.0f},
                {1, 5000, 4.0f, 4.0f, 4.0f},
                {1, 6000, 4.0f, 4.0f, 4.0f}},
               0, "[5000,1000,wrap][2000,3000]");
    // a run at each end at two places: two stops, no wrap
    CheckSpans("ends apart",
               {{1, 0, 4.0f, 4.0f, 4.0f},
                {1, 1000, 4.0f, 4.0f, 4.0f},
                {1, 3000, 4.0f, 4.0f, 20.0f},
                {1, 5000, 4.0f, 4.0f, 30.0f},
                {1, 6000, 4.0f, 4.0f, 30.0f}},
               0, "[0,1000][5000,6000]");
    // ends at one place but neither stands there: no stop
    CheckSpans("passing", {{1, 0, 4.0f, 4.0f, 4.0f}, {1, 3000, 4.0f, 4.0f, 20.0f}, {1, 6000, 4.0f, 4.0f, 4.0f}}, 0, "");
    // with a pause time the car stands at 0 and at the pause, whatever the keys say
    CheckSpans("pause", {{1, 0, 4.0f, 4.0f, 4.0f}, {1, 3000, 4.0f, 4.0f, 20.0f}, {1, 6000, 4.0f, 4.0f, 4.0f}}, 3000,
               "[0,0][3000,3000]");

    RouteCheckStopsRules::Span const wrap{5000, 1000, true};
    RouteCheckStopsRules::Span const atEnd{5000, 6000, false};
    Check(RouteCheckStopsRules::SpanEndProgress(wrap, 6000) == 1000, "a wrap span ends at its end");
    Check(RouteCheckStopsRules::SpanEndProgress(atEnd, 6000) == 5999, "a span at the period ends below it");
}

void PoseChecks()
{
    std::vector<AnimEntry> const entries = {{1, 0, 0.0f, 0.0f, 0.0f},           {1, 2500, 0.0f, 0.0f, 0.0f},
                                            {1, 7000, 0.317f, -1.25f, 42.123f}, {1, 9500, 0.317f, -1.25f, 42.123f},
                                            {1, 13333, 3.5f, 7.75f, 11.0f},     {1, 15000, 0.0f, 0.0f, 0.0f}};
    PathMap const path = MakePath(entries);
    std::vector<RotationEntry> const turns = {{1, 0, 0.0f, 0.0f, 0.0f, 1.0f},
                                              {1, 5000, 0.0f, 0.0f, 0.7071068f, 0.7071068f},
                                              {1, 11000, 0.0f, 0.0f, -0.3826834f, 0.9238795f}};
    RotationMap rotations;
    for (RotationEntry const& turn : turns)
        rotations[turn.TimeSeg] = &turn;
    RotationMap const noRotations;
    std::uint32_t const total = 15000;

    struct SpawnCase
    {
        float z;
        float w;
        float o;
    };
    std::vector<SpawnCase> const spawns = {
        {0.0f, 1.0f, 0.0f}, {0.3826834f, 0.9238795f, 1.5620700f}, {-0.7071068f, 0.7071068f, 5.9f}, {1.0f, 0.0f, 3.1f}};
    int compared = 0;
    int differing = 0;
    for (SpawnCase const& sc : spawns)
        for (RotationMap const* rot : {static_cast<RotationMap const*>(&rotations), &noRotations})
            for (std::uint32_t progress = 0; progress < total; progress += 7)
            {
                RouteCheckStopsRules::Spawn const spawn{1595.26f, 188.644f, 55.7178809f, sc.o, sc.z, sc.w};
                RouteCheckStopsRules::Pose ours{}, engine{};
                bool const a = RouteCheckStopsRules::PoseAtProgress(path, *rot, total, spawn, progress, ours);
                bool const b = EngineRelocateToProgress(path, *rot, total, sc.z, sc.w, spawn.x, spawn.y, spawn.z,
                                                        spawn.o, progress, engine);
                ++compared;
                if (a != b || !SameBits(ours.x, engine.x) || !SameBits(ours.y, engine.y) ||
                    !SameBits(ours.z, engine.z) || !SameBits(ours.o, engine.o))
                    ++differing;
            }
    Check(compared > 8000 && differing == 0, "type-11 pose bit-equal to RelocateToProgress over " +
                                                 std::to_string(compared) + " progresses, " +
                                                 std::to_string(differing) + " differ");

    RouteCheckStopsRules::Pose pose{};
    RouteCheckStopsRules::Spawn const plain{10.0f, 20.0f, 30.0f, 1.0f, 0.0f, 1.0f};
    Check(!RouteCheckStopsRules::PoseAtProgress(path, rotations, total, plain, total, pose),
          "no pose at the period itself (the engine asserts there)");
    Check(RouteCheckStopsRules::PoseAtProgress(path, noRotations, total, plain, 8000, pose) &&
              std::fabs(pose.x - 10.317f) < 1e-4f && std::fabs(pose.y - 18.75f) < 1e-4f &&
              std::fabs(pose.z - 72.123f) < 1e-3f && std::fabs(pose.o - 1.0f) < 1e-6f,
          "a standing car's pose is its key plus the spawn");
}

void HeadingChecks()
{
    int differing = 0;
    int compared = 0;
    for (int i = -12; i <= 12; ++i)
        for (int j = -12; j <= 12; ++j)
        {
            float const dx = i * 0.25f + (i % 3) * 1e-4f;
            float const dy = j * 0.25f - (j % 5) * 1e-5f;
            Vector3 const dir(dx, dy, 0.0f);
            float const engine = EngineNormalizeOrientation(std::atan2(dir.y, dir.x) + M_PI);
            ++compared;
            if (!SameBits(engine, RouteCheckStopsRules::HeadingFromDirection(dx, dy)))
                ++differing;
        }
    Check(differing == 0, "heading B bit-equal to the moving spline's formula over " + std::to_string(compared) +
                              " directions, " + std::to_string(differing) + " differ");
    Check(std::fabs(RouteCheckStopsRules::HeadingFromDirection(1.0f, 0.0f) - static_cast<float>(M_PI)) < 1e-6f,
          "a vessel moving along +x faces -x (the model's front)");

    Check(std::fabs(RouteCheckStopsRules::AngleDelta(0.1f, 6.1831853f) - 0.2f) < 1e-5f, "angle delta over 0");
    Check(std::fabs(RouteCheckStopsRules::AngleDelta(1.0f, 4.0f) - 3.0f) < 1e-5f, "angle delta inside");
}

struct Frame
{
    std::uint32_t ArriveTime;
    std::uint32_t DepartureTime;
    bool stop;
    bool IsStopFrame() const { return stop; }
};

void FrameChecks()
{
    Check(RouteCheckStopsRules::ArrivingFrame(5, 0) == 4, "the first frame is arrived at from the last");
    Check(RouteCheckStopsRules::ArrivingFrame(5, 3) == 2, "a frame is arrived at from the one before");

    std::vector<Frame> const frames = {
        {0, 30000, true}, {40000, 40000, false}, {60000, 60000, false}, {90000, 120000, true}, {150000, 150000, false}};
    std::size_t frame = 99;
    Check(RouteCheckStopsRules::StopAt(frames, 0, frame) == 0 && frame == 0, "standing at stop 0 from its arrival");
    Check(RouteCheckStopsRules::StopAt(frames, 29999, frame) == 0, "standing at stop 0 until its departure");
    Check(RouteCheckStopsRules::StopAt(frames, 30000, frame) == -1, "moving at the departure");
    Check(RouteCheckStopsRules::StopAt(frames, 40000, frame) == -1, "moving through a frame that is no stop");
    Check(RouteCheckStopsRules::StopAt(frames, 100000, frame) == 1 && frame == 3, "stop 1 is the fourth frame");
}
}  // namespace

// The window a looping car's stop record gets at load (WT70d, PLAN 3.21 (4)): the one stop span whose pose at its start
// lies at the record's pose, or of several spans there the one numbered as the record's stop; none for a car with a
// pause time, a pose off every span, or several spans and none by the record's number
void WindowChecks()
{
    using RouteCheckStopsRules::StopWindowAt;
    RotationMap const noRotations;
    RouteCheckStopsRules::Spawn const spawn{10.0f, 20.0f, 30.0f, 1.0f, 0.0f, 1.0f};
    // the bottom from 11 s over the period's end to 2 s, the top (20 yd up) from 6 to 9 s; period 13 s
    std::vector<AnimEntry> const loop = {{1, 0, 0.0f, 0.0f, 0.0f},     {1, 2000, 0.0f, 0.0f, 0.0f},
                                         {1, 6000, 0.0f, 0.0f, 20.0f}, {1, 9000, 0.0f, 0.0f, 20.0f},
                                         {1, 11000, 0.0f, 0.0f, 0.0f}, {1, 13000, 0.0f, 0.0f, 0.0f}};
    PathMap const path = MakePath(loop);
    RouteCheckStopsRules::Span window{};
    std::size_t matched = 9;
    Check(StopWindowAt(path, noRotations, 13000, 0, spawn, 10.0f, 20.0f, 30.0f, 3.0f, 0, window, matched) &&
              matched == 1 && window.start == 11000 && window.end == 2000 && window.wrap,
          "window: the bottom's span wraps the period's end (11 s to 2 s), span 0");
    Check(StopWindowAt(path, noRotations, 13000, 0, spawn, 10.0f, 20.0f, 50.0f, 3.0f, 1, window, matched) &&
              matched == 1 && window.start == 6000 && window.end == 9000 && !window.wrap,
          "window: the top's span, 6 s to 9 s, span 1");
    Check(!StopWindowAt(path, noRotations, 13000, 0, spawn, 10.0f, 20.0f, 40.0f, 3.0f, 0, window, matched) &&
              matched == 0,
          "window: a record half way up matches no span");
    Check(!StopWindowAt(path, noRotations, 13000, 5000, spawn, 10.0f, 20.0f, 30.0f, 3.0f, 0, window, matched),
          "window: a car with a pause time waits for a state change: no window");
    // the car stands at the bottom twice in its period (0 to 1 s, 5 to 6 s), as a lift that stops at one height going
    // up and going down
    std::vector<AnimEntry> const twice = {{1, 0, 0.0f, 0.0f, 0.0f},     {1, 1000, 0.0f, 0.0f, 0.0f},
                                          {1, 3000, 0.0f, 0.0f, 20.0f}, {1, 5000, 0.0f, 0.0f, 0.0f},
                                          {1, 6000, 0.0f, 0.0f, 0.0f},  {1, 8000, 0.0f, 0.0f, 10.0f}};
    PathMap const twicePath = MakePath(twice);
    Check(StopWindowAt(twicePath, noRotations, 8000, 0, spawn, 10.0f, 20.0f, 30.0f, 3.0f, 1, window, matched) &&
              matched == 2 && window.start == 5000 && window.end == 6000,
          "window: two spans at the record's pose: the one numbered as the record (span 1, 5 s to 6 s)");
    Check(StopWindowAt(twicePath, noRotations, 8000, 0, spawn, 10.0f, 20.0f, 30.0f, 3.0f, 0, window, matched) &&
              window.start == 0 && window.end == 1000,
          "window: two spans at the record's pose: record 0 takes span 0");
    Check(!StopWindowAt(twicePath, noRotations, 8000, 0, spawn, 10.0f, 20.0f, 30.0f, 3.0f, 2, window, matched) &&
              matched == 2,
          "window: two spans at the record's pose and none numbered as the record: no window");
    // G330 (PA36, 36b-1, written before the repair): the shape of 190587's far stop. The car stands at its top from
    // 2.5 s to 5.5 s, but the keys of the stand differ by float noise (1e-5 yd), so StopSpans cuts it into three
    // spans (2.5-3, 3.1-4, 4.1-5.5 s) at one pose, numbered 1 to 3 (the bottom's wrapping span is 0). Every record at
    // that pose stands in all of them: its window is the whole stand, whatever span number it carries. The
    // counter-case, a car that leaves the pose between two spans (one height passed going up and going down), keeps
    // one span per record: `twice` above
    float const top = 20.0f;
    float const noise = 20.00001f;
    std::vector<AnimEntry> const jitter = {{1, 0, 0.0f, 0.0f, 0.0f},     {1, 1000, 0.0f, 0.0f, 0.0f},
                                           {1, 2500, 0.0f, 0.0f, top},   {1, 3000, 0.0f, 0.0f, top},
                                           {1, 3100, 0.0f, 0.0f, noise}, {1, 4000, 0.0f, 0.0f, noise},
                                           {1, 4100, 0.0f, 0.0f, top},   {1, 5500, 0.0f, 0.0f, top},
                                           {1, 7000, 0.0f, 0.0f, 0.0f},  {1, 8000, 0.0f, 0.0f, 0.0f}};
    PathMap const jitterPath = MakePath(jitter);
    for (std::size_t stop = 1; stop <= 3; ++stop)
    {
        // the window is read before the message is built (argument order is unspecified)
        window = {};
        bool const found =
            StopWindowAt(jitterPath, noRotations, 8000, 0, spawn, 10.0f, 20.0f, 50.0f, 3.0f, stop, window, matched);
        Check(found && window.start == 2500 && window.end == 5500,
              "window (G330): a stand cut by float noise into three spans at one pose: record " + std::to_string(stop) +
                  " stands the whole 2.5 s to 5.5 s (got " + std::to_string(window.start) + " to " +
                  std::to_string(window.end) + ")");
    }
}

int main()
{
    SpanChecks();
    PoseChecks();
    WindowChecks();
    HeadingChecks();
    FrameChecks();

    std::cout << "route-check-stops: " << passed << " passed, " << failed << " failed\n";
    return failed ? 1 : 0;
}
