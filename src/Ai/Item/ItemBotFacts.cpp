/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "ItemBotFacts.h"

#include <algorithm>
#include <memory>
#include <span>

#include "BotKind.h"
#include "DBCStores.h"
#include "Item.h"
#include "ItemUse.h"
#include "ItemUseConfig.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "StatsWeightCalculator.h"
#include "TestConstraints.h"

namespace ItemUse
{
BotItems& ItemsOf(PlayerbotAI* botAI)
{
    if (!botAI->botItems)
        botAI->botItems = std::make_unique<BotItems>();
    return *botAI->botItems;
}

Role RoleOf(Player* bot)
{
    if (PlayerbotAI::IsHeal(bot))
        return Role::Healer;
    if (PlayerbotAI::IsCaster(bot))
        return Role::Caster;
    if (PlayerbotAI::IsTank(bot))
        return Role::Tank;
    if (PlayerbotAI::IsMelee(bot))
        return Role::Melee;
    return Role::Ranged;
}

// The calculator makes its weights only while it values an item: any held buff serves
BuffWeights WeightsOf(Player* bot, Role role, std::uint32_t anyEntry)
{
    std::span<float const> const fixed = RoleWeights(role);
    BuffWeights weights{};
    std::copy_n(fixed.begin(), std::min(fixed.size(), weights.size()), weights.begin());
    if ((role != Role::Melee && role != Role::Ranged) || !anyEntry)
        return weights;

    StatsWeightCalculator calculator(bot);
    calculator.CalculateItem(anyEntry);
    float const* stats = calculator.GetStatWeights();
    float const attackPower = stats[STATS_TYPE_ATTACK_POWER];
    if (!(attackPower > 0.0f))
        return weights;
    weights[static_cast<std::size_t>(BuffStat::Strength)] = std::max(stats[STATS_TYPE_STRENGTH], 0.0f) / attackPower;
    weights[static_cast<std::size_t>(BuffStat::Agility)] = std::max(stats[STATS_TYPE_AGILITY], 0.0f) / attackPower;
    return weights;
}

bool InControl(PlayerbotAI* botAI, Player* bot) { return !TestConstraints::SpendsOnItsOwn(botAI->testConstraints); }

namespace
{
std::uint32_t EnchantOf(Record const& record)
{
    std::uint32_t enchantId = 0;
    for (std::size_t i = 0; i < record.effectCount && i < MAX_EFFECTS; ++i)
        if (record.effects[i].effect == SPELL_EFFECT_ENCHANT_ITEM_TEMPORARY)
            enchantId = static_cast<std::uint32_t>(record.effects[i].misc);
    return enchantId;
}
}  // namespace

// No names, no ids: the spell data says it
PoisonKind PoisonKindOf(Record const& record)
{
    std::uint32_t const enchantId = EnchantOf(record);
    SpellItemEnchantmentEntry const* enchant = enchantId ? sSpellItemEnchantmentStore.LookupEntry(enchantId) : nullptr;
    if (!enchant)
        return PoisonKind::None;
    PoisonKind kind = PoisonKind::None;
    for (std::uint8_t i = 0; i < MAX_SPELL_ITEM_ENCHANTMENT_EFFECTS; ++i)
    {
        if (enchant->type[i] != ITEM_ENCHANTMENT_TYPE_COMBAT_SPELL)
            continue;
        SpellInfo const* spell = sSpellMgr->GetSpellInfo(enchant->spellid[i]);
        if (!spell)
            continue;
        for (std::uint8_t e = 0; e < MAX_SPELL_EFFECTS; ++e)
        {
            if (spell->Effects[e].ApplyAuraName == SPELL_AURA_PERIODIC_DAMAGE)
                return PoisonKind::Deadly;
            if (spell->Effects[e].Effect == SPELL_EFFECT_SCHOOL_DAMAGE)
                kind = PoisonKind::Instant;
        }
    }
    return kind;
}

// Damage a hit counts as attack power, a stat entry by its item mod type, an equip spell by its auras
BuffWeights CoatingStats(Record const& record)
{
    BuffWeights stats{};
    std::uint32_t const enchantId = EnchantOf(record);
    SpellItemEnchantmentEntry const* enchant = enchantId ? sSpellItemEnchantmentStore.LookupEntry(enchantId) : nullptr;
    if (!enchant)
        return stats;

    for (std::uint8_t i = 0; i < MAX_SPELL_ITEM_ENCHANTMENT_EFFECTS; ++i)
    {
        float const amount = static_cast<float>(enchant->amount[i]);
        switch (enchant->type[i])
        {
            case ITEM_ENCHANTMENT_TYPE_DAMAGE:
                BuffDetail::Add(stats, BuffStat::AttackPower, amount);
                break;
            case ITEM_ENCHANTMENT_TYPE_STAT:
                AddModTypeStat(stats, enchant->spellid[i], amount);
                break;
            case ITEM_ENCHANTMENT_TYPE_EQUIP_SPELL:
                if (SpellInfo const* spell = sSpellMgr->GetSpellInfo(enchant->spellid[i]))
                {
                    Record aura;
                    for (std::uint8_t e = 0; e < MAX_SPELL_EFFECTS && aura.effectCount < MAX_EFFECTS; ++e)
                        if (spell->Effects[e].ApplyAuraName)
                        {
                            EffectFact& fact = aura.effects[aura.effectCount++];
                            fact.aura = static_cast<std::uint16_t>(spell->Effects[e].ApplyAuraName);
                            fact.amount = spell->Effects[e].CalcValue();
                            fact.misc = spell->Effects[e].MiscValue;
                        }
                    BuffWeights const extra = BuffStats(aura);
                    for (std::size_t s = 0; s < stats.size(); ++s)
                        stats[s] += extra[s];
                }
                break;
            default:
                break;
        }
    }
    return stats;
}

TargetView ViewOfWeapon(Item* weapon)
{
    TargetView view;
    view.kind = TargetKind::OwnItem;
    ItemTemplate const* proto = weapon ? weapon->GetTemplate() : nullptr;
    view.found = proto != nullptr;
    if (!proto)
        return view;
    view.itemClass = static_cast<std::int32_t>(proto->Class);
    view.itemSubClass = proto->SubClass;
    view.itemInvType = proto->InventoryType;
    view.itemLevel = static_cast<std::uint16_t>(proto->ItemLevel);
    view.requiredLevel = static_cast<std::uint16_t>(proto->RequiredLevel);
    view.itemHasCoating = weapon->GetEnchantmentId(TEMP_ENCHANTMENT_SLOT) != 0;
    return view;
}
}  // namespace ItemUse
