/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "ArenaRecruit.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <sstream>
#include <string>

#include "AcquaintanceMgr.h"
#include "ArenaTeam.h"
#include "ArenaTeamMgr.h"
#include "BotProfileMgr.h"
#include "CharacterCache.h"
#include "Chat.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "PlayerbotTextMgr.h"
#include "RandomPlayerbotMgr.h"
#include "StringConvert.h"
#include "ThreadCheck.h"
#include "WorldPacket.h"
#include "WorldSession.h"

namespace
{
constexpr uint8 SkillMin = 1;
constexpr uint8 SkillMax = 99;
constexpr uint32 PercentMax = 100;

// The bot's hidden skill; false when it has none (Playerbots.Skill.Enable = 0 or no profile)
bool BotSkill(ObjectGuid guid, float& skill)
{
    std::optional<BotProfile> const profile = BotProfileMgr::instance().Find(guid.GetCounter());
    if (!profile || !profile->Skill)
        return false;

    skill = static_cast<float>(profile->Skill);
    return true;
}

// Any of the bot's teams has a winning season record
bool BotIsLoyal(ObjectGuid bot, RecruitParams const& params)
{
    for (uint8 slot = 0; slot < MAX_ARENA_SLOT; ++slot)
    {
        uint32 const teamId = sCharacterCache->GetCharacterArenaTeamIdByGuid(bot, slot);
        ArenaTeam const* team = teamId ? sArenaTeamMgr->GetArenaTeamById(teamId) : nullptr;
        if (team && ArenaRecruit::IsWinning(team->GetStats().SeasonGames, team->GetStats().SeasonWins, params))
            return true;
    }

    return false;
}

// The best rating among the player's teams, 0 without one
uint32 BestTeamRating(ObjectGuid player)
{
    uint32 best = 0;
    for (uint8 slot = 0; slot < MAX_ARENA_SLOT; ++slot)
    {
        uint32 const teamId = sCharacterCache->GetCharacterArenaTeamIdByGuid(player, slot);
        ArenaTeam const* team = teamId ? sArenaTeamMgr->GetArenaTeamById(teamId) : nullptr;
        if (team)
            best = std::max(best, team->GetRating());
    }

    return best;
}

uint32 ScoreOf(ObjectGuid player, ObjectGuid bot)
{
    return sPlayerbotAIConfig.AcquaintanceEnable
               ? AcquaintanceMgr::Instance().GetScore(player.GetCounter(), bot.GetCounter())
               : 0;
}

// Name or guid of any character; empty when there is none
ObjectGuid ResolveCharacter(std::string const& target, std::string& name)
{
    ObjectGuid guid = ObjectGuid::Empty;
    name = target;
    if (std::optional<uint32> const number = Acore::StringTo<uint32>(target))
        guid = ObjectGuid::Create<HighGuid::Player>(*number);
    else if (normalizePlayerName(name))
        guid = sCharacterCache->GetCharacterGuidByName(name);

    if (guid.IsEmpty() || !sCharacterCache->GetCharacterNameByGuid(guid, name))
        return ObjectGuid::Empty;

    return guid;
}

void SayText(Player* bot, char const* name, char const* fallback)
{
    std::string const text = PlayerbotTextMgr::instance().GetBotTextOrDefault(name, fallback, {});
    bot->Say(text, LANG_UNIVERSAL);
}
}  // namespace

RecruitParams ArenaRecruit::ParamsFromConfig()
{
    RecruitParams params;
    params.ratingAtSkill1 = static_cast<float>(sPlayerbotAIConfig.ArenaRecruitRatingAtSkill1);
    params.ratingAtSkill99 = static_cast<float>(sPlayerbotAIConfig.ArenaRecruitRatingAtSkill99);
    params.deltaBase = static_cast<float>(sPlayerbotAIConfig.ArenaRecruitDeltaBase);
    params.deltaPerPoint = sPlayerbotAIConfig.ArenaRecruitDeltaPerAcquaintancePoint;
    params.deltaMax = static_cast<float>(sPlayerbotAIConfig.ArenaRecruitDeltaMax);
    params.loyaltyMinGames = sPlayerbotAIConfig.ArenaRecruitLoyaltyMinGames;
    params.loyaltyMinWinPercent = sPlayerbotAIConfig.ArenaRecruitLoyaltyMinWinPercent;
    return params;
}

bool ArenaRecruit::IsWinning(uint32 games, uint32 wins, RecruitParams const& params)
{
    return games >= params.loyaltyMinGames && games > 0 && wins * PercentMax >= params.loyaltyMinWinPercent * games;
}

RecruitResult ArenaRecruit::Decide(RecruitInput const& input, RecruitParams const& params, RecruitDetail& detail)
{
    float const skill = input.skillKnown
                            ? std::clamp(input.skill, static_cast<float>(SkillMin), static_cast<float>(SkillMax))
                            : UnknownSkill;
    float const span = static_cast<float>(SkillMax - SkillMin);
    detail.skillUsed = skill;
    detail.ownRating = std::max(
        0.0f, params.ratingAtSkill1 + (skill - SkillMin) / span * (params.ratingAtSkill99 - params.ratingAtSkill1));
    // The score widens the allowed gap, up to the cap: it never skips the level test
    detail.allowedDelta =
        std::min(params.deltaBase + params.deltaPerPoint * static_cast<float>(input.acquaintanceScore),
                 std::max(params.deltaMax, 0.0f));
    detail.gap = std::fabs(static_cast<float>(input.teamRating) - detail.ownRating);

    if (input.loyal)
        return RecruitResult::RefuseLoyal;

    return detail.gap <= detail.allowedDelta ? RecruitResult::Accept : RecruitResult::RefuseLevel;
}

char const* ArenaRecruit::ResultName(RecruitResult result)
{
    switch (result)
    {
        case RecruitResult::Accept:
            return "accept";
        case RecruitResult::RefuseLoyal:
            return "refuse (loyal)";
        case RecruitResult::RefuseLevel:
            return "refuse (level)";
    }

    return "?";
}

void ArenaRecruit::ProcessInvite(ObjectGuid botGuid, ObjectGuid inviterGuid)
{
    PLAYERBOT_CHECK_WORLD_THREAD("ArenaRecruit::ProcessInvite");

    // The bot may have logged out since the action queued this: the core drops the invite after a while
    Player* bot = ObjectAccessor::FindPlayer(botGuid);
    Player* inviter = ObjectAccessor::FindPlayer(inviterGuid);
    if (!bot || !bot->GetSession())
        return;

    ArenaTeam* team = sArenaTeamMgr->GetArenaTeamById(bot->GetArenaTeamIdInvited());
    if (!inviter || !team)
    {
        // The core clears the pending invite only on accept or decline: decline quietly so later invites work
        if (bot->GetArenaTeamIdInvited())
        {
            WorldPacket data(CMSG_ARENA_TEAM_DECLINE);
            bot->GetSession()->HandleArenaTeamDeclineOpcode(data);
        }
        return;
    }

    if (bot->GetArenaTeamId(team->GetSlot()))
    {
        // Already in a team of that size, as the plain accept action says
        SayText(bot, "arena_team_already_in_team", "Sorry, I am already in such team");
        WorldPacket data(CMSG_ARENA_TEAM_DECLINE);
        bot->GetSession()->HandleArenaTeamDeclineOpcode(data);
        LOG_INFO("playerbots", "Bot {} <{}> declines Arena Team invite", bot->GetGUID().ToString().c_str(),
                 bot->GetName().c_str());
        return;
    }

    RecruitParams const params = ParamsFromConfig();
    RecruitInput input;
    input.skillKnown = BotSkill(botGuid, input.skill);
    input.teamRating = team->GetRating();
    input.acquaintanceScore = ScoreOf(inviterGuid, botGuid);
    input.loyal = BotIsLoyal(botGuid, params);

    RecruitDetail detail;
    RecruitResult const result = Decide(input, params, detail);
    LOG_DEBUG("playerbots",
              "Arena recruit: bot {} <{}>, inviter <{}>, team {} <{}>: skill {}{}, own rating {:.0f}, team rating {}, "
              "score {}, allowed delta {:.0f}, gap {:.0f}, loyal {} -> {}",
              botGuid.ToString(), bot->GetName(), inviter->GetName(), team->GetId(), team->GetName(), detail.skillUsed,
              input.skillKnown ? "" : " (unknown)", detail.ownRating, input.teamRating, input.acquaintanceScore,
              detail.allowedDelta, detail.gap, input.loyal ? "yes" : "no", ResultName(result));

    if (result == RecruitResult::Accept)
    {
        WorldPacket data(CMSG_ARENA_TEAM_ACCEPT);
        bot->GetSession()->HandleArenaTeamAcceptOpcode(data);
        SayText(bot, "arena_team_recruit_accept", "Gladly. Let's see what we can do together.");
        LOG_INFO("playerbots", "Bot {} <{}> accepts Arena Team invite of <{}> (team {})",
                 bot->GetGUID().ToString().c_str(), bot->GetName().c_str(), inviter->GetName().c_str(), team->GetId());
        return;
    }

    if (result == RecruitResult::RefuseLoyal)
        SayText(bot, "arena_team_recruit_refuse_loyal", "Thanks for asking, but I'm happy where I am.");
    else
        SayText(bot, "arena_team_recruit_refuse_level",
                "I don't think we'd be a good fit yet. Maybe if we spent some time together first.");

    WorldPacket data(CMSG_ARENA_TEAM_DECLINE);
    bot->GetSession()->HandleArenaTeamDeclineOpcode(data);
    LOG_INFO("playerbots", "Bot {} <{}> declines Arena Team invite of <{}> (team {})",
             bot->GetGUID().ToString().c_str(), bot->GetName().c_str(), inviter->GetName().c_str(), team->GetId());
}

bool ArenaRecruit::HandleRecruitCommand(ChatHandler* handler, char const* args)
{
    if (!sPlayerbotAIConfig.ArenaRecruitEnable)
    {
        handler->PSendSysMessage("Arena recruiting rule is off (Playerbots.ArenaRecruit.Enable = 0).");
        return true;
    }

    PLAYERBOT_CHECK_WORLD_THREAD("ArenaRecruit::HandleRecruitCommand");

    std::istringstream in(args ? args : "");
    std::string botTarget;
    std::string playerTarget;
    in >> botTarget >> playerTarget;
    if (playerTarget.empty())
    {
        handler->PSendSysMessage(
            "Usage: .playerbots arena recruit <bot> <player> [rating <n>] [score <n>] [skill <0-99>] [loyal <0|1>]\n"
            "skill 0 = unknown (counts as the mean).");
        handler->SetSentErrorMessage(true);
        return false;
    }

    std::string botName;
    std::string playerName;
    ObjectGuid const bot = ResolveCharacter(botTarget, botName);
    ObjectGuid const player = ResolveCharacter(playerTarget, playerName);
    if (bot.IsEmpty() || player.IsEmpty())
    {
        handler->PSendSysMessage("No character {}.", bot.IsEmpty() ? botTarget : playerTarget);
        handler->SetSentErrorMessage(true);
        return false;
    }

    if (!sRandomPlayerbotMgr.IsRandomBot(bot.GetCounter()))
    {
        handler->PSendSysMessage("{} is not a random bot.", botName);
        handler->SetSentErrorMessage(true);
        return false;
    }

    RecruitParams const params = ParamsFromConfig();
    RecruitInput input;
    input.skillKnown = BotSkill(bot, input.skill);
    input.teamRating = BestTeamRating(player);
    input.acquaintanceScore = ScoreOf(player, bot);
    input.loyal = BotIsLoyal(bot, params);

    std::string key;
    std::string value;
    while (in >> key >> value)
    {
        std::optional<uint32> const number = Acore::StringTo<uint32>(value);
        if (!number)
        {
            handler->PSendSysMessage("{} needs a number, not '{}'.", key, value);
            handler->SetSentErrorMessage(true);
            return false;
        }

        if (key == "rating")
            input.teamRating = *number;
        else if (key == "score")
            input.acquaintanceScore = *number;
        else if (key == "skill")
        {
            input.skillKnown = *number > 0;
            input.skill = static_cast<float>(*number);
        }
        else if (key == "loyal")
            input.loyal = *number != 0;
        else
        {
            handler->PSendSysMessage("Unknown argument '{}' (rating, score, skill, loyal).", key);
            handler->SetSentErrorMessage(true);
            return false;
        }
    }

    RecruitDetail detail;
    RecruitResult const result = Decide(input, params, detail);
    LOG_DEBUG("playerbots",
              "Arena recruit (GM aid): bot {} <{}>, player <{}>: skill {}{}, own rating {:.0f}, team rating {}, score "
              "{}, allowed delta {:.0f}, gap {:.0f}, loyal {} -> {}",
              bot.ToString(), botName, playerName, detail.skillUsed, input.skillKnown ? "" : " (unknown)",
              detail.ownRating, input.teamRating, input.acquaintanceScore, detail.allowedDelta, detail.gap,
              input.loyal ? "yes" : "no", ResultName(result));
    handler->PSendSysMessage(
        "Recruit {} by {}: skill {}{}, own rating {:.0f}, team rating {}, score {}, allowed delta {:.0f}, gap "
        "{:.0f}, loyal {} -> {}",
        botName, playerName, detail.skillUsed, input.skillKnown ? "" : " (unknown, counts as the mean)",
        detail.ownRating, input.teamRating, input.acquaintanceScore, detail.allowedDelta, detail.gap,
        input.loyal ? "yes" : "no", ResultName(result));
    return true;
}
