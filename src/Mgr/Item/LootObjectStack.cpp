/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "LootObjectStack.h"

#include "CensusMgr.h"
#include "LootMgr.h"
#include "Object.h"
#include "ObjectAccessor.h"
#include "Playerbots.h"
#include "Spell.h"
#include "Unit.h"
#include "WorldLog.h"

#define MAX_LOOT_OBJECT_COUNT 200

constexpr uint8 MAX_LOOT_RETRY_ATTEMPTS = 5;
constexpr std::chrono::seconds LOOT_RETRY_MIN_DELAY(1);
constexpr std::chrono::seconds LOOT_RETRY_MAX_DELAY(16);

namespace
{
bool IsGatheringSkill(uint32 skillId)
{
    return skillId == SKILL_SKINNING || skillId == SKILL_HERBALISM || skillId == SKILL_MINING ||
           skillId == SKILL_ENGINEERING;
}
}  // namespace

LootTarget::LootTarget(ObjectGuid guid) : guid(guid), asOfTime(time(nullptr)) {}

LootTarget::LootTarget(ObjectGuid guid, time_t asOfTime) : guid(guid), asOfTime(asOfTime) {}

bool LootTarget::operator<(LootTarget const& other) const { return guid < other.guid; }

bool LootTarget::IsReady() const { return std::chrono::steady_clock::now() >= _retryUntil; }

void LootTarget::Defer()
{
    _retryCount = std::min<uint8>(_retryCount + 1, MAX_LOOT_RETRY_ATTEMPTS);
    _retryUntil = std::chrono::steady_clock::now() +
                  std::min(LOOT_RETRY_MIN_DELAY * (1u << (_retryCount - 1)), LOOT_RETRY_MAX_DELAY);
}

void LootTargetList::shrink(time_t fromTime)
{
    for (std::set<LootTarget>::iterator i = begin(); i != end();)
    {
        if (i->asOfTime <= fromTime && i->IsReady())
            erase(i++);
        else
            ++i;
    }
}

LootObject::LootObject(Player* bot, ObjectGuid guid) : guid(), skillId(SKILL_NONE), reqSkillValue(0), reqItem(0)
{
    Refresh(bot, guid);
}

void LootObject::Refresh(Player* bot, ObjectGuid lootGUID)
{
    skillId = SKILL_NONE;
    reqSkillValue = 0;
    reqItem = 0;
    _lockRequirementCount = 0;
    _lockType = 0;
    _hasUnsupportedLockRequirement = false;
    guid.Clear();

    PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
    if (!botAI)
    {
        return;
    }
    Creature* creature = botAI->GetCreature(lootGUID);
    if (creature && creature->getDeathState() == DeathState::Corpse)
    {
        if (creature->HasFlag(UNIT_DYNAMIC_FLAGS, UNIT_DYNFLAG_LOOTABLE))
            guid = lootGUID;

        if (creature->HasFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_SKINNABLE))
        {
            skillId = creature->GetCreatureTemplate()->GetRequiredLootSkill();
            uint32 targetLevel = creature->GetLevel();
            reqSkillValue = targetLevel < 10 ? 1 : targetLevel < 20 ? (targetLevel - 10) * 10 : targetLevel * 5;
            if (botAI->HasSkill((SkillType)skillId) && bot->GetSkillValue(skillId) >= reqSkillValue)
                guid = lootGUID;
        }

        return;
    }

    GameObject* go = botAI->GetGameObject(lootGUID);
    if (go && go->isSpawned() && go->GetGoState() == GO_STATE_READY)
    {
        // The client refuses to use hostile game objects.
        if (go->IsHostileTo(bot))
            return;

        bool onlyHasQuestItems = true;
        bool hasAnyQuestItems = false;
        bool neededQuestItem = false;

        GameObjectQuestItemList const* items = sObjectMgr->GetGameObjectQuestItemList(go->GetEntry());
        for (size_t i = 0; i < MAX_GAMEOBJECT_QUEST_ITEMS; i++)
        {
            if (!items || i >= items->size())
                break;

            uint32 itemId = uint32((*items)[i]);
            if (!itemId)
                continue;

            hasAnyQuestItems = true;

            if (IsNeededForQuest(bot, itemId))
            {
                // A gathering node can also drop a needed quest item (e.g.
                // Root Sample off Barrens herbs); gathering yields both, so
                // keep reading the lock below to set skillId rather than
                // bailing here.
                this->guid = lootGUID;
                neededQuestItem = true;
            }

            ItemTemplate const* proto = sObjectMgr->GetItemTemplate(itemId);
            if (!proto)
                continue;

            if (proto->Class != ITEM_CLASS_QUEST)
            {
                onlyHasQuestItems = false;
            }
        }

        // Retrieve the correct loot table entry
        uint32 lootEntry = go->GetGOInfo()->GetLootId();
        if (lootEntry == 0)
            return;

        // Check the main loot template
        if (LootTemplate const* lootTemplate = LootTemplates_Gameobject.GetLootFor(lootEntry))
        {
            Loot loot;
            lootTemplate->Process(loot, LootTemplates_Gameobject, 1, bot);

            for (LootItem const& item : loot.items)
            {
                uint32 itemId = item.itemid;
                if (!itemId)
                    continue;

                ItemTemplate const* proto = sObjectMgr->GetItemTemplate(itemId);
                if (!proto)
                    continue;

                if (proto->Class != ITEM_CLASS_QUEST)
                {
                    onlyHasQuestItems = false;
                    break;
                }

                // If this item references another loot table, process it
                if (LootTemplate const* refLootTemplate = LootTemplates_Reference.GetLootFor(itemId))
                {
                    Loot refLoot;
                    refLootTemplate->Process(refLoot, LootTemplates_Reference, 1, bot);

                    for (LootItem const& refItem : refLoot.items)
                    {
                        uint32 refItemId = refItem.itemid;
                        if (!refItemId)
                            continue;

                        ItemTemplate const* refProto = sObjectMgr->GetItemTemplate(refItemId);
                        if (!refProto)
                            continue;

                        if (refProto->Class != ITEM_CLASS_QUEST)
                        {
                            onlyHasQuestItems = false;
                            break;
                        }
                    }
                }
            }
        }

        // If gameobject has only quest items that bot doesn’t need, skip it.
        if (!neededQuestItem && hasAnyQuestItems && onlyHasQuestItems)
            return;

        // Otherwise, loot it.
        guid = lootGUID;

        uint32 goId = go->GetEntry();
        uint32 lockId = go->GetGOInfo()->GetLockId();
        if (!lockId)
            return;

        LockEntry const* lockInfo = sLockStore.LookupEntry(lockId);
        if (!lockInfo)
        {
            _hasUnsupportedLockRequirement = true;
            return;
        }

        for (uint8 i = 0; i < MAX_LOCK_CASE; ++i)
        {
            switch (lockInfo->Type[i])
            {
                case LOCK_KEY_ITEM:
                    if (lockInfo->Index[i])
                        AddLockRequirement({SKILL_NONE, 0, lockInfo->Index[i], 0});
                    else
                        _hasUnsupportedLockRequirement = true;
                    break;
                case LOCK_KEY_SKILL:
                    if (!lockInfo->Index[i])
                    {
                        _hasUnsupportedLockRequirement = true;
                        break;
                    }

                    if (goId == 13891 || goId == 19535)
                        AddLockRequirement({SKILL_NONE, 0, 0, lockInfo->Index[i]});
                    else
                        AddLockRequirement({uint32(SkillByLockType(LockType(lockInfo->Index[i]))),
                                            std::max(1u, lockInfo->Skill[i]), 0, lockInfo->Index[i]});
                    break;
                case LOCK_KEY_NONE:
                    break;
                default:
                    _hasUnsupportedLockRequirement = true;
                    break;
            }
        }
    }
}

bool LootObject::IsNeededForQuest(Player* bot, uint32 itemId)
{
    for (int qs = 0; qs < MAX_QUEST_LOG_SIZE; ++qs)
    {
        uint32 questId = bot->GetQuestSlotQuestId(qs);
        if (questId == 0)
            continue;

        QuestStatusData& qData = bot->getQuestStatusMap()[questId];
        if (qData.Status != QUEST_STATUS_INCOMPLETE)
            continue;

        Quest const* qInfo = sObjectMgr->GetQuestTemplate(questId);
        if (!qInfo)
            continue;

        for (int i = 0; i < QUEST_ITEM_OBJECTIVES_COUNT; ++i)
        {
            if (!qInfo->RequiredItemCount[i] || (qInfo->RequiredItemCount[i] - qData.ItemCount[i]) <= 0)
                continue;

            if (qInfo->RequiredItemId[i] != itemId)
                continue;

            return true;
        }
    }

    return false;
}

WorldObject* LootObject::GetWorldObject(Player* bot)
{
    Refresh(bot, guid);

    PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
    if (!botAI)
    {
        return nullptr;
    }
    Creature* creature = botAI->GetCreature(guid);
    if (creature && creature->getDeathState() == DeathState::Corpse && creature->IsInWorld())
        return creature;

    GameObject* go = botAI->GetGameObject(guid);
    if (go && go->isSpawned() && go->IsInWorld())
        return go;

    return nullptr;
}

void LootObject::AddLockRequirement(LootLockRequirement const& requirement)
{
    if (_lockRequirementCount < _lockRequirements.size())
        _lockRequirements[_lockRequirementCount++] = requirement;
}

bool LootObject::IsAtInteractDistance(Player* bot)
{
    if (IsEmpty() || !bot)
        return false;

    PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
    if (!botAI)
        return false;

    if (Creature* creature = botAI->GetCreature(guid))
        return bot->IsWithinDistInMap(creature, INTERACTION_DISTANCE);

    if (GameObject* go = botAI->GetGameObject(guid))
        return go->IsAtInteractDistance(bot);

    return false;
}

bool LootObject::IsLootPossible(Player* bot)
{
    if (IsEmpty() || !bot)
        return false;

    WorldObject* worldObj = GetWorldObject(bot);  // Store result to avoid multiple calls
    if (!worldObj)
        return false;

    PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
    if (!botAI)
        return false;

    if (abs(worldObj->GetPositionZ() - bot->GetPositionZ()) > INTERACTION_DISTANCE - 2.0f)
        return false;

    Creature* creature = botAI->GetCreature(guid);
    if (creature && creature->getDeathState() == DeathState::Corpse)
    {
        // Gathering does not need loot rights.
        if (!bot->isAllowedToLoot(creature) && !IsGatheringSkill(skillId))
            return false;
    }

    // Prevent bot from running to chests that are unlootable (e.g. Gunship Armory before completing the event) or on
    // respawn time
    GameObject* go = botAI->GetGameObject(guid);
    if (go && (go->HasFlag(GAMEOBJECT_FLAGS, GO_FLAG_NOT_SELECTABLE) || !go->isSpawned()))
        return false;

    // Conditional objects (quest chests, goobers, ...) are gated client-side on quest state.
    // A bot has no client, so make the same call the server makes for one.
    if (go && go->HasFlag(GAMEOBJECT_FLAGS, GO_FLAG_INTERACT_COND) && !go->ActivateToQuest(bot))
        return false;

    auto canUseRequirement = [bot, botAI](LootLockRequirement const& requirement)
    {
        if (requirement.ReqItem)
            return bot->HasItemCount(requirement.ReqItem, 1);

        if (requirement.SkillId == SKILL_NONE)
            return true;

        bool const gatheringObject = requirement.SkillId == SKILL_HERBALISM || requirement.SkillId == SKILL_MINING ||
                                     requirement.SkillId == SKILL_SKINNING || requirement.SkillId == SKILL_ENGINEERING;
        Player* master = botAI->GetMaster();
        bool const hasActivePlayerMaster = master && !GET_PLAYERBOT_AI(master);
        if (gatheringObject && !hasActivePlayerMaster)
        {
            constexpr uint8 maxGatheringBagUsage = 80;
            if (botAI->GetAiObjectContext()->GetValue<uint8>("bag space")->Get() > maxGatheringBagUsage)
                return false;
        }

        if (requirement.SkillId == SKILL_FISHING || !botAI->HasSkill((SkillType)requirement.SkillId) ||
            requirement.ReqSkillValue > uint32(bot->GetSkillValue(requirement.SkillId)))
            return false;

        if (requirement.SkillId == SKILL_MINING && !bot->HasItemCount(756, 1) && !bot->HasItemCount(778, 1) &&
            !bot->HasItemCount(1819, 1) && !bot->HasItemCount(1893, 1) && !bot->HasItemCount(1959, 1) &&
            !bot->HasItemCount(2901, 1) && !bot->HasItemCount(9465, 1) && !bot->HasItemCount(20723, 1) &&
            !bot->HasItemCount(40772, 1) && !bot->HasItemCount(40892, 1) && !bot->HasItemCount(40893, 1))
            return false;

        if (requirement.SkillId == SKILL_SKINNING && !bot->HasItemCount(7005, 1) && !bot->HasItemCount(40772, 1) &&
            !bot->HasItemCount(40893, 1) && !bot->HasItemCount(12709, 1) && !bot->HasItemCount(19901, 1))
            return false;

        return true;
    };

    if (_lockRequirementCount)
    {
        for (uint8 i = 0; i < _lockRequirementCount; ++i)
        {
            LootLockRequirement const& requirement = _lockRequirements[i];
            if (go && go->HasFlag(GAMEOBJECT_FLAGS, GO_FLAG_LOCKED) && !requirement.ReqItem &&
                requirement.SkillId == SKILL_NONE)
                continue;

            if (!canUseRequirement(requirement))
                continue;

            skillId = requirement.SkillId;
            reqSkillValue = requirement.ReqSkillValue;
            reqItem = requirement.ReqItem;
            _lockType = requirement.LockType;
            return true;
        }

        return false;
    }

    return !_hasUnsupportedLockRequirement && canUseRequirement({skillId, reqSkillValue, reqItem, 0});
}

bool LootObjectStack::Add(ObjectGuid guid)
{
    LootTargetList::iterator itr = availableLoot.find(guid);
    if (itr != availableLoot.end())
    {
        // Refresh instead of dropping the re-add, or the entry expires 30s after the first add
        // while the corpse is still there.
        LootTarget target = *itr;
        target.asOfTime = time(nullptr);
        availableLoot.erase(itr);
        availableLoot.insert(target);
        return false;
    }

    if (availableLoot.size() >= MAX_LOOT_OBJECT_COUNT)
        availableLoot.shrink(time(nullptr) - 30);
    if (availableLoot.size() >= MAX_LOOT_OBJECT_COUNT)
        std::erase_if(availableLoot, [](LootTarget const& target) { return target.IsReady(); });
    if (availableLoot.size() >= MAX_LOOT_OBJECT_COUNT)
        return false;

    return availableLoot.insert(LootTarget(guid)).second;
}

void LootObjectStack::Remove(ObjectGuid guid)
{
    LootTargetList::iterator i = availableLoot.find(guid);
    if (i != availableLoot.end())
        availableLoot.erase(i);
}

void LootObjectStack::Clear()
{
    if (HasInteraction())
        Cancel("clear");
    availableLoot.clear();
}

bool LootObjectStack::PrepareLoot(ObjectGuid guid, bool encounter, uint32 now)
{
    _interaction.Observe(now, false);
    if (!CanAttemptLoot(guid))
        return false;

    bool const starting = !HasInteraction();
    if (!_interaction.Prepare(guid.GetRawValue(), now))
        return false;
    if (!starting)
        return true;

    PlayerbotAI* const ai = GET_PLAYERBOT_AI(bot);
    Player* const master = ai->GetMaster();
    _interactionMaster = master ? master->GetGUID() : ObjectGuid::Empty;
    _interactionMap = bot->GetMapId();
    _interactionInstance = bot->GetInstanceId();
    _encounterInteraction = encounter;
    ai->activeLootInteraction = this;
    return true;
}

void LootObjectStack::BeginLoot(ObjectGuid guid, uint32 now)
{
    if ((!HasInteraction() && !PrepareLoot(guid, false, now)) || !_interaction.Begin(guid.GetRawValue(), now))
        return;

    Spell const* spell = bot->GetCurrentSpell(CURRENT_GENERIC_SPELL);
    if (!spell)
        spell = bot->GetCurrentSpell(CURRENT_CHANNELED_SPELL);
    if (spell && (spell->m_targets.GetGOTargetGUID() == guid || spell->m_targets.GetUnitTargetGUID() == guid))
        _castSpellId = spell->GetSpellInfo()->Id;

    Trace("request", "issued");
}

bool LootObjectStack::OwnsCast(Spell const* spell) const
{
    if (!spell || !_castSpellId || spell->GetSpellInfo()->Id != _castSpellId)
        return false;

    ObjectGuid const guid = InteractionGuid();
    return guid && (spell->m_targets.GetGOTargetGUID() == guid || spell->m_targets.GetUnitTargetGUID() == guid);
}

bool LootObjectStack::LootOpened(ObjectGuid guid)
{
    if (!_interaction.Open(guid.GetRawValue(), getMSTime()))
        return false;

    Trace("open", "response");
    return true;
}

void LootObjectStack::CompleteResponse(ObjectGuid guid, uint32 gold, uint32 stored)
{
    LootInteraction::CompleteResponse(
        _interaction, guid.GetRawValue(), gold, stored, guid.IsGameObject(), [&] { DeferLoot(guid); },
        [&] { Remove(guid); },
        [&]
        {
            WorldPacket* packet = new WorldPacket(CMSG_LOOT_RELEASE, 8);
            *packet << guid;
            bot->GetSession()->QueuePacket(packet);
        });
}

bool LootObjectStack::AcceptUnsolicitedLoot(ObjectGuid guid)
{
    if (guid == InteractionGuid())
        return false;
    if (IsDeferred(guid))
    {
        if (bot->GetLootGUID() == guid)
            bot->GetSession()->DoLootRelease(guid);
        return false;
    }
    return bot->GetLootGUID() == guid;
}

void LootObjectStack::ClearSelection(ObjectGuid guid)
{
    AiObjectContext* const context = GET_PLAYERBOT_AI(bot)->GetAiObjectContext();
    auto* const target = context->GetValue<LootObject>("loot target");
    if (target->Get().guid == guid)
        target->Set(LootObject());
    context->GetValue<bool>("can loot")->Reset();
}

void LootObjectStack::Finish()
{
    ClearSelection(InteractionGuid());
    _interaction.Clear();
    _castSpellId = 0;
    GET_PLAYERBOT_AI(bot)->activeLootInteraction = nullptr;
}

void LootObjectStack::Cancel(std::string_view reason)
{
    ObjectGuid const guid = InteractionGuid();
    if (!guid)
        return;

    for (CurrentSpellTypes const slot : {CURRENT_GENERIC_SPELL, CURRENT_CHANNELED_SPELL})
        if (OwnsCast(bot->GetCurrentSpell(slot)))
            bot->InterruptSpell(slot);

    if (bot->GetLootGUID() == guid)
    {
        bot->GetSession()->DoLootRelease(guid);
        Trace("release", reason);
    }
    DeferLoot(guid);
    if (WorldLog::Active())
        EventLog::Emit(bot, "TravelLootDefer", std::string(reason).c_str(), InteractionGeneration(),
                       static_cast<int64>(guid.GetRawValue()));
    Trace("defer", reason);
    Finish();
}

void LootObjectStack::Observe(uint32 now, bool canMove)
{
    ObjectGuid const guid = InteractionGuid();
    if (!guid)
        return;

    Player* const master = GET_PLAYERBOT_AI(bot)->GetMaster();
    if (bot->GetMapId() != _interactionMap || bot->GetInstanceId() != _interactionInstance ||
        (master ? master->GetGUID() : ObjectGuid::Empty) != _interactionMaster)
        return Cancel("owner");
    if (!bot->IsAlive())
        return Cancel("death");
    if (bot->IsInCombat() && !_encounterInteraction)
        return Cancel("combat");

    if (_interaction.Current == LootInteraction::Phase::AwaitingRelease)
    {
        if (LootInteraction::ConsumeClosed(_interaction, bot->GetLootGUID().GetRawValue(),
                                           GET_PLAYERBOT_AI(bot)->completedLootInteractions,
                                           [&](char const* detail)
                                           {
                                               EventLog::Emit(bot, "TravelLootRelease", detail, InteractionGeneration(),
                                                              static_cast<int64>(guid.GetRawValue()));
                                               Trace("release", detail);
                                               Finish();
                                           }))
            return;
    }
    else
    {
        WorldObject* const object = ObjectAccessor::GetWorldObject(*bot, guid);
        if (!object || (object->IsGameObject() && !object->ToGameObject()->isSpawned()) ||
            (object->IsCreature() && object->ToCreature()->IsAlive()))
            return Cancel("invalid");
    }

    _interaction.Observe(now, !canMove);
}

void LootObjectStack::Trace(std::string_view event, std::string_view detail)
{
    GET_PLAYERBOT_AI(bot)->TraceTravelInteraction(event, detail, InteractionGuid(), InteractionGeneration());
}

void LootObjectStack::DeferLoot(ObjectGuid guid)
{
    LootTargetList::iterator itr = availableLoot.find(guid);
    if (itr == availableLoot.end())
    {
        LootTarget target(guid);
        target.Defer();
        availableLoot.insert(target);
        return;
    }

    LootTarget target = *itr;
    availableLoot.erase(itr);
    target.asOfTime = time(nullptr);
    target.Defer();
    availableLoot.insert(target);
}

bool LootObjectStack::CanAttemptLoot(ObjectGuid guid) const
{
    return _interaction.AllowsAttempt(guid.GetRawValue()) && !IsDeferred(guid);
}

bool LootObjectStack::IsDeferred(ObjectGuid guid) const
{
    LootTargetList::const_iterator itr = availableLoot.find(guid);
    return itr != availableLoot.end() && !itr->IsReady();
}

LootObject LootObjectStack::GetLoot(float maxDistance) { return GetNearest(maxDistance); }

LootObject LootObjectStack::GetNearest(float maxDistance)
{
    availableLoot.shrink(time(nullptr) - 30);

    LootObject nearest;
    float nearestDistance = std::numeric_limits<float>::max();

    for (LootTargetList::iterator i = availableLoot.begin(); i != availableLoot.end();)
    {
        ObjectGuid guid = i->guid;

        if (!i->IsReady())
        {
            ++i;
            continue;
        }

        WorldObject* worldObj = ObjectAccessor::GetWorldObject(*bot, guid);
        if (!worldObj)
        {
            i = availableLoot.erase(i);
            continue;
        }

        float distance = bot->GetDistance(worldObj);

        if (distance >= nearestDistance || (maxDistance && distance > maxDistance))
        {
            ++i;
            continue;
        }

        LootObject lootObject(bot, guid);
        if (!lootObject.IsLootPossible(bot))
        {
            ++i;
            continue;
        }

        nearestDistance = distance;
        nearest = lootObject;
        ++i;
    }

    return nearest;
}
