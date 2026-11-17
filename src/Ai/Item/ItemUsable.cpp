/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "ItemUsable.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>

#include "AiObjectContext.h"
#include "Bag.h"
#include "BotKind.h"
#include "BotTraitMgr.h"
#include "CcTacticStrategy.h"
#include "CellImpl.h"
#include "Creature.h"
#include "DBCStores.h"
#include "DefensiveTacticStrategy.h"
#include "Engine.h"
#include "FleeRules.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Group.h"
#include "Item.h"
#include "ItemOfferRules.h"
#include "ItemSpendRules.h"
#include "ItemUse.h"
#include "ItemUseIndex.h"
#include "ItemWorthRules.h"
#include "Log.h"
#include "MoveSpline.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "ScriptMgr.h"
#include "SpellAuras.h"
#include "SpellInfo.h"
#include "TestConstraints.h"
#include "Timer.h"
#include "World.h"
#include "WorldLog.h"

namespace ItemUse
{
namespace
{
constexpr std::uint32_t SAMPLE_SHARE_MS = 50;  // the triggers of one AI tick share one sample
constexpr float BOOST_BALANCE = 50.0f;         // GenericBoostTrigger's default balance: at or below it the bot bursts
constexpr std::uint32_t OFFER_MS = 1000;       // the fight's offer is made again this often (a use: at once)
constexpr std::uint32_t BLOCK_MS = 10000;      // an entry Check refused is not offered again for this long
constexpr float FRIEND_YARDS = 40.0f;          // a group member this near is a unit of the situation
constexpr float VENDOR_YARDS = 60.0f;          // WI18 [E]: a repair vendor this near is "on the way": no repair gadget
constexpr std::uint32_t RECENTLY_BANDAGED = 11196;  // the engine's aura after a bandage: no bandage while it lasts

constexpr std::array<std::string_view, 5> ROLE_WORDS = {"tank", "healer", "melee", "ranged", "caster"};

std::string_view RoleWord(Role role)
{
    std::size_t const index = static_cast<std::size_t>(role);
    return index < ROLE_WORDS.size() ? ROLE_WORDS[index] : std::string_view("melee");
}

BotUsable& CacheOf(PlayerbotAI* botAI)
{
    if (!botAI->botUsable)
        botAI->botUsable = std::make_unique<BotUsable>();
    return *botAI->botUsable;
}

// The uses left of one stack (charges) or of the entry (the stacks' total)
std::uint32_t CountOf(Player* bot, Item* item, Record const& record, bool worn)
{
    if (record.charges)
        return static_cast<std::uint32_t>(std::abs(item->GetSpellCharges(record.spellIndex)));
    return worn ? 1 : bot->GetItemCount(item->GetEntry());
}

void Add(BotUsable& usable, Record const& record, ItemRef ref, Item* item, std::uint32_t count)
{
    Classified const classified = Classify(record);
    if (!Listed(classified, MixedTriggerSpell(record.spellId)))
        return;
    usable.entries.push_back(
        EntryOf(record, classified, ItemUseIndex::Get().IdOf(record), ref, count, CcIsPvpTrinket(item->GetTemplate())));
}

void CopyFight(BotUsable& usable)
{
    usable.fight.clear();
    for (UsableEntry const& entry : usable.entries)
        if (FightListed(entry))
            usable.fight.push_back(entry);
    usable.fightKinds = MaskOf(usable.fight);
}

void AddCarried(Player* bot, BotUsable& usable, Item* item, std::unordered_set<std::uint32_t>& seen)
{
    if (!item || !seen.insert(item->GetEntry()).second)
        return;
    ItemRef const ref{item->GetEntry(), 0, SLOT_BAGS};
    for (Record const& record : ItemUseIndex::Get().OfItem(item->GetEntry()))
        if (!record.inventoryType)  // a use that works only worn is not usable from the bags
            Add(usable, record, ref, item, CountOf(bot, item, record, false));
}

void Rebuild(Player* bot, BotUsable& usable)
{
    usable.entries.clear();
    ItemUseIndex const& index = ItemUseIndex::Get();
    for (std::uint8_t slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
    {
        Item* piece = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
        if (!piece)
            continue;
        ItemRef const ref{piece->GetEntry(), 0, slot};
        for (Record const& record : index.OfItem(piece->GetEntry()))
            Add(usable, record, ref, piece, CountOf(bot, piece, record, true));
        // a tinker: the enchantment's use spell, used through the piece (ItemUse::Worn)
        for (std::uint32_t enchantSlot = PERM_ENCHANTMENT_SLOT; enchantSlot < MAX_ENCHANTMENT_SLOT; ++enchantSlot)
            if (std::uint32_t const enchantId = piece->GetEnchantmentId(EnchantmentSlot(enchantSlot)))
                for (Record const& record : index.OfEnchant(enchantId))
                    Add(usable, record, ref, piece, 1);
    }

    std::unordered_set<std::uint32_t> seen;
    for (std::uint8_t slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
        AddCarried(bot, usable, bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot), seen);
    for (std::uint8_t bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
        if (Bag* bag = bot->GetBagByPos(bagSlot))
            for (std::uint32_t slot = 0; slot < bag->GetBagSize(); ++slot)
                AddCarried(bot, usable, bag->GetItemByPos(static_cast<std::uint8_t>(slot)), seen);

    usable.kinds = MaskOf(usable.entries);
    CopyFight(usable);
    // what the bot's time is worth (the spend rule's death cost): the level band's rate, as the errands read it
    usable.earnCopperPerMinute =
        sPlayerbotAIConfig.WorldTravel.SlotBands[WorldTravel::LevelBand(bot->GetLevel())].EarnCopperPerMinute;

    usable.thrift = 0.0f;
    usable.daring = 0.0f;
    if (std::optional<TraitRules::BotTraits> const traits = BotTraitMgr::instance().Find(bot->GetGUID().GetCounter()))
    {
        usable.thrift = SpendThrift(*traits);
        usable.daring = SpendDaring(*traits);
    }
    usable.dirty = false;
}

BotUsable* Fresh(PlayerbotAI* botAI)
{
    Player* bot = botAI ? botAI->GetBot() : nullptr;
    if (!bot)
        return nullptr;
    BotUsable& usable = CacheOf(botAI);
    if (usable.dirty)
    {
        Rebuild(bot, usable);
        usable.offerMs = 0;  // the offer's index is into the old list
    }
    return &usable;
}

Item* ItemOf(Player* bot, UsableEntry const& entry)
{
    if (entry.item.slot != SLOT_BAGS)
    {
        Item* piece = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, entry.item.slot);
        return piece && piece->GetEntry() == entry.item.entry ? piece : nullptr;
    }
    return bot->GetItemByEntry(entry.item.entry);
}

bool ReadyOf(Player* bot, UsableEntry const& entry)
{
    Record const* record = ItemUseIndex::Get().At(entry.record);
    Item* item = ItemOf(bot, entry);
    if (!record || !item || !entry.count)
        return false;
    // WI16: an item on the bot whose own effect is still on it waits for the effect to end (lab12 wi-survive: a worn
    // pendant's heal over time, ten charges, used ten times in 40 s)
    if (entry.shape == Shape::Self && record->spellId && bot->HasAura(record->spellId))
        return false;
    if (record->source != Source::EnchantSpell)
        return ReadyNow(bot, *record, item);
    // the tinker's record as the piece's, as Check takes it (ItemUse.cpp WornRecord)
    Record const piece =
        PieceRecord(*record, item->GetEntry(), static_cast<std::uint8_t>(item->GetTemplate()->InventoryType));
    return ReadyNow(bot, piece, item);
}

// The ready flags of the entries of the kinds; the fight copy follows
void RefreshReady(Player* bot, BotUsable& usable, KindMask kinds)
{
    if (!(usable.kinds & kinds))
        return;
    for (UsableEntry& entry : usable.entries)
        if (kinds & KindBit(entry.kind))
            entry.ready = ReadyOf(bot, entry);
    CopyFight(usable);
}

Rank RankOf(Unit* unit)
{
    if (unit->IsPlayer())
        return Rank::Player;
    Creature* creature = unit->ToCreature();
    if (!creature)
        return Rank::Normal;
    if (creature->isWorldBoss() || creature->IsDungeonBoss())
        return Rank::Boss;
    switch (creature->GetCreatureTemplate()->rank)
    {
        case CREATURE_ELITE_ELITE:
            return Rank::Elite;
        case CREATURE_ELITE_RAREELITE:
            return Rank::RareElite;
        case CREATURE_ELITE_WORLDBOSS:
            return Rank::Boss;
        case CREATURE_ELITE_RARE:
            return Rank::Rare;
        default:
            return Rank::Normal;
    }
}

Role RoleOf(Player* bot)
{
    if (PlayerbotAI::IsTank(bot))
        return Role::Tank;
    if (PlayerbotAI::IsHeal(bot))
        return Role::Healer;
    if (PlayerbotAI::IsCaster(bot))
        return Role::Caster;
    if (PlayerbotAI::IsRanged(bot))
        return Role::Ranged;
    return Role::Melee;
}

// A unit the bot sees, once per sample; false when the list is full, the unit is listed or out of sight (sightKnown:
// the caller's source checked it)
bool AddUnit(Situation& situation, Player* bot, Unit* unit, bool sightKnown = false)
{
    if (!unit || unit == bot || !unit->IsInWorld() || situation.unitCount >= MAX_NEAR)
        return false;
    std::uint64_t const guid = unit->GetGUID().GetRawValue();
    for (std::size_t i = 0; i < situation.unitCount; ++i)
        if (situation.units[i].guid == guid)
            return false;
    if (unit->GetMap() != bot->GetMap() || (!sightKnown && !bot->IsWithinLOSInMap(unit)))
        return false;

    NearUnit& listed = situation.units[situation.unitCount++];
    listed.guid = guid;
    listed.x = unit->GetPositionX();
    listed.y = unit->GetPositionY();
    listed.z = unit->GetPositionZ();
    listed.level = unit->GetLevel();
    listed.rank = RankOf(unit);
    listed.healthPct = unit->GetHealthPct();
    listed.health = unit->GetHealth();
    listed.inFight = unit->IsInCombat();
    listed.onBot = unit->GetVictim() == bot;
    listed.controlled = unit->HasBreakableByDamageCrowdControlAura();
    listed.flees = unit->HasUnitState(UNIT_STATE_FLEEING);
    listed.casts = unit->IsNonMeleeSpellCast(false);
    listed.friendly = bot->IsFriendlyTo(unit);
    return true;
}

// WI18: a member's body lies where it died (not released: a ghost stands at the graveyard)
bool LiesDead(Player* member) { return member->getDeathState() == DeathState::Corpse; }

// The group's members near; withDead (WI18: the fight list holds a resurrection) adds the bodies near too, a friendly
// unit with no health that only the resurrection need reads
void SampleGroup(Situation& situation, Player* bot, bool withDead)
{
    Group* group = bot->GetGroup();
    if (!group)
        return;
    situation.groupSize = static_cast<std::uint8_t>(std::min<std::uint32_t>(group->GetMembersCount(), 255));
    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (!member || member == bot || !member->IsInWorld() || member->GetMap() != bot->GetMap())
            continue;
        if (!member->IsAlive())
            ++situation.groupDead;
        else if (member->GetHealthPct() < HEAL_HEALTH_PCT)
            ++situation.groupHurt;
        if ((member->IsAlive() || (withDead && LiesDead(member))) && bot->IsWithinDistInMap(member, FRIEND_YARDS) &&
            AddUnit(situation, bot, member) && !member->IsAlive())
            situation.units[situation.unitCount - 1].healthPct = 0.0f;
    }
}

// WI18: the fight list holds a gadget of the role (a summon's sub); ready: one whose cooldown is over
bool HoldsSub(std::vector<UsableEntry> const& fight, Sub sub, bool ready)
{
    for (UsableEntry const& entry : fight)
        if (entry.kind == Kind::Summon && entry.sub == static_cast<std::uint8_t>(sub) && (entry.ready || !ready))
            return true;
    return false;
}

// WI18: a living repair vendor that is not hostile, within VENDOR_YARDS (a summoned repair bot counts: no second one)
struct RepairVendorCheck
{
    Player* bot;
    bool operator()(Creature* creature) const
    {
        return creature->IsAlive() && creature->HasNpcFlag(UNIT_NPC_FLAG_REPAIR) && !bot->IsHostileTo(creature) &&
               bot->IsWithinDistInMap(creature, VENDOR_YARDS);
    }
};

// A grid search that stops at the first vendor, with no sight test (lab12 WI18b: the town vendors stand indoors, out
// of the bot's sight, so the cached "nearest npcs" missed them); asked only with broken gear out of a fight and a
// ready repair gadget, at most once a second (the offer's sample)
bool VendorNear(Player* bot)
{
    Creature* found = nullptr;
    RepairVendorCheck check{bot};
    Acore::CreatureSearcher<RepairVendorCheck> searcher(bot, found, check);
    Cell::VisitObjects(bot, searcher, VENDOR_YARDS);
    return found != nullptr;
}

// WI17: how far from the bot an area item of the fight list can reach (its range and radius; a bot-centred one its
// radius); 0: none held
float AimReachOf(std::vector<UsableEntry> const& fight)
{
    float reach = 0.0f;
    for (UsableEntry const& entry : fight)
    {
        if (entry.kind != Kind::DamageArea || !(entry.radiusYards > 0.0f))
            continue;
        AimItem const item = AimItemOf(entry);
        float const range = item.selfCentred ? 0.0f : (item.onUnits ? OfferDetail::UnitReach(item.range) : item.range);
        reach = std::max(reach, range + item.radius);
    }
    return reach;
}

// WI17: the units near the bot within reach that are not friendly and not listed yet, nearest first while the list has
// room (the fight's units are listed first): a spot holding one that is not in the fight is forbidden, so nothing new
// is pulled. Source: the cached "nearest npcs" (players excluded, sight checked there, once a second); a critter, a
// totem or a unit the bot cannot attack is not pulled by a blast. Such a unit is in the fight only when its victim is
// the bot or a member of its group: a creature in another party's fight is a bystander here
void AddBystanders(Situation& situation, PlayerbotAI* botAI, Player* bot, float reach)
{
    std::array<std::pair<float, Unit*>, MAX_NEAR> nearest{};
    std::size_t count = 0;
    for (ObjectGuid const& guid : botAI->GetAiObjectContext()->GetValue<GuidVector>("nearest npcs")->Get())
    {
        Unit* unit = botAI->GetUnit(guid);
        if (!unit || !unit->IsInWorld() || !unit->IsAlive() || unit->IsTotem() ||
            unit->GetCreatureType() == CREATURE_TYPE_CRITTER || bot->IsFriendlyTo(unit) ||
            !bot->IsValidAttackTarget(unit))
            continue;
        float const yards = bot->GetExactDist(unit);
        if (yards > reach)
            continue;
        std::size_t at = count;
        while (at > 0 && nearest[at - 1].first > yards)
            --at;
        if (at >= MAX_NEAR)
            continue;
        for (std::size_t k = std::min(count, MAX_NEAR - 1); k > at; --k)
            nearest[k] = nearest[k - 1];
        nearest[at] = {yards, unit};
        count = std::min(count + 1, MAX_NEAR);
    }
    Group* group = bot->GetGroup();
    for (std::size_t k = 0; k < count && situation.unitCount < MAX_NEAR; ++k)
    {
        Unit* unit = nearest[k].second;
        if (!AddUnit(situation, bot, unit, true))
            continue;
        Unit* victim = unit->GetVictim();
        situation.units[situation.unitCount - 1].inFight =
            victim && (victim == bot || (group && victim->IsPlayer() && group->IsMember(victim->GetGUID())));
    }
}

// WI17: the sight of the candidate spots the pick needs, for each usable area item the fight wants (NextSightCheck:
// at most a few line-of-sight questions a sample, in the pick's order up to the best spot the bot sees); the question
// is the use path's own (ViewOfSpot), so Check does not refuse the pick for sight
void AimSight(Player* bot, Situation& situation, std::vector<UsableEntry> const& fight)
{
    if (!(WantedNeeds(situation) & NeedBit(Need::DamageArea)))
        return;
    bool aimed = false;
    std::uint32_t known = 0;
    std::uint32_t sight = 0;
    for (UsableEntry const& entry : fight)
    {
        if (!EntryAnswers(entry, Need::DamageArea) || !EntryUsable(situation, entry))
            continue;
        AimItem const item = AimItemOf(entry);
        if (item.selfCentred)
            continue;
        aimed = true;
        AimSpots const spots = AimCandidates(situation, item);
        for (std::int8_t k = NextSightCheck(spots, false, known, sight); k >= 0;
             k = NextSightCheck(spots, false, known, sight))
        {
            std::uint32_t const bit = std::uint32_t(1) << k;
            known |= bit;
            AimSpot const& spot = spots.spots[static_cast<std::size_t>(k)];
            if (bot->IsWithinLOS(spot.x, spot.y, spot.z, VMAP::ModelIgnoreFlags::M2, LINEOFSIGHT_ALL_CHECKS))
                sight |= bit;
        }
    }
    if (aimed)
        situation.aimSight = sight;
}

// The repair bill a death adds to the worn gear (the engine's durability loss on death, DurabilityLoss.OnDeath, of each
// piece's maximum, priced as StatsAction::EstRepair prices a point) and whether a piece is broken: O(19), once a
// sample. The bill already due is paid with or without a death, so the spend rule weighs only what the death adds
std::uint32_t RepairCopperOf(Player* bot, bool& broken)
{
    broken = false;
    std::uint32_t total = 0;
    float const deathLoss = std::clamp(sWorld->getRate(RATE_DURABILITY_LOSS_ON_DEATH) / 100.0f, 0.0f, 1.0f);
    for (std::uint8_t slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
    {
        Item* piece = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
        std::uint32_t const most = piece ? piece->GetUInt32Value(ITEM_FIELD_MAXDURABILITY) : 0;
        if (!most)
            continue;
        std::uint32_t const now = piece->GetUInt32Value(ITEM_FIELD_DURABILITY);
        if (!now)
            broken = true;
        std::uint32_t const lost = std::min(now, static_cast<std::uint32_t>(most * deathLoss));
        if (!lost)
            continue;
        ItemTemplate const* proto = piece->GetTemplate();
        DurabilityCostsEntry const* costs = sDurabilityCostsStore.LookupEntry(proto->ItemLevel);
        DurabilityQualityEntry const* quality = sDurabilityQualityStore.LookupEntry((proto->Quality + 1) * 2);
        if (!costs || !quality)
            continue;
        std::uint32_t const multiplier =
            costs->multiplier[ItemSubClassToDurabilityMultiplierId(proto->Class, proto->SubClass)];
        total += std::max<std::uint32_t>(
            1, static_cast<std::uint32_t>(lost * multiplier * static_cast<double>(quality->quality_mod)));
    }
    return total;
}

// The dispel types of the harmful auras on the bot; an aura bots leave on a friend (PlayerbotAI::dispel_whitelist, a
// Dreamless Sleep potion's: upstream #3024) is left on the bot too
std::uint32_t DispelMaskOf(PlayerbotAI* botAI, Player* bot)
{
    std::uint32_t mask = 0;
    for (auto const& applied : bot->GetAppliedAuras())
    {
        AuraApplication const* application = applied.second;
        if (!application || application->IsPositive())
            continue;
        SpellInfo const* spellInfo = application->GetBase()->GetSpellInfo();
        if (spellInfo && spellInfo->Dispel && spellInfo->Dispel < 32 && botAI->canDispel(spellInfo, spellInfo->Dispel))
            mask |= 1u << spellInfo->Dispel;
    }
    return mask;
}

Situation Sample(PlayerbotAI* botAI, Player* bot, BotUsable& usable, std::uint32_t nowMs)
{
    Situation situation;
    situation.level = bot->GetLevel();
    situation.role = RoleOf(bot);
    situation.healthPct = bot->GetHealthPct();
    situation.usesMana = bot->GetMaxPower(POWER_MANA) > 0;
    situation.manaPct = situation.usesMana ? bot->GetPowerPct(POWER_MANA) : 100.0f;
    situation.maxHealth = bot->GetMaxHealth();
    situation.inFight = bot->IsInCombat();
    situation.fightEnded = usable.lastInFight && !situation.inFight;
    if (situation.inFight && !usable.lastInFight)
        ++usable.fightSerial;  // a new fight: its item_spend rows begin again
    usable.lastInFight = situation.inFight;
    if (situation.inFight)
        usable.health.Push(nowMs, bot->GetHealth());
    else
        usable.health.Clear();
    situation.healthLossPerSecond = usable.health.LossPerSecond();
    situation.x = bot->GetPositionX();
    situation.y = bot->GetPositionY();
    situation.z = bot->GetPositionZ();
    situation.inArena = bot->InArena();

    // the units: the current target first, the attackers, then the group's members near
    AiObjectContext* context = botAI->GetAiObjectContext();
    Unit* target = context->GetValue<Unit*>("current target")->Get();
    if (AddUnit(situation, bot, target))
        situation.target = 0;
    for (ObjectGuid const& guid : context->GetValue<GuidVector>("attackers")->Get())
        AddUnit(situation, bot, botAI->GetUnit(guid));
    // the players in PvP combat with the bot, FleeAttackersAction::Foes's source: a caster, or the pack behind a
    // fleeing bot (its target cleared), is in no "attackers" list (WI16b: a flight sampled no enemy, so "won")
    for (auto const& [guid, ref] : bot->GetCombatManager().GetPvPCombatRefs())
    {
        Unit* const other = ref ? ref->GetOther(bot) : nullptr;
        if (other && other->IsAlive() && !other->IsTotem())
            AddUnit(situation, bot, other);
    }
    SampleGroup(situation, bot, (usable.fightKinds & KindBit(Kind::Resurrect)) != 0);
    // WI17: the units an area item could pull, only in a fight with one held
    if (situation.inFight)
        if (float const reach = AimReachOf(usable.fight); reach > 0.0f)
            AddBystanders(situation, botAI, bot, reach);

    // GenericBoostTrigger's rule (BurstsNow): against a player the defensive tactic's hold decides, else the balance
    situation.bursts = BurstsNow(botAI, target, BOOST_BALANCE, &situation.holdsBurst);
    situation.stake = target && target->ToPlayer() ? Stake::Player : Stake::Ordinary;

    situation.dispelMask = DispelMaskOf(botAI, bot);
    situation.rooted = bot->HasUnitState(UNIT_STATE_ROOT);
    situation.snared = bot->HasAuraType(SPELL_AURA_MOD_DECREASE_SPEED);
    situation.potionDrunk = bot->GetLastPotionId() != 0;
    situation.thrift = usable.thrift;
    situation.daring = usable.daring;
    situation.earnCopperPerMinute = usable.earnCopperPerMinute;
    situation.repairCopper = RepairCopperOf(bot, situation.gearBroken);
    // WI18: the vendor only when a repair gadget could be wanted; chases stays unset on purpose (a fleeing enemy is
    // NearUnit::flees)
    situation.vendorNear =
        situation.gearBroken && !situation.inFight && HoldsSub(usable.fight, Sub::Repair, true) && VendorNear(bot);
    FleeRules::Flight const& flight = context->GetValue<FleeRules::Flight&>("flee flight")->Get();
    SetFlight(situation, flight.active, FleeRules::Cornered(flight));
    situation.moving = bot->isMoving() || !bot->movespline->Finalized();
    situation.recentlyBandaged = bot->HasAura(RECENTLY_BANDAGED);
    situation.control = !TestConstraints::SpendsOnItsOwn(botAI->testConstraints) ||
                        (BotKind::IsPersistentBot(bot) &&
                         InControlCohort(bot->GetGUID().GetCounter(), sPlayerbotAIConfig.ItemUse.ControlShare));
    return situation;
}

std::string NeedWords(NeedMask mask)
{
    std::string words;
    for (std::size_t n = 0; n < NEED_COUNT; ++n)
        if (mask & NeedBit(static_cast<Need>(n)))
        {
            if (!words.empty())
                words += ',';
            words += NeedWord(static_cast<Need>(n));
        }
    return words.empty() ? "none" : words;
}
}  // namespace

bool MixedTriggerSpell(std::uint32_t spellId)
{
    static std::unordered_set<std::uint32_t> const mixed = []()
    {
        std::unordered_set<std::uint32_t> onUse;
        std::unordered_set<std::uint32_t> onEquip;
        std::unordered_set<std::uint32_t> both;
        ItemTemplateContainer const* templates = sObjectMgr->GetItemTemplateStore();
        if (!templates)
            return both;
        for (auto const& [entry, proto] : *templates)
        {
            if (proto.InventoryType != INVTYPE_TRINKET)
                continue;
            for (std::uint8_t index = 0; index < MAX_ITEM_PROTO_SPELLS; ++index)
            {
                _Spell const& spell = proto.Spells[index];
                if (spell.SpellId <= 0)
                    continue;
                std::uint32_t const id = static_cast<std::uint32_t>(spell.SpellId);
                if (spell.SpellTrigger == ITEM_SPELLTRIGGER_ON_USE)
                {
                    if (onEquip.count(id))
                        both.insert(id);
                    onUse.insert(id);
                }
                else if (spell.SpellTrigger == ITEM_SPELLTRIGGER_ON_EQUIP)
                {
                    if (onUse.count(id))
                        both.insert(id);
                    onEquip.insert(id);
                }
            }
        }
        return both;
    }();
    return mixed.count(spellId) != 0;
}

std::span<UsableEntry const> UsableOf(Player* bot)
{
    BotUsable* usable = Fresh(bot ? GET_PLAYERBOT_AI(bot) : nullptr);
    return usable ? std::span<UsableEntry const>(usable->entries) : std::span<UsableEntry const>();
}

std::span<UsableEntry const> FightUsableOf(Player* bot)
{
    BotUsable* usable = Fresh(bot ? GET_PLAYERBOT_AI(bot) : nullptr);
    return usable ? std::span<UsableEntry const>(usable->fight) : std::span<UsableEntry const>();
}

KindMask UsableKindsOf(Player* bot)
{
    BotUsable* usable = Fresh(bot ? GET_PLAYERBOT_AI(bot) : nullptr);
    return usable ? usable->kinds : 0;
}

KindMask FightKindsOf(PlayerbotAI* botAI)
{
    BotUsable* usable = Fresh(botAI);
    return usable ? usable->fightKinds : 0;
}

bool FightHoldsRepair(PlayerbotAI* botAI)
{
    BotUsable* usable = Fresh(botAI);
    return usable && (usable->fightKinds & KindBit(Kind::Summon)) && HoldsSub(usable->fight, Sub::Repair, false);
}

void MarkUsableDirty(PlayerbotAI* botAI)
{
    if (botAI && botAI->botUsable)
    {
        botAI->botUsable->dirty = true;
        botAI->botUsable->offerMs = 0;
    }
}

bool BurstsNow(PlayerbotAI* botAI, Unit* target, float balance, bool* holds)
{
    if (target && target->ToPlayer())
    {
        bool const hold = DefensiveHoldsBurst(botAI, target);
        if (holds)
            *holds = hold;
        return !hold;
    }
    if (holds)
        *holds = false;
    return botAI->GetAiObjectContext()->GetValue<uint8>("balance")->Get() <= balance;
}

namespace
{
// The item_spend row (world-log.md): one per fight and need at the first verdict, again when the verdict or its
// reason changes; the offer's 1 s cache bounds the rate, nothing is formatted while the world log is off
void NoteSpend(Player* bot, BotUsable& usable, Offer const& offer)
{
    if (!offer.consumed || offer.usable >= usable.fight.size())
        return;
    if (usable.spendFight != usable.fightSerial)
    {
        usable.spendFight = usable.fightSerial;
        usable.spendNoted.fill(0);
    }
    SpendVerdict const& verdict = offer.verdict;
    std::uint8_t const mark =
        static_cast<std::uint8_t>((verdict.spend ? 0x80 : 0) | (static_cast<std::uint8_t>(verdict.reason) + 1));
    std::uint8_t& noted = usable.spendNoted[static_cast<std::size_t>(offer.need)];
    if (noted == mark)
        return;
    noted = mark;
    if (!WorldLog::Active())
        return;
    UsableEntry const& entry = usable.fight[offer.usable];
    WorldLog::Decision(
        bot, WorldLogFormat::DecisionKind::ItemSpend, SpendDecision(entry.kind, verdict),
        SpendReasonWord(verdict.reason), verdict.savesCopper - verdict.costCopper,
        {{"saves", verdict.savesCopper}, {"cost", verdict.costCopper}, {"danger", verdict.danger * 100.0f}},
        entry.item.entry);
}
}  // namespace

Offer const& FightOffer(PlayerbotAI* botAI)
{
    static Offer const none{};
    BotUsable* usable = Fresh(botAI);
    if (!usable)
        return none;
    std::uint32_t const nowMs = getMSTime();
    if (usable->offerMs && getMSTimeDiff(usable->offerMs, nowMs) < OFFER_MS)
        return usable->offer;
    Situation const situation = SampleSituation(botAI);
    std::erase_if(usable->blocked,
                  [nowMs](BotUsable::Blocked const& b) { return static_cast<std::int32_t>(b.untilMs - nowMs) <= 0; });
    for (BotUsable::Blocked const& b : usable->blocked)
        for (UsableEntry& entry : usable->fight)
            if (entry.record == b.record && entry.item.entry == b.entry)
                entry.ready = false;
    usable->offer = MakeOffer(situation, usable->fight);
    usable->offerMs = nowMs ? nowMs : 1;
    NoteSpend(botAI->GetBot(), *usable, usable->offer);
    return usable->offer;
}

Request FightRequest(Player* bot, Offer const& offer, UsableEntry const& entry)
{
    Request request = entry.item.slot != SLOT_BAGS ? Worn(bot, entry.item.slot, Caller::Fight) : Request{};
    request.item.entry = entry.item.entry;
    request.caller = Caller::Fight;
    Record const* record = ItemUseIndex::Get().At(entry.record);
    request.spellId = record ? record->spellId : 0;
    request.target = offer.target;
    return request;
}

void BlockOffered(PlayerbotAI* botAI, UsableEntry const& entry, Cause cause)
{
    if (!botAI || !botAI->botUsable)
        return;
    BotUsable& usable = *botAI->botUsable;
    std::uint32_t const untilMs = getMSTime() + BLOCK_MS;
    for (BotUsable::Blocked& b : usable.blocked)
        if (b.record == entry.record && b.entry == entry.item.entry)
        {
            b.untilMs = untilMs;
            b.cause = cause;
            usable.offerMs = 0;
            return;
        }
    usable.blocked.push_back({entry.record, entry.item.entry, untilMs, cause});
    usable.offerMs = 0;
}

UsableEntry const* OfferedEntry(PlayerbotAI* botAI, Offer const& offer)
{
    if (!offer.any || !botAI || !botAI->botUsable)
        return nullptr;
    std::vector<UsableEntry> const& fight = botAI->botUsable->fight;
    return offer.usable < fight.size() ? &fight[offer.usable] : nullptr;
}

Situation SampleSituation(PlayerbotAI* botAI)
{
    BotUsable* usable = Fresh(botAI);
    if (!usable)
        return Situation{};
    std::uint32_t const nowMs = getMSTime();
    if (usable->sampledMs && getMSTimeDiff(usable->sampledMs, nowMs) < SAMPLE_SHARE_MS)
        return usable->sample;
    Player* bot = botAI->GetBot();
    usable->sample = Sample(botAI, bot, *usable, nowMs);
    usable->sampledMs = nowMs ? nowMs : 1;
    RefreshReady(bot, *usable, KindsOf(WantedNeeds(usable->sample)));
    AimSight(bot, usable->sample, usable->fight);
    return usable->sample;
}

namespace
{
// playerbots item spend: the spend rule's inputs and verdict for each consumed candidate of a wanted need, by the
// offer's own functions (console only)
void SpendPrints(std::string const& name, Situation const& situation, NeedMask wanted,
                 std::vector<UsableEntry> const& fight, Offer const& offer)
{
    float const danger = Danger(situation);
    LOG_INFO("playerbots",
             "ITEM spend: {} danger {:.2f} earn {}/min repair {} death {} thrift {:.2f} daring {:.2f} control {} "
             "potion {} won {} flees {} cornered {} moving {} bandaged {} broken {}",
             name, danger, situation.earnCopperPerMinute, situation.repairCopper,
             DeathCopper(situation.earnCopperPerMinute, situation.repairCopper), situation.thrift, situation.daring,
             situation.control ? 1 : 0, situation.potionDrunk ? 1 : 0, OfferDetail::Won(situation) ? 1 : 0,
             situation.flees ? 1 : 0, situation.cornered ? 1 : 0, situation.moving ? 1 : 0,
             situation.recentlyBandaged ? 1 : 0, situation.gearBroken ? 1 : 0);
    std::size_t weighed = 0;
    for (std::size_t n = 0; n < NEED_COUNT; ++n)
    {
        Need const need = static_cast<Need>(n);
        if (!(wanted & NeedBit(need)))
            continue;
        bool freeReady = false;
        for (UsableEntry const& entry : fight)
        {
            Target target;
            std::uint8_t hits = 1;
            freeReady = freeReady || (!entry.consumed && EntryUsable(situation, entry) && EntryAnswers(entry, need) &&
                                      AnswerTarget(situation, entry, need, target, hits));
        }
        for (std::size_t i = 0; i < fight.size(); ++i)
        {
            UsableEntry const& entry = fight[i];
            if (!entry.consumed || !EntryAnswers(entry, need))
                continue;
            Target target;
            std::uint8_t hits = 1;
            bool const usableNow = EntryUsable(situation, entry);
            bool const aimed = AnswerTarget(situation, entry, need, target, hits);
            SpendVerdict const verdict = Spend(SpendCaseFor(situation, entry, danger, hits, freeReady));
            ++weighed;
            LOG_INFO("playerbots",
                     "ITEM spend: {} need {} #{} record {} entry {} kind {} rank {} potion {} usable {} aimed {} "
                     "freeReady {} replace {} -> {} {} saves {:.0f} cost {:.0f}",
                     name, NeedWord(need), i, entry.record, entry.item.entry, KindWord(entry.kind), entry.rank,
                     entry.potion ? 1 : 0, usableNow ? 1 : 0, aimed ? 1 : 0, freeReady ? 1 : 0, entry.replaceCopper,
                     SpendDecision(entry.kind, verdict), SpendReasonWord(verdict.reason), verdict.savesCopper,
                     verdict.costCopper);
        }
    }
    bool const named = offer.usable < fight.size();
    LOG_INFO("playerbots", "ITEM spend: {} {} weighed, offer any {} need {} entry {} consumed {} verdict {} {}", name,
             weighed, offer.any ? 1 : 0, NeedWord(offer.need), named ? fight[offer.usable].item.entry : 0,
             offer.consumed ? 1 : 0,
             offer.consumed && named ? SpendDecision(fight[offer.usable].kind, offer.verdict) : std::string("-"),
             offer.consumed ? SpendReasonWord(offer.verdict.reason) : std::string_view("-"));
}
}  // namespace

void TakeUsablePrints(PlayerbotAI* botAI, Player* bot)
{
    botAI->EndWork(BotWork::ItemUsable);
    BotUsable* usable = Fresh(botAI);
    if (!usable)
        return;
    usable->sampledMs = 0;  // a fresh sample for the print
    Situation const situation = SampleSituation(botAI);
    RefreshReady(bot, *usable, ~KindMask(0));

    ItemUseIndex const& index = ItemUseIndex::Get();
    std::string const name = bot->GetName();
    LOG_INFO("playerbots", "ITEM usable: {} {} entries, {} in the fight list", name, usable->entries.size(),
             usable->fight.size());
    for (std::size_t i = 0; i < usable->entries.size(); ++i)
    {
        UsableEntry const& entry = usable->entries[i];
        LOG_INFO("playerbots",
                 "ITEM usable: {} #{} record {} '{}' entry {} slot {} kind {}/{} rank {} count {} ready {} consumed {} "
                 "shape {} radius {:.1f} range {:.1f} min {:.1f} friend {:.1f} pvp {}",
                 name, i, entry.record, index.NameOf(entry.record), entry.item.entry,
                 entry.item.slot == SLOT_BAGS ? -1 : static_cast<int>(entry.item.slot), KindWord(entry.kind), entry.sub,
                 entry.rank, entry.count, entry.ready ? 1 : 0, entry.consumed ? 1 : 0, ShapeWord(entry.shape),
                 entry.radiusYards, entry.rangeYards, entry.minRangeYards, entry.rangeFriendYards,
                 entry.pvpTrinket ? 1 : 0);
    }

    NeedMask const wanted = WantedNeeds(situation);
    LOG_INFO("playerbots",
             "ITEM usable: {} situation level {} role {} health {:.0f} mana {:.0f} usesMana {} loss {:.1f}/s fight {} "
             "ended {} bursts {} holds {} arena {} stake {} group {}/{}/{} dispel {:#x} rooted {} snared {} potion {} "
             "thrift {:.2f} daring {:.2f} control {} units {} target {} wanted {} sight {:#x}",
             name, situation.level, RoleWord(situation.role), situation.healthPct, situation.manaPct,
             situation.usesMana ? 1 : 0, situation.healthLossPerSecond, situation.inFight ? 1 : 0,
             situation.fightEnded ? 1 : 0, situation.bursts ? 1 : 0, situation.holdsBurst ? 1 : 0,
             situation.inArena ? 1 : 0, static_cast<int>(situation.stake), situation.groupSize, situation.groupHurt,
             situation.groupDead, situation.dispelMask, situation.rooted ? 1 : 0, situation.snared ? 1 : 0,
             situation.potionDrunk ? 1 : 0, situation.thrift, situation.daring, situation.control ? 1 : 0,
             situation.unitCount, situation.target, NeedWords(wanted), situation.aimSight);
    // what the bot's own AI does now (WI16b: a hurt bot after its fight that never ate)
    Engine* engine = botAI->GetCurrentEngine();
    LOG_INFO("playerbots",
             "ITEM usable: {} state {} combat {} pvp refs {} mounted {} stand {} food strategy {} food count {} "
             "work {:#x} last action '{}'",
             name, static_cast<int>(botAI->GetState()), bot->IsInCombat() ? 1 : 0,
             bot->GetCombatManager().GetPvPCombatRefs().size(), bot->IsMounted() ? 1 : 0,
             static_cast<int>(bot->getStandState()), botAI->HasStrategy("food", BOT_STATE_NON_COMBAT) ? 1 : 0,
             botAI->GetAiObjectContext()->GetValue<uint32>("item count", "food")->Get(), botAI->GetWork(),
             engine ? engine->GetLastAction() : std::string("-"));
    for (std::size_t i = 0; i < situation.unitCount; ++i)
    {
        NearUnit const& unit = situation.units[i];
        LOG_INFO("playerbots",
                 "ITEM usable: {} unit #{} guid {} level {} rank {} health {:.0f} fight {} onBot {} controlled {} "
                 "flees {} casts {} friendly {} yards {:.1f}",
                 name, i, ObjectGuid(unit.guid).GetCounter(), unit.level, static_cast<int>(unit.rank), unit.healthPct,
                 unit.inFight ? 1 : 0, unit.onBot ? 1 : 0, unit.controlled ? 1 : 0, unit.flees ? 1 : 0,
                 unit.casts ? 1 : 0, unit.friendly ? 1 : 0, bot->GetExactDist(unit.x, unit.y, unit.z));
    }

    std::vector<UsableEntry> const& fight = usable->fight;
    Offer const offer = MakeOffer(situation, fight);
    // the use path's answer to the offer's request now (none: the action would send it)
    std::string_view check = "-";
    if (offer.any && offer.usable < fight.size())
        check = CauseWord(Check(bot, FightRequest(bot, offer, fight[offer.usable])));
    LOG_INFO("playerbots",
             "ITEM usable: {} offer any {} need {} usable {} entry {} target {} {} consumed {} check {} blocked {}",
             name, offer.any ? 1 : 0, NeedWord(offer.need), offer.usable,
             offer.any && offer.usable < fight.size() ? fight[offer.usable].item.entry : 0,
             static_cast<int>(offer.target.kind), ObjectGuid(offer.target.guid).GetCounter(), offer.consumed ? 1 : 0,
             check, usable->blocked.size());
    for (BotUsable::Blocked const& b : usable->blocked)
        LOG_INFO("playerbots", "ITEM usable: {} blocked entry {} record {} cause {}", name, b.entry, b.record,
                 CauseWord(b.cause));
    SpendPrints(name, situation, wanted, fight, offer);
}
}  // namespace ItemUse

namespace
{
void MarkDirty(Player* player)
{
    if (PlayerbotAI* botAI = player ? GET_PLAYERBOT_AI(player) : nullptr)
        ItemUse::MarkUsableDirty(botAI);
}

// The usable list's rebuild marks: O(1) each, only bots have a list
class ItemUsablePlayerScript : public PlayerScript
{
public:
    ItemUsablePlayerScript()
        : PlayerScript("ItemUsablePlayerScript",
                       {PLAYERHOOK_ON_EQUIP, PLAYERHOOK_ON_UNEQUIP_ITEM, PLAYERHOOK_ON_STORE_NEW_ITEM,
                        PLAYERHOOK_ON_AFTER_MOVE_ITEM_FROM_INVENTORY, PLAYERHOOK_ON_AFTER_MOVE_ITEM_TO_INVENTORY,
                        PLAYERHOOK_CAN_APPLY_ENCHANTMENT, PLAYERHOOK_ON_LEVEL_CHANGED})
    {
    }

    void OnPlayerEquip(Player* player, Item* /*it*/, uint8 /*bag*/, uint8 /*slot*/, bool /*update*/) override
    {
        MarkDirty(player);
    }

    void OnPlayerUnequip(Player* player, Item* /*it*/) override { MarkDirty(player); }

    void OnPlayerStoreNewItem(Player* player, Item* /*item*/, uint32 /*count*/) override { MarkDirty(player); }

    void OnPlayerAfterMoveItemFromInventory(Player* player, Item* /*it*/, uint8 /*bag*/, uint8 /*slot*/,
                                            bool /*update*/) override
    {
        MarkDirty(player);
    }

    void OnPlayerAfterMoveItemToInventory(Player* player, Item* /*it*/, bool /*update*/) override { MarkDirty(player); }

    // Player::ApplyEnchantment asks this for every enchantment slot and type (a tinker too); the answer is always yes
    bool OnPlayerCanApplyEnchantment(Player* player, Item* /*item*/, EnchantmentSlot /*slot*/, bool /*apply*/,
                                     bool /*apply_dur*/, bool /*ignore_condition*/) override
    {
        MarkDirty(player);
        return true;
    }

    void OnPlayerLevelChanged(Player* player, uint8 /*oldlevel*/) override { MarkDirty(player); }
};
}  // namespace

void AddItemUsableScripts() { new ItemUsablePlayerScript(); }
