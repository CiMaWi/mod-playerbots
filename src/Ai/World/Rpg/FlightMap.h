/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_FLIGHTMAP_H
#define _PLAYERBOT_FLIGHTMAP_H

#include <memory>
#include <string>
#include <vector>

#include "FlightRules.h"

// The flying maps of Outland and Northrend (upkeep plan WU9): built once at startup on their own thread from the
// core's terrain files, published immutable; the rules and the search are in FlightRules.h
namespace Planner
{
// World thread, once after the area table is loaded: reads the terrain files on the loader's own thread
void StartFlightMap();
// Server shutdown: a build still running stops at its next file; joins the loader
void StopFlightMap();
// Any thread: the published maps (null before the build); a reader keeps its copy alive
std::shared_ptr<FlightRules::FlightMapSet const> FlightMaps();
// False until the build finished; flying legs are not offered meanwhile
bool FlightMapsReady();
// The flight-map lines of `playerbots planner status|indexes`
std::vector<std::string> FlightMapStatus();
// Any thread (the planner pool): FlightRules::Search on the published map of the request, timed as the PerfMonitor
// row "flight search"; fails with NoMap before the build or for a map without flying
FlightRules::FlightRoute SearchFlight(FlightRules::FlightRequest const& request);
}  // namespace Planner

#endif
