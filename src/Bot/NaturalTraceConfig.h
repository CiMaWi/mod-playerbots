/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_NATURALTRACECONFIG_H
#define _PLAYERBOT_NATURALTRACECONFIG_H

#include <string>

#include "Define.h"

// The trace of the natural timing (Playerbots.Natural.Trace.*, G0 instrumentation;
// PlayerbotAIConfig::NaturalTraceConfig): test toggles read once at config load
struct TraceConfig
{
    bool Enable = false;
    uint32 MaxUnits = 8;             // most units traced at once
    uint32 SampleEvery = 1;          // one match in N is traced (0 = none)
    bool Organic = false;            // organic arena matches chosen by SampleEvery are traced too
    std::string Dir;                 // the directory of the trace files; empty = <LogsDir>/natural
    uint32 IntervalMs = 100;         // the sample interval
    uint32 MaxRowsPerUnit = 200000;  // rows kept per traced unit
    // The format of the trace files (G0i): FormatText is the key's text (jsonl or binary), Binary its meaning
    std::string FormatText = "jsonl";
    bool Binary = false;
};

template <typename Struct>
struct ConfigFamily;

// The config table family (NaturalTraceConfig.cpp)
extern ConfigFamily<TraceConfig> const NaturalTraceFamily;

// The one instance (NaturalTraceConfig.cpp), filled by the config walk. Only PlayerbotAIConfig's constructor calls
// this: readers use sPlayerbotAIConfig.NaturalTraceConfig.
TraceConfig& TraceConfigStorage();

#endif
