/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_LOOTOBJECTSTACK_H
#define PLAYERBOTS_LOOTOBJECTSTACK_H

#include <array>
#include <chrono>
#include <string_view>

#include "LootInteraction.h"
#include "ObjectGuid.h"

class AiObjectContext;
class Player;
class Spell;
class WorldObject;

struct ItemTemplate;

class LootStrategy
{
public:
    LootStrategy() {}
    virtual ~LootStrategy() {};
    virtual bool CanLoot(ItemTemplate const* proto, AiObjectContext* context) = 0;
    virtual std::string const GetName() = 0;
};

constexpr uint8 MAX_LOOT_LOCK_REQUIREMENTS = 8;

struct LootLockRequirement
{
    uint32 SkillId = 0;
    uint32 ReqSkillValue = 0;
    uint32 ReqItem = 0;
    uint32 LockType = 0;
};

class LootObject
{
public:
    LootObject() : skillId(0), reqSkillValue(0), reqItem(0) {}
    LootObject(Player* bot, ObjectGuid guid);
    LootObject(LootObject const& other) = default;
    LootObject& operator=(LootObject const& other) = default;

    bool IsEmpty() { return !guid; }
    bool IsLootPossible(Player* bot);
    // True when the bot is within the server's interaction range for this target: the core
    // creature range for corpses, or the gameobject's own type/display/spell based range.
    bool IsAtInteractDistance(Player* bot);
    void Refresh(Player* bot, ObjectGuid guid);
    WorldObject* GetWorldObject(Player* bot);
    uint32 GetLockType() const { return _lockType; }
    ObjectGuid guid;

    uint32 skillId;
    uint32 reqSkillValue;
    uint32 reqItem;

private:
    void AddLockRequirement(LootLockRequirement const& requirement);

    std::array<LootLockRequirement, MAX_LOOT_LOCK_REQUIREMENTS> _lockRequirements;
    uint8 _lockRequirementCount = 0;
    uint32 _lockType = 0;
    bool _hasUnsupportedLockRequirement = false;
    static bool IsNeededForQuest(Player* bot, uint32 itemId);
};

class LootTarget
{
public:
    LootTarget(ObjectGuid guid);
    LootTarget(ObjectGuid guid, time_t asOfTime);
    LootTarget(LootTarget const& other) = default;

public:
    LootTarget& operator=(LootTarget const& other) = default;
    bool operator<(LootTarget const& other) const;
    bool IsReady() const;
    void Defer();

public:
    ObjectGuid guid;
    time_t asOfTime;

private:
    std::chrono::steady_clock::time_point _retryUntil;
    uint8 _retryCount = 0;
};

class LootTargetList : public std::set<LootTarget>
{
public:
    void shrink(time_t fromTime);
};

class LootObjectStack
{
public:
    LootObjectStack(Player* bot) : bot(bot) {}

    bool Add(ObjectGuid guid);
    void Remove(ObjectGuid guid);
    void Clear();
    LootObject GetLoot(float maxDistance = 0);

    bool IsLootPending() const { return _interaction.Pending(); }
    bool HasInteraction() const { return _interaction.Guid != 0; }
    bool InteractionExpired() const { return _interaction.Expired; }
    ObjectGuid InteractionGuid() const { return ObjectGuid(_interaction.Guid); }
    uint32 InteractionGeneration() const { return _interaction.Generation; }
    bool PrepareLoot(ObjectGuid guid, bool encounter, uint32 now);
    void BeginLoot(ObjectGuid guid, uint32 now);
    bool OwnsCast(Spell const* spell) const;
    bool LootOpened(ObjectGuid guid);
    void CompleteResponse(ObjectGuid guid, uint32 gold, uint32 stored);
    bool AcceptUnsolicitedLoot(ObjectGuid guid);
    void Cancel(std::string_view reason);
    void Observe(uint32 now, bool canMove);
    bool ConsumeExpiry() { return LootInteraction::ConsumeExpiry(_interaction, *this); }
    void Trace(std::string_view event, std::string_view detail);
    void DeferLoot(ObjectGuid guid);
    bool CanAttemptLoot(ObjectGuid guid) const;

private:
    LootObject GetNearest(float maxDistance = 0);
    bool IsDeferred(ObjectGuid guid) const;
    void Finish();
    void ClearSelection(ObjectGuid guid);

    Player* bot;
    LootTargetList availableLoot;
    LootInteraction::Transaction _interaction;
    ObjectGuid _interactionMaster;
    uint32 _interactionMap = 0;
    uint32 _interactionInstance = 0;
    uint32 _castSpellId = 0;
    bool _encounterInteraction = false;
};

#endif
