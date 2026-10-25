/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_RANDOMPLAYERBOTFACTORY_H
#define PLAYERBOTS_RANDOMPLAYERBOTFACTORY_H

#include "ArenaTeam.h"
#include "Battleground.h"
#include "Common.h"
#include "DBCEnums.h"
#include "SharedDefines.h"
#include <atomic>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

class Player;
class WorldSession;

class RandomPlayerbotFactory
{
public:
    enum class NameRaceAndGender : uint8
    {
        // Generic is the category used for human & undead
        GenericMale = 0,
        GenericFemale,
        GnomeMale,
        GnomeFemale,
        DwarfMale,
        DwarfFemale,
        NightelfMale,
        NightelfFemale,
        DraeneiMale,
        DraeneiFemale,
        OrcMale,
        OrcFemale,
        TrollMale,
        TrollFemale,
        TaurenMale,
        TaurenFemale,
        BloodelfMale,
        BloodelfFemale
    };

    static constexpr NameRaceAndGender CombineRaceAndGender(uint8 race, uint8 gender);

    RandomPlayerbotFactory() {};
    virtual ~RandomPlayerbotFactory() {}

    Player* CreateRandomBot(WorldSession* session, uint8 cls, std::unordered_map<NameRaceAndGender, std::vector<std::string>>& names);
    static void CreateRandomBots();
    static std::string const CreateRandomGuildName();
    static uint32 CalculateTotalAccountCount();
    static uint32 CalculateAvailableCharsPerAccount();
    static void LoadLocalizedNameColumns();
    static std::string GetGuildNameSelector(std::string const& column);
    static std::string GetArenaTeamNameSelector(std::string const& column);

    // Arena team management
    static void AssignBotToArenaTeam(Player* bot);
    // Both levels are in the same arena bracket (a team can only queue when all its members are in the captain's)
    static bool SameArenaBracket(uint8 level, uint8 otherLevel);
    static void DeleteBotArenaTeams();
    static uint32 GetBotArenaTeamCount(ArenaType type);
    // World thread: the team is in the bot arena team registry (the joins and the formation fill it)
    static bool IsRegisteredBotArenaTeam(uint8 type, uint32 teamId);
    // World thread: a 2v2 team of captain and partner (online or not), founded by an online bot, which leaves it at
    // once unless it is the captain. Not in the bot arena team registry (main's ladder pool). Returns the team id or 0
    static uint32 CreatePairArenaTeam(Player* founder, ObjectGuid captain, ObjectGuid partner);
    static void LoadArenaTeamData();
    // Sets every bot arena team's rating and its members' personal rating and MMR to the core's start values.
    // Returns the number of teams reset; resetMembers receives the number of members.
    static uint32 ResetBotArenaTeamRatings(uint32& resetMembers);

private:
    friend class ArenaTeamAssignOperation;
    friend class ArenaTeamFormation;

    static bool IsValidRaceClassCombination(uint8 race, uint8 class_, uint32 expansion);
    std::string const CreateRandomBotName(NameRaceAndGender raceAndGender);

    static bool IsEligibleForBotArenaTeam(Player* bot);
    // excludedTeamId: a team the bot just left (Playerbots.ArenaTeam.Formation.Enable)
    static void AssignBotToArenaTeamInternal(Player* bot, uint32 excludedTeamId = 0);
    static void CollectJoinableBotArenaTeams(ArenaType type, TeamId faction, uint8 level, std::vector<ArenaTeam*>& out);
    static void CreateBotArenaTeam(Player* bot, ArenaType type);
    static void SetRandomRatingAndEmblem(ArenaTeam* team);
    static bool IsBotArenaTeam(ArenaTeam const* team);
    static std::string CreateRandomArenaTeamName();
    static std::string GetLocalizedNameSelector(bool localized, std::string const& column);

    static inline std::vector<std::string> _availableArenaTeamNames;
    static inline std::map<ArenaType, std::vector<uint32>> _botArenaTeamRegistry;
    static inline std::map<ArenaType, uint32> _configTargets;
    // Whether the name table has a column for the default DBC locale
    static inline std::atomic<bool> _guildNamesLocalized{false};
    static inline std::atomic<bool> _arenaTeamNamesLocalized{false};
};

#endif
