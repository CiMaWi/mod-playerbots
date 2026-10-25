/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "ArenaTeamFormation.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <string>

#include "ArenaTeam.h"
#include "ArenaTeamMgr.h"
#include "BotProfileMgr.h"
#include "CharacterCache.h"
#include "Chat.h"
#include "Common.h"
#include "Group.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerbotAIConfig.h"
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

        // Strict at the top (the best score), forgiving at the bottom (weighted random)
        size_t const pick = frand(0.0f, 1.0f) < percentile ? PickBest(fits) : PickWeighted(fits);
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
    LOG_INFO("playerbots", "Arena formation: {} joined {} ({}v{}, rating {}, roster {})", bot->GetName(),
             team->GetName(), team->GetType(), team->GetType(), team->GetRating(), team->GetMembersSize());
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

    return FitScore(known, diff, tolerance, classes, candidate->getClass(), sPlayerbotAIConfig.ArenaTeamSynergyWeight);
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
