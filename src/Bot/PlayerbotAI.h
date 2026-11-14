/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_PLAYERBOTAI_H
#define PLAYERBOTS_PLAYERBOTAI_H

#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <stack>
#include <string_view>
#include <unordered_set>

#include "BotAttributes.h"
#include "Chat.h"
#include "ChatFilter.h"
#include "ChatHelper.h"
#include "CreatureData.h"
#include "Event.h"
#include "ForceRebuff.h"
#include "Item.h"
#include "LFG.h"
#include "NewRpgInfo.h"
#include "NewRpgStrategy.h"
#include "ObjectGuid.h"
#include "PlayerbotAIBase.h"
#include "PlayerbotAIConfig.h"
#include "PlayerbotSecurity.h"
#include "PlayerbotTextMgr.h"
#include "PvpTestCounters.h"
#include "SpellAuras.h"
#include "TacticGate.h"
#include "Tactics.h"
#include "Util.h"
#include "WorldPacket.h"

class Action;
class AiObjectContext;
class Creature;
class Engine;
class ExternalEventHelper;
class Group;
struct CcBotState;
struct CoverBotState;
struct DefensiveBotState;
struct DrinkBotState;
struct FakeBotState;
struct FocusBotState;
struct InterruptBotState;
struct JumpBotState;
struct KiteBotState;
struct PathBotState;
struct TrackBotState;
struct TurnBotState;
enum class TurnSnapSite : uint8;  // NaturalTurn.h
class ReactionEngine;
class Gameobject;
class Item;
class LootObjectStack;
class Player;
class PlayerbotMgr;
class Spell;
class SpellInfo;
class Transport;
class Unit;
class WorldObject;
class WorldPosition;

struct CreatureData;
struct GameObjectData;

namespace WorldLog
{
struct BotLog;
}

namespace WorldPvp
{
struct PvpMemory;
struct BotEnthusiasm;
}

enum StrategyType : uint32;

enum HealingItemId
{
    HEALTHSTONE = 5512,
    MAJOR_HEALING_POTION = 13446,
    WHIPPER_ROOT_TUBER = 11951,
    NIGHT_DRAGON_BREATH = 11952,
    LIMITED_INVULNERABILITY_POTION = 3387,
    GREATER_DREAMLESS_SLEEP_POTION = 22886,
    SUPERIOR_HEALING_POTION = 3928,
    CRYSTAL_RESTORE = 11564,
    DREAMLESS_SLEEP_POTION = 12190,
    GREATER_HEALING_POTION = 1710,
    HEALING_POTION = 929,
    LESSER_HEALING_POTION = 858,
    DISCOLORED_HEALING_POTION = 3391,
    MINOR_HEALING_POTION = 118,
    VOLATILE_HEALING_POTION = 28100,
    SUPER_HEALING_POTION = 22829,
    CRYSTAL_HEALING_POTION = 13462,
    FEL_REGENERATION_POTION = 28101,
    MAJOR_DREAMLESS_SLEEP_POTION = 20002
};

enum BotState
{
    BOT_STATE_COMBAT = 0,
    BOT_STATE_NON_COMBAT = 1,
    BOT_STATE_DEAD = 2,
    BOT_STATE_REACTION = 3,

    BOT_STATE_MAX
};

bool IsRealPlayer(Player* player);
bool IsSelfBot(Player* player);
bool IsAlliance(uint8 race);

class PlayerbotChatHandler : protected ChatHandler
{
public:
    explicit PlayerbotChatHandler(Player* pMasterPlayer);
    void sysmessage(std::string const str) { SendSysMessage(str.c_str()); }
    uint32 extractQuestId(std::string const str);
    uint32 extractSpellId(std::string const str)
    {
        char* source = (char*)str.c_str();
        return extractSpellIdFromLink(source);
    }
};

class MinValueCalculator
{
public:
    MinValueCalculator(float def = 0.0f) : param(nullptr), minValue(def) {}

    void probe(float value, void* p)
    {
        if (!param || minValue >= value)
        {
            minValue = value;
            param = p;
        }
    }

    void* param;
    float minValue;
};
enum ChatChannelSource
{
    SRC_GUILD,
    SRC_WORLD,
    SRC_GENERAL,
    SRC_TRADE,
    SRC_LOOKING_FOR_GROUP,
    SRC_LOCAL_DEFENSE,
    SRC_WORLD_DEFENSE,
    SRC_GUILD_RECRUITMENT,

    SRC_SAY,
    SRC_WHISPER,
    SRC_EMOTE,
    SRC_TEXT_EMOTE,
    SRC_YELL,

    SRC_PARTY,
    SRC_RAID,

    SRC_UNDEFINED
};
static std::map<ChatChannelSource, std::string> ChatChannelSourceStr = {
    {SRC_GUILD, "SRC_GUILD"},
    {SRC_WORLD, "SRC_WORLD"},
    {SRC_GENERAL, "SRC_GENERAL"},
    {SRC_TRADE, "SRC_TRADE"},
    {SRC_LOOKING_FOR_GROUP, "SRC_LOOKING_FOR_GROUP"},
    {SRC_LOCAL_DEFENSE, "SRC_LOCAL_DEFENSE"},
    {SRC_WORLD_DEFENSE, "SRC_WORLD_DEFENSE"},
    {SRC_GUILD_RECRUITMENT, "SRC_GUILD_RECRUITMENT"},

    {SRC_SAY, "SRC_SAY"},
    {SRC_WHISPER, "SRC_WHISPER"},
    {SRC_EMOTE, "SRC_EMOTE"},
    {SRC_TEXT_EMOTE, "SRC_TEXT_EMOTE"},
    {SRC_YELL, "SRC_YELL"},

    {SRC_PARTY, "SRC_PARTY"},
    {SRC_RAID, "SRC_RAID"},

    {SRC_UNDEFINED, "SRC_UNDEFINED"}};

enum ChatChannelId
{
    GENERAL = 1,
    TRADE = 2,
    LOCAL_DEFENSE = 22,
    WORLD_DEFENSE = 23,
    LOOKING_FOR_GROUP = 26,
    GUILD_RECRUITMENT = 25,
};

enum RoguePoisonId
{
    INSTANT_POISON      = 6947,
    INSTANT_POISON_II   = 6949,
    INSTANT_POISON_III  = 6950,
    INSTANT_POISON_IV   = 8926,
    INSTANT_POISON_V    = 8927,
    INSTANT_POISON_VI   = 8928,
    INSTANT_POISON_VII  = 21927,
    INSTANT_POISON_VIII = 43230,
    INSTANT_POISON_IX   = 43231,
    DEADLY_POISON       = 2892,
    DEADLY_POISON_II    = 2893,
    DEADLY_POISON_III   = 8984,
    DEADLY_POISON_IV    = 8985,
    DEADLY_POISON_V     = 20844,
    DEADLY_POISON_VI    = 22053,
    DEADLY_POISON_VII   = 22054,
    DEADLY_POISON_VIII  = 43232,
    DEADLY_POISON_IX    = 43233
};

enum SharpeningStoneId
{
    ROUGH_SHARPENING_STONE      = 2862,
    COARSE_SHARPENING_STONE     = 2863,
    HEAVY_SHARPENING_STONE      = 2871,
    SOLID_SHARPENING_STONE      = 7964,
    DENSE_SHARPENING_STONE      = 12404,
    ELEMENTAL_SHARPENING_STONE  = 18262,
    FEL_SHARPENING_STONE        = 23528,
    ADAMANTITE_SHARPENING_STONE = 23529
};

enum WeightstoneId
{
    ROUGH_WEIGHTSTONE      = 3239,
    COARSE_WEIGHTSTONE     = 3240,
    HEAVY_WEIGHTSTONE      = 3241,
    SOLID_WEIGHTSTONE      = 7965,
    DENSE_WEIGHTSTONE      = 12643,
    FEL_WEIGHTSTONE        = 28420,
    ADAMANTITE_WEIGHTSTONE = 28421
};

enum WizardOilId
{
    MINOR_WIZARD_OIL      = 20744,
    LESSER_WIZARD_OIL     = 20746,
    WIZARD_OIL            = 20750,
    BRILLIANT_WIZARD_OIL  = 20749,
    SUPERIOR_WIZARD_OIL   = 22522
};

enum ManaOilId
{
    MINOR_MANA_OIL        = 20745,
    LESSER_MANA_OIL       = 20747,
    BRILLIANT_MANA_OIL    = 20748,
    SUPERIOR_MANA_OIL     = 22521
};

enum class BotTypeNumber : uint8
{
    ACTIVITY_TYPE_NUMBER = 1,
    GROUPER_TYPE_NUMBER = 2,
    GUILDER_TYPE_NUMBER = 3,
};

enum class GrouperType : uint8
{
    SOLO = 0,
    MEMBER = 1,
    LEADER_2 = 2,
    LEADER_3 = 3,
    LEADER_4 = 4,
    LEADER_5 = 5
};

enum class GuilderType : uint8
{
    SOLO = 0,
    TINY = 30,
    SMALL = 50,
    MEDIUM = 70,
    LARGE = 120,
    VERY_LARGE = 250
};

enum ActivityType
{
    GRIND_ACTIVITY = 1,
    RPG_ACTIVITY = 2,
    TRAVEL_ACTIVITY = 3,
    OUT_OF_PARTY_ACTIVITY = 4,
    PACKET_ACTIVITY = 5,
    DETAILED_MOVE_ACTIVITY = 6,
    PARTY_ACTIVITY = 7,
    REACT_ACTIVITY = 8,
    ALL_ACTIVITY = 9,

    MAX_ACTIVITY_TYPE
};

// Playerbots.ArenaOnly.Enable: the strategy set AiFactory gave a random bot at its last reset
enum class ArenaOnlyState : uint8
{
    Off,             // not an arena-only bot
    Waiting,         // outside a battleground: the waiting strategies only
    InBattleground,  // inside: the full strategies
};

enum BotRoles : uint8
{
    BOT_ROLE_NONE = 0x00,
    BOT_ROLE_TANK = 0x01,
    BOT_ROLE_HEALER = 0x02,
    BOT_ROLE_DPS = 0x04
};

enum HUNTER_TABS
{
    HUNTER_TAB_BEAST_MASTERY,
    HUNTER_TAB_MARKSMANSHIP,
    HUNTER_TAB_SURVIVAL,
};

enum ROGUE_TABS
{
    ROGUE_TAB_ASSASSINATION,
    ROGUE_TAB_COMBAT,
    ROGUE_TAB_SUBTLETY,
};

enum PRIEST_TABS
{
    PRIEST_TAB_DISCIPLINE,
    PRIEST_TAB_HOLY,
    PRIEST_TAB_SHADOW,
};

enum DEATH_KNIGHT_TABS
{
    DEATH_KNIGHT_TAB_BLOOD,
    DEATH_KNIGHT_TAB_FROST,
    DEATH_KNIGHT_TAB_UNHOLY,
};

enum DRUID_TABS
{
    DRUID_TAB_BALANCE,
    DRUID_TAB_FERAL,
    DRUID_TAB_RESTORATION,
};

enum MAGE_TABS
{
    MAGE_TAB_ARCANE,
    MAGE_TAB_FIRE,
    MAGE_TAB_FROST,
};

enum SHAMAN_TABS
{
    SHAMAN_TAB_ELEMENTAL,
    SHAMAN_TAB_ENHANCEMENT,
    SHAMAN_TAB_RESTORATION,
};

enum PALADIN_TABS
{
    PALADIN_TAB_HOLY,
    PALADIN_TAB_PROTECTION,
    PALADIN_TAB_RETRIBUTION,
};

enum WARLOCK_TABS
{
    WARLOCK_TAB_AFFLICTION,
    WARLOCK_TAB_DEMONOLOGY,
    WARLOCK_TAB_DESTRUCTION,
};

enum WARRIOR_TABS
{
    WARRIOR_TAB_ARMS,
    WARRIOR_TAB_FURY,
    WARRIOR_TAB_PROTECTION,
};

class PacketHandlingHelper
{
public:
    void AddHandler(uint16 opcode, std::string const handler);
    void Handle(ExternalEventHelper& helper);
    void AddPacket(WorldPacket const& packet);

private:
    std::map<uint16, std::string> handlers;
    std::stack<WorldPacket> queue;
};

class ChatCommandHolder
{
public:
    ChatCommandHolder(std::string const command, ObjectGuid owner, PlayerbotSecurityLevel requiredLevel,
                      uint32 type = CHAT_MSG_WHISPER, time_t time = 0)
        : command(command), owner(owner), requiredLevel(requiredLevel), type(type), time(time)
    {
    }
    ChatCommandHolder(ChatCommandHolder const& other)
        : command(other.command),
          owner(other.owner),
          requiredLevel(other.requiredLevel),
          type(other.type),
          time(other.time)
    {
    }

    std::string const& GetCommand() { return command; }
    ObjectGuid GetOwnerGuid() { return owner; }
    PlayerbotSecurityLevel GetRequiredLevel() { return requiredLevel; }
    uint32& GetType() { return type; }
    time_t& GetTime() { return time; }

private:
    std::string const command;
    ObjectGuid owner;
    PlayerbotSecurityLevel requiredLevel;  // what the sender needed when the command was accepted
    uint32 type;
    time_t time;
};

// Verdicts of the skill multiplier (SkillStrategy) per notice category, counted since the skill was last set. Only the
// GM command .playerbots skill <bot> shows them.
struct SkillNoticeCounters
{
    static constexpr size_t Categories = 6;
    std::array<uint32, Categories> blocked{};
    std::array<uint32, Categories> allowed{};
};

class PlayerbotAI : public PlayerbotAIBase
{
    // Consumes queued chat commands from FindReaction(); needs HandleCommands().
    friend class ReactionEngine;

public:
    PlayerbotAI();
    PlayerbotAI(Player* bot);
    virtual ~PlayerbotAI();

    void UpdateAI(uint32 elapsed, bool minimal = false) override;
    void UpdateAIInternal(uint32 elapsed, bool minimal = false) override;
    bool UpdateAIReaction(uint32 elapsed, bool minimal, bool canControlSelf);
    Engine* GetCurrentEngine() const { return currentEngine; }
    void SetActionDuration(Action const* action);
    using PlayerbotAIBase::SetActionDuration;

    std::string const HandleRemoteCommand(std::string const command);
    void HandleCommand(uint32 type, std::string const text, Player* fromPlayer);
    void QueueChatResponse(const ChatQueuedReply reply);
    void HandleBotOutgoingPacket(WorldPacket const& packet);
    void HandleMasterIncomingPacket(WorldPacket const& packet);
    void HandleMasterOutgoingPacket(WorldPacket const& packet);
    void HandleTeleportAck();
    void ChangeEngine(BotState type);
    void ChangeEngineOnCombat();
    void ChangeEngineOnNonCombat();
    void DoNextAction(bool minimal = false);
    virtual bool DoSpecificAction(std::string const name, Event event = Event(), bool silent = false,
                                  std::string const qualifier = "");
    void ChangeStrategy(std::string const name, BotState type);
    void ClearStrategies(BotState type);
    void SelectiveResetStrategies(BotState type);
    std::vector<std::string> GetStrategies(BotState type);
    Strategy* GetStrategy(std::string const name, BotState type);
    void ApplyInstanceStrategies(uint32 mapId, bool tellMaster = false);
    bool IsInNonRaidDungeon() const;
    bool HasTargetExclusions() const;
    void EvaluateHealerDpsStrategy();
    bool ContainsStrategy(StrategyType type);
    bool HasStrategy(std::string const name, BotState type);
    BotState GetState() { return currentState; };
    void ResetStrategies(bool load = false);
    void ReInitCurrentEngine();
    void Reset(bool full = false, bool farTeleport = false);
    void LeaveOrDisbandGroup();
    static bool IsTank(Player* player, bool bySpec = false);
    static bool IsHeal(Player* player, bool bySpec = false);
    static bool IsDps(Player* player, bool bySpec = false);
    static bool IsRanged(Player* player, bool bySpec = false);
    static bool IsMelee(Player* player, bool bySpec = false);
    static bool IsCaster(Player* player, bool bySpec = false);
    static bool IsRangedDps(Player* player, bool bySpec = false);
    static bool IsCombo(Player* player);
    static ObjectGuid GetMainTankGuid(Group* group);
    static bool IsMainTank(Player* player);
    static bool IsExplicitMainTank(Player* player);
    static bool IsBotMainTank(Player* player);
    static uint32 GetGroupTankNum(Player* player);
    static bool IsAssistTank(Player* player);
    static bool IsAssistTankOfIndex(Player* player, uint8 index, bool indexLivingOnly = false);
    static bool IsAssistHealOfIndex(Player* player, uint8 index, bool indexLivingOnly = false);
    static bool IsAssistRangedDpsOfIndex(Player* player, uint8 index, bool indexLivingOnly = false);
    bool HasAggro(Unit* unit);
    bool IsMovementImpaired(Unit* unit);
    static int32 GetAssistTankIndex(Player* player);
    int32 GetGroupSlotIndex(Player* player);
    int32 GetRangedIndex(Player* player);
    int32 GetClassIndex(Player* player, uint8 cls);
    int32 GetRangedDpsIndex(Player* player);
    int32 GetMeleeIndex(Player* player);

    Creature* GetCreature(ObjectGuid guid);
    Unit* GetUnit(ObjectGuid guid);
    Player* GetPlayer(ObjectGuid guid);
    static Unit* GetUnit(CreatureData const* creatureData);
    GameObject* GetGameObject(ObjectGuid guid);
    // static GameObject* GetGameObject(GameObjectData const* gameObjectData);
    WorldObject* GetWorldObject(ObjectGuid guid);
    std::vector<Player*> GetAllPlayersInGroup();
    std::vector<Player*> GetRealPlayersInGroup();
    AreaTableEntry const* GetCurrentArea();
    AreaTableEntry const* GetCurrentZone();
    static std::string GetLocalizedAreaName(AreaTableEntry const* entry);
    static std::string GetLocalizedCreatureName(uint32 entry);
    static std::string GetLocalizedGameObjectName(uint32 entry);
    static constexpr size_t MAX_CHAT_MESSAGE_BYTES = 255;
    bool TellMaster(std::ostringstream& stream, PlayerbotSecurityLevel securityLevel = PLAYERBOT_SECURITY_ALLOW_ALL);
    bool TellMaster(std::string const text, PlayerbotSecurityLevel securityLevel = PLAYERBOT_SECURITY_ALLOW_ALL);
    bool TellMasterNoFacing(std::ostringstream& stream,
                            PlayerbotSecurityLevel securityLevel = PLAYERBOT_SECURITY_ALLOW_ALL);
    bool TellMasterNoFacing(std::string const text,
                            PlayerbotSecurityLevel securityLevel = PLAYERBOT_SECURITY_ALLOW_ALL);
    bool TellError(std::string const text, PlayerbotSecurityLevel securityLevel = PLAYERBOT_SECURITY_ALLOW_ALL);
    bool SayToGuild(std::string const& msg);
    bool SayToWorld(std::string const& msg);
    bool SayToChannel(std::string const& msg, ChatChannelId const& chanId);
    bool SayToParty(std::string const& msg);
    bool SayToRaid(std::string const& msg);
    bool Yell(std::string const& msg);
    bool Say(std::string const& msg);
    bool Whisper(std::string const& msg, std::string const& receiverName);

    void SpellInterrupted(uint32 spellid);
    int32 CalculateGlobalCooldown(uint32 spellid);
    void RequestSpellInterrupt();
    void RemoveAura(std::string const name);
    void RemoveShapeshift();
    void WaitForSpellCast(Spell* spell);
    bool PlaySound(uint32 emote);
    bool PlayEmote(uint32 emote);
    void Ping(float x, float y);
    Item* FindPoison() const;
    Item* FindAmmo() const;
    Item* FindBandage() const;
    Item* FindOpenableItem() const;
    Item* FindLockedItem() const;
    Item* FindConsumable(uint32 itemId) const;
    Item* FindStoneFor(Item* weapon) const;
    Item* FindOilFor(Item* weapon) const;
    void ImbueItem(Item* item, uint32 targetFlag, ObjectGuid targetGUID);
    void ImbueItem(Item* item, uint8 targetInventorySlot);
    void ImbueItem(Item* item, Unit* target);
    void ImbueItem(Item* item);
    void EnchantItemT(uint32 spellid, uint8 slot);
    int32 GetNearGroupMemberCount(float dis = sPlayerbotAIConfig.SightDistance);

    virtual bool CanCastSpell(std::string const name, Unit* target, Item* itemTarget = nullptr);
    virtual bool CastSpell(std::string const name, Unit* target, Item* itemTarget = nullptr);
    virtual bool HasSpell(std::string const spellName) const;
    virtual bool HasAura(std::string const spellName, Unit* player, bool maxStack = false, bool checkIsOwner = false,
                         int maxAmount = -1, bool checkDuration = false);
    virtual bool HasAnyAuraOf(Unit* player, ...);

    virtual bool IsInterruptableSpellCasting(Unit* player, std::string const spell);
    virtual bool HasAuraToDispel(Unit* player, uint32 dispelType);
    bool CanCastSpell(uint32 spellid, Unit* target, bool checkHasSpell = true, Item* itemTarget = nullptr,
                      Item* castItem = nullptr);
    bool CanCastSpell(uint32 spellid, GameObject* goTarget, bool checkHasSpell = true, Item* castItem = nullptr);
    bool CanCastSpell(uint32 spellid, float x, float y, float z, bool checkHasSpell = true,
                      Item* itemTarget = nullptr);

    Aura* GetAura(std::string const spellName, Unit* unit, bool checkIsOwner = false, bool checkDuration = false,
                  int checkStack = -1);
    bool CastSpell(uint32 spellId, Unit* target, Item* itemTarget = nullptr);
    // The core's SpellCastResult when the last CastSpell(spellId, target) failed in Spell::prepare(), else -1 (upkeep
    // WU11b fix 7: why a flying mount the cast check allowed was not cast); read right after the call
    int32 LastPrepareResult() const { return lastPrepareResult; }
    bool CastSpell(uint32 spellId, float x, float y, float z, Item* itemTarget = nullptr);
    bool canDispel(SpellInfo const* spellInfo, uint32 dispelType);

    bool CanCastVehicleSpell(uint32 spellid, Unit* target);
    bool CastVehicleSpell(uint32 spellId, Unit* target);
    bool CastVehicleSpell(uint32 spellId, float x, float y, float z);
    bool IsInVehicle(bool canControl = false, bool canCast = false, bool canAttack = false, bool canTurn = false,
                     bool fixed = false);

    uint32 GetEquipGearScore(Player* player);
    //uint32 GetEquipGearScore(Player* player, bool withBags, bool withBank);
    static uint32 GetMixedGearScore(Player* player, bool withBags, bool withBank, uint32 topN = 0);
    bool HasSkill(SkillType skill);
    bool IsAllowedCommand(std::string const text);
    float GetRange(std::string const type);

    Player* GetBot() { return bot; }
    Player* GetMaster() { return master; }
    Player* FindNewMaster();

    // Get the group leader or the master of the bot.
    // Checks if the bot is summoned an altbot of a player
    bool IsAltBot();
    bool HasGameClientMaster();
    Player* GetGroupLeader();
    uint32 GetFixedBotNumber(uint32 maxNum = 100);
    GrouperType GetGrouperType();
    GuilderType GetGuilderType();
    bool HasPlayerNearby(WorldPosition* pos, float range = sPlayerbotAIConfig.ReactDistance);
    bool HasPlayerNearby(float range = sPlayerbotAIConfig.ReactDistance);
    bool AllowActive(ActivityType activityType);
    bool AllowActivity(ActivityType activityType = ALL_ACTIVITY, bool checkNow = false);
    bool IsActivityAllowedCached() const { return allowActive[ALL_ACTIVITY]; }
    // LFG state of the bot and of its group, cached because LFGMgr's getters insert on read and must not run on a map
    // thread. RefreshLfgState runs on the world thread only (PlayerbotsMgr::RefreshLfgStates, about once a second).
    void RefreshLfgState();
    lfg::LfgState GetCachedLfgState() const { return lfgBotState; }
    // Called after queueing CMSG_LFG_JOIN, so the bot does not join again before the next refresh reads the real state
    void MarkLfgJoinQueued() { lfgBotState = lfg::LFG_STATE_QUEUED; }
    uint32 AutoScaleActivity(uint32 mod);

    // Check if player is safe to use.
    bool IsSafe(Player* player);
    bool IsSafe(WorldObject* obj);
    ChatChannelSource GetChatChannelSource(Player* bot, uint32 type, std::string channelName);

    bool StarterLevelDistanceCheck(Player* player, WorldLocation const& loc, bool fromStartUp = false);

    bool HasCheat(BotCheatMask mask)
    {
        return ((uint32)mask & (uint32)cheatMask) != 0 ||
               ((uint32)mask & (uint32)sPlayerbotAIConfig.BotCheatMask) != 0;
    }
    BotCheatMask GetCheat() { return cheatMask; }
    void SetCheat(BotCheatMask mask) { cheatMask = mask; }

    void SetMaster(Player* newMaster) { master = newMaster; }
    AiObjectContext* GetAiObjectContext() { return aiObjectContext; }
    ChatHelper* GetChatHelper() { return &chatHelper; }
    bool IsOpposing(Player* player);
    static bool IsOpposing(uint8 race1, uint8 race2);
    PlayerbotSecurity* GetSecurity() { return &security; }

    Position GetJumpDestination() { return jumpDestination; }
    void SetJumpDestination(Position pos) { jumpDestination = pos; }
    void ResetJumpDestination() { jumpDestination = Position(); }

    bool CanMove();
    bool IsInRealGuild();
    static std::vector<std::string> dispel_whitelist;
    bool EqualLowercaseName(std::string s1, std::string s2);
    InventoryResult CanEquipItem(uint8 slot, uint16& dest, Item* pItem, bool swap, bool not_loading = true) const;
    uint8 FindEquipSlot(ItemTemplate const* proto, uint32 slot, bool swap) const;
    std::vector<Item*> GetInventoryAndEquippedItems();
    std::vector<Item*> GetInventoryItems();
    uint32 GetInventoryItemsCountWithId(uint32 itemId);

    /**
     * Consolidate all stackable items in the bot's bags: merge partial stacks of the same
     * entry into the fullest stacks until at most one partial stack remains per entry
     * (the client's "Consolidate" button; WotLK has no server-side equivalent). Uses the
     * core's SwapItem merge path, so no quest/progress hooks fire for moved stacks.
     */
    void ConsolidateItems();
    bool HasItemInInventory(uint32 itemId);
    std::vector<std::pair<Quest const*, uint32>> GetCurrentQuestsRequiringItemId(uint32 itemId);
    uint32 GetReactDelay();
    uint32 ComputeReactDelay();
    // Hidden PvP skill 1-99 (BotProfileMgr), set at login; 0 when Playerbots.Skill.Enable is 0. Only GM commands and
    // the PvP harness records may show it.
    uint8 GetPvpSkill() const { return pvpSkill; }
    // firstRoll (G6b, DT.14): the skill the fixed attributes read; 0 = the skill itself (no growth)
    void SetPvpSkill(uint8 skill, uint8 firstRoll = 0);
    // The first roll SetPvpSkill was given (the skill when it had none), 0 before the first call
    uint8 GetPvpSkillFirstRoll() const { return pvpSkillFirstRoll; }
    void SetArenaOnlyState(ArenaOnlyState state) { arenaOnlyState = state; }
    // Reaction-delay factor from the hidden skill (Playerbots.Skill.React*), cached by SetPvpSkill. 1.0 when the
    // skill system or the factor range is off, and in PvE fights when Playerbots.Skill.FullAbilityInPvE is on.
    // Also 1.0 where the natural cadence and the natural reaction both run (the model replaces the stretch).
    float GetEffectiveSkillReactFactor() const;
    // Natural timing (Playerbots.Natural.Cadence.*), refreshed at each AI check: the bot is in the cadence's contexts
    // (also tracked while the cadence is off and PerfMon on, for its rows), in combat in a started match there, and
    // the cadence's faster check applies
    bool IsInNaturalTimingScope() const { return timingScope; }
    bool IsInNaturalTimingCombat() const { return timingCombat; }
    bool IsNaturalCadenceActive() const { return cadenceCombat; }
    bool IsSkillStretchOff() const { return stretchOff; }
    // The bot's last map update diff (ms): the natural reaction's server ticks are half of it
    uint32 GetLastUpdateDiff() const { return lastUpdateDiff; }
    // At each AI check, by the bot's own update; O(1): the context probe and the match status
    void RefreshNaturalTiming();
    // PvP fight: battleground, arena, duel or PvP combat. Everything else counts as PvE for the skill knobs.
    bool IsPvpFight() const;
    // The same test for any player (situation budgets classify the real players with it). World thread or the
    // player's own map thread.
    static bool IsPlayerInPvpFight(Player* player);
    // Skill knob 2, noticing (Playerbots.Skill.Notice*, SkillStrategy): chance to miss a window and longest notice
    // delay from the hidden skill, cached by SetPvpSkill; both 0 when the knob is off.
    uint32 GetSkillNoticeMissPercent() const { return skillNoticeMissPercent; }
    uint32 GetSkillNoticeDelayMs() const { return skillNoticeDelayMs; }
    SkillNoticeCounters& GetSkillNoticeCounters() { return skillNoticeCounters; }
    // Natural reaction (Playerbots.Natural.Reaction.*, NaturalReaction.h): ln(median) and spread of the bot's reaction
    // time, cached by SetPvpSkill for every skilled bot (read only while the option is on)
    ReactionState const& GetReactionState() const { return reactionState; }
    // The reaction engine, for the world log's case hold line (null before the engines are made)
    ReactionEngine const* GetReactionEngine() const { return reactionEngine; }
    // G0h3's reaction types: the choice, load, ease, think, anticipation and queue prices from the bot's attributes,
    // cached by SetPvpSkill (all 0 at skill 0)
    ReactionTypeState const& GetReactionTypes() const { return reactionTypes; }
    // What the bot saw its enemies cast and cancel in the current match (DT.12; bounded), and its own cast and GCD
    // ends for the queue-like follow-up and the busy-bot rules (G0h3-b). The bot's own update only.
    OpponentMemory& GetOpponentMemory() { return opponentMemory; }
    ReactionBusyState& GetReactionBusy() { return reactionBusy; }
    // PvP test harness: while a match runs, the actions of the notice categories that executed (cast successfully),
    // one slot per category in the order of SkillNoticeCounters. Counting costs nothing outside a match: one flag.
    // Turning it on zeroes the counts.
    void SetPvpTestCounting(bool on);
    bool IsPvpTestCounting() const { return pvpTestCounting; }
    std::array<uint32, SkillNoticeCounters::Categories> const& GetPvpTestExecuted() const { return pvpTestExecuted; }
    // Crowd control by DR group (casts, receipts, trinket and racial breaks) and the tactics' named counters; counted
    // only while counting is on, zeroed when it is turned on
    PvpTestCcCounters const& GetPvpTestCc() const { return pvpTestCc; }
    std::array<uint32, PVPTEST_TACTIC_COUNT> const& GetPvpTestTactics() const { return pvpTestTactics; }
    void CountPvpTestReceivedCc(uint32 drGroup);
    void CountPvpTestCast(uint32 spellId, uint32 drGroup, bool atImmune);
    void CountPvpTestOwnBreak()
    {
        if (pvpTestCounting)
            ++pvpTestCc.OwnBreaks;
    }
    // G5a: the bot dispelled a partner's CC that had been on it ageMs (dispel cc on party)
    void CountPvpTestPartnerDispel(uint32 ageMs)
    {
        if (!pvpTestCounting)
            return;

        ++pvpTestCc.PartnerDispel;
        pvpTestTactics[static_cast<uint32>(TacticCounter::PartnerDispelMs)] += ageMs;
    }
    void CountTactic(TacticCounter counter, uint32 amount = 1)
    {
        if (pvpTestCounting)
            pvpTestTactics[static_cast<uint32>(counter)] += amount;
    }
    // Tactics (Tactics.h): one bit per TacticId. The mask comes from config at login (ResetTacticMask); the PvP
    // harness overrides it per side for a match, and turning counting off gives the config mask back. Written on the
    // world thread or the bot's own, read by the bot's update: atomic.
    uint32 GetTacticMask() const { return tacticMask.load(std::memory_order_relaxed); }
    bool IsTacticMaskForced() const { return tacticMaskForced.load(std::memory_order_relaxed); }
    void SetTacticMask(uint32 mask, bool forced);
    void ResetTacticMask() { SetTacticMask(sPlayerbotAIConfig.TacticsConfigMask, false); }
    // The context probed at the start of the bot's last update (only while Playerbots.Tactics.Enable is 1; Pve before)
    TacticContext GetTacticContext() const
    {
        return static_cast<TacticContext>(tacticContext.load(std::memory_order_relaxed));
    }
    static TacticContext ProbeTacticContext(Player* player);
    // The tactic is in the bot's mask and its scope holds the current context. No master exemption (DT.2).
    bool TacticEnabledHere(TacticId id) const
    {
        return IsTacticAllowed(sPlayerbotAIConfig.TacticsEnable, GetTacticMask(), id,
                               sPlayerbotAIConfig.TacticConfig[static_cast<uint32>(id)].Scope, GetTacticContext());
    }
    // Playerbots.Skill.ExemptRealMasterBots 1 and a game-client master: the skill knobs and the tactic gate leave the
    // bot at full ability. Asks the master (not free) only while the option is on: call it after the cheap exits.
    bool IsSkillExempt() { return sPlayerbotAIConfig.SkillExemptRealMasterBots && HasGameClientMaster(); }
    // Full ability for the tactic gate: skill 0, PvE with Playerbots.Skill.FullAbilityInPvE, or exempt
    bool HasFullSkillAbility();
    // The tactic gate (TacticGate.h), cached by SetPvpSkill: enabled here (mask and scope), known at the bot's skill
    // and used in the current window. Later tactics call this, then NoteTacticUse when they act.
    bool TacticActive(TacticId id);
    TacticGateState const& GetTacticGate() const { return tacticGate; }
    // Effort 0-100 (100 = tries everything) and style traits 0-100 (50 = neutral), hidden like the skill
    uint8 GetSkillEffort() { return HasFullSkillAbility() ? SKILL_GATE_FULL : tacticGate.Effort; }
    uint8 GetSkillStyle(SkillStyleTrait trait) const { return tacticGate.Style[static_cast<uint32>(trait)]; }
    // Attributes (Playerbots.Skill.Attributes.*, BotAttributes.h): eight abilities on the skill's 1-99 scale and four
    // traits (0-100, 50 neutral) drawn from the skill and the guid hash, cached by SetPvpSkill (abilities 0 and traits
    // 50 at skill 0). With the defaults every ability is the skill. Hidden like the skill: GM commands, records only.
    BotAttributeValues const& GetAttributes() const { return attributes; }
    uint8 GetAttribute(BotAttribute attribute) const { return attributes.Get(attribute); }
    // Lab copies (G0j-b): the guid whose hashes draw the attributes and the tactic gate (0 = the bot's own). Set on the
    // world thread, then SetPvpSkill with the template's skill.
    uint32 GetAttributeTemplate() const { return attributeTemplate; }
    void SetAttributeTemplate(uint32 templateGuid) { attributeTemplate = templateGuid; }
    // A tactic fired: counted per tactic and context for .playerbots tactics stats
    void NoteTacticUse(TacticId id) const { NoteTacticActivation(id, GetTacticContext()); }
    // Target hold and team plan (FocusState.h, G1b): the cached FocusParams and the hold's and the plan's state.
    // Touched by the bot's own update; the GM commands read and force it on the world thread while the maps wait.
    FocusBotState& GetFocusState();
    // Crowd control (CcState.h, G4b): the cached CcParams, the DR memory, the rank-resolved table and the trigger's
    // answer. The bot's own update and the aura hooks on its map thread; the GM commands on the world thread.
    CcBotState& GetCcState();
    // Interrupts (ArenaInterrupt.h, G5b): the bot's decisions on the enemy casts in view. The bot's own update.
    InterruptBotState& GetInterruptState();
    // Defensives on burst (ArenaDefensive.h, G5d), fake casts (NaturalCast.h, G5e), natural turning (NaturalTurn.h,
    // G2a) and tracking (NaturalMove.h, G2b): each bot's state, made at first use. The bot's own update (and the
    // aura or spell hooks on its map thread); the GM commands on the world thread while the maps wait.
    DefensiveBotState& GetDefensiveState();
    FakeBotState& GetFakeState();
    TurnBotState& GetTurnState();
    TrackBotState& GetTrackState();
    // Natural paths (NaturalPath.h, G2c) and the Los and Pillar tactics' cover state (ArenaCover.h, G2e, G2f): the same
    PathBotState& GetPathState();
    CoverBotState& GetCoverState();
    // The Kite, Drink and Jump tactics' state (ArenaKite.h, ArenaDrink.h, ArenaJump.h; G3a, G3b, G2g): the same
    KiteBotState& GetKiteState();
    DrinkBotState& GetDrinkState();
    JumpBotState& GetJumpState();
    // The jump test (G2g, `tactics jump test`): while set, UpdateAI runs StepJumpTest instead of the engines (a frozen
    // test bot); one bool test else
    void StepJumpTest(uint32 nowMs);
    void SetJumpTest(bool on) { jumpTest = on; }
    bool IsJumpTest() const { return jumpTest; }
    // The natural pre-gate stepper (G5e's cancel instant, G2a's turn steps): called from UpdateAI before the AI
    // gates on every Player::Update while SetNaturalPreGate(true), else one bool test. nowMs = getMSTime().
    void StepNaturalPreGate(uint32 nowMs);
    void SetNaturalPreGate(bool on) { naturalPreGate = on; }
    bool IsNaturalPreGate() const { return naturalPreGate; }
    // Face the target the way the bot's turn state allows (G2a): true when the bot's facing holds towards it. With
    // the natural facing off this is the instant snap (ServerFacade::SetFacingTo) and always true.
    // site: the caller, for the snap's trace line (13e-decide)
    bool FaceTarget(WorldObject* target, TurnSnapSite site = TurnSnapSite(0));
    // The Interrupt tactic's hook (enabled here): true when the bot's kick may go at the enemy's cast now. One
    // decision per cast (the shortcut or the judging path), its commit point from the bar's start.
    bool DecideInterrupt(Unit* enemy);
    // Debug snapshots for .playerbots tactics show (world thread): the last "fight enemies" result, and the strategy
    // lists, taken at the bot's next update after a request
    void SetFightEnemiesSnapshot(GuidVector const& enemies);
    uint32 GetFightEnemiesSnapshot(std::array<ObjectGuid, TACTIC_FIGHT_ENEMIES_MAX>& enemies) const;
    void RequestStrategySnapshot() { strategySnapshotRequested.store(true, std::memory_order_relaxed); }
    bool GetStrategySnapshot(std::string& text, uint32& takenMs) const;
    // Around Action::Execute: Begin returns the category (0 = not counted) and the current target for a target switch,
    // End counts the action once it has executed (a target switch only when the current target changed)
    uint8 BeginExecutedCount(Action* action, ObjectGuid& previousTarget);
    void EndExecutedCount(uint8 category, ObjectGuid previousTarget);
    // What the counting cost since the start of the process, all bots: the actions that went through
    // BeginExecutedCount (a multiple of 8 per thread) and the summed time of the sampled name lookups
    struct CountingStats
    {
        uint64 Calls = 0;
        uint64 Samples = 0;
        uint64 SampledNs = 0;
    };
    static CountingStats GetCountingStats();

    std::vector<Quest const*> GetAllCurrentQuests();
    std::vector<Quest const*> GetCurrentIncompleteQuests();
    std::set<uint32> GetAllCurrentQuestIds();
    std::set<uint32> GetCurrentIncompleteQuestIds();
    void PetFollow();
    bool ReconcileFollowPassenger(Transport const& expected);
    bool HasLootInteraction() const { return activeLootInteraction != nullptr; }
    uint32 CompletedLootInteractions() const { return completedLootInteractions; }
    bool LootInteractionExpired() const;
    bool ConsumeLootExpiry();
    void CancelLootInteraction(std::string_view reason);
    void TraceTravelInteraction(std::string_view event, std::string_view detail, ObjectGuid loot = ObjectGuid::Empty,
                                uint32 interaction = 0, std::string_view path = "none", float yards = 0.0f,
                                std::optional<std::pair<uint32, uint32>> leg = std::nullopt);
    TravelProgress::Motion ReadTravelWalk(bool checkPosition = false) const;
    enum class MountPath : uint8
    {
        None,
        Timer,
        Packet
    };
    void TraceTravelMount(MountPath path, bool accepted);
    bool HasPendingDismount() const { return pendingDismount.load(std::memory_order_relaxed); }
    static float GetItemScoreMultiplier(ItemQualities quality);
    static bool IsHealingSpell(uint32 spellFamilyName, flag96 spellFamilyFlags);
    static SpellFamilyNames Class2SpellFamilyName(uint8 cls);
    NewRpgInfo rpgInfo;
    NewRpgStatistic rpgStatistic;
    std::unique_ptr<WorldLog::BotLog> worldLog;  // the world log's state (WorldLogBotState.h), lazily
    // World PvP's persistence cache and the ghost's wait (GhostWait.h), lazily
    std::unique_ptr<WorldPvp::PvpMemory> pvpMemory;
    // World PvP's enthusiasm (FightEnthusiasm.h), lazily; not saved
    std::unique_ptr<WorldPvp::BotEnthusiasm> enthusiasm;
    std::unordered_set<uint32> countedQuestCompletions;
    void RecordQuestAccepted(uint32 questId);
    void RecordQuestCompleted(uint32 questId);
    void RecordQuestRewarded(uint32 questId);
    void RecordQuestDropped(uint32 questId, QuestDropReason reason);
    void RecordQuestAbandoned(uint32 questId, QuestAbandonReason reason);
    std::unordered_set<uint32> lowPriorityQuest;
    time_t bgReleaseAttemptTime = 0;
    ForceRebuffState forceRebuff;

    // Schedules a callback to run once after <delayMs> milliseconds.
    void AddTimedEvent(std::function<void()> callback, uint32 delayMs);

    // A test placed the bot on a transport's deck (`route aboard`): the passenger check runs every pass until it rides
    // that transport (`Aboard placed`) or the deadline passes (`Aboard failed`)
    void BeginAboard(uint32 entry);
    static constexpr uint32 ABOARD_DEADLINE_MS = 5000;
    static constexpr char const* ABOARD_EVENT_TYPE = "Aboard";
    // Set on the world thread when an order for this bot is posted (TravelOrders::Post), cleared when none is left: the
    // map thread's one atomic load per update
    std::atomic<bool> orderPosted{false};

private:
    friend class LootObjectStack;
    LootObjectStack* activeLootInteraction = nullptr;
    uint32 completedLootInteractions = 0;
    std::atomic<bool> pendingDismount{false};
    MountPath tracedMount = MountPath::None;
    uint32 tracedMountAt = 0;

    static void _fillGearScoreData(Player* player, Item* item, std::vector<uint32>* gearScore, uint32& twoHandScore,
                                   bool mixed = false);
    bool IsTellAllowed(PlayerbotSecurityLevel securityLevel = PLAYERBOT_SECURITY_ALLOW_ALL);
    void UpdateAIGroupMaster();
    Item* FindItemInInventory(std::function<bool(ItemTemplate const*)> checkItem) const;
    void HandleCommands();
    void HandleCommand(uint32 type, std::string const& text, Player& fromPlayer, const uint32 lang = LANG_UNIVERSAL);
    inline bool IsValidUnit(Unit const* unit) const
    {
        return unit && unit->IsInWorld() && !unit->IsDuringRemoveFromWorld();
    }
    inline bool IsValidPlayer(Player const* player) const
    {
        return player && player->GetSession() && player->IsInWorld() && !player->IsDuringRemoveFromWorld() &&
               !player->IsBeingTeleported();
    }
protected:
    Player* bot;
    Player* master;
    uint32 accountId;
    AiObjectContext* aiObjectContext;
    Engine* currentEngine;
    Engine* engines[BOT_STATE_MAX];
    BotState currentState;
    ChatHelper chatHelper;
    std::list<ChatCommandHolder> chatCommands;
    std::list<ChatQueuedReply> chatReplies;
    PacketHandlingHelper botOutgoingPacketHandlers;
    PacketHandlingHelper masterIncomingPacketHandlers;
    PacketHandlingHelper masterOutgoingPacketHandlers;
    CompositeChatFilter chatFilter;
    PlayerbotSecurity security;
    std::map<std::string, time_t> whispers;
    std::pair<ChatMsg, time_t> currentChat;
    static std::set<std::string> unsecuredCommands;
    bool allowActive[MAX_ACTIVITY_TYPE];
    time_t allowActiveCheckTimer[MAX_ACTIVITY_TYPE];
    // Written by RefreshLfgState (world thread, maps parked) and by MarkLfgJoinQueued (the bot's own update)
    lfg::LfgState lfgBotState = lfg::LFG_STATE_NONE;
    lfg::LfgState lfgGroupState = lfg::LFG_STATE_NONE;
    ReactionEngine* reactionEngine = nullptr;
    bool inCombat = false;
    BotCheatMask cheatMask = BotCheatMask::none;
    Position jumpDestination = Position();
    TravelLifecycle::Cadence passengerCadence;
    void ObserveTravelProgress(NewRpgInfo::Travel& travel, uint32 nowMs, bool revalidate, bool canMove);
    uint32 aboardEntry = 0;    // BeginAboard's transport; 0 = none pending
    uint32 aboardSinceMs = 0;  // getMSTime() of the placement
    bool spellInterruptRequested = false;
    uint8 pvpSkill = 0;
    uint8 pvpSkillFirstRoll = 0;
    ArenaOnlyState arenaOnlyState = ArenaOnlyState::Off;
    uint32 arenaOnlyLeftAt = 0;  // getMSTime() when the bot came back from a battleground, 0 once it queued again
    float skillReactFactor = 1.0f;
    uint32 skillNoticeMissPercent = 0;
    uint32 skillNoticeDelayMs = 0;
    SkillNoticeCounters skillNoticeCounters;
    ReactionState reactionState;
    ReactionTypeState reactionTypes;
    OpponentMemory opponentMemory;
    ReactionBusyState reactionBusy;
    bool timingScope = false;
    bool timingCombat = false;
    bool cadenceCombat = false;
    bool stretchOff = false;
    uint32 lastUpdateDiff = 0;
    TacticGateState tacticGate = ComputeTacticGate(TacticGateParams(), {}, 0, 0);
    std::unique_ptr<FocusBotState> focusState;  // made at first use (GetFocusState); complete in PlayerbotAI.cpp
    std::unique_ptr<CcBotState> ccState;        // made at first use (GetCcState); complete in PlayerbotAI.cpp
    std::unique_ptr<InterruptBotState> interruptState;  // made at first use (GetInterruptState); complete in the .cpp
    std::unique_ptr<DefensiveBotState> defensiveState;  // the same (GetDefensiveState)
    std::unique_ptr<FakeBotState> fakeState;            // the same (GetFakeState)
    std::unique_ptr<TurnBotState> turnState;            // the same (GetTurnState)
    std::unique_ptr<TrackBotState> trackState;          // the same (GetTrackState)
    std::unique_ptr<PathBotState> pathState;            // the same (GetPathState)
    std::unique_ptr<CoverBotState> coverState;          // the same (GetCoverState)
    std::unique_ptr<KiteBotState> kiteState;            // the same (GetKiteState)
    std::unique_ptr<DrinkBotState> drinkState;          // the same (GetDrinkState)
    std::unique_ptr<JumpBotState> jumpState;            // the same (GetJumpState)
    bool jumpTest = false;        // true while the jump test drives the bot (StepJumpTest); one bool test else
    bool naturalPreGate = false;  // true while a pre-gate stepper has work (a turn, a fake cast); one bool test else
    BotAttributeValues attributes = ComputeBotAttributes(BotAttributeParams(), SkillDistributionParams(), 0, 0, 0);
    uint32 attributeTemplate = 0;
    bool pvpTestCounting = false;
    int32 lastPrepareResult = -1;  // LastPrepareResult
    std::array<uint32, SkillNoticeCounters::Categories> pvpTestExecuted{};
    PvpTestCcCounters pvpTestCc;
    std::array<uint32, PVPTEST_TACTIC_COUNT> pvpTestTactics{};
    std::atomic<uint32> tacticMask{0};
    std::atomic<bool> tacticMaskForced{false};
    std::atomic<uint8> tacticContext{static_cast<uint8>(TacticContext::Pve)};
    std::atomic<uint32> fightEnemyCount{0};
    std::array<std::atomic<uint64>, TACTIC_FIGHT_ENEMIES_MAX> fightEnemies{};
    std::atomic<bool> strategySnapshotRequested{false};
    mutable std::mutex strategySnapshotLock;
    std::string strategySnapshot;
    uint32 strategySnapshotMs = 0;
    void TakeStrategySnapshot();
};

#endif
