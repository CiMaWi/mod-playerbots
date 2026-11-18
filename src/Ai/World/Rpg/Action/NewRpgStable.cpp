/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include <algorithm>
#include <cctype>
#include <limits>
#include <list>
#include <memory>
#include <string>
#include <variant>

#include "CellImpl.h"
#include "CensusMgr.h"
#include "Creature.h"
#include "DBCStores.h"
#include "GameTime.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Map.h"
#include "NewRpgAction.h"
#include "NewRpgErrandInternal.h"
#include "NewRpgInfo.h"
#include "NewRpgInternal.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Pet.h"
#include "PlannerService.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "SharedDefines.h"
#include "Spell.h"
#include "SpellInfo.h"
#include "UpkeepIndex.h"
#include "UpkeepNeedRules.h"
#include "UpkeepRules.h"
#include "World.h"
#include "WorldLog.h"
#include "WorldPacket.h"
#include "WorldSession.h"

using namespace NewRpgInternal;

namespace
{
// The family's word for PetTamed's source: the DBC's English name in lower case, '_' for a space ("wind_serpent")
std::string FamilyWord(uint32 family)
{
    CreatureFamilyEntry const* entry = sCreatureFamilyStore.LookupEntry(family);
    std::string word = entry && entry->Name[0] && *entry->Name[0] ? entry->Name[0] : "unknown";
    for (char& c : word)
        c = c == ' ' ? '_' : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return word;
}

// A beast the bot may tame now: alive, wild, out of combat (no other's fight), at or below the bot's level and
// tameable by it (an exotic one with Beast Mastery's aura), as the core's Tame Beast checks
bool TameableNow(Player* bot, Creature const* beast, bool inOwnFight = false)
{
    return beast && beast->IsAlive() && (!beast->IsInCombat() || (inOwnFight && beast->GetVictim() == bot)) &&
           !beast->IsPet() && beast->GetOwnerGUID().IsEmpty() && beast->GetLevel() <= bot->GetLevel() &&
           beast->GetCreatureTemplate()->IsTameable(bot->CanTameExoticPets());
}

// A cast that ended without a pet: the row tame/channel_broken (its length as the AI updates saw it run, the beast's
// yards, -1 dead or gone); the stop's rest starts now, its reach bound at the rest's end
void NoteBrokenChannel(Player* bot, NewRpgInfo::ErrandStop& stop, uint32 now)
{
    stop.tameCast = false;
    stop.tameSinceMs = now;
    stop.tamePickMs = 0;
    Creature const* beast = ObjectAccessor::GetCreature(*bot, stop.tameBeast);
    float const channelS =
        stop.tameSeenMs ? static_cast<float>(getMSTimeDiff(stop.tameCastMs, stop.tameSeenMs)) / IN_MILLISECONDS : 0.0f;
    WorldLog::Decision(bot, WorldLogFormat::DecisionKind::Upkeep, "tame", "channel_broken", bot->GetHealthPct(),
                       {{"tries", static_cast<float>(stop.tameTries)},
                        {"channel_s", channelS},
                        {"beast_yd", beast && beast->IsAlive() ? bot->GetExactDist(beast) : -1.0f}},
                       stop.tameEntry);
}

// The core's cast, as a player casts it (the bot's combat cast layer is for rotations): no swing at the beast (a melee
// swing breaks the channel), faced and selected; a cast counts a try and writes the row tame/cast
SpellCastResult CastTame(Player* bot, NewRpgInfo::ErrandStop& stop, Creature* beast)
{
    if (bot->GetVictim())
        bot->AttackStop();
    bot->SetSelection(beast->GetGUID());
    bot->SetFacingToObject(beast);
    SpellCastResult const result = bot->CastSpell(beast, UpkeepNeeds::SPELL_TAME_BEAST, false);
    if (result != SPELL_CAST_OK)
        return result;

    ++stop.tameTries;
    stop.tameCast = true;
    stop.tameCastMs = getMSTime();
    stop.tameSeenMs = 0;
    WorldLog::Decision(bot, WorldLogFormat::DecisionKind::Upkeep, "tame", "cast", static_cast<float>(beast->GetLevel()),
                       {{"tries", static_cast<float>(stop.tameTries)}}, beast->GetEntry());
    return result;
}

// The channel's end in the beast's fight without a pet (WU21b-2 round 4: Swoop's stun and Wing Buffet's knockback
// broke it at 6-8 s, damage only pushes it back): cast again on the same beast at once while TameRecastsInFight
// holds, waiting out the stun or the knockback's fall; tameSeenMs, the channel's end, marks the cast again as due.
// True while the bot's AI waits for it; false lets the bot fight, and the stop reads the rest after the fight
bool RecastTameInFight(Player* bot, NewRpgInfo::ErrandStop& stop)
{
    uint32 const now = getMSTime();
    // Any fight at the stop, a clear's or an attacker's, restarts the rest and the reach bound at its end (WU21e run
    // 3: a Bladewing Bloodletter's aggro fight spent the bound of the walk to the next beast)
    stop.tameClearing = true;
    if (stop.tameCast)
    {
        Pet const* pet = bot->GetPet();
        if (pet && pet->getPetType() == HUNTER_PET)
            return false;
        uint32 const endMs = stop.tameSeenMs ? stop.tameSeenMs : stop.tameCastMs;
        if (!UpkeepNeeds::TameRecastsInFight(stop.tameTries, bot->GetHealthPct(), getMSTimeDiff(endMs, now)))
            return false;
        NoteBrokenChannel(bot, stop, now);
        stop.tameSeenMs = endMs ? endMs : now;
    }
    else if (!stop.tameSeenMs)
    {
        // Before a cast: a beast of the stop that fights the bot (it aggroes at its spawn or on the way; WU21c run 2:
        // a hunter killed the Mistwing Rogue it came for) becomes the pick and is tamed in that fight, as a player
        // tames; the pick's TAME_REACH_MS bounds it, then the bot fights. Only as the bot's one attacker: another
        // would join the channel's fight (the site rule, WU.23)
        Creature* attacker = nullptr;
        for (Unit* unit : bot->getAttackers())
        {
            Creature* each = unit->ToCreature();
            if (each && bot->getAttackers().size() == 1 && each->GetEntry() == stop.tameEntry &&
                TameableNow(bot, each, true))
            {
                attacker = each;
                break;
            }
        }
        bool const picked = attacker && attacker->GetGUID() == stop.tameBeast;
        if (!attacker ||
            (picked && stop.tamePickMs && getMSTimeDiff(stop.tamePickMs, now) > UpkeepNeeds::TAME_REACH_MS))
            return false;
        if (!picked)
        {
            stop.tameBeast = attacker->GetGUID();
            stop.tamePickMs = now;
        }
        // The picked beast attacks after a reset: its bound starts now
        stop.tamePickMs = UpkeepNeeds::TameReachStartMs(stop.tamePickMs, now);
        stop.tameSeenMs = now;
    }

    Creature* beast = ObjectAccessor::GetCreature(*bot, stop.tameBeast);
    if (!UpkeepNeeds::TameRecastsInFight(stop.tameTries, bot->GetHealthPct(), getMSTimeDiff(stop.tameSeenMs, now)) ||
        !TameableNow(bot, beast, true))
    {
        stop.tameSeenMs = 0;
        return false;
    }
    if (bot->IsFalling() || bot->HasUnitState(UNIT_STATE_CONTROLLED))
        return true;
    if (bot->isMoving())
        bot->StopMoving();
    // Out of range or still moving after a knockback: tried again on the next update, until TAME_RECAST_MS
    CastTame(bot, stop, beast);
    return true;
}

// The live units around the bot within yards
std::list<Unit*> UnitsAround(Player* bot, float yards)
{
    std::list<Unit*> found;
    Acore::AnyUnitInObjectRangeCheck check(bot, yards);
    Acore::UnitListSearcher<Acore::AnyUnitInObjectRangeCheck> searcher(bot, found, check);
    Cell::VisitObjects(bot, searcher, yards);
    return found;
}

// The creatures of the tame's site that would join the channel's fight (WU.23, UpkeepNeeds::TameThreat), counted,
// and the nearest of them to the bot; spot: where the bot stands at the cast
struct SiteThreats
{
    uint32 count{0};
    Creature* nearest{nullptr};
};

SiteThreats TameSiteThreats(Player* bot, Creature* beast, Position const& spot, std::list<Unit*> const& units)
{
    float const assistYards = sWorld->getFloatConfig(CONFIG_CREATURE_FAMILY_ASSISTANCE_RADIUS);
    SiteThreats threats;
    float nearestYards = std::numeric_limits<float>::max();
    for (Unit* unit : units)
    {
        Creature* each = unit->ToCreature();
        bool const attacking = each && each->GetVictim() == bot;
        if (!each || each == beast || !each->IsAlive() || (each->IsInCombat() && !attacking) || each->IsCivilian() ||
            !each->GetOwnerGUID().IsEmpty())
            continue;
        UpkeepNeeds::TameSiteUnit const site{
            each->GetPositionX(),          each->GetPositionY(),
            each->GetAggroRange(bot),      each->HasReactState(REACT_AGGRESSIVE) && each->IsHostileTo(bot),
            each->CanAssistTo(beast, bot), attacking};
        if (!UpkeepNeeds::TameThreat(site, beast->GetPositionX(), beast->GetPositionY(), spot.GetPositionX(),
                                     spot.GetPositionY(), assistYards) ||
            !bot->IsValidAttackTarget(each))
            continue;
        ++threats.count;
        float const yards = bot->GetExactDist(each);
        if (yards < nearestYards)
        {
            nearestYards = yards;
            threats.nearest = each;
        }
    }
    return threats;
}

// Where the bot stands to tame a beast: on its way in, TAME_CAST_YARDS / 2 short of it (the approach's range)
Position TameSpot(Player* bot, Creature const* beast)
{
    float const yards = bot->GetExactDist2d(beast);
    float const share = yards > UpkeepNeeds::TAME_CAST_YARDS / 2 ? UpkeepNeeds::TAME_CAST_YARDS / 2 / yards : 1.0f;
    return Position(beast->GetPositionX() + (bot->GetPositionX() - beast->GetPositionX()) * share,
                    beast->GetPositionY() + (bot->GetPositionY() - beast->GetPositionY()) * share,
                    beast->GetPositionZ());
}

// The beast the bot may tame now within TAME_SEARCH_YARDS, by UpkeepNeeds::TameBetter: the fewest threats at its
// site (WU.23), then one of the stop's entry (the planned spawn: another hunter may have tamed it), then the nearest.
// Only an entry the index keeps as a tame target on the map (normal rank, no spell that breaks the channel, several
// standing spawns: one lookup, WU21d) and no patrolling beast; familyRule: only a family the hunter wants (the tame
// after a stable swap, WU21c: never the unwanted kind it just stored)
Creature* NearestTameable(Player* bot, uint32 entry, bool familyRule = false)
{
    std::shared_ptr<Planner::UpkeepIndex const> const index =
        Planner::UpkeepIndexReady() ? Planner::CurrentUpkeepIndex() : nullptr;
    if (!index)
        return nullptr;

    std::list<Unit*> const found = UnitsAround(bot, UpkeepNeeds::TAME_SEARCH_YARDS);
    Creature* picked = nullptr;
    UpkeepNeeds::TamePick best;
    for (Unit* unit : found)
    {
        Creature* each = unit->ToCreature();
        if (!each || !index->TameTarget(bot->GetMapId(), each->GetEntry()) || !TameableNow(bot, each) ||
            !UpkeepNeeds::TameSpawnStands(static_cast<uint8>(each->GetDefaultMovementType())) ||
            (familyRule && !UpkeepNeeds::WantedFamily(static_cast<uint8>(each->GetCreatureTemplate()->family),
                                                      bot->CanTameExoticPets())))
            continue;
        UpkeepNeeds::TamePick const pick{TameSiteThreats(bot, each, TameSpot(bot, each), found).count,
                                         each->GetEntry() != entry, bot->GetExactDist(each)};
        if (!picked || UpkeepNeeds::TameBetter(pick, best))
        {
            best = pick;
            picked = each;
        }
    }
    return picked;
}

// No beast to tame at the stop (WU.23, UpkeepNeeds::TameWaitMs): the soonest a standing spawn of the stop's entry
// within TAME_SEARCH_YARDS is free (UpkeepNeeds::TameFreeInS): a beast in another's fight, a corpse at its own respawn
// time, a spawn the core removed at the map's respawn record (one pass over the map's records per arming; the
// spawn-group respawn mode keeps no creature for it). A wait that fits the budget is armed on the stop and writes the
// row tame/wait (value its seconds; alternatives the respawn's seconds, the seconds waited before, wanted 1 for the
// long budget). True while it waits
bool ArmTameWait(Player* bot, NewRpgInfo::ErrandStop& stop, uint32 now)
{
    std::list<Creature*> beasts;
    Acore::AllCreaturesOfEntryInRange check(bot, stop.tameEntry, UpkeepNeeds::TAME_SEARCH_YARDS);
    Acore::CreatureListSearcher<Acore::AllCreaturesOfEntryInRange> searcher(bot, beasts, check);
    Cell::VisitObjects(bot, searcher, UpkeepNeeds::TAME_SEARCH_YARDS);
    time_t const gameNow = GameTime::GetGameTime().count();
    uint32 respawnS = UpkeepNeeds::TAME_FREE_NONE;
    for (Creature const* each : beasts)
    {
        bool const othersFight = each->IsAlive() && each->IsInCombat() && each->GetVictim() != bot;
        if (each->GetSpawnId() && (othersFight || !each->IsAlive()) &&
            UpkeepNeeds::TameSpawnStands(static_cast<uint8>(each->GetDefaultMovementType())))
            respawnS = UpkeepNeeds::TameFreeInS(respawnS, othersFight, each->GetRespawnTime(), gameNow);
    }
    for (auto const& [spawnId, respawnAt] : bot->GetMap()->GetCreatureRespawnTimes())
    {
        CreatureData const* data = sObjectMgr->GetCreatureData(spawnId);
        if (data && data->id == stop.tameEntry && data->mapid == bot->GetMapId() &&
            UpkeepNeeds::TameSpawnStands(data->movementType) &&
            bot->GetExactDist2d(data->posX, data->posY) <= UpkeepNeeds::TAME_SEARCH_YARDS)
            respawnS = UpkeepNeeds::TameFreeInS(respawnS, false, respawnAt, gameNow);
    }
    if (respawnS == UpkeepNeeds::TAME_FREE_NONE)
        return false;

    CreatureTemplate const* info = sObjectMgr->GetCreatureTemplate(stop.tameEntry);
    bool const wanted = stop.tameStabled || (info && info->IsExotic());
    uint32 const waitedMs = stop.tameWaitFirstMs ? getMSTimeDiff(stop.tameWaitFirstMs, now) : 0;
    uint32 const waitMs = UpkeepNeeds::TameWaitMs(std::min<uint32>(respawnS, HOUR) * IN_MILLISECONDS, waitedMs, wanted);
    if (!waitMs)
        return false;
    if (!stop.tameWaitFirstMs)
        stop.tameWaitFirstMs = now;
    stop.tameWaitUntilMs = waitedMs + waitMs;
    stop.tamePickMs = 0;  // the respawned beast's reach bound starts after the wait
    WorldLog::Decision(bot, WorldLogFormat::DecisionKind::Upkeep, "tame", "wait",
                       static_cast<float>(waitMs) / IN_MILLISECONDS,
                       {{"respawn_s", static_cast<float>(respawnS)},
                        {"waited_s", static_cast<float>(waitedMs) / IN_MILLISECONDS},
                        {"wanted", wanted ? 1.0f : 0.0f}},
                       stop.tameEntry);
    return true;
}

// The bot fights a threat of the tame's site as AttackAction starts a fight: the combat engine takes over
void AttackThreat(PlayerbotAI* botAI, Creature* threat)
{
    Player* bot = botAI->GetBot();
    bot->SetSelection(threat->GetGUID());
    botAI->GetAiObjectContext()->GetValue<Unit*>("current target")->Set(threat);
    botAI->ChangeEngine(BOT_STATE_COMBAT);
    bot->Attack(threat, PlayerbotAI::IsMelee(bot));
}
}  // namespace

namespace NewRpgInternal
{
// The tame part of an errand stop (upkeep §3.11, WU21b-2): the bot stands at the spawn of the pet need's beast, picks
// the nearest beast of that entry it may tame, walks into reach and casts Tame Beast; the channel holds while the beast
// fights (HoldTameChannel holds the bot's AI and breaks it off at low health). Each end of a channel is read on the
// next tick out of combat: a hunter pet now is the tame (PetTamed, id the beast's entry, amount the pet's level, source
// the family word, and the upkeep row tame/tamed); else the row tame/channel_broken and another try, after a rest, with
// the same beast while it may be tamed or the next nearest. Rows: tame/cast per cast; skip/tame_failed (tries used, or
// no beast left after a broken channel), skip/no_beast (none at the stop, no respawn to wait for or the waits' budget
// spent), skip/tame_refused (the core refused the cast: value its SpellCastResult), skip/tame_stuck (no cast within
// TAME_REACH_MS of the rest's end, whatever the re-picks), skip/tame_site (the site stayed busy after TAME_MAX_CLEARS
// fights), all with the tries made and the seconds waited for a respawn. The site (WU.23): the pick avoids beasts whose
// fight others would join, the cast fights such a creature first (tame/clear), and with no beast the stop waits for a
// respawn (tame/wait)
TameStep TameAtBeast(PlayerbotAI* botAI, NewRpgInfo::ErrandStop& stop)
{
    Player* bot = botAI->GetBot();
    if (bot->GetCurrentSpell(CURRENT_CHANNELED_SPELL) || bot->GetCurrentSpell(CURRENT_GENERIC_SPELL))
        return TameStep::Busy;

    uint32 const now = getMSTime();
    if (!stop.tameSinceMs)
        stop.tameSinceMs = now;
    if (stop.tameClearing)
    {
        // A fight at the stop is over (a clear, an attacker: RecastTameInFight): the rest starts again, the reach
        // bound at its end
        stop.tameClearing = false;
        stop.tameSinceMs = now;
        stop.tamePickMs = 0;
    }
    auto const skip = [bot, &stop, now](char const* reason)
    {
        float const waitedS =
            stop.tameWaitFirstMs ? static_cast<float>(getMSTimeDiff(stop.tameWaitFirstMs, now)) / IN_MILLISECONDS : 0;
        WorldLog::Decision(bot, WorldLogFormat::DecisionKind::Upkeep, "skip", reason,
                           static_cast<float>(stop.tameTries),
                           {{"tries", static_cast<float>(stop.tameTries)}, {"waited_s", waitedS}}, stop.tameEntry);
        return TameStep::Done;
    };

    if (stop.tameCast)
    {
        Pet* pet = bot->GetPet();
        if (pet && pet->getPetType() == HUNTER_PET)
        {
            CreatureTemplate const* info = pet->GetCreatureTemplate();
            uint32 const family = info ? info->family : 0;
            std::string const word = FamilyWord(family);
            // After a stable swap (WU21c) the source says so; the old pet's entry is the row's alternative stabled
            EventLog::Emit(bot, Upkeep::EVENT_PET_TAMED, stop.tameStabled ? "stabled" : word.c_str(), pet->GetEntry(),
                           int64(pet->GetLevel()));
            WorldLog::Decision(bot, WorldLogFormat::DecisionKind::Upkeep, "tame", "tamed",
                               static_cast<float>(pet->GetLevel()),
                               {{"tries", static_cast<float>(stop.tameTries)},
                                {"family", static_cast<float>(family)},
                                {"stabled", static_cast<float>(stop.tameStabled)}},
                               pet->GetEntry());
            return TameStep::Done;
        }
        NoteBrokenChannel(bot, stop, now);
    }

    // A wait for a respawn runs: other actions (food) may run meanwhile
    if (stop.tameWaitUntilMs && getMSTimeDiff(stop.tameWaitFirstMs, now) < stop.tameWaitUntilMs)
        return TameStep::Rest;

    switch (UpkeepNeeds::TameNextStep(stop.tameTries, bot->GetHealthPct(), getMSTimeDiff(stop.tameSinceMs, now)))
    {
        case UpkeepNeeds::TameNext::GiveUp:
            return skip("tame_failed");
        case UpkeepNeeds::TameNext::Rest:
            return TameStep::Rest;
        case UpkeepNeeds::TameNext::Cast:
            break;
    }

    Creature* beast = stop.tameBeast.IsEmpty() ? nullptr : ObjectAccessor::GetCreature(*bot, stop.tameBeast);
    if (!TameableNow(bot, beast))
    {
        beast = NearestTameable(bot, stop.tameEntry, stop.tameStabled != 0);
        if (!beast)
            return ArmTameWait(bot, stop, now) ? TameStep::Rest : skip(stop.tameTries ? "tame_failed" : "no_beast");
        stop.tameBeast = beast->GetGUID();
    }

    stop.tamePickMs = UpkeepNeeds::TameReachStartMs(stop.tamePickMs, now);
    if (getMSTimeDiff(stop.tamePickMs, now) > UpkeepNeeds::TAME_REACH_MS)
    {
        WorldLog::Decision(bot, WorldLogFormat::DecisionKind::Upkeep, "skip", "tame_stuck", bot->GetExactDist(beast),
                           {{"dz", beast->GetPositionZ() - bot->GetPositionZ()},
                            {"los", bot->IsWithinLOSInMap(beast) ? 1.0f : 0.0f},
                            {"moving", bot->isMoving() ? 1.0f : 0.0f}},
                           beast->GetEntry());
        return TameStep::Done;
    }
    if (bot->GetExactDist(beast) > UpkeepNeeds::TAME_CAST_YARDS || !bot->IsWithinLOSInMap(beast))
        return TameStep::Approach;
    // Tame Beast is not cast mounted or moving (a trip arrives mounted)
    if (bot->IsMounted())
    {
        bot->RemoveAurasByType(SPELL_AURA_MOUNTED);
        return TameStep::Busy;
    }
    if (bot->isMoving())
    {
        bot->StopMoving();
        return TameStep::Busy;
    }

    // The site from the bot's spot: nothing else may join the channel's fight, else the bot fights it first
    SiteThreats const threats =
        TameSiteThreats(bot, beast, bot->GetPosition(), UnitsAround(bot, UpkeepNeeds::TAME_SITE_YARDS));
    switch (UpkeepNeeds::TameSiteNext(threats.count, stop.tameClears))
    {
        case UpkeepNeeds::TameSite::GiveUp:
            return skip("tame_site");
        case UpkeepNeeds::TameSite::Clear:
            ++stop.tameClears;
            stop.tameClearing = true;
            WorldLog::Decision(
                bot, WorldLogFormat::DecisionKind::Upkeep, "tame", "clear", static_cast<float>(threats.count),
                {{"clears", static_cast<float>(stop.tameClears)}, {"threat_yd", bot->GetExactDist(threats.nearest)}},
                threats.nearest->GetEntry());
            AttackThreat(botAI, threats.nearest);
            return TameStep::Busy;
        case UpkeepNeeds::TameSite::Cast:
            break;
    }

    SpellCastResult const result = CastTame(bot, stop, beast);
    if (result != SPELL_CAST_OK)
    {
        WorldLog::Decision(bot, WorldLogFormat::DecisionKind::Upkeep, "skip", "tame_refused",
                           static_cast<float>(result), {{"tries", static_cast<float>(stop.tameTries)}},
                           beast->GetEntry());
        return TameStep::Done;
    }
    return TameStep::Busy;
}

// The stable visit of an errand stop (upkeep §3.11, WU21c): the pet need's swap form (recomputed here) stores the
// hunter's unwanted pet through the core's stable handlers, in an empty bought slot or in the next slot, bought first
// when the gold above the reserve pays it (the handler takes the price). The pet must be there and alive: a trip
// arrives mounted, and the core brings the pet back after the dismount (waited for up to STABLE_PET_WAIT_MS). Rows:
// upkeep stable/bought|free (value the copper paid; alternatives price, above = the gold above the reserve before the
// purchase, slots; ref the old pet's entry), or skip/stable_none (no swap need now), stable_gold (the gold above the
// reserve does not pay the slot), stable_pet (no live pet to store), stable_failed (a handler refused): the bot keeps
// its pet then. stored: the old pet's entry once in the stable
StableStep StableAtMaster(PlayerbotAI* botAI, Creature* master, NewRpgInfo::ErrandStop& stop, uint32& stored)
{
    Player* bot = botAI->GetBot();
    uint32 const now = getMSTime();
    if (!stop.stableSinceMs)
        stop.stableSinceMs = now;
    bool const waited = getMSTimeDiff(stop.stableSinceMs, now) >= UpkeepNeeds::STABLE_PET_WAIT_MS;
    if (bot->IsMounted() && !waited)
    {
        bot->RemoveAurasByType(SPELL_AURA_MOUNTED);
        return StableStep::Wait;
    }
    PetStable& stable = bot->GetOrInitPetStable();
    Pet* pet = bot->GetPet();
    bool const live = pet ? pet->IsAlive() && pet->getPetType() == HUNTER_PET
                          : stable.UnslottedPets.size() == 1 && stable.UnslottedPets[0].Health &&
                                stable.UnslottedPets[0].Type == HUNTER_PET;
    if (!live && stable.CurrentPet && !waited)
        return StableStep::Wait;

    std::shared_ptr<Planner::UpkeepIndex const> const index =
        Planner::UpkeepIndexReady() ? Planner::CurrentUpkeepIndex() : nullptr;
    UpkeepNeeds::NeedList needs;
    if (index)
        needs = ErrandNeeds(botAI, index)->needs;
    auto const swapNeed = std::find_if(needs.needs.begin(), needs.needs.end(), [](UpkeepNeeds::Need const& need)
                                       { return need.kind == UpkeepKind::Pet && need.swap; });
    UpkeepNeeds::Need const* swap = swapNeed != needs.needs.end() ? &*swapNeed : nullptr;
    uint64 const above = needs.money > needs.reserve ? needs.money - needs.reserve : 0;
    auto const skip = [bot, master, above](char const* reason, float value)
    {
        WorldLog::Decision(bot, WorldLogFormat::DecisionKind::Upkeep, "skip", reason, value,
                           {{"above", static_cast<float>(above)}, {"gold", static_cast<float>(bot->GetMoney())}},
                           master->GetEntry());
        return StableStep::Skipped;
    };
    if (!swap)
        return skip("stable_none", 0.0f);
    if (!swap->due)
        return skip("stable_gold", static_cast<float>(swap->copper));
    if (!live)
        return skip("stable_pet", 0.0f);

    uint32 const oldEntry = pet ? pet->GetEntry() : stable.UnslottedPets[0].CreatureId;
    uint32 const oldNumber =
        pet ? (stable.CurrentPet ? stable.CurrentPet->PetNumber : 0) : stable.UnslottedPets[0].PetNumber;
    bool freeSlot = false;
    for (uint8 slot = 0; slot < stable.MaxStabledPets && slot < MAX_PET_STABLES; ++slot)
        freeSlot = freeSlot || !stable.StabledPets[slot];
    uint32 const moneyBefore = bot->GetMoney();
    uint8 const slotsBefore = stable.MaxStabledPets;
    // The packets as the client sends them (the stable master's guid); the handlers check the master's reach
    if (!freeSlot)
    {
        WorldPacket buy(CMSG_BUY_STABLE_SLOT, 8);
        buy << master->GetGUID();
        bot->GetSession()->HandleBuyStableSlot(buy);
        if (bot->GetOrInitPetStable().MaxStabledPets <= slotsBefore)
            return skip("stable_failed", static_cast<float>(swap->copper));
    }
    WorldPacket store(CMSG_STABLE_PET, 8);
    store << master->GetGUID();
    bot->GetSession()->HandleStablePet(store);

    PetStable const& after = bot->GetOrInitPetStable();
    bool const inStable = std::any_of(after.StabledPets.begin(), after.StabledPets.end(),
                                      [oldNumber](Optional<PetStable::PetInfo> const& slot)
                                      { return slot && slot->PetNumber == oldNumber; });
    uint32 const paid = moneyBefore > bot->GetMoney() ? moneyBefore - bot->GetMoney() : 0;
    if (!oldNumber || !inStable || after.CurrentPet || bot->GetPet())
        return skip("stable_failed", static_cast<float>(paid));

    WorldLog::Decision(bot, WorldLogFormat::DecisionKind::Upkeep, "stable",
                       after.MaxStabledPets > slotsBefore ? "bought" : "free", static_cast<float>(paid),
                       {{"price", static_cast<float>(swap->copper)},
                        {"above", static_cast<float>(above)},
                        {"slots", static_cast<float>(after.MaxStabledPets)}},
                       oldEntry);
    stored = oldEntry;
    return StableStep::Stored;
}
}  // namespace NewRpgInternal

bool NewRpgErrandsAction::HoldTameChannel(PlayerbotAI* botAI)
{
    Player* bot = botAI->GetBot();
    auto* errands = std::get_if<NewRpgInfo::Errands>(&botAI->rpgInfo.data);
    NewRpgInfo::ErrandStop* stop =
        errands && errands->stop < errands->stops.size() ? &errands->stops[errands->stop] : nullptr;
    Spell const* channel = bot->GetCurrentSpell(CURRENT_CHANNELED_SPELL);
    if (!channel || channel->GetSpellInfo()->Id != UpkeepNeeds::SPELL_TAME_BEAST)
    {
        // No channel of Tame Beast: cast again in the fight of a beast that broke it
        if (!stop || !stop->tameEntry || channel || bot->GetCurrentSpell(CURRENT_GENERIC_SPELL) || !bot->IsInCombat())
            return false;
        return RecastTameInFight(bot, *stop);
    }

    // The channel runs: noted for the stop's channel_broken row
    if (stop)
        stop->tameSeenMs = getMSTime();
    if (!UpkeepNeeds::TameBreaksOff(bot->GetHealthPct()))
    {
        // The bot stands and channels: no swing or shot at the beast it tames
        if (bot->GetVictim())
            bot->AttackStop();
        if (bot->GetCurrentSpell(CURRENT_AUTOREPEAT_SPELL))
            bot->InterruptSpell(CURRENT_AUTOREPEAT_SPELL);
        return true;
    }

    Unit const* beast = channel->m_targets.GetUnitTarget();
    WorldLog::Decision(bot, WorldLogFormat::DecisionKind::Upkeep, "tame", "broke_off", bot->GetHealthPct(), {},
                       beast ? beast->GetEntry() : 0);
    bot->InterruptSpell(CURRENT_CHANNELED_SPELL);
    return false;
}
