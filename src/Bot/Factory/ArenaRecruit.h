/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_ARENARECRUIT_H
#define _PLAYERBOT_ARENARECRUIT_H

#include "Define.h"
#include "ObjectGuid.h"

class ChatHandler;

// Scouting (Playerbots.ArenaRecruit.*): a real player's arena team invite to a random bot goes through a hidden rule.
// The bot accepts only if it is not loyal to a winning team and the inviter's team rating is near what the bot
// considers its own level (from its hidden skill); the acquaintance score widens the allowed gap but never replaces
// the test. Everything that reads acquaintances or changes a team runs on the world thread.
struct RecruitParams
{
    float ratingAtSkill1 = 0.0f;
    float ratingAtSkill99 = 2000.0f;
    float deltaBase = 300.0f;
    float deltaPerPoint = 4.0f;
    float deltaMax = 1500.0f;
    uint32 loyaltyMinGames = 5;
    uint32 loyaltyMinWinPercent = 55;
};

struct RecruitInput
{
    bool skillKnown = false;
    float skill = 0.0f;
    uint32 teamRating = 0;
    uint32 acquaintanceScore = 0;
    // The bot's current team is a winning one
    bool loyal = false;
};

enum class RecruitResult : uint8
{
    Accept,
    RefuseLoyal,
    RefuseLevel
};

struct RecruitDetail
{
    float skillUsed = 0.0f;
    float ownRating = 0.0f;
    float allowedDelta = 0.0f;
    float gap = 0.0f;
};

class ArenaRecruit
{
public:
    // The skill a bot without one counts as: the population mean (BotProfileMgr::Roll)
    static constexpr float UnknownSkill = 50.0f;

    static RecruitParams ParamsFromConfig();
    // Pure: games >= minimum and the win share >= the minimum percent
    static bool IsWinning(uint32 games, uint32 wins, RecruitParams const& params);
    // Pure: the decision, detail gets the numbers behind it
    static RecruitResult Decide(RecruitInput const& input, RecruitParams const& params, RecruitDetail& detail);
    static char const* ResultName(RecruitResult result);

    // World thread (ArenaTeamRecruitOperation): decides an invite the bot got, speaks, accepts or declines it
    static void ProcessInvite(ObjectGuid botGuid, ObjectGuid inviterGuid);

    // .playerbots arena recruit <bot> <player> [rating <n>] [score <n>] [skill <n>] [loyal <0|1>]: evaluates the
    // rule for the stored inputs (the arguments replace them) and prints the numbers; changes nothing
    static bool HandleRecruitCommand(ChatHandler* handler, char const* args);
};

#endif
