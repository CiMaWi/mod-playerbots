/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "SkillStrategy.h"

#include <algorithm>
#include <string>

#include "ArenaInterrupt.h"
#include "GenericSpellActions.h"
#include "NaturalReaction.h"
#include "PerfMonitor.h"
#include "Playerbots.h"
#include "Spell.h"
#include "SpellAuraEffects.h"
#include "SpellAuras.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "SplitMix64.h"
#include "Timer.h"

namespace
{
constexpr uint64 PhaseSalt = 0x3C6EF372FE94F82BULL;
constexpr uint64 NoticeSalt = 0xA54FF53A5F1D36F1ULL;
// The busy path's stop for a kick (G0h3-b) belongs to a kick query within this long
constexpr uint32 KickStopWindowMs = 500;

static_assert(static_cast<size_t>(SkillNoticeCategory::TargetSwitch) == SkillNoticeCounters::Categories,
              "SkillNoticeCounters needs one slot per notice category");

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
        {"team cc", C::CrowdControl},  // the Cc tactic's (G4b)
        // Breaking crowd control (G5a: the PvP trinket's "pvp trinket" and the racials; "use trinket" is a damage
        // boost and skips the PvP trinket where the Trinket tactic acts)
        {"pvp trinket", C::CrowdControlBreak},
        {"every man for himself", C::CrowdControlBreak},
        {"will of the forsaken", C::CrowdControlBreak},
        {"escape artist", C::CrowdControlBreak},
        // Freeing a party member from crowd control (G5a's Dispel tactic)
        {"dispel cc on party", C::Dispel},
        // Target switches ("attack my target" is a command from the master and is not delayed)
        {"dps assist", C::TargetSwitch},
        {"tank assist", C::TargetSwitch},
        {"attack enemy player", C::TargetSwitch},
        {"cc retarget", C::TargetSwitch},  // OwnCc drops a target its team holds in CC (G4b)
        {"attack enemy flag carrier", C::TargetSwitch},
        // Out of a ground effect (G0h3-b; a reaction only, Playerbots.Natural.Reaction.Movement.Enable)
        {"avoid aoe", C::Movement},
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

// The ease of an action's button (G0h3): by its category (defensive cooldowns and crowd-control breaks are rare
// buttons, a target switch acts on a new target), with the long-cooldown exceptions by name
ReactionEase LookupEase(std::string const& name, SkillNoticeCategory category)
{
    static std::unordered_map<std::string, ReactionEase> const overrides = {
        {"strangulate", ReactionEase::Rare},
        {"strangulate on enemy healer", ReactionEase::Rare},
        {"mass dispel", ReactionEase::Rare},
    };
    auto const it = overrides.find(name);
    if (it != overrides.end())
        return it->second;

    switch (category)
    {
        case SkillNoticeCategory::Defensive:
        case SkillNoticeCategory::CrowdControlBreak:
            return ReactionEase::Rare;
        case SkillNoticeCategory::TargetSwitch:
            return ReactionEase::NewTarget;
        default:
            return ReactionEase::Common;
    }
}

// What matters most when events pile up (the load's top event): interrupt, defensive, out of a ground effect,
// crowd-control break, dispel, crowd control, target switch
uint8 ReactionImportance(SkillNoticeCategory category)
{
    switch (category)
    {
        case SkillNoticeCategory::Interrupt:
            return 7;
        case SkillNoticeCategory::Defensive:
            return 6;
        case SkillNoticeCategory::Movement:
            return 5;
        case SkillNoticeCategory::CrowdControlBreak:
            return 4;
        case SkillNoticeCategory::Dispel:
            return 3;
        case SkillNoticeCategory::CrowdControl:
            return 2;
        case SkillNoticeCategory::TargetSwitch:
            return 1;
        default:
            return 0;
    }
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
        case SkillNoticeCategory::Movement:
            return "movement";
        default:
            return "none";
    }
}

SkillNoticeCategory SkillNoticeCategoryOf(std::string const& actionName) { return LookupCategory(actionName); }

float SkillMultiplier::GetValue(Action* action)
{
    // Cheap exits first: most bots' actions end at the cached category lookup
    if (action == nullptr || botAI->GetPvpSkill() == 0)
        return 1.0f;

    ActionClass const& actionClass = GetClass(action);
    SkillNoticeCategory const category = actionClass.Category;
    if (category == SkillNoticeCategory::None)
        return 1.0f;

    // avoid aoe is a reaction only (G0h3-b): through the natural model with its Movement part, else at once
    bool const movement = category == SkillNoticeCategory::Movement;
    if (movement && (!sPlayerbotAIConfig.NaturalReactionActive || !sPlayerbotAIConfig.NaturalReaction.MovementEnable))
        return 1.0f;

    if (botAI->IsSkillExempt())
        return 1.0f;

    if (sPlayerbotAIConfig.SkillFullAbilityInPvE && !botAI->IsPvpFight())
        return 1.0f;

    // Delay switching, never engaging: without a live target the assist actions are how a bot starts to fight
    Unit* switchTarget = nullptr;
    if (category == SkillNoticeCategory::TargetSwitch && !IsTargetSwitch(action, switchTarget))
        return 1.0f;

    if (movement)
        return HasReacted(category, actionClass.Ease, action, nullptr) ? 1.0f : 0.0f;

    SkillNoticeCounters& counters = botAI->GetSkillNoticeCounters();
    size_t const index = static_cast<size_t>(category) - 1;
    bool const noticed = sPlayerbotAIConfig.NaturalReactionActive
                             ? HasReacted(category, actionClass.Ease, action, switchTarget)
                             : HasNoticed(category);
    if (noticed)
    {
        ++counters.allowed[index];
        return 1.0f;
    }

    ++counters.blocked[index];
    return 0.0f;
}

SkillMultiplier::ActionClass const& SkillMultiplier::GetClass(Action* action)
{
    auto const it = categories.find(action);
    if (it != categories.end())
        return it->second;

    // getName() builds a string; resolved once per action and bot
    std::string const name = action->getName();
    ActionClass actionClass;
    actionClass.Category = LookupCategory(name);
    actionClass.Ease = LookupEase(name, actionClass.Category);
    return categories.emplace(action, actionClass).first->second;
}

bool SkillMultiplier::IsTargetSwitch(Action* action, Unit*& target)
{
    Unit* const current = AI_VALUE(Unit*, "current target");
    if (!current || !current->IsAlive() || !current->IsInWorld())
        return false;

    target = action->GetTarget();
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

namespace
{
// The elapsed part of the caster's cast bar (a channel's of its base duration: a hasted channel looks a little
// older), 0 without a cast
uint32 CastBarAgeMs(Unit* caster, ReactionStampSource& source)
{
    if (!caster)
        return 0;

    Spell* const spell = caster->GetCurrentSpell(CURRENT_GENERIC_SPELL);
    if (spell && spell->getState() == SPELL_STATE_PREPARING && spell->GetCastTime() > 0)
    {
        source = ReactionStampSource::CastBar;
        return static_cast<uint32>(std::max(0, spell->GetCastTime() - spell->GetCastTimeRemaining()));
    }

    Spell* const channel = caster->GetCurrentSpell(CURRENT_CHANNELED_SPELL);
    int32 const duration = channel ? channel->GetSpellInfo()->GetDuration() : 0;
    if (channel && channel->getState() == SPELL_STATE_CASTING && duration > 0)
    {
        source = ReactionStampSource::CastBar;
        return static_cast<uint32>(std::max(0, duration - channel->GetCastTimeRemaining()));
    }

    return 0;
}

// The elapsed part of the newest timed aura on the unit of the other side's kind: a debuff on a friend (on the bot
// for a crowd-control break), a buff on an enemy; for a dispel only dispellable ones. The newest is the latest stamp,
// so a wrong pick makes the bot later, never earlier. 0 without one.
uint32 AuraAgeMs(Player* bot, Unit* unit, bool dispellable, ReactionStampSource& source)
{
    if (!unit)
        return 0;

    bool const friendly = unit == bot || bot->IsFriendlyTo(unit);
    bool found = false;
    int32 newest = 0;
    for (auto const& entry : unit->GetAppliedAuras())
    {
        AuraApplication const* application = entry.second;
        Aura const* aura = application->GetBase();
        int32 const maxDuration = aura->GetMaxDuration();
        if (maxDuration <= 0 || application->IsPositive() == friendly)
            continue;

        if (dispellable && aura->GetSpellInfo()->Dispel == DISPEL_NONE)
            continue;

        int32 const elapsed = std::max(0, maxDuration - aura->GetDuration());
        if (!found || elapsed < newest)
            newest = elapsed;

        found = true;
    }

    if (!found)
        return 0;

    source = ReactionStampSource::Aura;
    return static_cast<uint32>(newest);
}

// The age of the newest ground effect hurting the bot (a periodic damage aura of a dynamic object, whose duration is
// the effect's), 0 without one: a trap or a trigger NPC has no visible start
uint32 GroundEffectAgeMs(Player* bot, ReactionStampSource& source)
{
    bool found = false;
    int32 newest = 0;
    for (AuraEffect const* effect : bot->GetAuraEffectsByType(SPELL_AURA_PERIODIC_DAMAGE))
    {
        Aura const* aura = effect->GetBase();
        if (aura->GetType() != DYNOBJ_AURA_TYPE || aura->GetMaxDuration() <= 0)
            continue;

        int32 const elapsed = std::max(0, aura->GetMaxDuration() - aura->GetDuration());
        if (!found || elapsed < newest)
            newest = elapsed;

        found = true;
    }

    if (!found)
        return 0;

    source = ReactionStampSource::Aura;
    return static_cast<uint32>(newest);
}

// How long before now the event the action answers began, from what a player in the bot's seat sees (DT.12)
uint32 VisibleEventAgeMs(Player* bot, SkillNoticeCategory category, Unit* target, ReactionStampSource& source)
{
    switch (category)
    {
        case SkillNoticeCategory::Interrupt:
            return CastBarAgeMs(target, source);
        case SkillNoticeCategory::Dispel:
            return AuraAgeMs(bot, target, true, source);
        case SkillNoticeCategory::CrowdControlBreak:
            return AuraAgeMs(bot, bot, false, source);
        case SkillNoticeCategory::Movement:
            return GroundEffectAgeMs(bot, source);
        default:
            // Defensive cooldowns, crowd control and target switches have no visible start: the first query
            return 0;
    }
}
}  // namespace

ReactionEventInputs SkillMultiplier::EventInputs(SkillNoticeCategory category, ReactionEase ease, Unit* target,
                                                 uint32 nowMs)
{
    ReactionParams const& params = sPlayerbotAIConfig.NaturalReaction;
    ReactionEventInputs inputs;
    // Choice: a switch and crowd control weigh the attackers the bot has already listed (no new scan); casts worth a
    // kick and party members to dispel have no count without one, so they are one option
    if (params.Choice.Enable &&
        (category == SkillNoticeCategory::TargetSwitch || category == SkillNoticeCategory::CrowdControl))
        inputs.Options = std::max<uint32>(1, static_cast<uint32>(AI_VALUE_LAZY(GuidVector, "attackers").size()));

    if (params.Load.Enable)
    {
        size_t const index = static_cast<size_t>(category) - 1;
        uint8 const rank = ReactionImportance(category);
        inputs.LoadTop = true;
        for (size_t other = 0; other < ReactionCategories; ++other)
        {
            if (other == index || !reactionNatural[other] || !ReactionEventPending(reactionEvents[other], nowMs))
                continue;

            ++inputs.Load;
            if (ReactionImportance(static_cast<SkillNoticeCategory>(other + 1)) >= rank)
                inputs.LoadTop = false;
        }
    }

    if (params.Ease.Enable)
    {
        inputs.Ease = ease;
        // An action on an enemy that is not the bot's target acts on a new target; on a friend the button's ease stays
        if (ease != ReactionEase::NewTarget && target && target != bot && bot->IsHostileTo(target) &&
            AI_VALUE(Unit*, "current target") != target)
            inputs.Ease = ReactionEase::NewTarget;
    }

    if (category != SkillNoticeCategory::Interrupt)
        return inputs;

    // Without the Interrupt tactic a kick goes at the first cast the bot sees, without judging the spell: the shortcut.
    // With it (G5b) a bot that judged the cast pays the deep check and, with several enemies casting, the choice.
    inputs.Thinks = true;
    inputs.Depth = ReactionThinkDepth::Shortcut;
    if (target && botAI->TacticEnabledHere(TacticId::Interrupt))
    {
        InterruptCastSlot const* const decided = botAI->GetInterruptState().Latest(target->GetGUID().GetCounter());
        if (decided && decided->Deep)
        {
            inputs.Depth = ReactionThinkDepth::Deep;
            if (params.Choice.Enable)
                inputs.Options = std::max<uint32>(1, decided->Casting);
        }
    }

    if (params.Anticipate.Enable && target)
    {
        Spell* spell = target->GetCurrentSpell(CURRENT_GENERIC_SPELL);
        if (!spell)
            spell = target->GetCurrentSpell(CURRENT_CHANNELED_SPELL);

        if (spell && spell->GetSpellInfo())
        {
            // The bar the memory has in view (a pushback moves its end) or the one seen now
            OpponentMemory const& memory = botAI->GetOpponentMemory();
            uint32 const guid = target->GetGUID().GetCounter();
            uint32 const spellId = spell->GetSpellInfo()->Id;
            OpponentMemorySlot const* slot = memory.Find(guid);
            uint32 const endMs = slot && slot->ViewEndMs != 0 && slot->SpellId == spellId
                                     ? slot->ViewEndMs
                                     : nowMs + static_cast<uint32>(std::max(0, spell->GetCastTimeRemaining()));
            inputs.Predicted = memory.Predicts(guid, spellId, endMs, params.Memory.EndToleranceMs);
        }
    }

    return inputs;
}

bool SkillMultiplier::HasReacted(SkillNoticeCategory category, ReactionEase ease, Action* action, Unit* target)
{
    PerfMonitorScope scope(sPerfMonitor.acquire(PERF_MON_TOTAL, "NaturalReaction::HasReacted"));
    size_t const index = static_cast<size_t>(category) - 1;
    ReactionEvent& event = reactionEvents[index];
    uint32 const now = getMSTime();
    ReactionParams const& params = sPlayerbotAIConfig.NaturalReaction;
    // A kick skipped as out of reach (user note 2) waits for the next enemy cast, not for the miss window
    ReactionBusyState& busy = botAI->GetReactionBusy();
    if (event.Active && event.Busy == static_cast<uint8>(ReactionBusyAnswer::Skipped) &&
        static_cast<int32>(now - busy.SkipEndMs) >= 0)
        event.Active = false;

    if (ReactionEventStarts(event, now, sPlayerbotAIConfig.SkillNoticeWindowMs))
    {
        PerfMonitorScope startScope(sPerfMonitor.acquire(PERF_MON_TOTAL, "NaturalReaction::EventStart"));
        // Once per event: the context, the target (a target switch brings its own) and the view cone
        reactionNatural[index] =
            ScopeAllows(sPlayerbotAIConfig.NaturalReactionContexts, PlayerbotAI::ProbeTacticContext(bot));
        if (!target && category != SkillNoticeCategory::TargetSwitch)
            target = action->GetTarget();

        bool const behind = reactionNatural[index] && target && target != bot && bot->IsHostileTo(target) &&
                            !bot->HasInArc(params.ConeDegrees * static_cast<float>(M_PI) / 180.0f, target);
        ReactionEventInputs const inputs =
            reactionNatural[index] ? EventInputs(category, ease, target, now) : ReactionEventInputs();
        StartReactionEvent(event, reactionFocus, botAI->GetReactionState(), params, bot->GetGUID().GetCounter(),
                           static_cast<uint8>(category), botAI->GetSkillNoticeMissPercent(), behind, now,
                           botAI->GetReactionTypes(), inputs);
        // End to end only where the faster check runs: at upstream's combat interval (250 ms in battlegrounds and
        // arenas, 500 ms elsewhere, x the skill's stretch, plus up to 200 ms of stagger) the learnt latency would eat
        // most of the draw
        if (reactionNatural[index] && params.EndToEnd && botAI->IsNaturalCadenceActive())
        {
            ReactionStampSource source = ReactionStampSource::FirstQuery;
            uint32 const age = VisibleEventAgeMs(bot, category, target, source);
            uint32 const tick =
                params.ServerTickMs >= 0 ? static_cast<uint32>(params.ServerTickMs) : botAI->GetLastUpdateDiff() / 2;
            StampReactionEvent(event, reactionFocus, params, age, source, tick);
        }

        if (reactionNatural[index])
            MeetBusy(event, category, action, target, now, true);

        if (reactionNatural[index] && event.Missed)
            LOG_TRACE("playerbots.reaction",
                      "reaction cat {} skill {} ready {} pass -1 behind {} switch {} stamp {} src {} target {} e2e -1 "
                      "lat {} at {} type {} options {} load {} ease {} think {} cost {} busy {} own {}",
                      static_cast<uint32>(category), botAI->GetPvpSkill(), event.ReadyAfterMs, event.Behind ? 1 : 0,
                      event.Switched ? 1 : 0, event.StampAgeMs, static_cast<uint32>(event.StampSource), event.TargetMs,
                      static_cast<uint32>(reactionFocus.LatencyMs + 0.5f), now, static_cast<uint32>(event.Type),
                      static_cast<uint32>(event.Options), static_cast<uint32>(event.Load),
                      static_cast<uint32>(event.Ease), event.ThinkMs, event.CostMs, static_cast<uint32>(event.Busy),
                      event.OwnMs);
    }
    else if (category == SkillNoticeCategory::Interrupt && reactionNatural[index] && busy.KickStopMs != 0)
        MeetBusy(event, category, action, target ? target : action->GetTarget(), now, false);

    // Outside the configured contexts (PvE by default) D4's uniform delay applies (a movement goes at once)
    if (!reactionNatural[index])
    {
        event.LastQueryMs = now;
        return category == SkillNoticeCategory::Movement || HasNoticed(category);
    }

    // An overshoot beyond two check delays is a pause of the bot's own (a cast), not its check latency
    bool const wasNoticed = event.Noticed;
    if (!ReactionEventPasses(event, reactionFocus, static_cast<uint8>(category), now, 2 * botAI->GetLastCheckDelay()))
        return false;

    // pass counts from the first query, e2e from the event's server time to the engine's pass (the action follows in
    // the same update)
    if (!wasNoticed)
    {
        // The latency never exceeds the faster check's interval, so a pass loses at most that much of its draw
        reactionFocus.LatencyMs =
            std::min(reactionFocus.LatencyMs, static_cast<float>(sPlayerbotAIConfig.NaturalCadenceCheckMs));
        LOG_TRACE("playerbots.reaction",
                  "reaction cat {} skill {} ready {} pass {} behind {} switch {} stamp {} src {} target {} e2e {} lat "
                  "{} at {} type {} options {} load {} ease {} think {} cost {} busy {} own {}",
                  static_cast<uint32>(category), botAI->GetPvpSkill(), event.ReadyAfterMs, now - event.StartMs,
                  event.Behind ? 1 : 0, event.Switched ? 1 : 0, event.StampAgeMs,
                  static_cast<uint32>(event.StampSource), event.TargetMs, now - event.StartMs + event.StampAgeMs,
                  static_cast<uint32>(reactionFocus.LatencyMs + 0.5f), now, static_cast<uint32>(event.Type),
                  static_cast<uint32>(event.Options), static_cast<uint32>(event.Load), static_cast<uint32>(event.Ease),
                  event.ThinkMs, event.CostMs, static_cast<uint32>(event.Busy), event.OwnMs);
    }

    return true;
}

void SkillMultiplier::MeetBusy(ReactionEvent& event, SkillNoticeCategory category, Action* action, Unit* target,
                               uint32 nowMs, bool starting)
{
    ReactionBusyState& busy = botAI->GetReactionBusy();
    if (starting)
    {
        // The bot's own cast between the event's server time and its first query: the time it did not look
        uint32 const eventMs = nowMs - event.StampAgeMs;
        uint32 const from = static_cast<int32>(busy.CastStartMs - eventMs) > 0 ? busy.CastStartMs : eventMs;
        uint32 const to = static_cast<int32>(busy.CastEndMs - nowMs) < 0 ? busy.CastEndMs : nowMs;
        event.OwnMs = static_cast<int32>(to - from) > 0 ? to - from : 0;
    }

    if (category != SkillNoticeCategory::Interrupt)
        return;

    // User note 1: the busy path stopped the own cast for this kick; that was the reaction, so the pass is due now
    if (busy.KickStopMs != 0)
    {
        bool const recent = nowMs - busy.KickStopMs <= KickStopWindowMs;
        bool const forThis =
            recent && !event.Noticed && target && target->GetGUID().GetCounter() == busy.KickTargetGuid;
        if (forThis || !recent)
            busy.KickStopMs = 0;

        if (forThis)
        {
            event.Missed = false;
            event.Corrected = false;
            event.DueAfterMs = nowMs - event.StartMs;
            event.Busy = static_cast<uint8>(ReactionBusyAnswer::StoppedOwnCast);
            return;
        }
    }

    ReactionReevalParams const& rules = sPlayerbotAIConfig.NaturalReaction.Reeval;
    if (!starting || !rules.BusySkip || event.Missed || !target)
        return;

    // User note 2 (a channel is worth cutting late): no kick a busy bot reaches only at the cast's end
    Spell* const cast = target->GetCurrentSpell(CURRENT_GENERIC_SPELL);
    if (!cast || cast->getState() != SPELL_STATE_PREPARING || cast->GetCastTime() <= 0)
        return;

    // Busy: its own cast kept it from looking, or the kick waits for its cooldown or the GCD; skipped only when that
    // is what makes the kick late (user note 2: busy with a long cast or a cooldown), not its reaction alone
    uint32 waitMs = 0;
    if (CastSpellAction* const kick = dynamic_cast<CastSpellAction*>(action))
    {
        uint32 const spellId = AI_VALUE2(uint32, "spell id", kick->getSpell());
        SpellInfo const* const info = spellId ? sSpellMgr->GetSpellInfo(spellId) : nullptr;
        if (info)
        {
            waitMs = bot->GetSpellCooldownDelay(spellId);
            if (info->StartRecoveryCategory != 0 && static_cast<int32>(busy.GcdEndMs - nowMs) > 0 &&
                bot->GetGlobalCooldownMgr().HasGlobalCooldown(info))
                waitMs = std::max(waitMs, busy.GcdEndMs - nowMs);
        }
    }

    uint32 const enemyLeft = static_cast<uint32>(std::clamp(cast->GetCastTimeRemaining(), 0, cast->GetCastTime()));
    ReactionParams const& params = sPlayerbotAIConfig.NaturalReaction;
    bool const stamped = params.EndToEnd && botAI->IsNaturalCadenceActive();
    int32 const freeMs = BusyFreeDueMs(event, static_cast<uint32>(reactionFocus.LatencyMs + 0.5f), stamped);
    if (!InterruptOutOfReach(rules, std::max(event.DueAfterMs, waitMs), freeMs, enemyLeft))
        return;

    event.Missed = true;
    event.Busy = static_cast<uint8>(ReactionBusyAnswer::Skipped);
    busy.SkipEndMs = nowMs + enemyLeft;
    ++busy.SkippedBusy;
}

void SkillStrategy::InitMultipliers(std::vector<Multiplier*>& multipliers)
{
    multipliers.push_back(new SkillMultiplier(botAI));
}
