/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "ArenaTeamFormation.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>

#include "AiFactory.h"
#include "ArenaTeam.h"
#include "ArenaTeamMgr.h"
#include "BotProfileMgr.h"
#include "CharacterCache.h"
#include "Chat.h"
#include "Common.h"
#include "Config.h"
#include "Group.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerbotAIConfig.h"
#include "PvpTestRecord.h"
#include "Random.h"
#include "RandomPlayerbotFactory.h"
#include "RandomPlayerbotMgr.h"
#include "SharedDefines.h"
#include "StringFormat.h"
#include "ThreadCheck.h"

namespace
{
// The lowest level AssignBotToArenaTeam gives a team to
constexpr uint8 MinArenaTeamLevel = 70;
// One summary line every this many passes
constexpr uint32 SummaryEveryPasses = 5;
constexpr uint32 PercentMax = 100;
// Closeness of a candidate whose skill (or whose team's) is unknown
constexpr float UnknownCloseness = 0.5f;
// Every candidate keeps a small chance in a weighted pick
constexpr float MinPickWeight = 0.05f;
// Comp learning: the most playing groups scored per candidate, the default table file, lines shown
constexpr size_t MaxCompGroups = 32;
constexpr char const* DefaultCompsFile = "comp-strength.json";
constexpr size_t CompRowsShown = 6;
constexpr size_t DiversityTopShown = 3;

// Class synergy, from the classes alone (members may be offline)
constexpr float SynergyBase = 0.5f;
constexpr float SynergyFirstHealer = 0.3f;
constexpr float SynergyDamageBesideHealer = 0.2f;
constexpr float SynergyTankAndHealer = 0.1f;
constexpr float SynergySameClass = 0.3f;
constexpr float SynergyNoHealerInThree = 0.2f;
constexpr size_t TeamSizeNeedingHealer = 3;

std::array<ArenaType, 3> constexpr ArenaTypes{ARENA_TYPE_2v2, ARENA_TYPE_3v3, ARENA_TYPE_5v5};

bool CanHeal(uint8 cls)
{
    return cls == CLASS_PRIEST || cls == CLASS_PALADIN || cls == CLASS_DRUID || cls == CLASS_SHAMAN;
}

bool CanTank(uint8 cls)
{
    return cls == CLASS_WARRIOR || cls == CLASS_PALADIN || cls == CLASS_DRUID || cls == CLASS_DEATH_KNIGHT;
}

bool IsPureDamage(uint8 cls)
{
    return cls == CLASS_ROGUE || cls == CLASS_MAGE || cls == CLASS_WARLOCK || cls == CLASS_HUNTER;
}

char const* ClassName(uint8 cls)
{
    switch (cls)
    {
        case CLASS_WARRIOR:
            return "Warrior";
        case CLASS_PALADIN:
            return "Paladin";
        case CLASS_HUNTER:
            return "Hunter";
        case CLASS_ROGUE:
            return "Rogue";
        case CLASS_PRIEST:
            return "Priest";
        case CLASS_DEATH_KNIGHT:
            return "Death Knight";
        case CLASS_SHAMAN:
            return "Shaman";
        case CLASS_MAGE:
            return "Mage";
        case CLASS_WARLOCK:
            return "Warlock";
        case CLASS_DRUID:
            return "Druid";
        default:
            return "?";
    }
}

// The bot's hidden skill (Playerbots.Skill.Enable); false when it has none
bool BotSkill(ObjectGuid guid, float& skill)
{
    std::optional<BotProfile> const profile = BotProfileMgr::instance().Find(guid.GetCounter());
    if (!profile || !profile->Skill)
        return false;

    skill = static_cast<float>(profile->Skill);
    return true;
}

float Lerp(float bottom, float top, float percentile) { return bottom + (top - bottom) * percentile; }

float Tolerance(float percentile)
{
    return std::max(1.0f, Lerp(static_cast<float>(sPlayerbotAIConfig.ArenaTeamToleranceBottom),
                               static_cast<float>(sPlayerbotAIConfig.ArenaTeamToleranceTop), percentile));
}

uint32 LeaveChance(float percentile)
{
    return static_cast<uint32>(
        std::lround(Lerp(static_cast<float>(sPlayerbotAIConfig.ArenaTeamLeaveChanceBottom),
                         static_cast<float>(sPlayerbotAIConfig.ArenaTeamLeaveChanceTop), percentile)));
}

// Weighted random index (weights >= 0; every one gets MinPickWeight on top)
size_t PickWeighted(std::vector<float> const& weights)
{
    float total = 0.0f;
    for (float weight : weights)
        total += weight + MinPickWeight;

    float roll = frand(0.0f, total);
    for (size_t i = 0; i < weights.size(); ++i)
    {
        roll -= weights[i] + MinPickWeight;
        if (roll <= 0.0f)
            return i;
    }

    return weights.size() - 1;
}

size_t PickBest(std::vector<float> const& weights)
{
    return static_cast<size_t>(std::distance(weights.begin(), std::max_element(weights.begin(), weights.end())));
}
}  // namespace

bool ArenaTeamFormation::IsActive()
{
    return sPlayerbotAIConfig.ArenaTeamFormationEnable &&
           sPlayerbotAIConfig.RandomBotArenaTeamRatingMode == ARENA_RATING_MODE_EARNED &&
           !sPlayerbotAIConfig.DeleteRandomBotArenaTeams;
}

uint32 ArenaTeamFormation::MaxRoster(uint32 type)
{
    switch (type)
    {
        case ARENA_TYPE_2v2:
            return sPlayerbotAIConfig.ArenaTeamMaxRoster2v2;
        case ARENA_TYPE_3v3:
            return sPlayerbotAIConfig.ArenaTeamMaxRoster3v3;
        default:
            return sPlayerbotAIConfig.ArenaTeamMaxRoster5v5;
    }
}

uint32 ArenaTeamFormation::MinRoster(uint32 type)
{
    return std::clamp<uint32>(sPlayerbotAIConfig.ArenaTeamMinMembers, type, MaxRoster(type));
}

float ArenaTeamFormation::ClassSynergy(std::vector<uint8> const& teamClasses, uint8 candidateClass)
{
    bool teamHealer = false;
    bool teamTank = false;
    uint32 sameClass = 0;
    for (uint8 cls : teamClasses)
    {
        teamHealer = teamHealer || CanHeal(cls);
        teamTank = teamTank || CanTank(cls);
        if (cls == candidateClass)
            ++sameClass;
    }

    bool const candidateHealer = CanHeal(candidateClass);
    float synergy = SynergyBase - SynergySameClass * static_cast<float>(sameClass);
    if (!teamHealer && candidateHealer)
        synergy += SynergyFirstHealer;

    if (teamHealer && IsPureDamage(candidateClass))
        synergy += SynergyDamageBesideHealer;

    if ((teamTank && candidateHealer) || (teamHealer && CanTank(candidateClass)))
        synergy += SynergyTankAndHealer;

    if (teamClasses.size() + 1 >= TeamSizeNeedingHealer && !teamHealer && !candidateHealer)
        synergy -= SynergyNoHealerInThree;

    return std::clamp(synergy, 0.0f, 1.0f);
}

float ArenaTeamFormation::FitScore(bool skillKnown, float skillDiff, float tolerance,
                                   std::vector<uint8> const& teamClasses, uint8 candidateClass, float synergyWeight)
{
    float const closeness =
        skillKnown ? std::max(0.0f, 1.0f - std::fabs(skillDiff) / std::max(tolerance, 1.0f)) : UnknownCloseness;
    float const weight = std::clamp(synergyWeight, 0.0f, 1.0f);
    return closeness * (1.0f - weight) + ClassSynergy(teamClasses, candidateClass) * weight;
}

void ArenaTeamFormation::Update(uint32 diff)
{
    if (!IsActive())
    {
        _timer = 0;
        return;
    }

    // The comp table is read once the option is on (startup, or the first tick after a config reload turned it on)
    if (sPlayerbotAIConfig.ArenaTeamCompLearningEnable && !_compsTried)
    {
        PLAYERBOT_CHECK_WORLD_THREAD("ArenaTeamFormation::LoadComps");
        _compsTried = true;
        std::string message;
        LoadComps(message);
    }

    _timer += diff;
    if (_timer < sPlayerbotAIConfig.ArenaTeamUpdateSeconds * IN_MILLISECONDS)
        return;

    _timer = 0;
    PLAYERBOT_CHECK_WORLD_THREAD("ArenaTeamFormation::Update");
    RunPass();
}

void ArenaTeamFormation::RunPass()
{
    ++_pass;
    time_t const now = time(nullptr);
    uint32 joinsLeft = sPlayerbotAIConfig.ArenaTeamRecruitsPerPass;
    std::vector<Candidate> pool;
    bool poolBuilt = false;
    bool const logSummary = _pass % SummaryEveryPasses == 0;
    _percentiles.clear();
    std::vector<std::pair<ArenaType, std::vector<ArenaTeam*>>> typeTeams;

    for (ArenaType type : ArenaTypes)
    {
        auto const it = RandomPlayerbotFactory::_botArenaTeamRegistry.find(type);
        if (it == RandomPlayerbotFactory::_botArenaTeamRegistry.end())
            continue;

        std::vector<ArenaTeam*> teams;
        for (uint32 teamId : it->second)
        {
            if (ArenaTeam* team = sArenaTeamMgr->GetArenaTeamById(teamId))
                teams.push_back(team);
        }

        if (teams.empty())
            continue;

        // Ladder position: the share of the type's bot teams with a lower rating (0 bottom, 1 top)
        std::vector<uint32> ratings;
        ratings.reserve(teams.size());
        for (ArenaTeam const* team : teams)
            ratings.push_back(team->GetRating());

        std::sort(ratings.begin(), ratings.end());
        float const span = teams.size() > 1 ? static_cast<float>(teams.size() - 1) : 1.0f;
        for (ArenaTeam const* team : teams)
        {
            auto const lower = std::lower_bound(ratings.begin(), ratings.end(), team->GetRating());
            _percentiles[team->GetId()] = static_cast<float>(std::distance(ratings.begin(), lower)) / span;
        }

        for (ArenaTeam* team : teams)
        {
            if (TrackLosses(team))
                TryLeave(team, now);
        }

        typeTeams.emplace_back(type, std::move(teams));
    }

    // Teams below MinRoster fill up to it first (every type); then teams between it and the max roster may take one
    // more (RecruitAboveMinChance), so members keep leaving and joining after the login fill
    for (bool const aboveMin : {false, true})
    {
        for (auto const& [type, teams] : typeTeams)
        {
            uint32 const minRoster = MinRoster(type);
            uint32 const maxRoster = MaxRoster(type);
            for (ArenaTeam* team : teams)
            {
                if (!joinsLeft)
                    break;

                uint32 const size = team->GetMembersSize();
                if (size >= maxRoster || (size >= minRoster) != aboveMin)
                    continue;

                if (aboveMin && !WantsRecruitAboveMin(team, now))
                    continue;

                if (!poolBuilt)
                {
                    BuildPool(pool);
                    poolBuilt = true;
                }

                Recruit(team, pool, joinsLeft, now, aboveMin ? size + 1 : minRoster);
            }
        }
    }

    if (logSummary)
    {
        for (auto const& [type, teams] : typeTeams)
        {
            std::vector<uint32> rosterCounts(MaxRoster(type) + 1, 0);
            for (ArenaTeam const* team : teams)
                ++rosterCounts[std::min<size_t>(team->GetMembersSize(), rosterCounts.size() - 1)];

            std::string sizes;
            std::string counts;
            for (size_t size = 1; size < rosterCounts.size(); ++size)
            {
                sizes += (size > 1 ? "/" : "") + std::to_string(size);
                counts += (size > 1 ? "/" : "") + std::to_string(rosterCounts[size]);
            }

            LOG_INFO("playerbots", "Arena formation: {}v{} {} teams (roster {}: {}), since startup {} leaves, {} joins",
                     type, type, teams.size(), sizes, counts, _leaves, _joins);
        }

        if (CompLearningOn())
        {
            for (std::string const& line : DiversityLines())
                LOG_INFO("playerbots", "Arena formation comps: {}", line);
        }
    }

    // Members no team listed this pass (deleted teams, members removed elsewhere)
    for (auto it = _tracks.begin(); it != _tracks.end();)
    {
        if (it->second.SeenPass != _pass)
            it = _tracks.erase(it);
        else
            ++it;
    }
}

bool ArenaTeamFormation::TrackLosses(ArenaTeam* team)
{
    bool lost = false;
    for (ArenaTeamMember const& member : team->GetMembers())
    {
        auto [it, inserted] = _tracks.try_emplace(member.Guid.GetCounter());
        MemberTrack& track = it->second;
        if (inserted || track.TeamId != team->GetId() || member.SeasonGames < track.SeasonGames ||
            member.SeasonWins < track.SeasonWins)
        {
            track = MemberTrack{team->GetId(), member.SeasonGames, member.SeasonWins, 0, 0, _pass};
            continue;
        }

        uint32 const games = member.SeasonGames - track.SeasonGames;
        uint32 const wins = member.SeasonWins - track.SeasonWins;
        uint32 const losses = games > wins ? games - wins : 0;
        // The order of several matches within one pass is unknown: the losses count after the wins
        if (wins)
            track.LossStreak = 0;

        track.LossStreak += losses;
        track.LossesSinceJoin += losses;
        track.SeasonGames = member.SeasonGames;
        track.SeasonWins = member.SeasonWins;
        track.SeenPass = _pass;
        lost = lost || losses > 0;
    }

    return lost;
}

void ArenaTeamFormation::TryLeave(ArenaTeam* team, time_t now)
{
    uint32 const teamId = team->GetId();
    if (team->GetMembersSize() <= MinRoster(team->GetType()))
        return;

    auto const changed = _teamChangedAt.find(teamId);
    if (changed != _teamChangedAt.end() && now < changed->second + sPlayerbotAIConfig.ArenaTeamMinTeamStaySeconds)
        return;

    // The leaver: the longest loss streak, then the most losses since joining, then random (skill plays no part)
    ArenaTeamMember const* leaverMember = nullptr;
    MemberTrack const* leaverTrack = nullptr;
    uint32 ties = 0;
    for (ArenaTeamMember const& member : team->GetMembers())
    {
        auto const it = _tracks.find(member.Guid.GetCounter());
        if (it == _tracks.end() || it->second.TeamId != teamId)
            continue;

        MemberTrack const& track = it->second;
        bool better =
            !leaverTrack || track.LossStreak > leaverTrack->LossStreak ||
            (track.LossStreak == leaverTrack->LossStreak && track.LossesSinceJoin > leaverTrack->LossesSinceJoin);
        bool const tie = leaverTrack && track.LossStreak == leaverTrack->LossStreak &&
                         track.LossesSinceJoin == leaverTrack->LossesSinceJoin;
        if (tie)
            better = urand(0, ties++) == 0;
        else if (better)
            ties = 1;

        if (better)
        {
            leaverMember = &member;
            leaverTrack = &track;
        }
    }

    if (!leaverMember || leaverTrack->LossStreak < sPlayerbotAIConfig.ArenaTeamLossStreakToConsider)
        return;

    // Never while the team plays or queues
    Player* captain = ObjectAccessor::FindConnectedPlayer(team->GetCaptain());
    if (captain && (captain->InBattleground() || captain->InBattlegroundQueue()))
        return;

    ObjectGuid const leaverGuid = leaverMember->Guid;
    Player* leaver = ObjectAccessor::FindConnectedPlayer(leaverGuid);
    if (leaver && (leaver->InBattleground() || leaver->InBattlegroundQueue()))
        return;

    uint32 const chance = std::min(LeaveChance(LadderPercentile(teamId)), PercentMax);
    if (!chance || urand(1, PercentMax) > chance)
        return;

    std::string const leaverName = leaverMember->Name;
    uint32 const streak = leaverTrack->LossStreak;

    // The captain queues and stays in every pair: a leaving captain hands the team over first (an online member, then
    // the shortest loss streak, then random)
    if (leaverGuid == team->GetCaptain())
    {
        ObjectGuid successor;
        std::string successorName;
        bool successorOnline = false;
        uint32 successorStreak = 0;
        uint32 successorTies = 0;
        for (ArenaTeamMember const& member : team->GetMembers())
        {
            if (member.Guid == leaverGuid)
                continue;

            bool const online = ObjectAccessor::FindConnectedPlayer(member.Guid) != nullptr;
            uint32 const memberStreak = LossStreakOf(member.Guid, teamId);
            bool better = !successor || (online && !successorOnline) ||
                          (online == successorOnline && memberStreak < successorStreak);
            bool const tie = successor && online == successorOnline && memberStreak == successorStreak;
            if (tie)
                better = urand(0, successorTies++) == 0;
            else if (better)
                successorTies = 1;

            if (better)
            {
                successor = member.Guid;
                successorName = member.Name;
                successorOnline = online;
                successorStreak = memberStreak;
            }
        }

        if (!successor)
            return;

        team->SetCaptain(successor);
        LOG_INFO("playerbots", "Arena formation: {} is the new captain of {}", successorName, team->GetName());
    }

    // Out of the team's standing arena group (the captain re-forms it)
    if (leaver)
    {
        if (Group* group = leaver->GetGroup())
        {
            if (team->IsMember(group->GetLeaderGUID()))
                group->RemoveMember(leaverGuid);
        }
    }

    team->DelMember(leaverGuid, true);
    _tracks.erase(leaverGuid.GetCounter());
    _leftTeam[leaverGuid.GetCounter()] = teamId;
    _teamChangedAt[teamId] = now;
    ++_leaves;
    LOG_INFO("playerbots", "Arena formation: {} left {} ({}v{}, rating {}, loss streak {}, chance {}%)", leaverName,
             team->GetName(), team->GetType(), team->GetType(), team->GetRating(), streak, chance);

    // An online leaver looks for another team at once (or founds one); an offline one at its next login
    if (leaver)
        RandomPlayerbotFactory::AssignBotToArenaTeamInternal(leaver, teamId);
}

void ArenaTeamFormation::BuildPool(std::vector<Candidate>& pool) const
{
    for (auto it = sRandomPlayerbotMgr.GetPlayerBotsBegin(); it != sRandomPlayerbotMgr.GetPlayerBotsEnd(); ++it)
    {
        Player* bot = it->second;
        if (!bot || !bot->IsInWorld() || bot->GetLevel() < MinArenaTeamLevel)
            continue;

        if (!RandomPlayerbotFactory::IsEligibleForBotArenaTeam(bot))
            continue;

        if (bot->InBattleground() || bot->InBattlegroundQueue() || bot->IsInCombat())
            continue;

        bool teamed = false;
        for (uint8 slot = ARENA_SLOT_2v2; slot <= ARENA_SLOT_5v5 && !teamed; ++slot)
            teamed = bot->GetArenaTeamId(slot) || sCharacterCache->GetCharacterArenaTeamIdByGuid(bot->GetGUID(), slot);

        if (!teamed)
            pool.push_back(Candidate{bot->GetGUID(), LeftTeamOf(bot->GetGUID()), false});
    }
}

bool ArenaTeamFormation::WantsRecruitAboveMin(ArenaTeam const* team, time_t now) const
{
    uint32 const chance = sPlayerbotAIConfig.ArenaTeamRecruitAboveMinChance;
    if (!chance)
        return false;

    // Not within MinTeamStaySeconds of the team's last join or leave
    auto const changed = _teamChangedAt.find(team->GetId());
    if (changed != _teamChangedAt.end() && now < changed->second + sPlayerbotAIConfig.ArenaTeamMinTeamStaySeconds)
        return false;

    return urand(1, PercentMax) <= chance;
}

void ArenaTeamFormation::Recruit(ArenaTeam* team, std::vector<Candidate>& pool, uint32& joinsLeft, time_t now,
                                 uint32 target)
{
    CharacterCacheEntry const* captain = sCharacterCache->GetCharacterCacheByGuid(team->GetCaptain());
    if (!captain)
        return;

    TeamId const faction = Player::TeamIdForRace(captain->Race);
    uint32 const roster = std::min(target, MaxRoster(team->GetType()));
    float const percentile = LadderPercentile(team->GetId());

    while (joinsLeft && team->GetMembersSize() < roster)
    {
        std::vector<size_t> indices;
        std::vector<float> fits;
        for (size_t i = 0; i < pool.size(); ++i)
        {
            if (pool[i].Taken || pool[i].LeftTeamId == team->GetId())
                continue;

            Player* bot = ObjectAccessor::FindConnectedPlayer(pool[i].Guid);
            if (!bot || bot->GetTeamId() != faction ||
                !RandomPlayerbotFactory::SameArenaBracket(captain->Level, bot->GetLevel()))
                continue;

            float const fit = TeamFit(team, bot);
            if (fit < 0.0f)
                continue;

            indices.push_back(i);
            fits.push_back(fit);
        }

        if (indices.empty())
            return;

        // Strict at the top (the best score), forgiving at the bottom (weighted random). With comp learning a softmax
        // whose temperature falls with the percentile: the top mostly takes the best, never surely.
        size_t pick = 0;
        if (CompLearningOn())
            pick = CompLearning::PickSoftmax(fits, CompParams(percentile).Temperature, frand(0.0f, 1.0f));
        else
            pick = frand(0.0f, 1.0f) < percentile ? PickBest(fits) : PickWeighted(fits);

        Candidate& candidate = pool[indices[pick]];
        candidate.Taken = true;
        Player* bot = ObjectAccessor::FindConnectedPlayer(candidate.Guid);
        if (!bot || !team->AddMember(candidate.Guid))
            continue;

        OnJoined(bot, team, now);
        --joinsLeft;
    }
}

bool ArenaTeamFormation::JoinBestTeam(Player* bot, std::vector<ArenaTeam*> const& candidates, uint32 excludedTeamId)
{
    PLAYERBOT_CHECK_WORLD_THREAD("ArenaTeamFormation::JoinBestTeam");

    // Teams short of members first; among them the bot picks weighted by fit (each team's tolerance is its strictness)
    std::vector<ArenaTeam*> needy;
    std::vector<float> needyFits;
    std::vector<ArenaTeam*> others;
    std::vector<float> otherFits;
    uint32 const leftTeamId = LeftTeamOf(bot->GetGUID());
    for (ArenaTeam* team : candidates)
    {
        if (!team || team->GetId() == excludedTeamId || team->GetId() == leftTeamId)
            continue;

        float const fit = TeamFit(team, bot);
        if (fit < 0.0f)
            continue;

        bool const isShort = team->GetMembersSize() < MinRoster(team->GetType());
        (isShort ? needy : others).push_back(team);
        (isShort ? needyFits : otherFits).push_back(fit);
    }

    for (auto* teams : {&needy, &others})
    {
        std::vector<float>& fits = teams == &needy ? needyFits : otherFits;
        while (!teams->empty())
        {
            size_t const pick = PickWeighted(fits);
            ArenaTeam* team = (*teams)[pick];
            teams->erase(teams->begin() + pick);
            fits.erase(fits.begin() + pick);
            if (!team->AddMember(bot->GetGUID()))
                continue;

            OnJoined(bot, team, time(nullptr));
            return true;
        }
    }

    return false;
}

void ArenaTeamFormation::OnJoined(Player* bot, ArenaTeam* team, time_t now)
{
    _tracks.erase(bot->GetGUID().GetCounter());
    _teamChangedAt[team->GetId()] = now;
    ++_joins;
    std::string learned;
    std::vector<CompLearning::GroupValue> groups;
    float value = 0.0f;
    if (CompLearningOn() && LearnedTerm(team, bot, value, &groups))
    {
        learned = Acore::StringFormat(", learned {:.3f} at percentile {:.2f}:", value, LadderPercentile(team->GetId()));
        for (CompLearning::GroupValue const& group : groups)
        {
            learned += Acore::StringFormat(" {} {:.3f} ({}/{}{})", group.Key, group.Value, group.Counts.Wins,
                                           group.Counts.Games, group.Counts.Exact ? "" : " pooled");
        }
    }

    LOG_INFO("playerbots", "Arena formation: {} joined {} ({}v{}, rating {}, roster {}{})", bot->GetName(),
             team->GetName(), team->GetType(), team->GetType(), team->GetRating(), team->GetMembersSize(), learned);
}

void ArenaTeamFormation::OnFounded(Player* bot, ArenaTeam const* team)
{
    _tracks.erase(bot->GetGUID().GetCounter());
    _teamChangedAt[team->GetId()] = time(nullptr);
    LOG_INFO("playerbots", "Arena formation: {} founded {} ({}v{})", bot->GetName(), team->GetName(), team->GetType(),
             team->GetType());
}

float ArenaTeamFormation::TeamFit(ArenaTeam* team, Player* candidate) const
{
    std::vector<uint8> classes;
    float skillSum = 0.0f;
    uint32 skillCount = 0;
    for (ArenaTeamMember const& member : team->GetMembers())
    {
        classes.push_back(member.Class);
        float skill = 0.0f;
        if (BotSkill(member.Guid, skill))
        {
            skillSum += skill;
            ++skillCount;
        }
    }

    float candidateSkill = 0.0f;
    bool const known = skillCount && BotSkill(candidate->GetGUID(), candidateSkill);
    float const diff = known ? candidateSkill - skillSum / static_cast<float>(skillCount) : 0.0f;
    float const tolerance = Tolerance(LadderPercentile(team->GetId()));
    if (known && std::fabs(diff) > tolerance)
        return -1.0f;

    float const fit =
        FitScore(known, diff, tolerance, classes, candidate->getClass(), sPlayerbotAIConfig.ArenaTeamSynergyWeight);
    float learned = 0.0f;
    if (!CompLearningOn() || !LearnedTerm(team, candidate, learned, nullptr))
        return fit;

    return CompLearning::Blend(fit, learned, CompParams(LadderPercentile(team->GetId())).Weight);
}

bool ArenaTeamFormation::CompLearningOn() const
{
    return sPlayerbotAIConfig.ArenaTeamCompLearningEnable && _comps.Loaded && IsActive();
}

std::string ArenaTeamFormation::CompsPath()
{
    std::string const& file = sPlayerbotAIConfig.ArenaTeamCompLearningFile;
    std::string const name = file.empty() ? std::string(DefaultCompsFile) : file;
    return std::filesystem::path(name).is_absolute() ? name : sConfigMgr->GetConfigPath() + name;
}

CompLearning::Params ArenaTeamFormation::CompParams(float percentile)
{
    return CompLearning::ParamsAt(
        percentile, sPlayerbotAIConfig.ArenaTeamCompLearningWeightBottom,
        sPlayerbotAIConfig.ArenaTeamCompLearningWeightTop, sPlayerbotAIConfig.ArenaTeamCompLearningExploreBottom,
        sPlayerbotAIConfig.ArenaTeamCompLearningExploreTop, sPlayerbotAIConfig.ArenaTeamCompLearningTemperatureBottom,
        sPlayerbotAIConfig.ArenaTeamCompLearningTemperatureTop);
}

bool ArenaTeamFormation::LoadComps(std::string& message)
{
    std::string const path = CompsPath();
    std::ifstream in(path, std::ios::binary);
    if (!in)
    {
        message = Acore::StringFormat("cannot read {} ({})", path,
                                      _comps.Loaded ? "the previous table stays" : "comp learning stays off");
        LOG_WARN("playerbots", "Arena formation: {}", message);
        return false;
    }

    std::stringstream buffer;
    buffer << in.rdbuf();
    CompLearning::Table table;
    std::string error;
    if (!CompLearning::ParseTable(buffer.str(), table, error))
    {
        message = Acore::StringFormat("{}: {} ({})", path, error,
                                      _comps.Loaded ? "the previous table stays" : "comp learning stays off");
        LOG_WARN("playerbots", "Arena formation: {}", message);
        return false;
    }

    _comps = std::move(table);
    _compsSource = path;
    message = Acore::StringFormat("comp table loaded from {}: {} rows in {} bands, prior {}, generated {}", path,
                                  _comps.Rows, _comps.Bands.size(), _comps.Prior, _comps.GeneratedAt);
    LOG_INFO("playerbots", "Arena formation: {}", message);
    return true;
}

std::string ArenaTeamFormation::MemberEntry(ObjectGuid guid, uint8 cls) const
{
    auto const it = _entries.find(guid.GetCounter());
    if (it != _entries.end() && it->second.first == _pass)
        return it->second.second;

    if (Player* player = ObjectAccessor::FindConnectedPlayer(guid))
    {
        std::string entry = PvpTestRecordWriter::ClassName(cls);
        std::string const spec = AiFactory::GetPlayerSpecName(player);
        if (!spec.empty())
            entry += "/" + spec;

        std::transform(entry.begin(), entry.end(), entry.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        _entries[guid.GetCounter()] = {_pass, entry};
        return entry;
    }

    return it != _entries.end() ? it->second.second : PvpTestRecordWriter::ClassName(cls);
}

bool ArenaTeamFormation::LearnedTerm(ArenaTeam* team, Player* candidate, float& value,
                                     std::vector<CompLearning::GroupValue>* groups) const
{
    int32 const band = CompLearning::DataBand(_comps, team->GetRating());
    if (band < 0)
        return false;

    std::vector<std::string> members;
    for (ArenaTeamMember const& member : team->GetMembers())
    {
        if (member.Guid != candidate->GetGUID())
            members.push_back(MemberEntry(member.Guid, member.Class));
    }

    std::string const entry = MemberEntry(candidate->GetGUID(), candidate->getClass());
    float const explore = CompParams(LadderPercentile(team->GetId())).Explore;
    return CompLearning::CandidateValue(_comps, band, members, entry, team->GetType(), explore, MaxCompGroups, value,
                                        groups);
}

std::vector<std::string> ArenaTeamFormation::DiversityLines() const
{
    std::vector<std::string> lines;
    for (ArenaType type : ArenaTypes)
    {
        auto const it = RandomPlayerbotFactory::_botArenaTeamRegistry.find(type);
        if (it == RandomPlayerbotFactory::_botArenaTeamRegistry.end())
            continue;

        // band index (-1: no table, or outside every band) -> weighted playing-group comps; a team's groups share 1
        std::map<int32, std::vector<std::pair<std::string, float>>> byBand;
        for (uint32 teamId : it->second)
        {
            ArenaTeam* team = sArenaTeamMgr->GetArenaTeamById(teamId);
            if (!team)
                continue;

            std::vector<std::string> roster;
            for (ArenaTeamMember const& member : team->GetMembers())
                roster.push_back(MemberEntry(member.Guid, member.Class));

            std::vector<std::vector<size_t>> const sets =
                CompLearning::Combinations(roster.size(), type, MaxCompGroups);
            std::vector<std::pair<std::string, float>>& comps = byBand[CompLearning::BandOf(_comps, team->GetRating())];
            for (std::vector<size_t> const& set : sets)
            {
                std::vector<std::string> entries;
                for (size_t index : set)
                    entries.push_back(roster[index]);

                comps.emplace_back(CompLearning::CompKey(entries), 1.0f / static_cast<float>(sets.size()));
            }
        }

        for (auto const& [band, comps] : byBand)
        {
            CompLearning::Diversity const diversity = CompLearning::Summarize(comps, DiversityTopShown);
            std::string const range = band < 0
                                          ? std::string("all ratings")
                                          : Acore::StringFormat("{}-{}", _comps.Bands[band].Lo, _comps.Bands[band].Hi);
            std::string top;
            for (auto const& [key, weight] : diversity.Top)
                top += Acore::StringFormat("{}{} {:.1f}", top.empty() ? "" : ", ", key, weight);

            lines.push_back(Acore::StringFormat("{}v{} band {}: {:.0f} teams, {} comps (effective {:.1f}); most: {}",
                                                type, type, range, diversity.Teams, diversity.Distinct,
                                                diversity.Effective, top));
        }
    }

    return lines;
}

float ArenaTeamFormation::LadderPercentile(uint32 teamId) const
{
    auto const it = _percentiles.find(teamId);
    return it != _percentiles.end() ? it->second : 0.0f;
}

uint32 ArenaTeamFormation::LossStreakOf(ObjectGuid guid, uint32 teamId) const
{
    auto const it = _tracks.find(guid.GetCounter());
    return it != _tracks.end() && it->second.TeamId == teamId ? it->second.LossStreak : 0;
}

uint32 ArenaTeamFormation::LeftTeamOf(ObjectGuid guid) const
{
    auto const it = _leftTeam.find(guid.GetCounter());
    return it != _leftTeam.end() ? it->second : 0;
}

bool ArenaTeamFormation::ShouldRepick(Player* captain, ArenaTeam const* team) const
{
    if (!IsActive() || !sPlayerbotAIConfig.ArenaTeamRandomPair || !sPlayerbotAIConfig.ArenaTeamRepickEachQueue)
        return false;

    if (team->GetMembersSize() <= team->GetType() || captain->InBattleground() || captain->InBattlegroundQueue())
        return false;

    return !IsPairFresh(team->GetId(), team->GetStats().SeasonGames);
}

bool ArenaTeamFormation::IsPairFresh(uint32 teamId, uint32 teamSeasonGames) const
{
    std::lock_guard<std::mutex> guard(_pairLock);
    auto const it = _pairFormedAtGames.find(teamId);
    return it != _pairFormedAtGames.end() && it->second == teamSeasonGames;
}

void ArenaTeamFormation::OnPairFormed(uint32 teamId, uint32 teamSeasonGames)
{
    std::lock_guard<std::mutex> guard(_pairLock);
    _pairFormedAtGames[teamId] = teamSeasonGames;
}

bool ArenaTeamFormation::HandleTeamsCommand(ChatHandler* handler, char const* /*args*/)
{
    ArenaTeamFormation const& formation = Instance();
    handler->PSendSysMessage("Bot arena teams (formation {}):", IsActive() ? "on" : "off");
    uint32 shown = 0;
    for (ArenaType type : ArenaTypes)
    {
        auto const it = RandomPlayerbotFactory::_botArenaTeamRegistry.find(type);
        if (it == RandomPlayerbotFactory::_botArenaTeamRegistry.end())
            continue;

        for (uint32 teamId : it->second)
        {
            ArenaTeam* team = sArenaTeamMgr->GetArenaTeamById(teamId);
            if (!team)
                continue;

            std::string roster;
            for (ArenaTeamMember const& member : team->GetMembers())
            {
                uint32 const losses = member.SeasonGames - std::min(member.SeasonWins, member.SeasonGames);
                roster += Acore::StringFormat("{}{}{} ({}, {}/{}, streak {})", roster.empty() ? "" : ", ",
                                              member.Guid == team->GetCaptain() ? "*" : "", member.Name,
                                              ClassName(member.Class), member.SeasonWins, losses,
                                              formation.LossStreakOf(member.Guid, teamId));
            }

            handler->PSendSysMessage("#{} <{}> {}v{} rating {}, roster {}: {}", teamId, team->GetName(), type, type,
                                     team->GetRating(), team->GetMembersSize(), roster);
            ++shown;
        }
    }

    handler->PSendSysMessage("{} teams (* = captain; W/L this season); since startup {} leaves, {} joins", shown,
                             formation._leaves, formation._joins);
    return true;
}

bool ArenaTeamFormation::HandleCompsCommand(ChatHandler* handler, char const* args)
{
    ArenaTeamFormation& formation = Instance();
    std::istringstream words(args ? args : "");
    std::string mode;
    words >> mode;

    if (mode == "reload")
    {
        PLAYERBOT_CHECK_WORLD_THREAD("ArenaTeamFormation::HandleCompsCommand");
        formation._compsTried = true;
        std::string message;
        formation.LoadComps(message);
        handler->PSendSysMessage("Arena comps: {}", message);
        return true;
    }

    if (mode == "selftest")
    {
        std::vector<std::string> lines;
        uint32 const failures = CompLearning::RunSelftest(lines);
        for (std::string const& line : lines)
            handler->PSendSysMessage("{}", line);

        handler->PSendSysMessage("comps selftest: {} ({} failure(s))", failures ? "FAIL" : "PASS", failures);
        LOG_INFO("playerbots", "Arena formation: comps selftest {} ({} failure(s))", failures ? "FAIL" : "PASS",
                 failures);
        return true;
    }

    CompLearning::Table const& table = formation._comps;
    if (mode == "teams")
    {
        uint32 shown = 0;
        for (ArenaType type : ArenaTypes)
        {
            auto const it = RandomPlayerbotFactory::_botArenaTeamRegistry.find(type);
            if (it == RandomPlayerbotFactory::_botArenaTeamRegistry.end())
                continue;

            for (uint32 teamId : it->second)
            {
                ArenaTeam* team = sArenaTeamMgr->GetArenaTeamById(teamId);
                if (!team)
                    continue;

                std::vector<std::string> roster;
                for (ArenaTeamMember const& member : team->GetMembers())
                    roster.push_back(formation.MemberEntry(member.Guid, member.Class));

                float const percentile = formation.LadderPercentile(teamId);
                int32 const band = CompLearning::DataBand(table, team->GetRating());
                std::string groups;
                for (std::vector<size_t> const& set : CompLearning::Combinations(roster.size(), type, CompRowsShown))
                {
                    std::vector<std::string> entries;
                    for (size_t index : set)
                        entries.push_back(roster[index]);

                    CompLearning::Lookup const lookup = CompLearning::Find(table, band, entries);
                    float const value = band < 0
                                            ? 0.5f
                                            : CompLearning::LearnedValue(lookup, table.Prior, table.Bands[band].Games,
                                                                         CompParams(percentile).Explore);
                    groups += Acore::StringFormat("{}{} {:.3f} ({}/{})", groups.empty() ? "" : ", ",
                                                  CompLearning::CompKey(entries), value, lookup.Wins, lookup.Games);
                }

                handler->PSendSysMessage("#{} <{}> {}v{} rating {}, percentile {:.2f}, data band {}: {}", teamId,
                                         team->GetName(), type, type, team->GetRating(), percentile, band, groups);
                ++shown;
            }
        }

        handler->PSendSysMessage("{} teams (groups: learned value with this percentile's exploration, wins/games)",
                                 shown);
        return true;
    }

    handler->PSendSysMessage(
        "Comp learning {} (option {}, formation {}); table {}", formation.CompLearningOn() ? "on" : "off",
        sPlayerbotAIConfig.ArenaTeamCompLearningEnable ? "on" : "off", IsActive() ? "on" : "off",
        table.Loaded ? Acore::StringFormat("{}: {} rows, prior {}, generated {}", formation._compsSource, table.Rows,
                                           table.Prior, table.GeneratedAt)
                     : "not loaded (" + CompsPath() + "; .playerbots arena comps reload)");
    for (size_t band = 0; band < table.Bands.size(); ++band)
    {
        CompLearning::Band const& rows = table.Bands[band];
        std::vector<std::pair<float, std::string>> ranked;
        for (auto const& [key, row] : rows.Comps)
        {
            float const shrunk = CompLearning::LearnedValue(CompLearning::Lookup{row.Games, row.Wins, true},
                                                            table.Prior, rows.Games, 0.0f);
            ranked.emplace_back(shrunk, Acore::StringFormat("{} {:.3f} ({}/{})", key, shrunk, row.Wins, row.Games));
        }

        std::sort(ranked.begin(), ranked.end(), [](auto const& a, auto const& b)
                  { return a.first != b.first ? a.first > b.first : a.second < b.second; });
        std::string best;
        for (size_t i = 0; i < ranked.size() && i < CompRowsShown; ++i)
            best += (best.empty() ? "" : ", ") + ranked[i].second;

        handler->PSendSysMessage("band {} [{}, {}): {} comps, {} games; best: {}", band, rows.Lo, rows.Hi,
                                 rows.Comps.size(), rows.Games, best.empty() ? "-" : best);
    }

    std::vector<std::string> const lines = formation.DiversityLines();
    handler->PSendSysMessage("Bot teams by rating band ({} lines; playing-group comps, a team's groups share 1):",
                             lines.size());
    for (std::string const& line : lines)
        handler->PSendSysMessage("{}", line);

    return true;
}
