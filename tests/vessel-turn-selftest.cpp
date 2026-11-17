/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// Offline selftest of the turn between world coordinates and a vessel's own (the core's VesselFrame.h), the invariant
// of the path finder's vessel mode (navigation plan, NV30b): a deck offset turned into the world and back is the
// offset, a world point turned into an offset and back is the point, and a foot logged on a docked vessel lands on
// the offset NV30 recorded for it (hub, measurements/nav/nv30/vessel-report.json, key "checks").

#include <cmath>
#include <iostream>
#include <string>

#include "VesselFrame.h"

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

// The engine's formulas, copied from TransportBase (the core's VehicleDefines.h, which needs the whole game to
// compile): VesselFrame must give the same floats, since the spline turns the path finder's world points back with
// these.
void EnginePassengerPosition(float& x, float& y, float& z, float transX, float transY, float transZ, float transO)
{
    float inx = x, iny = y, inz = z;

    x = transX + inx * std::cos(transO) - iny * std::sin(transO);
    y = transY + iny * std::cos(transO) + inx * std::sin(transO);
    z = transZ + inz;
}

void EnginePassengerOffset(float& x, float& y, float& z, float transX, float transY, float transZ, float transO)
{
    z -= transZ;
    y -= transY;
    x -= transX;
    float inx = x, iny = y;
    y = (iny - inx * std::tan(transO)) / (std::cos(transO) + std::sin(transO) * std::tan(transO));
    x = (inx + iny * std::tan(transO)) / (std::cos(transO) + std::sin(transO) * std::tan(transO));
}

float const TOLERANCE = 0.01f;  // yards

bool Near(float ax, float ay, float az, float bx, float by, float bz)
{
    return std::fabs(ax - bx) <= TOLERANCE && std::fabs(ay - by) <= TOLERANCE && std::fabs(az - bz) <= TOLERANCE;
}

std::string Name(VesselFrame::Pose const& pose, float x, float y, float z)
{
    return "pose (" + std::to_string(pose.x) + ", " + std::to_string(pose.y) + ", " + std::to_string(pose.z) + ", o " +
           std::to_string(pose.o) + ") point (" + std::to_string(x) + ", " + std::to_string(y) + ", " +
           std::to_string(z) + ")";
}

// a foot logged on a docked vessel: the vessel's model, its entry, its pose at the stop, the foot in the world and
// the offset NV30's report recorded for it
struct DockedFoot
{
    int model;
    int entry;
    float poseX, poseY, poseZ, poseO;
    float worldX, worldY, worldZ;
    float offsetX, offsetY, offsetZ;
};

DockedFoot const DOCKED_FEET[] = {
    {3015, 176231, -4016.38989f, -4740.58838f, 0.0f, 2.45594461f, -4008.07f, -4737.27f, 6.1f, -4.33855708f,
     -7.83640055f, 6.1f},
    {3015, 176310, 6406.04102f, 822.885803f, 0.0f, 4.42249046f, 6415.41f, 825.3f, 6.1f, -4.99163017f, 8.28793355f,
     6.1f},
    {3031, 164871, 2062.37598f, 292.998047f, 114.972954f, 2.87231028f, 2070.37f, 296.3f, 97.41f, -6.82748457f,
     -5.30968545f, -17.5629538f},
    {3031, 175080, 1360.85376f, -4631.31152f, 71.8596878f, 2.16994423f, 1367.09f, -4634.25f, 54.19f, -5.94350137f,
     -3.49286819f, -17.6696878f},
    {455, 20652, 1595.26f, 188.644f, 55.7178809f, 1.56207f, 1595.63f, 191.13f, 55.25f, 2.48913405f, -0.348292539f,
     -0.46788092f},
    {3031, 176495, 2062.59888f, 235.264435f, 117.733627f, 6.07047914f, 2056.7f, 233.56f, 100.01f, -5.40611904f,
     -2.91130973f, -17.7236273f},
    {3015, 176231, -3905.22485f, -585.808472f, 0.0f, 3.64336478f, -3899.66f, -593.51f, 6.1f, -1.17460194f, 9.4287562f,
     6.1f},
    {3015, 20808, -1005.6134f, -3841.64771f, 0.0f, 2.62347502f, -997.82f, -3836.79f, 6.1f, -4.3647846f, -8.07979513f,
     6.1f},
    {3031, 164871, 1318.10681f, -4658.04688f, 71.8604279f, 4.8073145f, 1313.4f, -4652.33f, 54.22f, -6.1372632f,
     -4.14375855f, -17.6404279f},
    {3015, 176310, -8650.71875f, 1346.05127f, 0.0f, 3.55644463f, -8645.63f, 1339.22f, 6.1f, -1.90372812f, 8.30285729f,
     6.1f},
    {3831, 176085, 4.49883f, -11.3475f, 7.01107f, 1.5708f, 5.5f, -11.35f, -3.46f, -0.0025036775f, -1.00116999f,
     -10.47107f},
    {3031, 175080, -12464.0088f, 231.564774f, 49.5343666f, 1.07984916f, -12458.44f, 220.81f, 33.48f, -6.85902271f,
     -9.98150156f, -16.0543666f},
    {3031, 190549, 1125.35413f, -4134.94434f, 70.2978439f, 1.31018974f, 1133.81f, -4143.91f, 52.71f, -6.48413095f,
     -10.4805049f, -17.5878439f},
    {3031, 176495, -12406.8906f, 211.837677f, 49.6984367f, 1.60867368f, -12402.9f, 206.5f, 32.01f, -5.48496667f,
     -3.78563393f, -17.6884367f},
    {455, 20655, 1544.24f, 240.773f, 55.7178809f, 6.27445831f, 1548.0f, 240.75f, 55.25f, 3.76005754f, 0.00981397933f,
     -0.46788092f},
    {3015, 20808, -14277.7451f, 582.86615f, 0.0f, 2.81907836f, -14275.69f, 574.31f, 6.1f, -4.66104914f, 7.46363375f,
     6.1f},
    {3015, 176310, 6406.04102f, 822.885803f, 0.0f, 4.42249046f, 6414.54f, 822.34f, 6.1f, -1.90644858f, 8.30036688f,
     6.1f},
    {3015, 20808, -1005.6134f, -3841.64771f, 0.0f, 2.62347502f, -1005.26f, -3850.44f, 6.1f, -4.66136644f, 7.46331071f,
     6.1f},
    {3031, 190549, -1026.85596f, 375.784729f, 150.412735f, 0.133188562f, -1031.57f, 363.46f, 132.78f, -6.30895714f,
     -11.5895732f, -17.632735f},
    {3031, 190549, -1026.85596f, 375.784729f, 150.412735f, 0.133188562f, -1028.71f, 359.45f, 131.93f, -4.0067952f,
     -15.9438527f, -18.482735f},
    {3031, 190549, 1125.35413f, -4134.94434f, 70.2978439f, 1.31018974f, 1135.61f, -4143.15f, 52.59f, -5.28599315f,
     -12.0238992f, -17.7078439f},
    {3015, 176231, -3905.22485f, -585.808472f, 0.0f, 3.64336478f, -3904.0f, -575.65f, 6.6f, -5.95988919f, -8.31712293f,
     6.6f},
    {3015, 176310, -8650.71875f, 1346.05127f, 0.0f, 3.55644463f, -8641.88f, 1340.8f, 6.6f, -5.97246415f, 8.36833348f,
     6.6f},
    {3015, 20808, -14277.7451f, 582.86615f, 0.0f, 2.81907836f, -14269.66f, 589.11f, 6.6f, -5.68925858f, -8.48452243f,
     6.6f},
    {3015, 176231, -4016.38989f, -4740.58838f, 0.0f, 2.45594461f, -4022.81f, -4748.14f, 6.6f, 0.187724485f, 9.91007164f,
     6.6f},
    {3831, 176081, 4.52807f, 8.43529f, 7.01107f, 1.5708f, 5.53f, 8.44f, -3.46f, 0.00470631971f, -1.00193002f,
     -10.47107f},
    {455, 20652, 1595.26f, 188.644f, 55.7178809f, 1.56207f, 1595.26f, 188.64f, 55.11f, -0.0039998477f,
     -0.0000349048642f, -0.60788092f},
    {3015, 176231, -3905.22485f, -585.808472f, 0.0f, 3.64336478f, -3905.19f, -576.86f, 6.1f, -4.33459284f, -7.82864388f,
     6.1f},
    {3015, 176231, -3905.22485f, -585.808472f, 0.0f, 3.64336478f, -3899.66f, -593.5f, 6.1f, -1.17941174f, 9.41998888f,
     6.1f},
    {3015, 176310, -8650.71875f, 1346.05127f, 0.0f, 3.55644463f, -8642.81f, 1340.48f, 6.1f, -4.99237304f, 8.28634914f,
     6.1f},
    {3031, 164871, 1318.10681f, -4658.04688f, 71.8604279f, 4.8073145f, 1318.03f, -4653.05f, 53.92f, -4.98165927f,
     0.397153207f, -17.9404279f},
    {3031, 175080, -12464.0088f, 231.564774f, 49.5343666f, 1.07984916f, -12463.73f, 224.68f, 31.86f, -5.94015196f,
     -3.49176532f, -17.6743666f},
    {3031, 190549, 1125.35413f, -4134.94434f, 70.2978439f, 1.31018974f, 1133.73f, -4144.34f, 52.73f, -6.92022485f,
     -10.5140029f, -17.5678439f},
    {3031, 176495, -12406.8906f, 211.837677f, 49.6984367f, 1.60867368f, -12403.78f, 206.55f, 31.98f, -5.40167843f,
     -2.90815854f, -17.7184367f},
    {3015, 20808, -14277.7451f, 582.86615f, 0.0f, 2.81907836f, -14271.04f, 589.15f, 6.1f, -4.36773121f, -8.08506602f,
     6.1f},
    {3015, 176231, -4016.38989f, -4740.58838f, 0.0f, 2.45594461f, -4021.44f, -4748.63f, 6.1f, -1.18292331f, 9.42188661f,
     6.1f},
    {3031, 164871, 2062.37598f, 292.998047f, 114.972954f, 2.87231028f, 2069.4f, 295.36f, 97.33f, -6.14251872f,
     -4.14550257f, -17.6429538f},
    {3031, 175080, 1360.85376f, -4631.31152f, 71.8596878f, 2.16994423f, 1376.4f, -4626.43f, 53.99f, -4.7358889f,
     -15.5912223f, -17.8696878f},
    {3031, 176495, 2062.59888f, 235.264435f, 117.733627f, 6.07047914f, 2056.43f, 232.71f, 100.04f, -5.49059413f,
     -3.79915203f, -17.6936273f},
    {3031, 190549, -1026.85596f, 375.784729f, 150.412735f, 0.133188562f, -1028.62f, 359.21f, 131.92f, -3.94946311f,
     -16.1936787f, -18.492735f},
    {3031, 190549, 1125.35413f, -4134.94434f, 70.2978439f, 1.31018974f, 1134.93f, -4144.03f, 52.67f, -6.3114923f,
     -11.5936068f, -17.6278439f},
    {455, 20652, 1595.26f, 188.644f, 55.7178809f, 1.56207f, 1595.26f, 188.64f, 54.97f, -0.0039998477f,
     -0.0000349048642f, -0.74788092f},
    {3015, 176231, -4016.38989f, -4740.58838f, 0.0f, 2.45594461f, -4008.08f, -4737.27f, 6.1f, -4.33081699f, -7.8300688f,
     6.1f},
    {3015, 20808, -14277.7451f, 582.86615f, 0.0f, 2.81907836f, -14271.05f, 589.15f, 6.1f, -4.3582468f, -8.0818965f,
     6.1f},
    {3031, 164871, 1318.10681f, -4658.04688f, 71.8604279f, 4.8073145f, 1313.26f, -4652.65f, 54.71f, -5.83197348f,
     -4.31345883f, -17.1504279f},
    {455, 20652, 1595.26f, 188.644f, 55.7178809f, 1.56207f, 1595.26f, 188.64f, 55.14f, -0.0039998477f,
     -0.0000349048642f, -0.57788092f},
    {3031, 176495, 2062.59888f, 235.264435f, 117.733627f, 6.07047914f, 2056.71f, 233.57f, 100.01f, -5.39845546f,
     -2.89942404f, -17.7236273f},
    {3015, 176231, -3905.22485f, -585.808472f, 0.0f, 3.64336478f, -3905.19f, -576.85f, 6.1f, -4.33940264f, -7.8374112f,
     6.1f},
    {3031, 164871, 1318.10681f, -4658.04688f, 71.8604279f, 4.8073145f, 1313.84f, -4652.36f, 54.2f, -6.06569373f,
     -3.70858294f, -17.6604279f},
    {3031, 176495, -12406.8906f, 211.837677f, 49.6984367f, 1.60867368f, -12403.77f, 206.55f, 31.98f, -5.40205712f,
     -2.91815136f, -17.7184367f},
    {3031, 175080, 1360.85376f, -4631.31152f, 71.8596878f, 2.16994423f, 1376.4f, -4626.42f, 54.04f, -4.72763073f,
     -15.5968617f, -17.8196878f},
    {3031, 190549, -1026.85596f, 375.784729f, 150.412735f, 0.133188562f, -1032.31f, 364.44f, 132.85f, -6.9122641f,
     -10.5199841f, -17.562735f},
};

// deck offsets a passenger can have: the hull's length and width, decks above and below the vessel's origin
float const OFFSETS_XY[] = {-45.0f, -12.5f, -1.0f, 0.0f, 0.37f, 7.25f, 38.0f};
float const OFFSETS_Z[] = {-17.5f, 0.0f, 6.1f, 31.0f};

void CheckPose(VesselFrame::Pose const& pose)
{
    for (float const ox : OFFSETS_XY)
    {
        for (float const oy : OFFSETS_XY)
        {
            for (float const oz : OFFSETS_Z)
            {
                // offset -> world -> offset
                float x = ox, y = oy, z = oz;
                VesselFrame::ToWorld(pose, x, y, z);
                float const worldX = x, worldY = y, worldZ = z;

                float ex = ox, ey = oy, ez = oz;
                EnginePassengerPosition(ex, ey, ez, pose.x, pose.y, pose.z, pose.o);
                Check(x == ex && y == ey && z == ez, "to world equals the engine's formula: " + Name(pose, ox, oy, oz));

                VesselFrame::ToOffset(pose, x, y, z);
                Check(Near(x, y, z, ox, oy, oz), "offset -> world -> offset: " + Name(pose, ox, oy, oz));

                ex = worldX;
                ey = worldY;
                ez = worldZ;
                EnginePassengerOffset(ex, ey, ez, pose.x, pose.y, pose.z, pose.o);
                Check(x == ex && y == ey && z == ez,
                      "to offset equals the engine's formula: " + Name(pose, worldX, worldY, worldZ));

                // world -> offset -> world
                VesselFrame::ToWorld(pose, x, y, z);
                Check(Near(x, y, z, worldX, worldY, worldZ),
                      "world -> offset -> world: " + Name(pose, worldX, worldY, worldZ));
            }
        }
    }
}
}  // namespace

int main()
{
    float const pi = 3.14159265358979323846f;

    // the four headings where a sign or an axis swap shows, at the origin and far out on a map
    for (float const heading : {0.0f, pi / 2.0f, pi, 3.0f * pi / 2.0f})
    {
        CheckPose({0.0f, 0.0f, 0.0f, heading});
        CheckPose({-4016.39f, -4740.59f, 0.0f, heading});
        CheckPose({6406.04f, 822.886f, 114.97f, heading});
    }

    // the docked poses NV30 used (models 3015, 3031, 455, 3831), and the feet logged there
    for (DockedFoot const& foot : DOCKED_FEET)
    {
        VesselFrame::Pose const pose{foot.poseX, foot.poseY, foot.poseZ, foot.poseO};
        std::string const name = "model " + std::to_string(foot.model) + " entry " + std::to_string(foot.entry) + " " +
                                 Name(pose, foot.worldX, foot.worldY, foot.worldZ);
        CheckPose(pose);

        float x = foot.worldX, y = foot.worldY, z = foot.worldZ;
        VesselFrame::ToOffset(pose, x, y, z);
        Check(Near(x, y, z, foot.offsetX, foot.offsetY, foot.offsetZ),
              "a logged foot lands on its recorded offset: " + name);

        x = foot.offsetX;
        y = foot.offsetY;
        z = foot.offsetZ;
        VesselFrame::ToWorld(pose, x, y, z);
        Check(Near(x, y, z, foot.worldX, foot.worldY, foot.worldZ),
              "a recorded offset lands on its logged foot: " + name);
    }

    std::cout << "vessel-turn: " << passed << " passed, " << failed << " failed\n";
    return failed ? 1 : 0;
}
