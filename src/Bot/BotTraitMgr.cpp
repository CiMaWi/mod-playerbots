/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "BotTraitMgr.h"

#include <algorithm>
#include <cstdlib>
#include <iterator>
#include <limits>
#include <mutex>
#include <sstream>
#include <vector>

#include "CharacterCache.h"
#include "Chat.h"
#include "ChatHelper.h"
#include "DatabaseEnv.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "PlayerbotsDatabase.h"
#include "Random.h"
#include "SharedDefines.h"
#include "StringConvert.h"
#include "StringFormat.h"
#include "ThreadCheck.h"

namespace
{
constexpr uint32 FirstVersion = 1;
// Columns after guid and version: the three interests, then the traits in their stored order (TraitRules::Trait)
constexpr uint32 FirstInterestField = 2;
constexpr uint32 FirstTraitField = FirstInterestField + TraitRules::MAX_INTERESTS;
static_assert(TraitRules::MAX_INTERESTS == 3, "the table has the columns interest1..3");

constexpr uint32 DefaultSample = 10;
constexpr uint32 MaxSample = 50;
constexpr uint32 ValuesPerLine = 6;
constexpr uint32 RaceClassTries = 1000;

constexpr uint8 PlayableRaces[] = {RACE_HUMAN,  RACE_ORC,   RACE_DWARF, RACE_NIGHTELF, RACE_UNDEAD_PLAYER,
                                   RACE_TAUREN, RACE_GNOME, RACE_TROLL, RACE_BLOODELF, RACE_DRAENEI};
constexpr uint8 PlayableClasses[] = {CLASS_WARRIOR,      CLASS_PALADIN, CLASS_HUNTER, CLASS_ROGUE,   CLASS_PRIEST,
                                     CLASS_DEATH_KNIGHT, CLASS_SHAMAN,  CLASS_MAGE,   CLASS_WARLOCK, CLASS_DRUID};

// In-server selftest (the offline tests/trait-selftest.cpp is the proof): determinism, ranges and the mean counts
constexpr uint32 SelftestGuid = 4242;
constexpr uint32 SelftestBots = 2000;
constexpr uint32 MinChangedOnReroll = 20;  // of 30 traits; a new version draws every trait again
constexpr double NoticeableMin = 13.0;
constexpr double NoticeableMax = 17.0;
constexpr double StrongMin = 3.5;
constexpr double StrongMax = 5.5;

std::string const& Columns()
{
    static std::string const columns = []
    {
        std::string text = "`guid`, `version`, `interest1`, `interest2`, `interest3`";
        for (TraitRules::TraitWords const& words : TraitRules::WORDS)
            text += Acore::StringFormat(", `{}`", words.Name);

        return text;
    }();
    return columns;
}

std::string Join(std::vector<std::string> const& parts)
{
    std::string joined;
    for (std::string const& part : parts)
        joined += (joined.empty() ? "" : ", ") + part;

    return joined.empty() ? "none" : joined;
}

std::string LlmLine(TraitRules::BotTraits const& traits)
{
    return Join(TraitRules::Describe(traits)) + " | " + Join(TraitRules::InterestNames(traits));
}

// A race and class a player can have (falls back to a human warrior when the world DB knows none)
void RandomRaceClass(uint8& race, uint8& cls)
{
    for (uint32 i = 0; i < RaceClassTries; ++i)
    {
        race = PlayableRaces[urand(0, static_cast<uint32>(std::size(PlayableRaces)) - 1)];
        cls = PlayableClasses[urand(0, static_cast<uint32>(std::size(PlayableClasses)) - 1)];
        if (sObjectMgr->GetPlayerInfo(race, cls))
            return;
    }

    race = RACE_HUMAN;
    cls = CLASS_WARRIOR;
}

bool InterestsValid(TraitRules::BotTraits const& traits)
{
    std::size_t count = 0;
    for (std::size_t i = 0; i < TraitRules::MAX_INTERESTS; ++i)
    {
        uint8 const interest = traits.Interests[i];
        if (!interest)
            continue;

        // Packed first, valid, distinct
        if (i != count || interest > TraitRules::INTEREST_COUNT)
            return false;

        for (std::size_t j = 0; j < i; ++j)
            if (traits.Interests[j] == interest)
                return false;

        ++count;
    }

    return count >= 1;
}

bool Selftest(ChatHandler* handler)
{
    uint32 checks = 0;
    uint32 failures = 0;
    auto const check = [&](bool ok, char const* what)
    {
        ++checks;
        if (ok)
            return;

        ++failures;
        handler->PSendSysMessage("TRAITS selftest failed: {}", what);
    };

    TraitRules::BotTraits const first = TraitRules::Roll(SelftestGuid, FirstVersion, RACE_HUMAN, CLASS_PRIEST);
    TraitRules::BotTraits const again = TraitRules::Roll(SelftestGuid, FirstVersion, RACE_HUMAN, CLASS_PRIEST);
    TraitRules::BotTraits const next = TraitRules::Roll(SelftestGuid, FirstVersion + 1, RACE_HUMAN, CLASS_PRIEST);
    check(first.Values == again.Values && first.Interests == again.Interests, "the same guid and version roll alike");
    uint32 changed = 0;
    for (std::size_t i = 0; i < TraitRules::TRAIT_COUNT; ++i)
        changed += first.Values[i] != next.Values[i] ? 1 : 0;

    check(changed >= MinChangedOnReroll, "a new version changes most traits");
    check(TraitRules::Unit(first, TraitRules::Trait::Piety) ==
              first.Values[TraitRules::Index(TraitRules::Trait::Piety)] / static_cast<float>(TraitRules::MAX_VALUE),
          "the accessor is value / 100");

    bool inRange = true;
    bool interests = true;
    double noticeable = 0.0;
    double strong = 0.0;
    for (uint32 guid = 1; guid <= SelftestBots; ++guid)
    {
        uint8 const race = PlayableRaces[guid % std::size(PlayableRaces)];
        uint8 const cls = PlayableClasses[(guid / std::size(PlayableRaces)) % std::size(PlayableClasses)];
        TraitRules::BotTraits const traits = TraitRules::Roll(guid, FirstVersion, race, cls);
        for (int8 const value : traits.Values)
        {
            inRange = inRange && value >= TraitRules::MIN_VALUE && value <= TraitRules::MAX_VALUE;
            noticeable += std::abs(value) >= TraitRules::NOTICEABLE ? 1.0 : 0.0;
            strong += std::abs(value) >= TraitRules::STRONG ? 1.0 : 0.0;
        }

        interests = interests && InterestsValid(traits);
    }

    noticeable /= SelftestBots;
    strong /= SelftestBots;
    check(inRange, "every value within -100..100");
    check(interests, "one to three distinct valid interests");
    check(noticeable >= NoticeableMin && noticeable <= NoticeableMax, "about 15 noticeable traits per bot");
    check(strong >= StrongMin && strong <= StrongMax, "about 4.5 strong traits per bot");
    handler->PSendSysMessage(
        "TRAITS selftest: {} checks, {} failures ({}); {:.2f} noticeable, {:.2f} strong traits per bot over {} bots",
        checks, failures, failures ? "FAIL" : "PASS", noticeable, strong, SelftestBots);
    return !failures;
}

void Usage(ChatHandler* handler)
{
    handler->PSendSysMessage(
        "Usage: .playerbots traits [<bot name|guid> [set <trait> <{}..{}> | reroll] | sample <1-{}> | selftest]",
        TraitRules::MIN_VALUE, TraitRules::MAX_VALUE, MaxSample);
}
}  // namespace

void BotTraitMgr::Load()
{
    std::unique_lock<std::shared_mutex> guard(_lock);
    EnsureLoaded();
}

void BotTraitMgr::EnsureLoaded()
{
    if (_loaded)
        return;

    _loaded = true;
    // One query for all bots (startup, world thread), so a bot login reads only the map. The table comes with an update
    // file that a server with Playerbots.Updates.EnableDatabases = 0 may lack: then traits live in memory only
    _table = PlayerbotsDatabase.Query(
                 "SELECT 1 FROM information_schema.TABLES WHERE TABLE_SCHEMA = DATABASE() AND "
                 "TABLE_NAME = 'playerbots_bot_traits'") != nullptr;
    if (!_table)
        LOG_WARN("playerbots",
                 "Bot traits: no table playerbots_bot_traits (update 2026_11_09_00 not applied): traits "
                 "are rolled at each login and kept in memory only");
    else if (QueryResult result = PlayerbotsDatabase.Query("SELECT " + Columns() + " FROM playerbots_bot_traits"))
    {
        do
        {
            Field* fields = result->Fetch();
            TraitRules::BotTraits traits;
            traits.Version = fields[1].Get<uint32>();
            for (std::size_t i = 0; i < TraitRules::MAX_INTERESTS; ++i)
            {
                uint8 const interest = fields[FirstInterestField + i].Get<uint8>();
                if (interest <= TraitRules::INTEREST_COUNT)
                    traits.Interests[i] = interest;
            }

            for (std::size_t i = 0; i < TraitRules::TRAIT_COUNT; ++i)
                traits.Values[i] = static_cast<int8>(std::clamp<int>(fields[FirstTraitField + i].Get<int8>(),
                                                                     TraitRules::MIN_VALUE, TraitRules::MAX_VALUE));

            _traits[fields[0].Get<uint32>()] = traits;
        } while (result->NextRow());
    }

    LOG_INFO("playerbots", "Bot traits loaded: {} bots", _traits.size());
}

void BotTraitMgr::Save(uint32 guid, TraitRules::BotTraits const& traits) const
{
    if (!_table)
        return;

    std::string values = Acore::StringFormat("{}, {}", guid, traits.Version);
    std::string updates = Acore::StringFormat("`version` = {}", traits.Version);
    for (std::size_t i = 0; i < TraitRules::MAX_INTERESTS; ++i)
    {
        uint32 const interest = traits.Interests[i];
        values += Acore::StringFormat(", {}", interest);
        updates += Acore::StringFormat(", `interest{}` = {}", i + 1, interest);
    }

    for (std::size_t i = 0; i < TraitRules::TRAIT_COUNT; ++i)
    {
        int32 const value = traits.Values[i];
        values += Acore::StringFormat(", {}", value);
        updates += Acore::StringFormat(", `{}` = {}", TraitRules::WORDS[i].Name, value);
    }

    PlayerbotsDatabase.Execute("INSERT INTO playerbots_bot_traits (" + Columns() + ") VALUES (" + values +
                               ") ON DUPLICATE KEY UPDATE " + updates);
}

TraitRules::BotTraits BotTraitMgr::LoadOrCreate(uint32 guid, uint8 race, uint8 cls)
{
    std::unique_lock<std::shared_mutex> guard(_lock);
    EnsureLoaded();
    auto const it = _traits.find(guid);
    if (it != _traits.end())
        return it->second;

    TraitRules::BotTraits const traits = TraitRules::Roll(guid, FirstVersion, race, cls);
    _traits[guid] = traits;
    Save(guid, traits);
    return traits;
}

std::optional<TraitRules::BotTraits> BotTraitMgr::Find(uint32 guid)
{
    std::shared_lock<std::shared_mutex> guard(_lock);
    auto const it = _traits.find(guid);
    if (it == _traits.end())
        return std::nullopt;

    return it->second;
}

float BotTraitMgr::Trait(uint32 guid, TraitRules::Trait trait)
{
    std::shared_lock<std::shared_mutex> guard(_lock);
    auto const it = _traits.find(guid);
    return it == _traits.end() ? 0.0f : TraitRules::Unit(it->second, trait);
}

void BotTraitMgr::Forget(uint32 guid)
{
    PLAYERBOT_CHECK_WORLD_THREAD("BotTraitMgr::Forget");
    std::unique_lock<std::shared_mutex> guard(_lock);
    _traits.erase(guid);
}

TraitRules::BotTraits BotTraitMgr::Reroll(uint32 guid, uint8 race, uint8 cls)
{
    std::unique_lock<std::shared_mutex> guard(_lock);
    EnsureLoaded();
    auto const it = _traits.find(guid);
    uint32 const version = (it != _traits.end() ? it->second.Version : FirstVersion) + 1;
    TraitRules::BotTraits const traits = TraitRules::Roll(guid, version, race, cls);
    _traits[guid] = traits;
    Save(guid, traits);
    return traits;
}

std::optional<TraitRules::BotTraits> BotTraitMgr::SetTrait(uint32 guid, TraitRules::Trait trait, int value)
{
    std::unique_lock<std::shared_mutex> guard(_lock);
    EnsureLoaded();
    auto const it = _traits.find(guid);
    if (it == _traits.end())
        return std::nullopt;

    it->second.Values[TraitRules::Index(trait)] =
        static_cast<int8>(std::clamp(value, TraitRules::MIN_VALUE, TraitRules::MAX_VALUE));
    Save(guid, it->second);
    return it->second;
}

bool BotTraitMgr::HandleTraitsCommand(ChatHandler* handler, char const* args)
{
    std::istringstream in(args ? args : "");
    std::string target;
    std::string verb;
    std::string traitName;
    std::string valueText;
    in >> target >> verb >> traitName >> valueText;
    if (target == "selftest")
        return Selftest(handler);

    if (target == "sample")
    {
        std::optional<uint32> const count =
            verb.empty() ? std::optional<uint32>(DefaultSample) : Acore::StringTo<uint32>(verb);
        if (!count || *count < 1 || *count > MaxSample)
        {
            Usage(handler);
            return false;
        }

        handler->PSendSysMessage("Sample of {} rolled personalities (random guids, first roll), as the LLM sees them:",
                                 *count);
        for (uint32 i = 0; i < *count; ++i)
        {
            uint8 race = 0;
            uint8 cls = 0;
            RandomRaceClass(race, cls);
            uint32 const guid = urand(1, std::numeric_limits<int32>::max());
            handler->PSendSysMessage("{} {} | {}", ChatHelper::FormatRace(race), ChatHelper::FormatClass(cls),
                                     LlmLine(TraitRules::Roll(guid, FirstVersion, race, cls)));
        }

        return true;
    }

    BotTraitMgr& mgr = instance();
    bool const allowEdit = sPlayerbotAIConfig.WorldTravel.TraitsAllowEdit;
    if (target.empty())
    {
        std::size_t count = 0;
        bool table = false;
        {
            std::unique_lock<std::shared_mutex> guard(mgr._lock);
            mgr.EnsureLoaded();
            count = mgr._traits.size();
            table = mgr._table;
        }

        handler->PSendSysMessage("Bot traits: {} bots, table {}, edits {} (Playerbots.WorldTravel.Traits.AllowEdit)",
                                 count, table ? "present" : "missing", allowEdit ? "on" : "off");
        Usage(handler);
        return true;
    }

    ObjectGuid guid = ObjectGuid::Empty;
    std::string name = target;
    if (std::optional<uint32> const number = Acore::StringTo<uint32>(target))
        guid = ObjectGuid::Create<HighGuid::Player>(*number);
    else if (normalizePlayerName(name))
        guid = sCharacterCache->GetCharacterGuidByName(name);

    CharacterCacheEntry const* character = guid.IsEmpty() ? nullptr : sCharacterCache->GetCharacterCacheByGuid(guid);
    if (!character)
    {
        handler->PSendSysMessage("No character {}.", target);
        return false;
    }

    name = character->Name;
    // Looked up per command; the pointers are not kept
    Player* player = ObjectAccessor::FindConnectedPlayer(guid);
    PlayerbotAI* botAI = player ? GET_PLAYERBOT_AI(player) : nullptr;
    uint8 const race = player ? player->getRace(true) : character->Race;
    uint8 const cls = player ? player->getClass() : character->Class;
    uint32 const low = guid.GetCounter();
    if (!verb.empty() && !allowEdit)
    {
        handler->PSendSysMessage("Trait edits are off (Playerbots.WorldTravel.Traits.AllowEdit = 0; labs only).");
        return false;
    }

    if (!verb.empty() && player && !botAI)
    {
        handler->PSendSysMessage("{} is not a bot.", name);
        return false;
    }

    TraitRules::BotTraits traits;
    char const* source = "stored";
    if (verb.empty())
    {
        if (botAI)
            traits = mgr.LoadOrCreate(low, race, cls);
        else if (std::optional<TraitRules::BotTraits> const found = mgr.Find(low))
            traits = *found;
        else
        {
            traits = TraitRules::Roll(low, FirstVersion, race, cls);
            source = "rolled on the fly, not saved";
        }
    }
    else if (verb == "reroll")
        traits = mgr.Reroll(low, race, cls);
    else if (verb == "set")
    {
        TraitRules::Trait trait = TraitRules::Trait::Drive;
        std::optional<int32> const value = Acore::StringTo<int32>(valueText);
        if (!TraitRules::FindTrait(traitName, trait) || !value || *value < TraitRules::MIN_VALUE ||
            *value > TraitRules::MAX_VALUE)
        {
            std::string names;
            for (TraitRules::TraitWords const& words : TraitRules::WORDS)
                names += (names.empty() ? "" : " ") + std::string(words.Name);

            Usage(handler);
            handler->PSendSysMessage("Traits: {}", names);
            return false;
        }

        mgr.LoadOrCreate(low, race, cls);
        std::optional<TraitRules::BotTraits> const changed = mgr.SetTrait(low, trait, *value);
        if (!changed)
            return false;

        traits = *changed;
    }
    else
    {
        Usage(handler);
        return false;
    }

    handler->PSendSysMessage(
        "{} (guid {}, {} {}, {}, {}): traits version {}; ! noticeable (|value| >= {}), !! strong "
        "(>= {})",
        name, low, ChatHelper::FormatRace(race), ChatHelper::FormatClass(cls),
        botAI ? "online bot" : (player ? "online" : "offline"), source, traits.Version, TraitRules::NOTICEABLE,
        TraitRules::STRONG);
    std::string line;
    for (std::size_t i = 0; i < TraitRules::TRAIT_COUNT; ++i)
    {
        int32 const value = traits.Values[i];
        char const* mark = std::abs(value) >= TraitRules::STRONG       ? "!!"
                           : std::abs(value) >= TraitRules::NOTICEABLE ? "!"
                                                                       : "";
        line += Acore::StringFormat("{}{} {:+}{}", line.empty() ? "" : ", ", TraitRules::WORDS[i].Name, value, mark);
        if ((i + 1) % ValuesPerLine == 0 || i + 1 == TraitRules::TRAIT_COUNT)
        {
            handler->PSendSysMessage("{}", line);
            line.clear();
        }
    }

    handler->PSendSysMessage("As the LLM sees it: {}", LlmLine(traits));
    return true;
}
