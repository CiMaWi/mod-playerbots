/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_EVENTLOGFORMAT_H
#define _PLAYERBOT_EVENTLOGFORMAT_H

#include <cstdint>
#include <string>
#include <string_view>

// The Player-free part of the event log (EventLog::Emit, the world log's event rows): field cleaning and source
// words (the thread-local source scope is in EventLog.h).
// Header-only so tests/event-log-selftest.cpp checks the same code the server runs.
namespace EventLog
{
constexpr char const* SOURCE_OTHER = "other";
constexpr char FIELD_SEPARATOR = ',';
constexpr char FIELD_REPLACEMENT = '_';
// Copies a field without the characters that would split the CSV line or the row
inline void AppendField(std::string& out, std::string_view field)
{
    for (char const c : field)
        out.push_back(c == FIELD_SEPARATOR || c == '"' || c == '\n' || c == '\r' ? FIELD_REPLACEMENT : c);
}

struct SourceWord
{
    std::string_view key;
    char const* word;
};

// Executing action name -> the plan's source word; the first key the name contains wins, other names pass through
inline constexpr SourceWord SOURCE_WORDS[] = {
    {"loot", "loot"},     {"quest", "quest"},      {"reward", "quest"},    {"repair", "repair"},
    {"sell", "vendor"},   {"buy", "vendor"},       {"train", "train"},     {"taxi", "flight"},
    {"flight", "flight"}, {"mail", "mail"},        {"auction", "auction"}, {"trade", "trade"},
    {"craft", "craft"},   {"disenchant", "craft"}, {"corpse", "corpse"},   {"spirit healer", "spirit_healer"},
};

inline std::string_view SourceWordOf(std::string_view actionName)
{
    if (actionName.empty())
        return SOURCE_OTHER;

    for (SourceWord const& entry : SOURCE_WORDS)
        if (actionName.find(entry.key) != std::string_view::npos)
            return entry.word;

    return actionName;
}

// The item StoreNewItem stored last on this thread, with its entry. The item hooks that follow StoreNewItem (loot,
// group roll, quest reward, create) run after the hooks of scripts registered earlier, which may destroy the item
// (mod-junk-to-gold sells looted junk at once; a new item is deleted on the spot). Those hooks only compare the
// pointer with this note and never read the item; the core compares a possibly deleted item's pointer the same way
// (Player::MoveItemToInventory).
struct StoredItemNote
{
    void const* item = nullptr;
    uint32_t entry = 0;
};

inline thread_local StoredItemNote storedItemNote;

inline void NoteStoredItem(void const* item, uint32_t entry) { storedItemNote = {item, entry}; }

// The entry noted for this item by the StoreNewItem just before, or 0; a note answers once
inline uint32_t TakeStoredEntry(void const* item)
{
    StoredItemNote const note = storedItemNote;
    storedItemNote = {};
    return item && item == note.item ? note.entry : 0;
}

// The lesson open on this thread: a purchase that teaches spells, from its debit to its end (Trainer::TeachSpell
// between OnPlayerBeforeTrainSpell and OnPlayerAfterTrainSpell; TrainerAction::Learn and the riding mount purchase
// around their own debit and learn). Every learn of its player inside it is paid; the first one takes the price,
// the spells the lesson teaches with it are free. thread_local: a purchase debits and learns in one call.
struct LessonNote
{
    void const* player = nullptr;
    uint32_t cost = 0;
};

inline thread_local LessonNote lessonNote;

// A free purchase opens nothing: no money moved, so its learns are not paid
inline void OpenLesson(void const* player, uint32_t cost)
{
    lessonNote = cost ? LessonNote{player, cost} : LessonNote{};
}

inline void CloseLesson() { lessonNote = {}; }

// Whether this player's learn is inside a lesson; cost: the price for the first learn, 0 for the later ones
inline bool TakeLessonCost(void const* player, uint32_t& cost)
{
    cost = 0;
    if (!player || player != lessonNote.player)
        return false;

    cost = lessonNote.cost;
    lessonNote.cost = 0;
    return true;
}

// The lesson of a module purchase (the core's lesson is bracketed by its two trainer hooks)
class LessonScope
{
public:
    LessonScope(void const* player, uint32_t cost) { OpenLesson(player, cost); }
    ~LessonScope() { CloseLesson(); }

    LessonScope(LessonScope const&) = delete;
    LessonScope& operator=(LessonScope const&) = delete;
};

// A swap's replaced equipment, from the swap hook (OnPlayerBeforeSwapItem, Player::SwapItem and the auto-equip
// handler) to the EquipItem calls of the same call: per equipment slot the swap fills, the item moving in and the
// level of the one it replaces. A swap fills up to two slots (two rings change places), a swap inside the bags none.
// The pointers are only compared.
struct SwapSlot
{
    void const* item = nullptr;
    uint8_t slot = 0xFF;
    uint32_t oldLevel = 0;
};

struct SwapNote
{
    SwapSlot slots[2];
};

inline thread_local SwapNote swapNote;

inline void NoteSwap(SwapSlot intoDst, SwapSlot intoSrc) { swapNote = {{intoDst, intoSrc}}; }

// A SwapSlot's item for a ReplaceScope: any item equipped into the slot inside the scope
inline constexpr char ANY_ITEM = 0;

// The level of the item this equip replaces when the swap just before moved this item into this slot (answers
// once); false for an equip into an empty slot
inline bool TakeReplacedLevel(void const* item, uint8_t slot, uint32_t& oldLevel)
{
    oldLevel = 0;
    if (!item)
        return false;

    for (SwapSlot& noted : swapNote.slots)
        if ((noted.item == item || noted.item == &ANY_ITEM) && noted.slot == slot)
        {
            oldLevel = noted.oldLevel;
            noted = {};
            return true;
        }

    return false;
}

// A replacement without a swap (PlayerbotFactory's upgrade: the old item goes to the bags, then EquipNewItem puts
// the new one into the emptied slot): the equip into that slot inside the scope replaces an item at oldLevel
class ReplaceScope
{
public:
    ReplaceScope(uint8_t slot, uint32_t oldLevel) { NoteSwap(SwapSlot{&ANY_ITEM, slot, oldLevel}, SwapSlot{}); }
    ~ReplaceScope() { swapNote = {}; }

    ReplaceScope(ReplaceScope const&) = delete;
    ReplaceScope& operator=(ReplaceScope const&) = delete;
};

// The world log's WL6 words. The rank and XP source tables mirror the core's enums by value (CREATURE_ELITE_*,
// PlayerXPSource), so the selftests need no server headers.
constexpr uint32_t RANK_COUNT = 5;
inline constexpr char const* RANK_WORDS[RANK_COUNT] = {"normal", "elite", "rare_elite", "world_boss", "rare"};

// Kill src: the creature's rank
inline char const* RankWord(uint32_t rank) { return rank < RANK_COUNT ? RANK_WORDS[rank] : SOURCE_OTHER; }

constexpr uint32_t XP_SOURCE_COUNT = 5;
inline constexpr char const* XP_SOURCE_WORDS[XP_SOURCE_COUNT] = {"kill", "quest", "quest", "explore", SOURCE_OTHER};

// XpGain src: kill, quest (also the dungeon finder's), explore; battleground XP is other
inline char const* XpSourceWord(uint32_t source)
{
    return source < XP_SOURCE_COUNT ? XP_SOURCE_WORDS[source] : SOURCE_OTHER;
}

enum class MapKind : uint8_t
{
    World,
    Dungeon,
    Raid,
    Battleground,
    Arena
};

constexpr uint32_t MAP_KIND_COUNT = 5;
inline constexpr char const* MAP_KIND_WORDS[MAP_KIND_COUNT] = {"world", "dungeon", "raid", "battleground", "arena"};

// MapChange src: the new map's kind
inline char const* MapKindWord(MapKind kind)
{
    uint32_t const index = static_cast<uint32_t>(kind);
    return index < MAP_KIND_COUNT ? MAP_KIND_WORDS[index] : SOURCE_OTHER;
}

constexpr char const* FLIGHT_START = "start";
constexpr char const* FLIGHT_END = "end";
constexpr char const* LEARN_TRAINER = "trainer";
constexpr char const* LEARN_TALENT = "talent";
constexpr char const* LEARN_QUEST = "quest";
constexpr char const* RESURRECT_SPIRIT_HEALER = "spirit_healer";
constexpr char const* RESURRECT_CORPSE = "corpse";
constexpr char const* RESURRECT_SPELL = "spell";

// SpellLearn src: trainer (learned inside a paid lesson, or by a training action), talent, quest (a quest action),
// else other. scopeWord is SourceWordOf(CurrentSource()).
inline char const* SpellLearnWord(std::string_view scopeWord, bool paid, bool talent)
{
    if (paid || scopeWord == "train")
        return LEARN_TRAINER;
    if (talent)
        return LEARN_TALENT;
    if (scopeWord == "quest")
        return LEARN_QUEST;

    return SOURCE_OTHER;
}

// Resurrect src: the action's word (spirit_healer, corpse); else a spirit healer applies sickness, a resurrection
// spell restores nothing in the call (health is set after it), anything else is other
inline char const* ResurrectWord(std::string_view scopeWord, bool applySickness, float restorePercent)
{
    if (scopeWord == RESURRECT_SPIRIT_HEALER || applySickness)
        return RESURRECT_SPIRIT_HEALER;
    if (scopeWord == RESURRECT_CORPSE)
        return RESURRECT_CORPSE;
    if (restorePercent <= 0.0f)
        return RESURRECT_SPELL;

    return SOURCE_OTHER;
}
}  // namespace EventLog

#endif
