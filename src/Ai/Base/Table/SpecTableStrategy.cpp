/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "SpecTableStrategy.h"

#include <array>
#include <string_view>

#include "AiObjectContext.h"
#include "Chat.h"
#include "GenericSpellActions.h"
#include "Item.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "StringFormat.h"
#include "Timer.h"
#include "Trigger.h"
#include "Value.h"

namespace
{
constexpr std::array<std::string_view, 4> ROLE_WORDS = {"melee", "ranged", "healer", "tank"};

bool Knows(AiObjectContext* context, std::string const& spell)
{
    return context->GetValue<uint32>("spell id", spell)->Get() != 0;
}

ItemTemplate const* Equipped(Player* bot, uint8 slot)
{
    Item* const item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
    return item ? item->GetTemplate() : nullptr;
}

bool Wields(Player* bot, spec::Gear gear)
{
    switch (gear)
    {
        case spec::Gear::OffHandWeapon:
        {
            ItemTemplate const* const item = Equipped(bot, EQUIPMENT_SLOT_OFFHAND);
            return item && item->Class == ITEM_CLASS_WEAPON;
        }
        case spec::Gear::Shield:
        {
            ItemTemplate const* const item = Equipped(bot, EQUIPMENT_SLOT_OFFHAND);
            return item && item->Class == ITEM_CLASS_ARMOR && item->SubClass == ITEM_SUBCLASS_ARMOR_SHIELD;
        }
        case spec::Gear::TwoHand:
        {
            ItemTemplate const* const item = Equipped(bot, EQUIPMENT_SLOT_MAINHAND);
            return item && item->InventoryType == INVTYPE_2HWEAPON;
        }
        case spec::Gear::Wand:
        {
            ItemTemplate const* const item = Equipped(bot, EQUIPMENT_SLOT_RANGED);
            return item && item->Class == ITEM_CLASS_WEAPON && item->SubClass == ITEM_SUBCLASS_WEAPON_WAND;
        }
        case spec::Gear::Ranged:
        {
            ItemTemplate const* const item = Equipped(bot, EQUIPMENT_SLOT_RANGED);
            return item && item->Class == ITEM_CLASS_WEAPON &&
                   (item->SubClass == ITEM_SUBCLASS_WEAPON_BOW || item->SubClass == ITEM_SUBCLASS_WEAPON_GUN ||
                    item->SubClass == ITEM_SUBCLASS_WEAPON_CROSSBOW || item->SubClass == ITEM_SUBCLASS_WEAPON_THROWN);
        }
        default:
            return false;
    }
}
}  // namespace

SpecTableStrategy::SpecTableStrategy(PlayerbotAI* botAI, spec::TableView table)
    : CombatStrategy(botAI),
      table(table),
      name(table.name),
      standing(table.count, spec::Standing::Known),
      counts(table.count)
{
}

void SpecTableStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    CombatStrategy::InitTriggers(triggers);
    RoleNodes(table.role, triggers);
    for (spec::Node const& node : spec::Nodes(table))
        triggers.push_back(new TriggerNode(node.trigger, {NextAction(std::string(node.action), node.relevance, true)}));

    // Only a mark: CF4's name check runs this on a throwaway context with the bot's real PlayerbotAI
    rebuilt = true;
}

spec::Standing SpecTableStrategy::StandingAt(size_t index, uint32 now)
{
    if (rebuilt || now - refreshedMs >= spec::SPELL_ID_REFRESH_MS)
        Refresh(now);
    return standing[index];
}

void SpecTableStrategy::Refresh(uint32 now)
{
    rebuilt = false;
    refreshedMs = now;
    AiObjectContext* const context = botAI->GetAiObjectContext();
    Player* const player = botAI->GetBot();
    bool changed = !standingVersion;
    for (size_t i = 0; i < table.count; ++i)
    {
        spec::Row const& row = table.rows[i];
        Action* const action = context->GetAction(std::string(row.action));
        CastSpellAction* const cast = dynamic_cast<CastSpellAction*>(action);

        spec::StandingFacts facts;
        facts.actionExists = action != nullptr;
        facts.castsSpell = cast != nullptr;
        facts.spellKnown = !cast || Knows(context, cast->getSpell());
        facts.requirementsHold = spec::RequirementsHold(row,
                                                        [&](spec::Requirement const& requirement)
                                                        {
                                                            return requirement.need == spec::Need::Wields
                                                                       ? Wields(player, requirement.gear)
                                                                       : Knows(context, std::string(requirement.spell));
                                                        });
        facts.optionOn = row.option.empty() || botAI->HasStrategy(std::string(row.option), BOT_STATE_COMBAT);

        spec::Standing const state = spec::StandingOf(facts);
        if (state != standing[i])
            changed = true;
        standing[i] = state;
    }
    if (changed)
        ++standingVersion;
}

spec::SharedAnswer* SpecTableStrategy::AnswerFor(Trigger* trigger)
{
    for (auto const& answer : answers)
        if (answer.first == trigger)
            return answer.second.get();

    answers.emplace_back(trigger, std::make_unique<spec::SharedAnswer>());
    return answers.back().second.get();
}

void SpecTableStrategy::ClearCounts()
{
    for (RowCount& count : counts)
        count = RowCount();
}

SpecTableStrategy* SpecTableStrategy::Of(PlayerbotAI* botAI)
{
    if (!botAI)
        return nullptr;

    for (std::string const& strategy : botAI->GetStrategies(BOT_STATE_COMBAT))
        if (SpecTableStrategy* const table =
                dynamic_cast<SpecTableStrategy*>(botAI->GetStrategy(strategy, BOT_STATE_COMBAT)))
            return table;
    return nullptr;
}

void RoleNodes(spec::Role role, std::vector<TriggerNode*>& triggers)
{
    switch (role)
    {
        case spec::Role::Melee:
            triggers.push_back(new TriggerNode("enemy out of melee", {NextAction("reach melee", ACTION_HIGH + 1)}));
            break;
        case spec::Role::Healer:
            triggers.push_back(new TriggerNode("party member to heal out of spell range",
                                               {NextAction("reach party member to heal", ACTION_CRITICAL_HEAL + 10)}));
            triggers.push_back(new TriggerNode("enemy too close for spell", {NextAction("flee", ACTION_MOVE + 9)}));
            break;
        default:  // Ranged and Tank: nothing yet
            break;
    }
}

namespace SpecTableView
{
std::string Describe(PlayerbotAI* botAI)
{
    SpecTableStrategy* const strategy = SpecTableStrategy::Of(botAI);
    if (!strategy)
        return "no table strategy on the combat engine";

    spec::TableView const& table = strategy->Table();
    uint32 const now = getMSTime();
    std::vector<spec::Node> const nodes = spec::Nodes(table);
    std::string text =
        Acore::StringFormat("{} ({}), {} rows", table.name, ROLE_WORDS[static_cast<size_t>(table.role)], table.count);
    for (size_t i = 0; i < table.count; ++i)
    {
        spec::Row const& row = table.rows[i];
        text += Acore::StringFormat("\n#{} {} {:.2f} {} {}", i, spec::BandInfo(spec::BandOf(row.kind)).word,
                                    nodes[i].relevance, spec::KindWord(row.kind), row.action);
        if (!row.option.empty())
            text += Acore::StringFormat(" option {}", row.option);
        text += Acore::StringFormat(" {}", spec::StandingWord(strategy->StandingAt(i, now)));
        if (!row.instead.empty())
            text += Acore::StringFormat(" [instead {}]", row.instead);
    }
    return text;
}

bool HandleTableCommand(ChatHandler* handler, char const* args)
{
    std::string const name = args ? args : "";
    Player* const bot = name.empty() ? nullptr : ObjectAccessor::FindPlayerByName(name, false);
    PlayerbotAI* const botAI = bot ? GET_PLAYERBOT_AI(bot) : nullptr;
    if (!botAI)
    {
        handler->PSendSysMessage("COMBAT table {}: no bot of that name online", name);
        return true;
    }

    std::string text;
    uint32 takenMs = 0;
    botAI->RequestCombatTableSnapshot();
    if (!botAI->GetCombatTableSnapshot(text, takenMs))
    {
        handler->PSendSysMessage("COMBAT table {}: requested, taken at the bot's next update (show again)",
                                 bot->GetName());
        return true;
    }

    handler->PSendSysMessage("COMBAT table {} ({} ms old; a new snapshot is taken at the bot's next update):",
                             bot->GetName(), GetMSTimeDiffToNow(takenMs));
    size_t start = 0;
    while (start <= text.size())
    {
        size_t end = text.find('\n', start);
        if (end == std::string::npos)
            end = text.size();
        handler->PSendSysMessage("COMBAT table {} {}", bot->GetName(), text.substr(start, end - start));
        start = end + 1;
    }
    return true;
}
}  // namespace SpecTableView
