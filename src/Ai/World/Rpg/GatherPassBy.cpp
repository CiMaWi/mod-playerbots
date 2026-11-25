/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "GatherPassBy.h"

#include <algorithm>
#include <array>
#include <variant>
#include <vector>

#include "Creature.h"
#include "GameObject.h"
#include "GatherNodeMgr.h"
#include "GatherRules.h"
#include "GoalInterface.h"
#include "LootObjectStack.h"
#include "MoveSpline.h"
#include "NewRpgInfo.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "ProfessionConfig.h"
#include "WorldLog.h"
#include "WorldLogFormat.h"

namespace GatherPassBy
{
namespace
{
constexpr float RISK_YARDS = 40.0f;  // the hostile creatures this near a target make its place's danger (as WR13b's)
constexpr float THERE_AND_BACK = 2.0f;

using Planner::GoalApply::BotGoals;

// The goals of a persistent bot whose newest plan brought gather rates; null: the bot is not gated
BotGoals* RatedGoals(PlayerbotAI* botAI)
{
    if (!botAI->KeepsPossessions())
        return nullptr;
    BotGoals* goals = botAI->planner.goals.get();
    return goals && goals->gatherRates ? goals : nullptr;
}

Gather::Gatherer GathererOf(Player* bot, Materials::Rates const& rates)
{
    Gather::Gatherer gatherer;
    for (uint16 const skill : {SKILL_HERBALISM, SKILL_MINING, SKILL_SKINNING})
        gatherer.skills.push_back({skill, bot->GetSkillValue(skill), bot->GetMaxSkillValue(skill)});
    gatherer.rates = rates;
    gatherer.rates.runSpeed = bot->GetSpeed(MOVE_RUN);
    gatherer.server = sGatherNodeMgr.Rates();
    return gatherer;
}

// The yields at the bot's prices: what its skill path pays, else a vendor's price
std::vector<Gather::Yield> YieldsOf(std::vector<GatherYield> const* yields, SkillPath::Path const* path)
{
    std::vector<Gather::Yield> priced;
    if (!yields)
        return priced;
    priced.reserve(yields->size());
    for (GatherYield const& yield : *yields)
    {
        SkillPath::Material const* material = path ? SkillPath::MaterialOf(*path, yield.item) : nullptr;
        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(yield.item);
        priced.push_back(
            {yield.item, yield.perCast, material ? material->copperEach : 0u, proto ? proto->SellPrice : 0u, 0u});
    }
    return priced;
}

// The live danger at a target: the hostile creatures within RISK_YARDS of it, the strongest one's level above the
// bot's (a mean let a few low creatures around hide a strong one beside the node) and their elite share (the bot's
// cached nearest hostile npcs, no search of its own)
Materials::PlaceRisk LiveRisk(PlayerbotAI* botAI, Player* bot, WorldObject const* at)
{
    Materials::PlaceRisk risk;
    uint32 hostiles = 0;
    uint32 elites = 0;
    uint32 strongest = 0;
    GuidVector const npcs = botAI->GetAiObjectContext()->GetValue<GuidVector>("nearest hostile npcs")->Get();
    for (ObjectGuid const guid : npcs)
    {
        Unit* unit = botAI->GetUnit(guid);
        if (!unit || unit == at || !unit->IsAlive() || !unit->IsHostileTo(bot) || at->GetExactDist(unit) > RISK_YARDS)
            continue;
        ++hostiles;
        strongest = std::max<uint32>(strongest, unit->GetLevel());
        if (Creature* creature = unit->ToCreature(); creature && creature->isElite())
            ++elites;
    }
    if (!hostiles)
        return risk;
    risk.levelsAbove = std::max(static_cast<float>(strongest) - static_cast<float>(bot->GetLevel()), 0.0f);
    risk.eliteShare = static_cast<float>(elites) / static_cast<float>(hostiles);
    return risk;
}

// The yards a target adds to the bot's way: to it and on to where the bot moves, less the way it goes; a bot that
// stands goes there and back
float DetourYards(Player* bot, WorldObject const* target)
{
    float const there = bot->GetExactDist(target);
    if (bot->movespline && !bot->movespline->Finalized())
    {
        G3D::Vector3 const end = bot->movespline->FinalDestination();
        float const direct = bot->GetExactDist(end.x, end.y, end.z);
        float const after = target->GetExactDist(end.x, end.y, end.z);
        return std::max(there + after - direct, 0.0f);
    }
    return there * THERE_AND_BACK;
}

Gather::Verdict JudgeCorpse(PlayerbotAI* botAI, Player* bot, BotGoals const& goals, Creature* creature, bool inCombat)
{
    Gather::Hide hide;
    hide.creatureLevel = creature->GetLevel();
    hide.yields = YieldsOf(sGatherNodeMgr.HideYields(creature->GetCreatureTemplate()->SkinLootId), goals.path.get());
    hide.risk = LiveRisk(botAI, bot, creature);
    return Gather::JudgeHide(hide, GathererOf(bot, *goals.gatherRates), inCombat, bot->GetFreeInventorySpace());
}

// A corpse of the current creature stop's kind (WR20c) is the circuit's: taken without a judgement, written by the
// stop's row, not as a take on the way
bool CircuitCorpse(PlayerbotAI* botAI, Creature const* creature)
{
    auto const* gather = std::get_if<NewRpgInfo::DoGather>(&botAI->rpgInfo.data);
    return gather && gather->creatureEntry && creature->GetEntry() == gather->creatureEntry &&
           (gather->targetGuid.IsEmpty() || gather->targetGuid == creature->GetGUID());
}
}  // namespace

bool Admit(PlayerbotAI* botAI, ObjectGuid guid, float& reach)
{
    reach = 0.0f;
    BotGoals* goals = RatedGoals(botAI);
    if (!goals)
        return true;
    Player* bot = botAI->GetBot();
    if (guid.IsGameObject())
    {
        GameObject* go = botAI->GetGameObject(guid);
        if (!go)
            return true;
        // A node's skill comes with its lock requirement, which IsLootPossible reads (the stack's own check)
        LootObject loot(bot, guid);
        if (loot.IsEmpty() || !loot.IsLootPossible(bot) ||
            (loot.skillId != SKILL_HERBALISM && loot.skillId != SKILL_MINING))
            return true;
        // A planned circuit's stop is the circuit's (WR13b)
        if (auto const* gather = std::get_if<NewRpgInfo::DoGather>(&botAI->rpgInfo.data);
            gather && gather->nodeSpawnId && go->GetSpawnId() == gather->nodeSpawnId)
            return true;
        Gather::Seen seen;
        seen.node.id = go->GetSpawnId();
        seen.node.skill = static_cast<uint16>(loot.skillId);
        seen.node.required = loot.reqSkillValue;
        seen.node.at = {go->GetPositionX(), go->GetPositionY(), go->GetPositionZ()};
        seen.node.risk = LiveRisk(botAI, bot, go);
        seen.node.yields = YieldsOf(sGatherNodeMgr.NodeYields(go->GetEntry()), goals->path.get());
        seen.detourYards = DetourYards(bot, go);
        Gather::Verdict const verdict = Gather::JudgeNode(seen, GathererOf(bot, *goals->gatherRates));
        goals->passes.Judge(guid.GetRawValue(), verdict, seen.detourYards);
        if (!verdict.take)
            return false;
        reach = sPlayerbotAIConfig.SightDistance;
        return true;
    }
    // A corpse with loot is looted as before; a looted one that can be skinned is judged
    Creature* creature = botAI->GetCreature(guid);
    if (!creature || creature->IsAlive() || creature->HasFlag(UNIT_DYNAMIC_FLAGS, UNIT_DYNFLAG_LOOTABLE) ||
        !creature->HasFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_SKINNABLE) ||
        creature->GetCreatureTemplate()->GetRequiredLootSkill() != SKILL_SKINNING || CircuitCorpse(botAI, creature))
        return true;
    Gather::Verdict const verdict = JudgeCorpse(botAI, bot, *goals, creature, bot->IsInCombat());
    goals->passes.Judge(guid.GetRawValue(), verdict, 0.0f);
    return verdict.take;
}

void NoteTake(PlayerbotAI* botAI, ObjectGuid guid, bool skin)
{
    BotGoals* goals = RatedGoals(botAI);
    Gather::PassEntry const* entry = goals ? goals->passes.Take(guid.GetRawValue()) : nullptr;
    if (!entry)
        return;
    using namespace Professions;
    uint32 spawnId = 0;
    if (skin)
    {
        if (Creature* creature = botAI->GetCreature(guid))
            spawnId = creature->GetSpawnId();
    }
    else if (GameObject* go = botAI->GetGameObject(guid))
        spawnId = go->GetSpawnId();
    // The take (WR13c): its net, the target's entry (skin: the creature's) and the detour; ref the spawn (0: none)
    WorldLog::Decision(botAI->GetBot(), WorldLogFormat::DecisionKind::Profession, ChoiceWord(Choice::Gather),
                       ReasonWord(Reason::OnTheWay), static_cast<float>(entry->net),
                       {{skin ? "skin" : "entry", static_cast<float>(guid.GetEntry())}, {"detour", entry->detourYards}},
                       spawnId);
}

bool LootsEmpty(PlayerbotAI* botAI, ObjectGuid corpse)
{
    BotGoals* goals = RatedGoals(botAI);
    Creature* creature = goals && corpse.IsCreatureOrVehicle() ? botAI->GetCreature(corpse) : nullptr;
    if (!creature || !creature->GetCreatureTemplate()->SkinLootId ||
        creature->GetCreatureTemplate()->GetRequiredLootSkill() != SKILL_SKINNING)
        return false;
    if (CircuitCorpse(botAI, creature))
        return botAI->HasSkill(SKILL_SKINNING);  // emptied, so the circuit's skinner can skin it
    // Judged as after the fight: a corpse looted in part can never be skinned, so a hide left here is passed
    Gather::Verdict const verdict = JudgeCorpse(botAI, botAI->GetBot(), *goals, creature, false);
    goals->passes.Judge(corpse.GetRawValue(), verdict, 0.0f);
    return verdict.take;
}

bool Skins(PlayerbotAI* botAI, ObjectGuid corpse)
{
    BotGoals* goals = RatedGoals(botAI);
    Creature* creature = goals ? botAI->GetCreature(corpse) : nullptr;
    if (!creature || creature->GetCreatureTemplate()->GetRequiredLootSkill() != SKILL_SKINNING ||
        CircuitCorpse(botAI, creature))
        return true;
    // A corpse kept on the stack from its loot reaches the cast unjudged: the rules decide here too
    Player* bot = botAI->GetBot();
    Gather::Verdict const verdict = JudgeCorpse(botAI, bot, *goals, creature, bot->IsInCombat());
    goals->passes.Judge(corpse.GetRawValue(), verdict, 0.0f);
    return verdict.take;
}

void NoteStatusEnd(PlayerbotAI* botAI, std::uint32_t oldStatus)
{
    if (!botAI->KeepsPossessions() || !botAI->planner.goals)
        return;
    std::array<std::uint32_t, Gather::PASS_CAUSES> const passes = botAI->planner.goals->passes.Flush();
    uint32 total = 0;
    for (std::uint32_t const count : passes)
        total += count;
    if (!total)
        return;
    using namespace Professions;
    auto const of = [&passes](Gather::PassCause cause)
    { return static_cast<float>(passes[static_cast<std::size_t>(cause)]); };
    // The status's passes (WR13c): their number, by cause; ref the ended status
    WorldLog::Decision(botAI->GetBot(), WorldLogFormat::DecisionKind::Profession, ChoiceWord(Choice::Skip),
                       ReasonWord(Reason::OnTheWay), static_cast<float>(total),
                       {{ReasonWord(Reason::DetourTooLong), of(Gather::PassCause::DetourTooLong)},
                        {ReasonWord(Reason::TooDangerous), of(Gather::PassCause::TooDangerous)},
                        {ReasonWord(Reason::NotWorthIt), of(Gather::PassCause::NotWorthIt)}},
                       oldStatus);
}
}  // namespace GatherPassBy
