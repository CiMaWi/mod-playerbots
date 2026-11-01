/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "ArenaJump.h"

#include <cctype>
#include <sstream>
#include <string>
#include <vector>

#include "Battleground.h"
#include "BattlegroundDS.h"
#include "Chat.h"
#include "GameObject.h"
#include "Log.h"
#include "Map.h"
#include "MotionMaster.h"
#include "MoveSpline.h"
#include "ObjectAccessor.h"
#include "PathGenerator.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "StringConvert.h"
#include "StringFormat.h"
#include "Timer.h"

// The game side of the jump spots (G2g-a): `tactics jump scan` and `tactics jump test` run on the world thread
// between the map updates (they read a map no map thread updates now, as `tactics cover scan` does); the test arms
// the bot's JumpBotState and PlayerbotAI::StepJumpTest steps it on the bot's own map thread.

namespace
{
constexpr float JUMP_NOMINAL_RUN_SPEED = 7.0f;  // the scan's arcs (the test uses the bot's run speed)
constexpr float JUMP_FLOOR_SEARCH = 50.0f;      // DEFAULT_HEIGHT_SEARCH
constexpr uint32 JUMP_MAX_TEST_JUMPS = 50;

struct JumpWorld
{
    Map* WorldMap = nullptr;
    uint32 PhaseMask = 0;

    bool Floor(float x, float y, float zTop, float& z) const
    {
        z = WorldMap->GetHeight(PhaseMask, x, y, zTop, true, JUMP_FLOOR_SEARCH);
        return z > INVALID_HEIGHT + 1.0f;
    }

    bool Los(CoverVec const& a, CoverVec const& b) const { return ArenaCoverLos(WorldMap, PhaseMask, a, b); }
};

struct JumpPathInfo
{
    uint32 Type = 0;
    bool Normal = false;  // a whole path (PATHFIND_NORMAL, no other bit)
    bool OnMesh = false;  // the start lies on the navmesh
    float Yards = 0.0f;
    float Straight = 0.0f;
};

JumpPathInfo JumpPath(WorldObject const* owner, CoverVec const& a, CoverVec const& b)
{
    JumpPathInfo info;
    PathGenerator path(owner);
    path.CalculatePath(a.X, a.Y, a.Z, b.X, b.Y, b.Z, false);
    info.Type = static_cast<uint32>(path.GetPathType());
    info.Normal = info.Type == PATHFIND_NORMAL;
    info.OnMesh = !(info.Type & (PATHFIND_FARFROMPOLY_START | PATHFIND_NOPATH));
    Movement::PointsArray const& points = path.GetPath();
    for (size_t index = 1; index < points.size(); ++index)
        info.Yards += (points[index] - points[index - 1]).length();

    float const dx = b.X - a.X;
    float const dy = b.Y - a.Y;
    float const dz = b.Z - a.Z;
    info.Straight = std::sqrt(dx * dx + dy * dy + dz * dz);
    return info;
}

uint32 JumpGates(Map* map)
{
    BattlegroundMap* bgMap = map ? map->ToBattlegroundMap() : nullptr;
    Battleground* bg = bgMap ? bgMap->GetBG() : nullptr;
    uint32 gates = ArenaCoverActiveGates(bg);
    if (bg && bg->GetMapId() == COVER_MAP_DALARAN)
    {
        // DS: WATER_1 at GO_STATE_READY = the waterfall is on (G2c's NaturalHopArenaOk)
        if (GameObject* water = bg->GetBGObject(BG_DS_OBJECT_WATER_1))
            if (water->GetGoState() == GO_STATE_READY)
                gates |= JUMP_GATE_DS_WATERFALL;
    }

    return gates;
}

struct JumpSurvey
{
    ArenaCoverTable const* Table = nullptr;
    JumpScanResult Scan;
    std::vector<JumpSpot> Spots;
    CoverVec Centre;
    bool CentreOk = false;
    uint32 Ms = 0;
};

// The map's spots (hand rows, refused rows, scan candidates) with their static checks
bool SurveyJumpSpots(Player* owner, JumpSurvey& survey, std::string& error)
{
    Map* map = owner->GetMap();
    if (!map || !map->IsBattleArena())
    {
        error = Acore::StringFormat("{} is not in an arena", owner->GetName());
        return false;
    }

    survey.Table = FindArenaCoverTable(map->GetId());
    if (!survey.Table)
    {
        error = Acore::StringFormat("no arena table for map {}", map->GetId());
        return false;
    }

    uint32 const startMs = getMSTime();
    JumpWorld const world{map, owner->GetPhaseMask()};
    auto const floorAt = [&world](float x, float y, float zTop, float& z) { return world.Floor(x, y, zTop, z); };
    auto const los = [&world](CoverVec const& a, CoverVec const& b) { return world.Los(a, b); };
    ArenaCoverTable const& table = *survey.Table;
    JumpScanParams params;
    params.CentreX = table.CentreX;
    params.CentreY = table.CentreY;
    params.RefZ = table.CentreZ;
    params.HalfSize = table.HalfSize;
    survey.Scan = JumpScan(params, floorAt);

    for (JumpHint const& hint : ARENA_JUMP_HINTS)
    {
        if (hint.MapId == table.MapId)
            survey.Spots.push_back(ResolveJumpHint(hint, floorAt));
    }

    for (JumpSpot const& spot : JumpRefusedPillarSpots(table))
        survey.Spots.push_back(spot);

    for (size_t index = 0; index < survey.Scan.Spots.size(); ++index)
    {
        JumpCandidate const& candidate = survey.Scan.Spots[index];
        JumpSpot spot;
        spot.MapId = table.MapId;
        spot.Id = JUMP_ID_SCAN + static_cast<uint32>(index);
        spot.Name = Acore::StringFormat("edge {:.0f} deg", candidate.Yaw * 180.0f / COVER_PI);
        spot.From = candidate.From;
        spot.Land = candidate.Land;
        spot.Yaw = candidate.Yaw;
        spot.Drop = candidate.Drop;
        survey.Spots.push_back(spot);
    }

    float centreZ = 0.0f;
    survey.CentreOk = floorAt(table.CentreX, table.CentreY, table.CentreZ + 2.0f, centreZ);
    survey.Centre = {table.CentreX, table.CentreY, survey.CentreOk ? centreZ : table.CentreZ};
    for (JumpSpot& spot : survey.Spots)
    {
        if (spot.Refused)
            continue;

        JumpStatic& check = spot.Static;
        check.Checked = true;
        JumpPlan const plan = PlanJumpArc(spot.From, spot.Land, JUMP_NOMINAL_RUN_SPEED, los, floorAt);
        check.ArcPass = plan.Check.Pass;
        check.ArcWhy = plan.Check.Why;
        check.ArcRays = plan.Check.Rays;
        check.SpeedZ = plan.Arc.SpeedZ;
        check.AirMs = plan.Arc.AirSeconds * 1000.0f;
        JumpLanding const landing = CheckJumpLanding(spot.Land, floorAt);
        check.Ground = landing.Ground;
        check.GroundZ = landing.GroundZ;
        JumpPathInfo const toCentre = JumpPath(owner, spot.Land, survey.Centre);
        check.PathType = toCentre.Type;
        check.OnMesh = toCentre.OnMesh;
        check.PathYards = toCentre.Yards;
        check.StraightYards = toCentre.Straight;
        check.Connected = JumpPathConnects(toCentre.Normal, toCentre.Yards, toCentre.Straight);
        check.WayBack = JumpPath(owner, spot.Land, spot.From).Normal;
    }

    survey.Ms = GetMSTimeDiffToNow(startMs);
    return true;
}

std::string JumpVecJson(CoverVec const& point)
{
    return Acore::StringFormat("[{:.2f},{:.2f},{:.2f}]", point.X, point.Y, point.Z);
}

std::string JumpSpotVerdict(JumpSpot const& spot)
{
    if (spot.Refused)
        return Acore::StringFormat("REFUSED ({})", spot.Refused);

    JumpFail const fail = JumpStaticVerdict(spot.Static);
    return fail == JumpFail::None ? "PASS" : Acore::StringFormat("FAIL {}", JumpFailName(fail));
}

uint32 ParseJumpMap(std::string const& text)
{
    std::string lower = text;
    for (char& c : lower)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    if (lower == "na")
        return COVER_MAP_NAGRAND;

    if (lower == "be")
        return COVER_MAP_BLADES_EDGE;

    if (lower == "rl")
        return COVER_MAP_LORDAERON;

    if (lower == "ds")
        return COVER_MAP_DALARAN;

    if (lower == "rv")
        return COVER_MAP_RING_OF_VALOR;

    return Acore::StringTo<uint32>(lower).value_or(0);
}

void ExecuteJumpCmd(Player* bot, JumpBotState& state, JumpTestCmd const& cmd)
{
    MotionMaster& mm = *bot->GetMotionMaster();
    switch (cmd.Kind)
    {
        case JumpTestCmdKind::Teleport:
            mm.Clear();
            bot->StopMoving();
            bot->NearTeleportTo(cmd.Point.X, cmd.Point.Y, cmd.Point.Z + 0.1f, cmd.Yaw);
            break;
        case JumpTestCmdKind::Walk:
            mm.Clear();
            mm.MovePoint(0, cmd.Point.X, cmd.Point.Y, cmd.Point.Z);
            break;
        case JumpTestCmdKind::Jump:
        {
            CoverVec const start{bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ()};
            float const speed = bot->GetSpeed(MOVE_RUN);
            mm.Clear();
            mm.MoveJump(cmd.Point.X, cmd.Point.Y, cmd.Point.Z, speed, cmd.SpeedZ);
            // the arc from the real start (the bot stands within ArriveYards of the from-point)
            JumpWorld const world{bot->GetMap(), bot->GetPhaseMask()};
            JumpArcResult const arc = CheckJumpArc(
                start, cmd.Point, MakeJumpArc(start, cmd.Point, speed, cmd.SpeedZ),
                [&world](CoverVec const& a, CoverVec const& b) { return world.Los(a, b); },
                [&world](float x, float y, float zTop, float& z) { return world.Floor(x, y, zTop, z); });
            state.Machine.NoteArc(arc.Pass);
            break;
        }
        default:
            break;
    }
}

void LogJumpSpot(Player* bot, JumpBotState const& state, char const* aborted)
{
    JumpTestMachine const& machine = state.Machine;
    JumpSpot const& spot = machine.Spot();
    std::string jumps;
    float errMax = 0.0f;
    float errSum = 0.0f;
    for (JumpTestRecord const& record : machine.Records())
    {
        errMax = std::max(errMax, record.Err2d);
        errSum += record.Err2d;
        jumps +=
            Acore::StringFormat("{}[\"{}\",{},{},{},{:.2f},{:.2f},{:.2f},{},{},{},{},{}]", jumps.empty() ? "" : ",",
                                JumpFailName(record.Fail), record.WalkMs, record.AirMs, record.ExpectedAirMs,
                                record.Err2d, record.ErrZ, record.GroundDelta, record.HealthDelta, record.BackMs,
                                record.ArcPass ? 1 : 0, record.Connected ? 1 : 0, record.Gates);
    }

    size_t const count = machine.Records().size();
    std::string const json = Acore::StringFormat(
        "{{\"bot\":\"{}\",\"map\":{},\"instance\":{},\"id\":{},\"name\":\"{}\",\"hand\":{},\"from\":{},\"land\":{},"
        "\"yaw\":{:.3f},\"drop\":{:.2f},\"speedZ\":{:.2f},\"static\":\"{}\",\"wayBack\":{},\"n\":{},\"done\":{},"
        "\"ok\":{},\"pass\":{},\"errMax\":{:.2f},\"errMean\":{:.2f},\"aborted\":\"{}\",\"jumps\":[{}]}}",
        bot->GetName(), state.MapId, state.InstanceId, spot.Id, spot.Name, spot.Hand ? 1 : 0, JumpVecJson(spot.From),
        JumpVecJson(spot.Land), spot.Yaw, spot.Drop, spot.Static.SpeedZ, JumpFailName(JumpStaticVerdict(spot.Static)),
        spot.Static.WayBack ? 1 : 0, machine.Jumps(), count, machine.Passes(), machine.Passed() ? 1 : 0, errMax,
        count ? errSum / static_cast<float>(count) : 0.0f, aborted ? aborted : "", jumps);
    LOG_INFO("playerbots", "TACTICS jump test json {}", json);
}
}  // namespace

// tactics jump scan [name]: the arena the player stands in, its candidate spots (the scan's edges, the hand rows,
// the refused rows) with their static checks; one line per spot and one JSON line (also in the Playerbots log)
bool RunArenaJumpScan(ChatHandler* handler, Player* player)
{
    JumpSurvey survey;
    std::string error;
    if (!SurveyJumpSpots(player, survey, error))
    {
        handler->PSendSysMessage("TACTICS jump scan: {}", error);
        return false;
    }

    Map* map = player->GetMap();
    JumpScanResult const& scan = survey.Scan;
    uint32 passed = 0;
    std::string spots;
    for (JumpSpot const& spot : survey.Spots)
    {
        JumpStatic const& check = spot.Static;
        bool const pass = !spot.Refused && JumpStaticVerdict(check) == JumpFail::None;
        passed += pass ? 1 : 0;
        handler->PSendSysMessage(
            "TACTICS jump spot {} {}{}: from ({:.1f}, {:.1f}, {:.1f}) land ({:.1f}, {:.1f}, {:.1f}) drop {:.1f}; arc "
            "{} speedZ {:.1f} air {:.0f} ms, ground {}, mesh {}, connected {} (path {:.1f} of {:.1f} yd, type {:#x}), "
            "way back {}: {}",
            spot.Id, spot.Name, spot.Hand ? " (hand)" : "", spot.From.X, spot.From.Y, spot.From.Z, spot.Land.X,
            spot.Land.Y, spot.Land.Z, spot.Drop, check.ArcPass ? "clear" : (*check.ArcWhy ? check.ArcWhy : "-"),
            check.SpeedZ, check.AirMs, check.Ground ? "yes" : "no", check.OnMesh ? "yes" : "no",
            check.Connected ? "yes" : "no", check.PathYards, check.StraightYards, check.PathType,
            check.WayBack ? "yes" : "no", JumpSpotVerdict(spot));
        spots += Acore::StringFormat("{}[{},{},{},{},{:.3f},{:.2f},{:.2f},{},{},{},{},{},{},{:.1f},{:.1f},{},\"{}\"]",
                                     spots.empty() ? "" : ",", spot.Id, spot.Hand ? 1 : 0, JumpVecJson(spot.From),
                                     JumpVecJson(spot.Land), spot.Yaw, spot.Drop, check.SpeedZ, check.ArcPass ? 1 : 0,
                                     check.Ground ? 1 : 0, check.OnMesh ? 1 : 0, check.Connected ? 1 : 0,
                                     check.WayBack ? 1 : 0, check.PathType, check.PathYards, check.StraightYards,
                                     pass ? 1 : 0, spot.Refused ? "refused" : JumpFailName(JumpStaticVerdict(check)));
    }

    handler->PSendSysMessage(
        "TACTICS jump scan {} (map {}, instance {}): {} spots ({} from {} edges{}), {} pass the static checks; {} of "
        "{} cells with floor, {} levels; centre floor {}; gates {:#x}; {} ms",
        survey.Table->Name, map->GetId(), map->GetInstanceId(), survey.Spots.size(), scan.Spots.size(), scan.Edges,
        scan.Capped ? ", capped" : "", passed, scan.FloorCells, scan.Cells, scan.Levels,
        survey.CentreOk ? "found" : "missing", JumpGates(map), survey.Ms);
    std::string const json = Acore::StringFormat(
        "{{\"map\":{},\"name\":\"{}\",\"instance\":{},\"ms\":{},\"cells\":{},\"floorCells\":{},\"levels\":{},"
        "\"edges\":{},\"capped\":{},\"centre\":{},\"centreOk\":{},\"gates\":{},\"pass\":{},\"spots\":[{}]}}",
        map->GetId(), survey.Table->Name, map->GetInstanceId(), survey.Ms, scan.Cells, scan.FloorCells, scan.Levels,
        scan.Edges, scan.Capped ? 1 : 0, JumpVecJson(survey.Centre), survey.CentreOk ? 1 : 0, JumpGates(map), passed,
        spots);
    handler->PSendSysMessage("TACTICS jump scan json {}", json);
    LOG_INFO("playerbots", "TACTICS jump scan json {}", json);
    return true;
}

// tactics jump test <map> <id|all|id,id,...> [n] [bot] (bot: a name, else the GM's selected player), or
// tactics jump test stop [bot]: arms (or ends) the bot's jump test; the results go to the Playerbots log, one
// "TACTICS jump test json" line per spot and a "TACTICS jump test done" line per bot
bool RunArenaJumpTest(ChatHandler* handler, char const* args)
{
    std::istringstream stream(args ? args : "");
    std::vector<std::string> words;
    for (std::string word; stream >> word;)
        words.push_back(word);

    std::string const usage =
        "Usage: .playerbots tactics jump test <map: id or na|be|rl|ds|rv> <id|all|id,id,...> [n] [bot] | "
        "tactics jump test stop [bot]";
    bool const stop = !words.empty() && words[0] == "stop";
    if (!stop && words.size() < 2)
    {
        handler->SendSysMessage(usage);
        return false;
    }

    size_t next = stop ? 1 : 2;
    uint32 jumps = sPlayerbotAIConfig.TacticJump.TestJumps;
    if (!stop && next < words.size() && Acore::StringTo<uint32>(words[next]))
        jumps = std::min(*Acore::StringTo<uint32>(words[next++]), JUMP_MAX_TEST_JUMPS);

    Player* bot = nullptr;
    if (next < words.size())
        bot = ObjectAccessor::FindPlayerByName(words[next]);
    else if (handler->GetSession())
        bot = handler->getSelectedPlayer();

    PlayerbotAI* botAI = bot ? GET_PLAYERBOT_AI(bot) : nullptr;
    if (!bot || !botAI || IsRealPlayer(bot))
    {
        handler->PSendSysMessage("TACTICS jump test: no bot {} in the world. {}",
                                 next < words.size() ? words[next] : "", usage);
        return false;
    }

    JumpBotState& state = botAI->GetJumpState();
    if (stop)
    {
        state.Frozen = false;  // the bot's next update ends the test and logs the spot under way
        handler->PSendSysMessage("TACTICS jump test {}: stop requested", bot->GetName());
        return true;
    }

    uint32 const mapId = ParseJumpMap(words[0]);
    if (!bot->IsAlive() || bot->GetMapId() != mapId)
    {
        handler->PSendSysMessage("TACTICS jump test: {} is not alive on map {} (it is on map {})", bot->GetName(),
                                 mapId, bot->GetMapId());
        return false;
    }

    JumpSurvey survey;
    std::string error;
    if (!SurveyJumpSpots(bot, survey, error))
    {
        handler->PSendSysMessage("TACTICS jump test: {}", error);
        return false;
    }

    // all: every spot that is not refused and is a hand row or passes the static checks; ids: those spots, also
    // when their static checks fail (so the live test shows whether the static checks predict it)
    std::vector<JumpSpot> queue;
    std::string skipped;
    if (words[1] == "all")
    {
        for (JumpSpot const& spot : survey.Spots)
        {
            if (!spot.Refused && (spot.Hand || JumpStaticVerdict(spot.Static) == JumpFail::None))
                queue.push_back(spot);
        }
    }
    else
    {
        std::istringstream ids(words[1]);
        for (std::string id; std::getline(ids, id, ',');)
        {
            uint32 const wanted = Acore::StringTo<uint32>(id).value_or(0);
            JumpSpot const* found = nullptr;
            for (JumpSpot const& spot : survey.Spots)
            {
                if (spot.Id == wanted)
                    found = &spot;
            }

            if (found && !found->Refused)
                queue.push_back(*found);
            else
                skipped += Acore::StringFormat(" {}({})", id, found ? found->Refused : "no such spot");
        }
    }

    if (queue.empty())
    {
        handler->PSendSysMessage("TACTICS jump test {}: no spot to test on {}; skipped:{}", bot->GetName(),
                                 survey.Table->Name, skipped.empty() ? " none" : skipped);
        return false;
    }

    // the staging point 6 yd behind the from-point, on the from-point's floor (else the from-point itself)
    JumpWorld const world{bot->GetMap(), bot->GetPhaseMask()};
    JumpTestLimits limits;
    JumpConfig const& config = sPlayerbotAIConfig.TacticJump;
    limits.LandTol = static_cast<float>(config.LandingTolX10) / 10.0f;
    limits.WalkTimeoutMs = config.TestWalkTimeoutMs;
    limits.AirTimeoutMs = config.TestAirTimeoutMs;
    limits.SettleMs = config.TestSettleMs;
    limits.MaxStuckMs = config.TestMaxStuckMs;
    limits.RunSpeed = bot->GetSpeed(MOVE_RUN);
    std::string ids;
    for (JumpSpot& spot : queue)
    {
        CoverVec const back{spot.From.X - limits.StageBack * std::cos(spot.Yaw),
                            spot.From.Y - limits.StageBack * std::sin(spot.Yaw), spot.From.Z};
        float z = 0.0f;
        bool const floorThere =
            world.Floor(back.X, back.Y, spot.From.Z + 1.5f, z) && std::fabs(z - spot.From.Z) <= 1.5f;
        spot.Staging = floorThere ? CoverVec{back.X, back.Y, z} : spot.From;
        ids += Acore::StringFormat("{}{}", ids.empty() ? "" : ",", spot.Id);
    }

    state.Frozen = true;
    state.Running = false;
    state.MapId = bot->GetMapId();
    state.InstanceId = bot->GetInstanceId();
    state.Jumps = jumps;
    state.Next = 0;
    state.Passed = 0;
    state.ArmedMs = getMSTime();
    state.Centre = survey.Centre;
    state.Queue = std::move(queue);
    state.Limits = limits;
    botAI->SetJumpTest(true);
    handler->PSendSysMessage(
        "TACTICS jump test {}: armed {} spots ({}) x {} jumps on {}; skipped:{}; results in the Playerbots log "
        "(TACTICS jump test json)",
        bot->GetName(), state.Queue.size(), ids, jumps, survey.Table->Name, skipped.empty() ? " none" : skipped);
    LOG_INFO("playerbots", "TACTICS jump test armed {} map {} instance {} spots {} jumps {}", bot->GetName(),
             state.MapId, state.InstanceId, ids, jumps);
    return true;
}

// G2g: one step of the jump test on the bot's map thread (UpdateAI runs no engine meanwhile)
void PlayerbotAI::StepJumpTest(uint32 nowMs)
{
    JumpBotState& state = GetJumpState();
    Map* const map = bot->GetMap();
    char const* aborted = nullptr;
    if (!state.Frozen)
        aborted = "stopped";
    else if (!bot->IsAlive())
        aborted = "dead";
    else if (!map || map->GetId() != state.MapId || map->GetInstanceId() != state.InstanceId)
        aborted = "left the arena";

    if (aborted || state.Next >= state.Queue.size())
    {
        if (aborted && state.Running)
            LogJumpSpot(bot, state, aborted);

        LOG_INFO("playerbots", "TACTICS jump test done {}: {} of {} spots passed{}{}", bot->GetName(), state.Passed,
                 state.Queue.size(), aborted ? ", " : "", aborted ? aborted : "");
        state.Frozen = false;
        state.Running = false;
        state.Queue.clear();
        jumpTest = false;
        if (bot->IsInWorld())
            bot->GetMotionMaster()->Clear();

        return;
    }

    if (!state.Running)
    {
        JumpSpot const& spot = state.Queue[state.Next];
        state.Machine.Start(spot, spot.Staging, state.Jumps, state.Limits, nowMs);
        state.Running = true;
    }

    JumpTestObs obs;
    obs.NowMs = nowMs;
    obs.Pos = {bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ()};
    obs.Moving = !bot->movespline->Finalized();
    obs.Airborne = obs.Moving || bot->GetMotionMaster()->GetCurrentMovementGeneratorType() == EFFECT_MOTION_TYPE;
    obs.Health = bot->GetHealth();
    obs.Gates = JumpGates(map);
    JumpTestCmd cmd = state.Machine.Step(obs);
    if (cmd.Kind == JumpTestCmdKind::Measure)
    {
        JumpWorld const world{map, bot->GetPhaseMask()};
        JumpTestMeasure measure;
        measure.GroundValid = world.Floor(obs.Pos.X, obs.Pos.Y, obs.Pos.Z + JUMP_STAND_YARDS, measure.GroundZ);
        JumpPathInfo const toCentre = JumpPath(bot, obs.Pos, state.Centre);
        measure.OnMesh = toCentre.OnMesh;
        measure.PathOk = JumpPathConnects(toCentre.Normal, toCentre.Yards, toCentre.Straight);
        JumpPathInfo const back = JumpPath(bot, obs.Pos, state.Machine.Spot().From);
        measure.BackPath = back.Normal;
        measure.BackPathYards = back.Yards;
        cmd = state.Machine.NoteMeasure(measure, obs);
        if (!state.Machine.Records().empty() && cmd.Kind != JumpTestCmdKind::Walk)
        {
            JumpTestRecord const& record = state.Machine.Records().back();
            LOG_TRACE("playerbots", "Jump: {} spot {} jump {}: {}, landing off {:.2f} yd (z {:.2f}), air {} ms",
                      bot->GetName(), state.Machine.Spot().Id, state.Machine.Records().size(),
                      JumpFailName(record.Fail), record.Err2d, record.ErrZ, record.AirMs);
        }
    }

    ExecuteJumpCmd(bot, state, cmd);
    if (cmd.Kind == JumpTestCmdKind::Finish || (state.Machine.Done() && state.Running))
    {
        LogJumpSpot(bot, state, nullptr);
        state.Passed += state.Machine.Passed() ? 1 : 0;
        ++state.Next;
        state.Running = false;
    }
}
