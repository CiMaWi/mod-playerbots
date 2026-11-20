/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "BotProfessionMgr.h"

#include <iterator>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

#include "BotKind.h"
#include "BotRowStore.h"
#include "BotTraitMgr.h"
#include "CensusMgr.h"
#include "CharacterCache.h"
#include "Chat.h"
#include "ChatHelper.h"
#include "DatabaseEnv.h"
#include "GatherLive.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "PlannerRecipes.h"
#include "PlannerService.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "PlayerbotsDatabase.h"
#include "ProfessionConfig.h"
#include "ProfessionVisit.h"
#include "Random.h"
#include "SharedDefines.h"
#include "SkillPathLive.h"
#include "StringConvert.h"
#include "StringFormat.h"
#include "ThreadCheck.h"
#include "WorldSession.h"

using ProfessionRules::BotProfessions;
using ProfessionRules::Skill;

namespace
{
constexpr uint32 FirstVersion = 1;
constexpr uint32 DefaultSample = 10;
constexpr uint32 MaxSample = 50;
constexpr uint32 RaceClassTries = 1000;
constexpr uint32 SelftestGuid = 4242;
constexpr uint32 SelftestBotsPerClass = 1000;
constexpr char const* Columns = "`guid`, `version`, `origin`, `main1`, `main2`, `first_aid`, `fishing`, `cooking`";

constexpr uint8 PlayableRaces[] = {RACE_HUMAN,  RACE_ORC,   RACE_DWARF, RACE_NIGHTELF, RACE_UNDEAD_PLAYER,
                                   RACE_TAUREN, RACE_GNOME, RACE_TROLL, RACE_BLOODELF, RACE_DRAENEI};
constexpr uint8 PlayableClasses[] = {CLASS_WARRIOR,      CLASS_PALADIN, CLASS_HUNTER, CLASS_ROGUE,   CLASS_PRIEST,
                                     CLASS_DEATH_KNIGHT, CLASS_SHAMAN,  CLASS_MAGE,   CLASS_WARLOCK, CLASS_DRUID};

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

// The fitted chances of the loaded config (Playerbots.Professions.Share.* and Bias.*; ProfessionConfig.cpp)
ProfessionRules::ChanceTable const& Chances() { return sPlayerbotAIConfig.Professions.Chance; }

// The bot's traits (the roll at login stored them); the first roll when it has none yet
TraitRules::BotTraits TraitsOf(uint32 guid, uint8 race, uint8 cls)
{
    if (std::optional<TraitRules::BotTraits> const traits = BotTraitMgr::instance().Find(guid))
        return *traits;

    return TraitRules::Roll(guid, FirstVersion, race, cls);
}

std::vector<Skill> HeldSkills(Player* bot)
{
    std::vector<Skill> held;
    for (ProfessionRules::ProfessionInfo const& info : ProfessionRules::PROFESSIONS)
        if (bot->HasSkill(static_cast<uint32>(info.Which)))
            held.push_back(info.Which);

    return held;
}

// What the rules give the bot: its held professions (WR.7), else the first roll; nothing is stored
BotProfessions RulesAnswer(Player* bot)
{
    uint32 const guid = bot->GetGUID().GetCounter();
    uint8 const cls = bot->getClass();
    BotProfessions const rolled =
        ProfessionRules::Roll(guid, FirstVersion, cls, TraitsOf(guid, bot->getRace(true), cls), Chances());
    return ProfessionRules::Adopt(rolled, HeldSkills(bot));
}

bool Selftest(ChatHandler* handler)
{
    uint32 checks = 0;
    uint32 failures = 0;
    auto const check = [&](bool ok, std::string const& what)
    {
        ++checks;
        if (ok)
            return;

        ++failures;
        handler->PSendSysMessage("PROFESSIONS selftest failed: {}", what);
    };

    TraitRules::BotTraits const traits = TraitRules::Roll(SelftestGuid, FirstVersion, RACE_HUMAN, CLASS_WARRIOR);
    BotProfessions const first = ProfessionRules::Roll(SelftestGuid, FirstVersion, CLASS_WARRIOR, traits, Chances());
    BotProfessions const again = ProfessionRules::Roll(SelftestGuid, FirstVersion, CLASS_WARRIOR, traits, Chances());
    check(first.Mains == again.Mains && first.Secondary == again.Secondary, "the same guid and version roll alike");

    uint32 oneMain = 0;
    for (uint8 const cls : PlayableClasses)
        for (uint32 guid = 1; guid <= SelftestBotsPerClass; ++guid)
        {
            uint8 const race = PlayableRaces[guid % std::size(PlayableRaces)];
            BotProfessions const person = ProfessionRules::Roll(
                guid, FirstVersion, cls, TraitRules::Roll(guid, FirstVersion, race, cls), Chances());
            std::string const violation = ProfessionRules::Violation(person, cls);
            std::size_t const mains = ProfessionRules::MainCount(person);
            if (!violation.empty() || !mains)
                check(false, Acore::StringFormat("class {} guid {}: {}", cls, guid,
                                                 violation.empty() ? "no main profession" : violation));

            oneMain += mains == 1 ? 1 : 0;
        }

    BotProfessions const held = ProfessionRules::Adopt(first, {Skill::Tailoring, Skill::Skinning});
    check(held.Mains[0] == Skill::Skinning && held.Mains[1] == Skill::Tailoring &&
              held.From == ProfessionRules::Origin::Held,
          "a warrior's held tailoring and skinning become its person");
    uint32 const bots = SelftestBotsPerClass * static_cast<uint32>(std::size(PlayableClasses));
    handler->PSendSysMessage("PROFESSIONS selftest: {} checks, {} failures ({}); {:.1f} % of {} bots with one main",
                             checks, failures, failures ? "FAIL" : "PASS", 100.0 * oneMain / bots, bots);
    return !failures;
}

void Usage(ChatHandler* handler)
{
    handler->PSendSysMessage(
        "Usage: .playerbots professions [<bot name|guid> [set <profession>... | reroll] | sample <1-{}> | index "
        "[<profession>] [dump [<file>]] | stations [<focus>] | visit <bot> | path <bot> | nodes <bot> | selftest]",
        MaxSample);
}

std::string Words()
{
    std::string words;
    for (ProfessionRules::ProfessionInfo const& info : ProfessionRules::PROFESSIONS)
        words += (words.empty() ? "" : " ") + std::string(info.Name);

    return words;
}

// The store of the persons (BotRowStore.h): the forget's name and a row's asynchronous save
struct ProfessionRowPolicy
{
    static char const* Name() { return "professions"; }

    static void Save(uint32 guid, BotProfessions const& person);
};

using ProfessionStore = BotRowStore<BotProfessions, ProfessionRowPolicy>;

ProfessionStore& Rows() { return ProfessionStore::Instance(); }
}  // namespace

void BotProfessionMgr::Load()
{
    // One query for all bots (startup, world thread), so a bot login reads only the store. The table comes with update
    // 2026_11_18_07, which the startup check requires (PlayerbotsRequiredTables.h)
    if (QueryResult result =
            PlayerbotsDatabase.Query(Acore::StringFormat("SELECT {} FROM playerbots_bot_professions", Columns)))
    {
        do
        {
            Field* fields = result->Fetch();
            BotProfessions person;
            person.Version = fields[1].Get<uint32>();
            uint8 const origin = fields[2].Get<uint8>();
            person.From = origin <= static_cast<uint8>(ProfessionRules::Origin::Chosen)
                              ? static_cast<ProfessionRules::Origin>(origin)
                              : ProfessionRules::Origin::Rolled;
            for (std::size_t i = 0; i < ProfessionRules::MAX_MAINS; ++i)
            {
                Skill const main = static_cast<Skill>(fields[3 + i].Get<uint16>());
                person.Mains[i] = ProfessionRules::IsMain(main) ? main : Skill::None;
            }

            person.Secondary = (fields[5].Get<uint8>() ? ProfessionRules::SECONDARY_FIRST_AID : 0) |
                               (fields[6].Get<uint8>() ? ProfessionRules::SECONDARY_FISHING : 0) |
                               (fields[7].Get<uint8>() ? ProfessionRules::SECONDARY_COOKING : 0);
            Rows().LoadRow(fields[0].Get<uint32>(), person);
        } while (result->NextRow());
    }

    LOG_INFO("playerbots", "Bot professions loaded: {} bots", Rows().Size());
    Rows().RegisterForget();
}

void ProfessionRowPolicy::Save(uint32 guid, BotProfessions const& person)
{
    PlayerbotsDatabase.Execute(Acore::StringFormat(
        "INSERT INTO playerbots_bot_professions ({8}) VALUES ({0}, {1}, {2}, {3}, {4}, {5}, {6}, {7}) ON DUPLICATE KEY "
        "UPDATE `version` = {1}, `origin` = {2}, `main1` = {3}, `main2` = {4}, `first_aid` = {5}, `fishing` = {6}, "
        "`cooking` = {7}",
        guid, person.Version, static_cast<uint32>(person.From), static_cast<uint32>(person.Mains[0]),
        static_cast<uint32>(person.Mains[1]), ProfessionRules::Has(person, Skill::FirstAid) ? 1 : 0,
        ProfessionRules::Has(person, Skill::Fishing) ? 1 : 0, ProfessionRules::Has(person, Skill::Cooking) ? 1 : 0,
        Columns));
}

bool BotProfessionMgr::ChooseSecond(Player* bot, Skill craft, Skill gatherer)
{
    uint32 const guid = bot->GetGUID().GetCounter();
    std::optional<BotProfessions> const stored = Rows().Find(guid);
    if (!stored || stored->Mains[1] != craft || !ProfessionRules::Is(gatherer, ProfessionRules::Kind::Gatherer) ||
        ProfessionRules::HasMain(*stored, gatherer) || bot->HasSkill(static_cast<uint32>(craft)) ||
        bot->HasSkill(static_cast<uint32>(gatherer)))
        return false;

    Rows().Update(guid,
                  [gatherer](BotProfessions& person)
                  {
                      person.Mains[1] = gatherer;
                      person.From = ProfessionRules::Origin::Chosen;
                  });
    LOG_DEBUG("playerbots", "[Professions] {} takes {} in place of {} (short of gold)", bot->GetName(),
              ProfessionRules::Name(gatherer), ProfessionRules::Name(craft));
    return true;
}

BotProfessions BotProfessionMgr::LoadOrCreate(Player* bot)
{
    uint32 const guid = bot->GetGUID().GetCounter();
    if (std::optional<BotProfessions> const stored = Rows().Find(guid))
        return *stored;

    // The rules' answer reads the bot outside the store's lock; a row stored meanwhile wins
    BotProfessions const person = RulesAnswer(bot);
    return Rows().GetOrCreate(guid, [&person] { return person; });
}

std::optional<BotProfessions> BotProfessionMgr::Find(uint32 guid) { return Rows().Find(guid); }

BotProfessions BotProfessionMgr::PersonOf(Player* bot)
{
    if (BotKind::KeepsPossessions(bot))
        return LoadOrCreate(bot);

    if (std::optional<BotProfessions> const stored = Find(bot->GetGUID().GetCounter()))
        return *stored;

    return RulesAnswer(bot);
}

void BotProfessionMgr::EmitHeld(Player* bot, char const* source)
{
    for (ProfessionRules::ProfessionInfo const& info : ProfessionRules::PROFESSIONS)
    {
        uint32 const skillId = static_cast<uint32>(info.Which);
        if (bot->HasSkill(skillId))
            EventLog::Emit(bot, Professions::EVENT_PROFESSION_HELD, source, skillId, bot->GetPureSkillValue(skillId));
    }
}

void BotProfessionMgr::NoteSkillChange(Player* bot, uint32 skillId, uint32 oldValue, uint32 newValue)
{
    if ((oldValue && newValue) || oldValue == newValue || skillId > std::numeric_limits<uint16>::max() ||
        !ProfessionRules::Find(static_cast<Skill>(skillId)))
        return;

    // Only a bot with a person (BotKind::KeepsPossessions) writes its professions to the world log
    PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
    if (!botAI || !botAI->KeepsPossessions())
        return;

    EventLog::Emit(bot, Professions::EVENT_PROFESSION_HELD, newValue ? "learned" : "lost", skillId, newValue);
}

bool BotProfessionMgr::HandleProfessionsCommand(ChatHandler* handler, char const* args)
{
    std::istringstream in(args ? args : "");
    std::vector<std::string> words;
    for (std::string word; in >> word;)
        words.push_back(word);

    std::string const target = words.empty() ? "" : words[0];
    std::string const verb = words.size() > 1 ? words[1] : "";
    if (target == "selftest")
        return Selftest(handler);

    if (target == "index")  // read-only; the dump writes a file in the logs folder
        return Planner::HandleRecipeIndexCommand(handler, std::vector<std::string>(words.begin() + 1, words.end()));

    if (target == "stations")  // read-only: the spell focus objects (forge, anvil, cooking fire ...) by focus id
        return Planner::HandleStationCommand(handler, std::vector<std::string>(words.begin() + 1, words.end()));

    if (target == "path")  // read-only: the skill path computed now beside the kept one (WR11)
        return Planner::HandlePathCommand(handler, verb);

    if (target == "nodes")  // read-only: the gather rules on the bot's zone now (WR13a)
        return Planner::HandleNodesCommand(handler, verb);

    if (target == "visit")  // a GM test: the profession trainer stop's handler on the trainer in reach
    {
        WorldSession* const session = handler->GetSession();
        if (session && session->GetSecurity() < SEC_GAMEMASTER)
        {
            handler->PSendSysMessage("A profession visit needs a game master.");
            return false;
        }

        std::string name = verb;
        Player* bot = !name.empty() && normalizePlayerName(name) ? ObjectAccessor::FindPlayerByName(name) : nullptr;
        if (!bot || !bot->IsInWorld())
        {
            handler->SendErrorMessage("Profession visit: no bot {} online (usage: playerbots professions visit <bot>)",
                                      verb);
            return false;
        }

        std::string report;
        bool const done = ProfessionVisit::Run(bot, report);
        if (!done)
            handler->SendErrorMessage("Profession visit {}: refused, {}", bot->GetName(), report);
        else
            handler->PSendSysMessage("Profession visit {}: {}", bot->GetName(), report);
        return done;
    }

    if (target == "sample")
    {
        std::optional<uint32> const count =
            verb.empty() ? std::optional<uint32>(DefaultSample) : Acore::StringTo<uint32>(verb);
        if (!count || *count < 1 || *count > MaxSample)
        {
            Usage(handler);
            return false;
        }

        handler->PSendSysMessage("Sample of {} rolled persons (random guids, first roll; held professions not read):",
                                 *count);
        for (uint32 i = 0; i < *count; ++i)
        {
            uint8 race = 0;
            uint8 cls = 0;
            RandomRaceClass(race, cls);
            uint32 const guid = urand(1, std::numeric_limits<int32>::max());
            TraitRules::BotTraits const traits = TraitRules::Roll(guid, FirstVersion, race, cls);
            handler->PSendSysMessage(
                "{} {} | drive {} | {}", ChatHelper::FormatRace(race), ChatHelper::FormatClass(cls),
                static_cast<int32>(traits.Values[TraitRules::Index(TraitRules::Trait::Drive)]),
                ProfessionRules::Describe(ProfessionRules::Roll(guid, FirstVersion, cls, traits, Chances())));
        }

        return true;
    }

    BotProfessionMgr& mgr = instance();
    // The edits of a bot's person share the lab-only switch of the trait edits; main never sets it
    bool const allowEdit = sPlayerbotAIConfig.WorldTravel.TraitsAllowEdit;
    if (target.empty())
    {
        std::size_t const count = Rows().Size();
        handler->PSendSysMessage("Bot professions: {} bots, edits {} (Playerbots.WorldTravel.Traits.AllowEdit)", count,
                                 allowEdit ? "on" : "off");
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
    if (!verb.empty())
    {
        // Edits are a cheat: a game master's, on a lab
        WorldSession* session = handler->GetSession();
        if (session && session->GetSecurity() < SEC_GAMEMASTER)
        {
            handler->PSendSysMessage("Profession edits need a game master.");
            return false;
        }

        if (!allowEdit)
        {
            handler->PSendSysMessage(
                "Profession edits are off (Playerbots.WorldTravel.Traits.AllowEdit = 0; labs only).");
            return false;
        }

        // An offline character is a bot when its account is a random-bot account or it is an addclass bot
        bool const offlineBot = !player && (sPlayerbotAIConfig.IsInRandomAccountList(character->AccountId) ||
                                            sRandomPlayerbotMgr.IsAddclassBot(low));
        if (!botAI && !offlineBot)
        {
            handler->PSendSysMessage("{} is not a bot.", name);
            return false;
        }
    }

    BotProfessions person;
    char const* source = "stored";
    std::optional<BotProfessions> const stored = mgr.Find(low);
    if (verb.empty())
    {
        // Read-only: a show never creates or saves a person
        if (stored)
            person = *stored;
        else if (player)
        {
            person = RulesAnswer(player);
            source = botAI && botAI->KeepsPossessions()
                         ? "not stored yet (stored at its login); the rules' answer"
                         : "no person (not a persistent bot without a player master); the rules' answer, not saved";
        }
        else
        {
            person = ProfessionRules::Roll(low, FirstVersion, cls, TraitsOf(low, race, cls), Chances());
            source = "rolled on the fly, not saved; held professions are read at login";
        }
    }
    else if (verb == "reroll")
    {
        uint32 const version = (stored ? stored->Version : FirstVersion) + 1;
        person = ProfessionRules::Roll(low, version, cls, TraitsOf(low, race, cls), Chances());
        Rows().Replace(low, [&person](BotProfessions const*) { return person; });
    }
    else if (verb == "set")
    {
        person = BotProfessions();
        person.Version = stored ? stored->Version : FirstVersion;
        person.From = ProfessionRules::Origin::Set;
        std::size_t mains = 0;
        bool valid = words.size() > 2;
        for (std::size_t i = 2; i < words.size() && valid; ++i)
        {
            Skill skill = Skill::None;
            valid = ProfessionRules::FindByName(words[i], skill) && !ProfessionRules::Has(person, skill);
            if (!valid)
                break;

            if (uint8 const bit = ProfessionRules::SecondaryBit(skill))
                person.Secondary |= bit;
            else if (mains < ProfessionRules::MAX_MAINS)
                person.Mains[mains++] = skill;
            else
                valid = false;
        }

        if (!valid)
        {
            Usage(handler);
            handler->PSendSysMessage("At most {} different main professions; words: {}", ProfessionRules::MAX_MAINS,
                                     Words());
            return false;
        }

        Rows().Replace(low, [&person](BotProfessions const*) { return person; });
    }
    else
    {
        Usage(handler);
        return false;
    }

    handler->PSendSysMessage("{} (guid {}, {} {}, {}, {}): professions version {}, {}: {}", name, low,
                             ChatHelper::FormatRace(race), ChatHelper::FormatClass(cls),
                             botAI ? "online bot" : (player ? "online" : "offline"), source, person.Version,
                             ProfessionRules::OriginWord(person.From), ProfessionRules::Describe(person));
    if (!verb.empty())
        handler->PSendSysMessage("The factory grants by the new person at its next run; held skills stay.");

    if (player)
    {
        std::string held;
        for (ProfessionRules::ProfessionInfo const& info : ProfessionRules::PROFESSIONS)
        {
            uint32 const skillId = static_cast<uint32>(info.Which);
            if (player->HasSkill(skillId))
                held += Acore::StringFormat("{}{} {}/{}", held.empty() ? "" : ", ", info.Name,
                                            player->GetPureSkillValue(skillId), player->GetPureMaxSkillValue(skillId));
        }

        handler->PSendSysMessage("Held: {}", held.empty() ? "none" : held);
    }

    return true;
}
