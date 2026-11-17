/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "WorldLogConfig.h"

#include <array>

#include "ConfigTable.h"
#include "WorldLogFormat.h"
#include "WorldLogRecorder.h"

namespace
{
// The struct's defaults are the format headers' defaults
constexpr WorldLogFormat::Settings DefaultSettings;
static_assert(WorldLogOptions{}.StateSeconds == DefaultSettings.stateSeconds &&
              WorldLogOptions{}.SegmentMinutes == DefaultSettings.segmentMinutes &&
              WorldLogOptions{}.QueueRows == DefaultSettings.queueRows &&
              WorldLogOptions{}.RecorderSeconds == WorldLogFormat::RECORDER_SECONDS &&
              WorldLogOptions{}.RecorderActions == WorldLogFormat::RECORDER_ACTIONS &&
              WorldLogOptions{}.RecorderEvents == WorldLogFormat::RECORDER_EVENTS &&
              WorldLogOptions{}.RecorderCooldownSeconds == WorldLogFormat::RECORDER_COOLDOWN_SECONDS);

using WorldLogRow = ConfigRow<WorldLogOptions>;

// The bounds are the constants ClampSettings and ClampRecorderSettings use, so the numbers have one source
constexpr std::array<WorldLogRow, 8> WorldLogRows = {{
    {"Dir", &WorldLogOptions::Dir, 0, 0, ConfigKind::Role},
    {"StateSeconds", &WorldLogOptions::StateSeconds, WorldLogFormat::MIN_STATE_SECONDS,
     WorldLogFormat::MAX_STATE_SECONDS, ConfigKind::TuningListed},
    {"SegmentMinutes", &WorldLogOptions::SegmentMinutes, WorldLogFormat::MIN_SEGMENT_MINUTES,
     WorldLogFormat::MAX_SEGMENT_MINUTES, ConfigKind::TuningListed},
    {"QueueRows", &WorldLogOptions::QueueRows, WorldLogFormat::MIN_QUEUE_ROWS, WorldLogFormat::MAX_QUEUE_ROWS},
    {"RecorderSeconds", &WorldLogOptions::RecorderSeconds, 0, WorldLogFormat::MAX_RECORDER_SECONDS},
    {"RecorderActions", &WorldLogOptions::RecorderActions, WorldLogFormat::MIN_RECORDER_ROWS,
     WorldLogFormat::MAX_RECORDER_ROWS},
    {"RecorderEvents", &WorldLogOptions::RecorderEvents, WorldLogFormat::MIN_RECORDER_ROWS,
     WorldLogFormat::MAX_RECORDER_ROWS},
    {"RecorderCooldownSeconds", &WorldLogOptions::RecorderCooldownSeconds, 0,
     WorldLogFormat::MAX_RECORDER_COOLDOWN_SECONDS},
}};
}  // namespace

ConfigFamily<WorldLogOptions> const WorldLogFamily{"Playerbots.WorldLog.", WorldLogRows};
