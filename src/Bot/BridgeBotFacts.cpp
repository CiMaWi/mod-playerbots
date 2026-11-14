/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "BridgeBotFacts.h"

#include <algorithm>
#include <thread>

#include "AiFactory.h"
#include "BridgeBotFactsRules.h"
#include "DBCStores.h"
#include "Guild.h"
#include "GuildMgr.h"
#include "ObjectAccessor.h"
#include "ObjectGuid.h"
#include "PerfMonitor.h"
#include "Pet.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "ReputationMgr.h"
#include "ThreadCheck.h"

namespace PlayerbotBridge
{
namespace
{
void AddServant(std::vector<std::string>& servants, std::string const& name)
{
    if (!name.empty() && std::find(servants.begin(), servants.end(), name) == servants.end())
        servants.push_back(name);
}

void ReadServants(Player* bot, std::vector<std::string>& servants)
{
    // Out now: the pet and the guardian beside it
    if (Pet* pet = bot->GetPet())
        AddServant(servants, pet->GetName());
    if (Guardian* guardian = bot->GetGuardianPet())
        AddServant(servants, guardian->GetName());

    // A hunter's beasts: the one it calls and the stabled ones
    if (PetStable const* stable = bot->GetPetStable())
    {
        if (stable->CurrentPet)
            AddServant(servants, stable->CurrentPet->Name);
        for (Optional<PetStable::PetInfo> const& stabled : stable->StabledPets)
            if (stabled)
                AddServant(servants, stabled->Name);
        for (PetStable::PetInfo const& unslotted : stable->UnslottedPets)
            AddServant(servants, unslotted.Name);
    }

    // The rest are called by a spell the bot knows: a warlock's demons, a mage's elemental, a death knight's ghoul
    for (FactsRules::Servant const& servant : FactsRules::SERVANTS)
        if (servant.Class == bot->getClass() && bot->HasSpell(servant.Spell))
            AddServant(servants, std::string(servant.Name));
}

void ReadStanding(Player* bot, std::vector<std::pair<std::string, std::string>>& standing)
{
    ReputationMgr const& reputation = bot->GetReputationMgr();
    struct Marked
    {
        int distance;
        std::string name;
        std::string word;
    };
    std::vector<Marked> marked;
    for (auto const& [id, state] : reputation.GetStateList())
    {
        if (!(state.Flags & FACTION_FLAG_VISIBLE))
            continue;

        FactionEntry const* entry = sFactionStore.LookupEntry(state.ID);
        if (!entry)
            continue;

        std::size_t const rank = static_cast<std::size_t>(reputation.GetRank(entry));
        if (rank == FactsRules::NEUTRAL_STANDING || rank >= FactsRules::STANDINGS)
            continue;

        int const distance = rank > FactsRules::NEUTRAL_STANDING
                                 ? static_cast<int>(rank - FactsRules::NEUTRAL_STANDING)
                                 : static_cast<int>(FactsRules::NEUTRAL_STANDING - rank);
        marked.push_back({distance, entry->name[0], std::string(FactsRules::StandingWord(rank))});
    }

    std::sort(marked.begin(), marked.end(), [](Marked const& a, Marked const& b)
              { return a.distance != b.distance ? a.distance > b.distance : a.name < b.name; });
    for (Marked const& m : marked)
    {
        if (standing.size() >= FactsRules::MAX_STANDINGS)
            break;
        standing.emplace_back(m.name, m.word);
    }
}
}  // namespace

bool ReadBotFacts(ObjectGuid const& botGuid, BotFacts& facts)
{
    facts = {};
    // Map threads change this data; only the world thread reads it while the maps are idle.
    if (std::this_thread::get_id() != PlayerbotThreadCheck::worldThread.load(std::memory_order_relaxed))
        return false;
    Player* bot = ObjectAccessor::FindConnectedPlayer(botGuid);
    PlayerbotAI* ai = bot ? GET_PLAYERBOT_AI(bot) : nullptr;
    if (!bot || !bot->IsInWorld() || !ai || !ai->IsBotAI())
        return false;

    PerfMonitorOperation* pmo = sPerfMonitor.start(PERF_MON_TOTAL, "Read bot facts");
    facts.phrase = FactsRules::Phrase(bot->getClass(), AiFactory::GetPlayerSpecTab(bot));

    for (FactsRules::Craft const& craft : FactsRules::CRAFTS)
    {
        if (!bot->HasSkill(craft.Skill))
            continue;

        facts.crafts.emplace_back(std::string(craft.Name),
                                  std::string(FactsRules::RankWord(bot->GetPureSkillValue(craft.Skill))));
        facts.useful.emplace_back(craft.Useful);
    }

    ReadServants(bot, facts.servants);
    ReadStanding(bot, facts.standing);
    if (bot->GetGuildId())
        if (Guild const* guild = sGuildMgr->GetGuildById(bot->GetGuildId()))
            facts.guild = guild->GetName();

    if (pmo)
        pmo->finish();
    return true;
}
}  // namespace PlayerbotBridge
