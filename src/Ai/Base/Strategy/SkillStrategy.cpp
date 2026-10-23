/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "SkillStrategy.h"

#include <string>

#include "Playerbots.h"
#include "Timer.h"

namespace
{
constexpr uint64 PhaseSalt = 0x3C6EF372FE94F82BULL;
constexpr uint64 NoticeSalt = 0xA54FF53A5F1D36F1ULL;

static_assert(static_cast<size_t>(SkillNoticeCategory::TargetSwitch) == SkillNoticeCounters::Categories,
              "SkillNoticeCounters needs one slot per notice category");

// splitmix64, as in BotProfileMgr: a fixed integer mix, the same on every build
uint64 SplitMix64(uint64 state)
{
    state += 0x9E3779B97F4A7C15ULL;
    uint64 z = state;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

// Built once, read only afterwards (C++11 guarantees a thread-safe initialisation). The table also lists the
// alternatives of its actions (ActionNode /*A*/), so a blocked action does not fall through to a twin under another
// name: kick -> kidney shot, hibernate -> entangling roots, survival instincts -> barkskin, last stand -> intimidating
// shout, divine shield -> divine protection, repentance -> hammer of justice, the cure/purify/remove lesser curse
// fallbacks of the dispels.
std::unordered_map<std::string, SkillNoticeCategory> const& CategoryTable()
{
    using C = SkillNoticeCategory;
    static std::unordered_map<std::string, SkillNoticeCategory> const table = {
        // Interrupts (bash and kidney shot are the druid's and the combat rogue's interrupt stand-ins)
        {"kick", C::Interrupt},
        {"kick on enemy healer", C::Interrupt},
        {"kidney shot", C::Interrupt},
        {"pummel", C::Interrupt},
        {"pummel on enemy healer", C::Interrupt},
        {"shield bash", C::Interrupt},
        {"shield bash on enemy healer", C::Interrupt},
        {"wind shear", C::Interrupt},
        {"wind shear on enemy healer", C::Interrupt},
        {"mind freeze", C::Interrupt},
        {"mind freeze on enemy healer", C::Interrupt},
        {"strangulate", C::Interrupt},
        {"strangulate on enemy healer", C::Interrupt},
        {"spell lock", C::Interrupt},
        {"silence", C::Interrupt},
        {"silence on enemy healer", C::Interrupt},
        {"silencing shot", C::Interrupt},
        {"counterspell", C::Interrupt},
        {"counterspell on enemy healer", C::Interrupt},
        {"bash", C::Interrupt},
        {"bash on enemy healer", C::Interrupt},
        // Defensive cooldowns
        {"ice block", C::Defensive},
        {"divine shield", C::Defensive},
        {"divine protection", C::Defensive},
        {"divine protection on party", C::Defensive},
        {"evasion", C::Defensive},
        {"cloak of shadows", C::Defensive},
        {"vanish", C::Defensive},
        {"barkskin", C::Defensive},
        {"survival instincts", C::Defensive},
        {"shield wall", C::Defensive},
        {"last stand", C::Defensive},
        {"intimidating shout", C::Defensive},
        {"spell reflection", C::Defensive},
        {"icebound fortitude", C::Defensive},
        {"deterrence", C::Defensive},
        {"pain suppression", C::Defensive},
        {"pain suppression on party", C::Defensive},
        {"guardian spirit on party", C::Defensive},
        {"hand of freedom on party", C::Defensive},
        {"dispersion", C::Defensive},
        {"blink back", C::Defensive},
        {"stoneform", C::Defensive},
        {"set grounding totem", C::Defensive},
        {"set tremor totem", C::Defensive},
        {"tremor totem", C::Defensive},
        // Dispels
        {"dispel magic", C::Dispel},
        {"dispel magic on party", C::Dispel},
        {"dispel magic on target", C::Dispel},
        {"mass dispel", C::Dispel},
        {"purge", C::Dispel},
        {"devour magic purge", C::Dispel},
        {"devour magic cleanse", C::Dispel},
        {"tranquilizing shot", C::Dispel},
        {"cleanse magic", C::Dispel},
        {"cleanse magic on party", C::Dispel},
        {"cleanse poison", C::Dispel},
        {"cleanse poison on party", C::Dispel},
        {"cleanse disease", C::Dispel},
        {"cleanse disease on party", C::Dispel},
        {"purify poison", C::Dispel},
        {"purify poison on party", C::Dispel},
        {"purify disease", C::Dispel},
        {"purify disease on party", C::Dispel},
        {"cleanse spirit", C::Dispel},
        {"cleanse spirit curse on party", C::Dispel},
        {"cleanse spirit poison on party", C::Dispel},
        {"cleanse spirit disease on party", C::Dispel},
        {"cure toxins", C::Dispel},
        {"cure toxins poison on party", C::Dispel},
        {"cure toxins disease on party", C::Dispel},
        {"cleansing totem", C::Dispel},
        {"set cleansing totem", C::Dispel},
        {"remove curse", C::Dispel},
        {"remove curse on party", C::Dispel},
        {"remove lesser curse", C::Dispel},
        {"remove lesser curse on party", C::Dispel},
        {"cure disease", C::Dispel},
        {"cure disease on party", C::Dispel},
        {"cure poison", C::Dispel},
        {"cure poison on party", C::Dispel},
        {"abolish disease", C::Dispel},
        {"abolish disease on party", C::Dispel},
        {"abolish poison", C::Dispel},
        {"abolish poison on party", C::Dispel},
        // Crowd control (every name ending " on cc" too, see LookupCategory)
        {"polymorph", C::CrowdControl},
        {"sap", C::CrowdControl},
        {"shackle undead", C::CrowdControl},
        {"blind", C::CrowdControl},
        {"gouge", C::CrowdControl},
        {"fear", C::CrowdControl},
        {"cyclone", C::CrowdControl},
        {"hibernate", C::CrowdControl},
        {"entangling roots", C::CrowdControl},
        {"wyvern sting", C::CrowdControl},
        {"repentance", C::CrowdControl},
        {"repentance on enemy healer", C::CrowdControl},
        {"repentance on snare target", C::CrowdControl},
        {"hammer of justice", C::CrowdControl},
        {"hammer of justice on enemy healer", C::CrowdControl},
        {"hammer of justice on snare target", C::CrowdControl},
        {"shockwave on snare target", C::CrowdControl},
        // Breaking crowd control (the racials; no action uses a PvP trinket item, "use trinket" is a damage boost)
        {"every man for himself", C::CrowdControlBreak},
        {"will of the forsaken", C::CrowdControlBreak},
        {"escape artist", C::CrowdControlBreak},
        // Target switches ("attack my target" is a command from the master and is not delayed)
        {"dps assist", C::TargetSwitch},
        {"tank assist", C::TargetSwitch},
        {"attack enemy player", C::TargetSwitch},
        {"attack enemy flag carrier", C::TargetSwitch},
    };
    return table;
}

SkillNoticeCategory LookupCategory(std::string const& name)
{
    std::unordered_map<std::string, SkillNoticeCategory> const& table = CategoryTable();
    auto const it = table.find(name);
    if (it != table.end())
        return it->second;

    static std::string const ccSuffix = " on cc";
    if (name.size() > ccSuffix.size() && name.compare(name.size() - ccSuffix.size(), ccSuffix.size(), ccSuffix) == 0)
        return SkillNoticeCategory::CrowdControl;

    return SkillNoticeCategory::None;
}
}  // namespace

char const* SkillNoticeCategoryName(SkillNoticeCategory category)
{
    switch (category)
    {
        case SkillNoticeCategory::Interrupt:
            return "interrupt";
        case SkillNoticeCategory::Defensive:
            return "defensive";
        case SkillNoticeCategory::Dispel:
            return "dispel";
        case SkillNoticeCategory::CrowdControl:
            return "crowd control";
        case SkillNoticeCategory::CrowdControlBreak:
            return "cc break";
        case SkillNoticeCategory::TargetSwitch:
            return "target switch";
        default:
            return "none";
    }
}

float SkillMultiplier::GetValue(Action* action)
{
    // Cheap exits first: most bots' actions end at the cached category lookup
    if (action == nullptr || botAI->GetPvpSkill() == 0)
        return 1.0f;

    SkillNoticeCategory const category = GetCategory(action);
    if (category == SkillNoticeCategory::None)
        return 1.0f;

    if (botAI->HasGameClientMaster())
        return 1.0f;

    if (sPlayerbotAIConfig.SkillFullAbilityInPvE && !botAI->IsPvpFight())
        return 1.0f;

    // Delay switching, never engaging: without a live target the assist actions are how a bot starts to fight
    if (category == SkillNoticeCategory::TargetSwitch && !IsTargetSwitch(action))
        return 1.0f;

    SkillNoticeCounters& counters = botAI->GetSkillNoticeCounters();
    size_t const index = static_cast<size_t>(category) - 1;
    if (HasNoticed(category))
    {
        ++counters.allowed[index];
        return 1.0f;
    }

    ++counters.blocked[index];
    return 0.0f;
}

SkillNoticeCategory SkillMultiplier::GetCategory(Action* action)
{
    auto const it = categories.find(action);
    if (it != categories.end())
        return it->second;

    // getName() builds a string; resolved once per action and bot
    SkillNoticeCategory const category = LookupCategory(action->getName());
    categories.emplace(action, category);
    return category;
}

bool SkillMultiplier::IsTargetSwitch(Action* action)
{
    Unit* const current = AI_VALUE(Unit*, "current target");
    if (!current || !current->IsAlive() || !current->IsInWorld())
        return false;

    Unit* const target = action->GetTarget();
    return target && target != current;
}

bool SkillMultiplier::HasNoticed(SkillNoticeCategory category) const
{
    uint32 const missPercent = botAI->GetSkillNoticeMissPercent();
    uint32 const delayMs = botAI->GetSkillNoticeDelayMs();
    if (missPercent == 0 && delayMs == 0)
        return true;

    // A window is NoticeWindowMs plus the bot's longest delay, so even the slowest bot has NoticeWindowMs of noticing
    // per window. Each bot's windows start at its own phase so that bots do not notice in step.
    uint64 const guid = bot->GetGUID().GetCounter();
    uint32 const length = sPlayerbotAIConfig.SkillNoticeWindowMs + delayMs;
    uint64 const shifted = static_cast<uint64>(getMSTime()) + SplitMix64(guid ^ PhaseSalt) % length;
    uint64 const window = shifted / length;
    uint32 const elapsed = static_cast<uint32>(shifted % length);

    uint64 const hash = SplitMix64((guid << 32) ^ (static_cast<uint64>(category) << 56) ^ window ^ NoticeSalt);
    if (hash % 100 < missPercent)
        return false;

    return delayMs == 0 || elapsed >= (hash >> 16) % delayMs;
}

void SkillStrategy::InitMultipliers(std::vector<Multiplier*>& multipliers)
{
    multipliers.push_back(new SkillMultiplier(botAI));
}
