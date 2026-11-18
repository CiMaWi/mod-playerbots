/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "BuffAction.h"

#include <array>
#include <string>
#include <string_view>
#include <vector>

#include "Event.h"
#include "ItemCountValue.h"
#include "ItemUseIndex.h"
#include "ItemUseTypes.h"
#include "Playerbots.h"

namespace
{
// The groups the command tells, in this order, with the header of each (the headers predate the item use plan)
constexpr std::array<std::string_view, 5> HEADERS = {"--- Elixir ---", "--- Flask ---", "--- Scroll ---",
                                                     "--- Food ---", "--- Enchant ---"};

// The group of a buff slot or a coating; -1: no buff
int GroupOf(ItemUse::Exclude slot)
{
    switch (slot)
    {
        case ItemUse::Exclude::BattleElixir:
        case ItemUse::Exclude::GuardianElixir:
            return 0;
        case ItemUse::Exclude::Flask:
            return 1;
        case ItemUse::Exclude::Scroll:
            return 2;
        case ItemUse::Exclude::WellFed:
            return 3;
        case ItemUse::Exclude::Coating:
            return 4;
        default:
            return -1;
    }
}

// The buff items the bot could use now, by the item use index (item-use WI19): a record of a buff slot or a coating
// whose aura the bot does not have; a coating only while the weapon it goes on has none
class FindBuffVisitor : public IterateItemsVisitor
{
public:
    FindBuffVisitor(Player* bot) : IterateItemsVisitor(), bot(bot) {}

    bool Visit(Item* item) override
    {
        ItemTemplate const* proto = item->GetTemplate();
        if (bot->CanUseItem(proto) != EQUIP_ERR_OK)
            return true;

        for (ItemUse::Record const& record : ItemUseIndex::Get().OfItem(proto->ItemId))
        {
            int const group = GroupOf(record.exclude);
            if (group < 0)
                continue;

            if (bot->HasAura(record.spellId))
                return true;

            if (record.exclude == ItemUse::Exclude::Coating)
            {
                Item* itemForSpell =
                    *GET_PLAYERBOT_AI(bot)->GetAiObjectContext()->GetValue<Item*>("item for spell", record.spellId);
                if (itemForSpell && itemForSpell->IsInWorld() && itemForSpell->GetEnchantmentId(TEMP_ENCHANTMENT_SLOT))
                    return true;
            }

            items[static_cast<std::size_t>(group)].push_back(item);
            return true;
        }

        return true;
    }

    std::array<std::vector<Item*>, HEADERS.size()> items;

private:
    Player* bot;
};
}  // namespace

bool BuffAction::Execute(Event /*event*/)
{
    FindBuffVisitor visitor(bot);
    IterateItems(&visitor);

    for (std::size_t group = 0; group < HEADERS.size(); ++group)
    {
        if (visitor.items[group].empty())
            continue;

        botAI->TellMaster(std::string(HEADERS[group]));
        for (Item* item : visitor.items[group])
            botAI->TellMaster(chat->FormatItem(item->GetTemplate(), item->GetCount()));
    }

    return true;
}
