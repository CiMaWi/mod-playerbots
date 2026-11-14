/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "AutoMaintenanceOnLevelupAction.h"

#include "BankRulesInput.h"
#include "BroadcastHelper.h"
#include "CensusMgr.h"
#include "PlayerbotAIConfig.h"
#include "PlayerbotFactory.h"
#include "RandomPlayerbotMgr.h"
#include "SharedDefines.h"
#include "SpellMgr.h"
#include "StartingHistoryRules.h"
#include "Upkeep.h"

namespace
{
constexpr char const* SOURCE_LEVELUP = "levelup_maintenance";

bool HasHearthstone(Player* bot) { return bot->HasItemCount(StartingHistory::ITEM_HEARTHSTONE, 1, true); }

// Names the level-up step after which a persistent bot's hearthstone is gone; WT5 saw every level-up lose it
// without finding the step
void NoteHearthstoneLoss(Player* bot, bool& had, char const* step)
{
    if (!had || HasHearthstone(bot))
        return;

    had = false;
    LOG_INFO("playerbots", "[Hearthstone] {} level {}: gone at level-up step {}", bot->GetName(), bot->GetLevel(),
             step);
}
}  // namespace

bool AutoMaintenanceOnLevelupAction::Execute(Event /*event*/)
{
    bool const keepsPossessions = PlayerbotFactory::KeepsPossessions(bot);
    bool had = keepsPossessions;
    NoteHearthstoneLoss(bot, had, "before");
    AutoPickTalents();
    NoteHearthstoneLoss(bot, had, "talents");
    AutoLearnSpell();
    NoteHearthstoneLoss(bot, had, "spells");
    AutoTeleportForLevel();
    NoteHearthstoneLoss(bot, had, "teleport");
    AutoUpgradeEquip();
    BankMail::RefreshCarriedKeeps(botAI);  // the kept gear is decided again at the new level

    // Persistent bots keep their hearthstone (WT.3)
    if (keepsPossessions && !HasHearthstone(bot) && bot->StoreNewItemInBestSlots(StartingHistory::ITEM_HEARTHSTONE, 1))
        EventLog::Honesty(bot, "Item", "hearthstone_restored", StartingHistory::ITEM_HEARTHSTONE, 1);

    return true;
}

void AutoMaintenanceOnLevelupAction::AutoTeleportForLevel()
{
    if (!sPlayerbotAIConfig.AutoTeleportForLevel || !sRandomPlayerbotMgr.IsRandomBot(bot))
        return;

    if (botAI->HasGameClientMaster())
        return;

    // Persistent bots change zones by travelling (the planner's move_zone goal, WT20).
    if (sRandomPlayerbotMgr.HonestTravel())
        return;

    sRandomPlayerbotMgr.RandomTeleportForLevel(bot);
    return;
}

void AutoMaintenanceOnLevelupAction::AutoPickTalents()
{
    bool const isRandomBot = sRandomPlayerbotMgr.IsRandomBot(bot);
    if (!(isRandomBot ? sPlayerbotAIConfig.AutoPickTalents
                      : sPlayerbotAIConfig.AutoPickTalentsForPlayerControlledBots))
        return;

    if (bot->GetFreeTalentPoints() <= 0)
        return;

    PlayerbotFactory factory(bot, bot->GetLevel());
    // The free points are spent as a player does; the premade spec is laid over the choices (reset) only for a bot
    // granted the kind, so a persistent bot outside the handout share keeps the talents it has
    bool const reset = isRandomBot && Upkeep::Granted(bot, UpkeepKind::Talent);
    factory.InitTalentsTree(true, true, reset);
    if (reset)
        EventLog::Honesty(bot, "Talent", SOURCE_LEVELUP);
    factory.InitPetTalents();
}

void AutoMaintenanceOnLevelupAction::AutoLearnSpell()
{
    std::ostringstream out;
    LearnSpells(&out);

    if (!out.str().empty())
    {
        std::string const temp = out.str();
        out.seekp(0);
        out << "Learned spells: ";
        out << temp;
        out.seekp(-2, out.cur);
        out << ".";
        botAI->TellMaster(out);
    }
    return;
}

void AutoMaintenanceOnLevelupAction::LearnSpells(std::ostringstream* out)
{
    BroadcastHelper::BroadcastLevelup(botAI, bot);
    if (sPlayerbotAIConfig.AutoLearnTrainerSpells && sRandomPlayerbotMgr.IsRandomBot(bot))
        LearnTrainerSpells(out);

    if (sPlayerbotAIConfig.AutoLearnQuestSpells && sRandomPlayerbotMgr.IsRandomBot(bot) &&
        Upkeep::Granted(bot, UpkeepKind::ClassQuest))
        LearnQuestSpells(out);
}

void AutoMaintenanceOnLevelupAction::LearnTrainerSpells(std::ostringstream* /*out*/)
{
    PlayerbotFactory factory(bot, bot->GetLevel());
    bool const keepsPossessions = PlayerbotFactory::KeepsPossessions(bot);
    Upkeep::HandOut(bot, UpkeepKind::WeaponSkill, keepsPossessions, SOURCE_LEVELUP,
                    [&factory] { factory.InitSkills(); });
    Upkeep::HandOut(
        bot, UpkeepKind::TrainerSpell, keepsPossessions, SOURCE_LEVELUP, [&factory] { factory.InitClassSpells(); },
        [&factory] { factory.InitAvailableSpells(); });
    Upkeep::HandOut(bot, UpkeepKind::Pet, keepsPossessions, SOURCE_LEVELUP, [&factory] { factory.InitPet(); });
}

void AutoMaintenanceOnLevelupAction::LearnQuestSpells(std::ostringstream* out)
{
    ObjectMgr::QuestMap const& questTemplates = sObjectMgr->GetQuestTemplates();
    for (ObjectMgr::QuestMap::const_iterator i = questTemplates.begin(); i != questTemplates.end(); ++i)
    {
        Quest const* quest = i->second;

        if (!quest->GetRequiredClasses() || quest->IsRepeatable() || quest->GetMinLevel() < 10 ||
            quest->GetMinLevel() > bot->GetLevel())
        {
            continue;
        }

        if (!bot->SatisfyQuestClass(quest, false) || !bot->SatisfyQuestRace(quest, false) ||
            !bot->SatisfyQuestSkill(quest, false))
        {
            continue;
        }

        int32 spellId = quest->GetRewSpellCast();
        if (!spellId)
            continue;

        SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellId);
        if (!spellInfo)
            continue;

        bool found = false;
        for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
        {
            if (spellInfo->Effects[i].Effect == SPELL_EFFECT_LEARN_SPELL && spellInfo->Effects[i].TriggerSpell &&
                !bot->HasSpell(spellInfo->Effects[i].TriggerSpell))
            {
                if (SpellInfo const* triggeredInfo = sSpellMgr->GetSpellInfo(spellInfo->Effects[i].TriggerSpell))
                    if (triggeredInfo->Effects[0].Effect == SPELL_EFFECT_TRADE_SKILL)
                        break;

                found = true;
                break;
            }
        }

        if (!found)
            continue;

        bot->CastSpell(bot, spellId, true);
        Upkeep::Note(bot, UpkeepKind::ClassQuest, SOURCE_LEVELUP, spellId);

        uint32 rewSpellId = quest->GetRewSpell();
        if (rewSpellId)
        {
            if (SpellInfo const* rewSpellInfo = sSpellMgr->GetSpellInfo(rewSpellId))
            {
                *out << FormatSpell(rewSpellInfo) << ", ";
                continue;
            }
        }

        *out << FormatSpell(spellInfo) << ", ";
    }
}

std::string const AutoMaintenanceOnLevelupAction::FormatSpell(SpellInfo const* sInfo)
{
    std::ostringstream out;
    std::string const rank = sInfo->Rank[0];

    if (rank.empty())
        out << "|cffffffff|Hspell:" << sInfo->Id << "|h[" << sInfo->SpellName[LOCALE_enUS] << "]|h|r";
    else
        out << "|cffffffff|Hspell:" << sInfo->Id << "|h[" << sInfo->SpellName[LOCALE_enUS] << " " << rank << "]|h|r";

    return out.str();
}

void AutoMaintenanceOnLevelupAction::AutoUpgradeEquip()
{
    if (!sRandomPlayerbotMgr.IsRandomBot(bot))
        return;

    PlayerbotFactory factory(bot, bot->GetLevel());
    bool const keepsPossessions = PlayerbotFactory::KeepsPossessions(bot);
    bool had = keepsPossessions && HasHearthstone(bot);

    // The cleanup clears the way for the restock below: a bot not granted it keeps what it bought (the five
    // consumable kinds are honest together, UpkeepRules.h)
    if (Upkeep::Granted(bot, UpkeepKind::Consumable, keepsPossessions))
    {
        factory.CleanupConsumables();
        NoteHearthstoneLoss(bot, had, "cleanup consumables");
    }

    Upkeep::HandOut(bot, UpkeepKind::Ammo, keepsPossessions, SOURCE_LEVELUP, [&factory] { factory.InitAmmo(); });
    Upkeep::HandOut(bot, UpkeepKind::Reagent, keepsPossessions, SOURCE_LEVELUP, [&factory] { factory.InitReagents(); });
    Upkeep::HandOut(bot, UpkeepKind::Food, keepsPossessions, SOURCE_LEVELUP, [&factory] { factory.InitFood(); });
    auto const consumables = [&factory] { factory.InitConsumables(); };
    auto const potions = [&factory] { factory.InitPotions(); };
    if (bot->getClass() == CLASS_ROGUE)  // a rogue's consumables are its poisons
    {
        Upkeep::HandOut(bot, UpkeepKind::Poison, keepsPossessions, SOURCE_LEVELUP, consumables);
        Upkeep::HandOut(bot, UpkeepKind::Consumable, keepsPossessions, SOURCE_LEVELUP, potions);
    }
    else
        Upkeep::HandOut(bot, UpkeepKind::Consumable, keepsPossessions, SOURCE_LEVELUP, consumables, potions);
    NoteHearthstoneLoss(bot, had, "restock");

    if (sPlayerbotAIConfig.AutoUpgradeEquip)
    {
        factory.InitEquipment(true);
        NoteHearthstoneLoss(bot, had, "equipment");
        EventLog::Honesty(bot, "Item", SOURCE_LEVELUP);
    }
}
