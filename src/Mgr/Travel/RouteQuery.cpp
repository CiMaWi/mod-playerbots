/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "RouteQuery.h"

#include "Chat.h"
#include "TravelGraph.h"

// The search is pure and takes milliseconds; the console runs it on the world thread
bool RouteQuery::HandleCommand(ChatHandler* handler, char const* args)
{
    TravelGraph const& graph = TravelGraph::Instance();
    if (!graph.IsReady())
    {
        handler->PSendSysMessage("ROUTE query: the travel graph is not ready (see playerbots route status)");
        return true;
    }
    Parsed query;
    std::string error;
    if (!Parse(args ? args : "", query, error))
    {
        handler->SendErrorMessage("{}", error);
        return false;
    }
    for (std::string const& line : Run(graph, query))
        handler->PSendSysMessage("{}", line);
    return true;
}
