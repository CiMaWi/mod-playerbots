/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "PvpTestFight.h"

#include <algorithm>
#include <cmath>
#include <ctime>
#include <set>

#include "AiFactory.h"
#include "CharacterCache.h"
#include "Chat.h"
#include "DBCStores.h"
#include "Group.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "Opcodes.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "PvpTest.h"
#include "RandomPlayerbotMgr.h"
#include "ScriptMgr.h"
#include "StringFormat.h"
#include "World.h"
#include "WorldPacket.h"
#include "WorldSession.h"

using namespace PvpTestUtil;

namespace
{
constexpr uint32 SpellDuel = 7266;
constexpr uint32 ArrivalTimeoutMs = 30000;    // from the teleport until every bot stands at the spot
constexpr uint32 ChallengeTimeoutMs = 10000;  // from the duel cast until both bots have a duel
constexpr uint32 AcceptFallbackMs = 3000;     // the challenged bot gets the accept packet if its AI has not sent it
constexpr size_t MaxPerSide = 5;
constexpr float SideOffset = 4.0f;      // yards from the fight spot to each side
constexpr float MemberSpread = 1.5f;    // yards between the members of one side
constexpr float SpawnHeight = 0.5f;     // above the configured ground height, so nobody lands inside the terrain
constexpr float ArrivalRadius = 12.0f;  // yards from the fight spot that count as arrived
constexpr float BackRadius = 6.0f;      // yards from the saved position that count as back
constexpr float Pi = 3.14159265f;
constexpr uint32 DuelEndSeen = 0x80000000;
constexpr uint32 DuelTypeShift = 16;
constexpr uint32 DuelSlotShift = 8;
constexpr uint32 ByteMask = 0xFF;

constexpr char const* DuelUsage = "Usage: .playerbots pvptest duel <botA> <botB>";
constexpr char const* BrawlUsage =
    "Usage: .playerbots pvptest brawl <bot,bot,...> vs <bot,bot,...> (opposite factions)";

std::string SideFaction(PvpTestSide const& side)
{
    if (!side.Members.empty())
    {
        if (CharacterCacheEntry const* entry = sCharacterCache->GetCharacterCacheByGuid(side.Members.front()))
            return Player::TeamIdForRace(entry->Race) == TEAM_ALLIANCE ? "alliance" : "horde";
    }

    return "alliance";
}

// Reads a roster of bot names into a side; the bots are max-level random bots
bool ResolveRoster(std::string const& spec, PvpTestSide& side, std::string& error)
{
    side = PvpTestSide();
    for (std::string name : Split(spec, ','))
    {
        ObjectGuid const guid =
            normalizePlayerName(name) ? sCharacterCache->GetCharacterGuidByName(name) : ObjectGuid::Empty;
        if (guid.IsEmpty())
        {
            error = Acore::StringFormat("No character named '{}'", name);
            return false;
        }

        if (!CheckBot(guid, error))
            return false;

        side.Members.push_back(guid);
        side.Names.push_back(sCharacterCache->GetCharacterCacheByGuid(guid)->Name);
        side.Label += side.Label.empty() ? side.Names.back() : "," + side.Names.back();
    }

    side.LoginRequested.assign(side.Members.size(), false);
    return !side.Members.empty();
}

// The place of one bot at the fight spot: side A in front of the spot, side B behind it, facing each other
struct Spot
{
    float X = 0.0f;
    float Y = 0.0f;
    float Z = 0.0f;
    float O = 0.0f;
};

Spot MemberSpot(size_t side, size_t index, size_t count)
{
    float const forward = side == 0 ? SideOffset : -SideOffset;
    float const lateral = (static_cast<float>(index) - static_cast<float>(count - 1) / 2.0f) * MemberSpread;
    float const facing = sPlayerbotAIConfig.PvpTestFightO;
    Spot spot;
    spot.X = sPlayerbotAIConfig.PvpTestFightX + std::cos(facing) * forward - std::sin(facing) * lateral;
    spot.Y = sPlayerbotAIConfig.PvpTestFightY + std::sin(facing) * forward + std::cos(facing) * lateral;
    spot.Z = sPlayerbotAIConfig.PvpTestFightZ + SpawnHeight;
    spot.O = Position::NormalizeOrientation(side == 0 ? facing + Pi : facing);
    return spot;
}

// Full health and mana, no cooldowns: the start state of a fight and the state the bots are sent back in
void Restore(Player* player)
{
    player->RemoveAllSpellCooldown();
    player->SetFullHealth();
    if (player->GetMaxPower(POWER_MANA))
        player->SetPower(POWER_MANA, player->GetMaxPower(POWER_MANA));
}

// Points a brawler at an opponent the way AttackAction does, so the combat engine takes over
void Aim(Player* player, Player* target)
{
    PlayerbotAI* botAI = GET_PLAYERBOT_AI(player);
    if (!botAI)
        return;

    player->SetSelection(target->GetGUID());
    botAI->GetAiObjectContext()->GetValue<GuidVector>("prioritized targets")->Set({target->GetGUID()});
    botAI->GetAiObjectContext()->GetValue<Unit*>("current target")->Set(target);
    botAI->ChangeEngine(BOT_STATE_COMBAT);
    player->Attack(target, PlayerbotAI::IsMelee(player));
}

class PvpTestFightUnitScript : public UnitScript
{
public:
    PvpTestFightUnitScript()
        : UnitScript("PvpTestFightUnitScript", true, {UNITHOOK_ON_DAMAGE, UNITHOOK_ON_HEAL, UNITHOOK_ON_UNIT_DEATH}),
          _stats(PvpTestFightStats::Instance())
    {
    }

    void OnDamage(Unit* attacker, Unit* victim, uint32& damage) override { _stats.OnDamage(attacker, victim, damage); }

    void OnHeal(Unit* healer, Unit* receiver, uint32& gain) override { _stats.OnHeal(healer, receiver, gain); }

    // PlayerScript::OnPlayerPVPKill fires for player killers only; this one sees pets and ghouls
    void OnUnitDeath(Unit* unit, Unit* killer) override { _stats.OnUnitDeath(unit, killer); }

private:
    PvpTestFightStats& _stats;
};

class PvpTestFightPlayerScript : public PlayerScript
{
public:
    PvpTestFightPlayerScript()
        : PlayerScript("PvpTestFightPlayerScript", {PLAYERHOOK_ON_PVP_KILL, PLAYERHOOK_ON_DUEL_END}),
          _stats(PvpTestFightStats::Instance())
    {
    }

    void OnPlayerPVPKill(Player* killer, Player* killed) override { _stats.OnKill(killer, killed); }

    void OnPlayerDuelEnd(Player* winner, Player* loser, DuelCompleteType type) override
    {
        _stats.OnDuelEnd(winner, loser, static_cast<uint32>(type));
    }

private:
    PvpTestFightStats& _stats;
};
}  // namespace

void AddPvpTestFightScripts()
{
    new PvpTestFightUnitScript();
    new PvpTestFightPlayerScript();
}

// ---------------------------------------------------------------------------------------------------------------------
// Counters

PvpTestFightStats::PvpTestFightStats()
{
    for (size_t slot = 0; slot < MaxSlots; ++slot)
    {
        _guids[slot].store(0);
        _damage[slot].store(0);
        _healing[slot].store(0);
        _kills[slot].store(0);
        _deaths[slot].store(0);
    }
}

PvpTestFightStats& PvpTestFightStats::Instance()
{
    static PvpTestFightStats instance;
    return instance;
}

void PvpTestFightStats::Begin(std::vector<ObjectGuid> const& members, size_t sideASize)
{
    _active.store(false);
    for (size_t slot = 0; slot < MaxSlots; ++slot)
    {
        _guids[slot].store(slot < members.size() ? members[slot].GetRawValue() : 0);
        _damage[slot].store(0);
        _healing[slot].store(0);
        _kills[slot].store(0);
        _deaths[slot].store(0);
    }

    _sideASize.store(sideASize);
    _duelEnd.store(0);
    _active.store(true);
}

void PvpTestFightStats::End() { _active.store(false); }

PvpTestFightStats::Totals PvpTestFightStats::Get(size_t slot) const
{
    Totals totals;
    if (slot < MaxSlots)
    {
        totals.Damage = _damage[slot].load();
        totals.Healing = _healing[slot].load();
        totals.KillingBlows = _kills[slot].load();
        totals.Deaths = _deaths[slot].load();
    }

    return totals;
}

PvpTestFightStats::DuelEnd PvpTestFightStats::GetDuelEnd() const
{
    DuelEnd end;
    uint32 const packed = _duelEnd.load();
    if (packed & DuelEndSeen)
    {
        end.Seen = true;
        end.Type = (packed >> DuelTypeShift) & ByteMask;
        end.Winner = (packed >> DuelSlotShift) & ByteMask;
        end.Loser = packed & ByteMask;
    }

    return end;
}

size_t PvpTestFightStats::FindSlot(uint64 rawGuid) const
{
    for (size_t slot = 0; slot < MaxSlots; ++slot)
    {
        if (_guids[slot].load(std::memory_order_relaxed) == rawGuid)
            return slot;
    }

    return NoSlot;
}

bool PvpTestFightStats::SameSide(size_t first, size_t second) const
{
    size_t const sideASize = _sideASize.load(std::memory_order_relaxed);
    return (first < sideASize) == (second < sideASize);
}

// Hot path: one relaxed load per damage event while no fight runs; pets count for their owner
void PvpTestFightStats::OnDamage(Unit* attacker, Unit* victim, uint32 damage)
{
    if (!_active.load(std::memory_order_relaxed) || !attacker || !victim)
        return;

    Unit* source = attacker->GetCharmerOrOwnerOrSelf();
    size_t const from = FindSlot(source->GetGUID().GetRawValue());
    if (from == NoSlot)
        return;

    size_t const to = FindSlot(victim->GetGUID().GetRawValue());
    if (to == NoSlot || SameSide(from, to))
        return;

    // The damage of a killing blow is more than the health that was left
    _damage[from].fetch_add(std::min(damage, static_cast<uint32>(victim->GetHealth())), std::memory_order_relaxed);
}

void PvpTestFightStats::OnHeal(Unit* healer, Unit* receiver, uint32 gain)
{
    if (!_active.load(std::memory_order_relaxed) || !healer || !receiver)
        return;

    Unit* source = healer->GetCharmerOrOwnerOrSelf();
    size_t const from = FindSlot(source->GetGUID().GetRawValue());
    if (from == NoSlot)
        return;

    size_t const to = FindSlot(receiver->GetGUID().GetRawValue());
    if (to == NoSlot || !SameSide(from, to))
        return;

    _healing[from].fetch_add(gain, std::memory_order_relaxed);
}

void PvpTestFightStats::OnKill(Player* killer, Player* killed)
{
    if (!_active.load(std::memory_order_relaxed) || !killer || !killed)
        return;

    CountKill(killer->GetGUID().GetRawValue(), killed->GetGUID().GetRawValue());
}

// A pet, ghoul or other guardian that kills a member counts for its owner (O(1): one owner lookup)
void PvpTestFightStats::OnUnitDeath(Unit* victim, Unit* killer)
{
    if (!_active.load(std::memory_order_relaxed) || !victim || !killer || killer->IsPlayer() || !victim->IsPlayer())
        return;

    Unit* owner = killer->GetCharmerOrOwnerOrSelf();
    if (owner == killer || !owner->IsPlayer())
        return;

    CountKill(owner->GetGUID().GetRawValue(), victim->GetGUID().GetRawValue());
}

void PvpTestFightStats::CountKill(uint64 killerGuid, uint64 killedGuid)
{
    size_t const from = FindSlot(killerGuid);
    size_t const to = FindSlot(killedGuid);
    if (from == NoSlot || to == NoSlot || SameSide(from, to))
        return;

    _kills[from].fetch_add(1, std::memory_order_relaxed);
    _deaths[to].fetch_add(1, std::memory_order_relaxed);
}

void PvpTestFightStats::OnDuelEnd(Player* winner, Player* loser, uint32 type)
{
    if (!_active.load(std::memory_order_relaxed) || !winner || !loser)
        return;

    size_t const won = FindSlot(winner->GetGUID().GetRawValue());
    size_t const lost = FindSlot(loser->GetGUID().GetRawValue());
    if (won == NoSlot || lost == NoSlot)
        return;

    _duelEnd.store(DuelEndSeen | (type << DuelTypeShift) | (static_cast<uint32>(won) << DuelSlotShift) |
                   static_cast<uint32>(lost));
}

// ---------------------------------------------------------------------------------------------------------------------
// The job

PvpTestFightJob::PvpTestFightJob(PvpTestFightParams const& params, PvpTestSide sideA, PvpTestSide sideB,
                                 ObjectGuid issuer)
    : _params(params), _sideA(std::move(sideA)), _sideB(std::move(sideB)), _issuer(issuer)
{
    _sideASize = _sideA.Members.size();
    for (PvpTestSide const* side : {&_sideA, &_sideB})
    {
        _members.insert(_members.end(), side->Members.begin(), side->Members.end());
        _names.insert(_names.end(), side->Names.begin(), side->Names.end());
    }

    _loginRequested.assign(_members.size(), false);
    _arrived.assign(_members.size(), false);
    _origins.assign(_members.size(), Origin());

    // The record has every field from the start, so a failed or stopped fight is still a complete line
    _record.Kind = _params.Kind;
    _record.Id = _params.MatchId;
    _record.StartTime = PvpTestRecordWriter::IsoTimeNow();
    _record.ArenaType = 0;
    _record.Rated = false;
    for (size_t index = 0; index < _record.Sides.size(); ++index)
    {
        PvpTestSide const& side = index == 0 ? _sideA : _sideB;
        PvpTestSideRecord& sideRecord = _record.Sides[index];
        sideRecord.Label = side.Label;
        sideRecord.Side = index == 0 ? 'A' : 'B';
        sideRecord.Faction = SideFaction(side);
        for (size_t member = 0; member < side.Members.size(); ++member)
        {
            PvpTestBotRecord bot;
            bot.Guid = side.Members[member].GetCounter();
            bot.Name = side.Names[member];
            if (CharacterCacheEntry const* entry = sCharacterCache->GetCharacterCacheByGuid(side.Members[member]))
            {
                bot.ClassId = entry->Class;
                bot.RaceId = entry->Race;
                bot.Level = entry->Level;
            }

            sideRecord.Bots.push_back(std::move(bot));
        }
    }

    EnterStage(Stage::Gather);
}

PvpTestFightJob::~PvpTestFightJob()
{
    if (_statsBegun)
        PvpTestFightStats::Instance().End();
}

char const* PvpTestFightJob::StageName(Stage stage)
{
    switch (stage)
    {
        case Stage::Gather:
            return "gather";
        case Stage::Fight:
            return "fight";
        case Stage::TearDown:
            return "tear-down";
        case Stage::Done:
            return "done";
    }

    return "unknown";
}

bool PvpTestFightJob::Resolve(PvpTestFightParams const& params, PvpTestSide& sideA, PvpTestSide& sideB,
                              std::string& error)
{
    bool const duel = params.Kind == "duel";
    if (!ResolveRoster(params.SideA, sideA, error) || !ResolveRoster(params.SideB, sideB, error))
    {
        if (error.empty())
            error = "A side has no bot";

        return false;
    }

    if (duel && (sideA.Members.size() != 1 || sideB.Members.size() != 1))
    {
        error = "A duel is one bot against one bot";
        return false;
    }

    if (sideA.Members.size() > MaxPerSide || sideB.Members.size() > MaxPerSide)
    {
        error = Acore::StringFormat("A side has at most {} bots", MaxPerSide);
        return false;
    }

    std::set<ObjectGuid> distinct(sideA.Members.begin(), sideA.Members.end());
    distinct.insert(sideB.Members.begin(), sideB.Members.end());
    if (distinct.size() != sideA.Members.size() + sideB.Members.size())
    {
        error = "A bot is listed twice (the sides must not overlap)";
        return false;
    }

    // Only bots of opposing factions attack each other in the open world: same-faction bots can only duel
    if (!duel)
    {
        for (PvpTestSide const* side : {&sideA, &sideB})
        {
            std::set<TeamId> teams;
            for (ObjectGuid const& guid : side->Members)
                teams.insert(Player::TeamIdForRace(sCharacterCache->GetCharacterCacheByGuid(guid)->Race));

            if (teams.size() != 1)
            {
                error =
                    Acore::StringFormat("Side '{}' mixes factions; a brawl needs one faction per side", side->Label);
                return false;
            }
        }

        if (SideFaction(sideA) == SideFaction(sideB))
        {
            error = "A brawl needs opposite factions (bots of one faction are not hostile; use a duel)";
            return false;
        }
    }

    return true;
}

void PvpTestFightJob::EnterStage(Stage stage)
{
    _stage = stage;
    _stageMs = 0;
    if (stage == Stage::TearDown && _statsBegun)
    {
        // The counters are read before the next fight can reset them
        if (!_result.FightMs)
            _result.FightMs = _totalMs - _fightStartMs;

        CaptureStats();
        PvpTestFightStats::Instance().End();
        _statsBegun = false;
    }

    LOG_INFO("playerbots", "PVPTEST {} {} vs {}: stage {}", _params.Kind, _sideA.Label, _sideB.Label, StageName(stage));
}

void PvpTestFightJob::Fail(std::string const& reason)
{
    _result.Outcome = "failed";
    _result.FailedStage = StageName(_stage);
    _result.Reason = reason;
    EnterStage(Stage::TearDown);
}

void PvpTestFightJob::Abort(std::string const& reason)
{
    if (_stage == Stage::Done || _stage == Stage::TearDown)
        return;

    _result.Outcome = "stopped";
    _result.FailedStage = StageName(_stage);
    _result.Reason = reason;
    EnterStage(Stage::TearDown);
}

void PvpTestFightJob::Update(uint32 diff)
{
    if (_stage == Stage::Done)
        return;

    _stageMs += diff;
    _totalMs += diff;
    _tickMs += diff;
    if (_tickMs < TickIntervalMs)
        return;

    _tickMs = 0;
    switch (_stage)
    {
        case Stage::Gather:
            UpdateGather();
            break;
        case Stage::Fight:
            if (_phase == Phase::Arrive)
                UpdateArrive();
            else if (_phase == Phase::Challenge)
                UpdateChallenge();
            else if (IsDuel())
                UpdateDuel();
            else
                UpdateBrawl();
            break;
        case Stage::TearDown:
            UpdateTearDown();
            break;
        default:
            break;
    }
}

// The pattern of PvpTestArenaJob::GatherMember: log in, out of battlegrounds and queues, alive, out of combat
bool PvpTestFightJob::GatherMember(size_t index)
{
    Player* player = ObjectAccessor::FindConnectedPlayer(_members[index]);
    if (!player)
    {
        if (!_loginRequested[index])
        {
            // Asynchronous: the bot appears on a later tick
            _loginRequested[index] = true;
            sRandomPlayerbotMgr.AddPlayerBot(_members[index], 0);
        }

        return false;
    }

    if (!player->IsInWorld() || player->IsBeingTeleported())
        return false;

    PlayerbotAI* botAI = GET_PLAYERBOT_AI(player);
    if (!botAI)
    {
        Fail(Acore::StringFormat("{} is online without bot AI", _names[index]));
        return false;
    }

    if (botAI->HasGameClientMaster())
    {
        Fail(Acore::StringFormat("{} is with a real player", _names[index]));
        return false;
    }

    if (player->InBattleground())
    {
        player->LeaveBattleground();
        return false;
    }

    if (player->InBattlegroundQueue())
    {
        LeaveQueues(player);
        return false;
    }

    if (player->GetMap()->Instanceable())
    {
        Fail(Acore::StringFormat("{} is inside an instance (map {})", _names[index], player->GetMapId()));
        return false;
    }

    if (player->GetVehicle())
    {
        Fail(Acore::StringFormat("{} is on a vehicle", _names[index]));
        return false;
    }

    if (player->IsInFlight())
        return false;

    if (!player->IsAlive())
    {
        player->ResurrectPlayer(1.0f);
        player->SpawnCorpseBones();
        return false;
    }

    return !player->IsInCombat();
}

void PvpTestFightJob::UpdateGather()
{
    std::vector<std::string> waiting;
    for (size_t index = 0; index < _members.size(); ++index)
    {
        if (!GatherMember(index))
            waiting.push_back(_names[index]);

        if (_stage != Stage::Gather)
            return;  // a member failed the job
    }

    if (!waiting.empty())
    {
        if (_stageMs >= GatherTimeoutMs)
        {
            std::string names;
            for (std::string const& name : waiting)
                names += names.empty() ? name : ", " + name;

            Fail(Acore::StringFormat("not ready after {} s: {}", GatherTimeoutMs / MsPerSecond, names));
        }

        return;
    }

    SaveOriginsAndTeleport();
}

// Saves where every bot stands, takes it out of its group (a follower would walk back to its leader) and sends it to
// its place at the fight spot
void PvpTestFightJob::SaveOriginsAndTeleport()
{
    EnterStage(Stage::Fight);
    _phase = Phase::Arrive;
    for (size_t index = 0; index < _members.size(); ++index)
    {
        Player* player = ObjectAccessor::FindConnectedPlayer(_members[index]);
        PlayerbotAI* botAI = player ? GET_PLAYERBOT_AI(player) : nullptr;
        if (!botAI)
        {
            Fail(Acore::StringFormat("{} went offline", _names[index]));
            return;
        }

        Origin& origin = _origins[index];
        origin.Saved = true;
        origin.MapId = player->GetMapId();
        origin.X = player->GetPositionX();
        origin.Y = player->GetPositionY();
        origin.Z = player->GetPositionZ();
        if (player->IsFlying() || player->IsFalling())
        {
            // Mounted in the air: remember the ground below, so the bot does not fall when it is sent back
            float const ground = player->GetMapHeight(origin.X, origin.Y, origin.Z);
            if (ground > INVALID_HEIGHT)
                origin.Z = ground + SpawnHeight;
        }

        origin.O = player->GetOrientation();
        origin.Pvp = player->IsPvP();
        LOG_INFO("playerbots", "PVPTEST {} {} vs {}: {} saved at map {} ({:.1f} {:.1f} {:.1f})", _params.Kind,
                 _sideA.Label, _sideB.Label, _names[index], origin.MapId, origin.X, origin.Y, origin.Z);
    }

    for (size_t index = 0; index < _members.size(); ++index)
    {
        Player* player = ObjectAccessor::FindConnectedPlayer(_members[index]);
        if (!player)
            continue;

        if (Group* group = player->GetGroup())
            group->RemoveMember(_members[index]);

        if (PlayerbotAI* botAI = GET_PLAYERBOT_AI(player))
            botAI->Reset();

        player->RemoveAurasByType(SPELL_AURA_MOUNTED);
        size_t const side = SideOf(index);
        size_t const first = side == 0 ? 0 : _sideASize;
        size_t const count = side == 0 ? _sideASize : _members.size() - _sideASize;
        Spot const spot = MemberSpot(side, index - first, count);
        player->TeleportTo(sPlayerbotAIConfig.PvpTestFightMap, spot.X, spot.Y, spot.Z, spot.O);
    }
}

void PvpTestFightJob::UpdateArrive()
{
    std::vector<std::string> waiting;
    for (size_t index = 0; index < _members.size(); ++index)
    {
        Player* player = ObjectAccessor::FindConnectedPlayer(_members[index]);
        if (!player)
        {
            Fail(Acore::StringFormat("{} went offline", _names[index]));
            return;
        }

        bool const there =
            player->IsInWorld() && !player->IsBeingTeleported() &&
            player->GetMapId() == sPlayerbotAIConfig.PvpTestFightMap &&
            player->GetExactDist2d(sPlayerbotAIConfig.PvpTestFightX, sPlayerbotAIConfig.PvpTestFightY) < ArrivalRadius;
        _arrived[index] = there;
        if (!there)
            waiting.push_back(_names[index]);
    }

    if (!waiting.empty())
    {
        if (_stageMs >= ArrivalTimeoutMs)
        {
            std::string names;
            for (std::string const& name : waiting)
                names += names.empty() ? name : ", " + name;

            Fail(Acore::StringFormat("did not arrive at the fight spot after {} s: {}", ArrivalTimeoutMs / MsPerSecond,
                                     names));
        }

        return;
    }

    // At the spot: the area must allow the fight
    for (size_t index = 0; index < _members.size(); ++index)
    {
        Player* player = ObjectAccessor::FindConnectedPlayer(_members[index]);
        AreaTableEntry const* area = sAreaTableStore.LookupEntry(player->GetAreaId());
        if (sPlayerbotAIConfig.IsPvpProhibited(player->GetZoneId(), player->GetAreaId()))
        {
            Fail(Acore::StringFormat("the fight spot is in a PvP prohibited zone (zone {}, area {})",
                                     player->GetZoneId(), player->GetAreaId()));
            return;
        }

        if (IsDuel() && (!area || !(area->flags & AREA_FLAG_ALLOW_DUELS)))
        {
            Fail(Acore::StringFormat("the fight spot does not allow duels (zone {}, area {}, flags {:#x})",
                                     player->GetZoneId(), player->GetAreaId(), area ? area->flags : 0));
            return;
        }

        if (!IsDuel() && area && area->IsSanctuary())
        {
            Fail(Acore::StringFormat("the fight spot is a sanctuary (zone {}, area {})", player->GetZoneId(),
                                     player->GetAreaId()));
            return;
        }
    }

    // The same facts the arena records, taken while all bots are online
    for (size_t index = 0; index < _members.size(); ++index)
    {
        Player* player = ObjectAccessor::FindConnectedPlayer(_members[index]);
        size_t const side = SideOf(index);
        PvpTestBotRecord& bot = _record.Sides[side].Bots[index - (side == 0 ? 0 : _sideASize)];
        bot.ClassId = player->getClass();
        bot.RaceId = player->getRace();
        bot.Level = player->GetLevel();
        bot.Spec = AiFactory::GetPlayerSpecName(player);
        if (PlayerbotAI* botAI = GET_PLAYERBOT_AI(player))
        {
            bot.GearScore = botAI->GetEquipGearScore(player);
            if (sPlayerbotAIConfig.SkillEnable && botAI->GetPvpSkill())
                bot.Skill = botAI->GetPvpSkill();
        }

        bot.Entered = true;
        Restore(player);
        if (!IsDuel())
            player->UpdatePvP(true, true);
    }

    Player* first = ObjectAccessor::FindConnectedPlayer(_members.front());
    AreaTableEntry const* zone = sAreaTableStore.LookupEntry(first->GetZoneId());
    char const* const zoneName = zone ? zone->area_name[sWorld->GetDefaultDbcLocale()] : nullptr;
    _result.Map = zoneName ? zoneName : "";
    _record.Map = _result.Map;
    PvpTestFightStats::Instance().Begin(_members, _sideASize);
    _statsBegun = true;
    _fightStartMs = _totalMs;
    LOG_INFO("playerbots", "PVPTEST {} {} vs {}: at the fight spot in {} (map {} {:.1f} {:.1f} {:.1f}), match {}",
             _params.Kind, _sideA.Label, _sideB.Label, _result.Map, sPlayerbotAIConfig.PvpTestFightMap,
             sPlayerbotAIConfig.PvpTestFightX, sPlayerbotAIConfig.PvpTestFightY, sPlayerbotAIConfig.PvpTestFightZ,
             _params.MatchId);

    if (IsDuel())
    {
        // The challenge of Spell::EffectDuel; the challenged bot accepts through its duel strategy
        Player* challenger = ObjectAccessor::FindConnectedPlayer(_members[0]);
        Player* challenged = ObjectAccessor::FindConnectedPlayer(_members[_sideASize]);
        challenger->CastSpell(challenged, SpellDuel, true);
        _challengeMs = _stageMs;
        _phase = Phase::Challenge;
    }
    else
    {
        _phase = Phase::Active;
        UpdateBrawl();
    }
}

void PvpTestFightJob::UpdateChallenge()
{
    Player* challenger = ObjectAccessor::FindConnectedPlayer(_members[0]);
    Player* challenged = ObjectAccessor::FindConnectedPlayer(_members[_sideASize]);
    if (!challenger || !challenged)
    {
        Fail("a duelist went offline");
        return;
    }

    bool const timedOut = _stageMs - _challengeMs >= ChallengeTimeoutMs;
    if (challenger->duel && challenged->duel)
    {
        _duelSeen = true;
        DuelState const state = challenged->duel->State;
        if (state == DUEL_STATE_CHALLENGED)
        {
            if (timedOut)
            {
                Fail(Acore::StringFormat("the duel was not accepted within {} s", ChallengeTimeoutMs / MsPerSecond));
                return;
            }

            if (!_acceptSent && _stageMs - _challengeMs >= AcceptFallbackMs)
            {
                // What AcceptDuelAction sends, for a bot whose AI has not accepted in time
                _acceptSent = true;
                WorldPacket packet(CMSG_DUEL_ACCEPTED, 8);
                packet << challenged->GetGuidValue(PLAYER_DUEL_ARBITER);
                challenged->GetSession()->HandleDuelAcceptedOpcode(packet);
                LOG_INFO("playerbots", "PVPTEST duel {} vs {}: {} had not accepted after {} s, accept sent",
                         _sideA.Label, _sideB.Label, challenged->GetName(), AcceptFallbackMs / MsPerSecond);
            }

            return;
        }

        if (state == DUEL_STATE_COUNTDOWN)
            return;  // accepted; the fight starts when the 3 s countdown of the core ends

        _phase = Phase::Active;
        _duelStarted = true;
        _fightStartMs = _totalMs;
        LOG_INFO("playerbots", "PVPTEST duel {} vs {}: the duel starts", _sideA.Label, _sideB.Label);
        return;
    }

    if (_duelSeen || PvpTestFightStats::Instance().GetDuelEnd().Seen)
    {
        // The duel is gone again (declined, cancelled or already over): UpdateDuel reads how it ended
        _phase = Phase::Active;
        _fightStartMs = _totalMs;
        return;
    }

    if (timedOut)
        Fail(Acore::StringFormat("no duel was created within {} s", ChallengeTimeoutMs / MsPerSecond));
}

void PvpTestFightJob::SetWinner(size_t winnerSide, std::string const& reason)
{
    _result.Outcome = "winner";
    _result.Winner = winnerSide == 0 ? _sideA.Label : _sideB.Label;
    _result.WinnerSide = winnerSide == 0 ? "A" : "B";
    _result.Reason = reason;
}

void PvpTestFightJob::EndFight()
{
    _result.FightMs = _totalMs - _fightStartMs;
    EnterStage(Stage::TearDown);
}

void PvpTestFightJob::UpdateDuel()
{
    PvpTestFightStats::DuelEnd const end = PvpTestFightStats::Instance().GetDuelEnd();
    if (end.Seen)
    {
        if (end.Type == DUEL_INTERRUPTED && !_duelStarted)
        {
            _result.Outcome = "ended";
            _result.Reason = "the duel was declined or cancelled before it started";
        }
        else if (end.Type == DUEL_INTERRUPTED)
        {
            _result.Outcome = "draw";
            _result.Reason = "the duel was interrupted";
        }
        else
            SetWinner(SideOf(end.Winner), end.Type == DUEL_FLED ? "the loser left the duel area" : "");

        EndFight();
        return;
    }

    Player* challenger = ObjectAccessor::FindConnectedPlayer(_members[0]);
    Player* challenged = ObjectAccessor::FindConnectedPlayer(_members[_sideASize]);
    if (!challenger || !challenged)
    {
        Fail("a duelist went offline");
        return;
    }

    if (!challenger->duel && !challenged->duel)
    {
        _result.Outcome = "ended";
        _result.Reason = "the duel ended without a result";
        EndFight();
        return;
    }

    if (_totalMs - _fightStartMs >= sPlayerbotAIConfig.PvpTestTimeoutSeconds * MsPerSecond)
    {
        _result.Outcome = "timeout";
        _result.Reason =
            Acore::StringFormat("no winner after {} s, ended as a draw", sPlayerbotAIConfig.PvpTestTimeoutSeconds);
        challenger->DuelComplete(DUEL_INTERRUPTED);
        EndFight();
    }
}

// Ends when one side has nobody left alive; the survivors are pointed at living opponents
void PvpTestFightJob::UpdateBrawl()
{
    std::array<std::vector<std::pair<size_t, Player*>>, 2> living;
    for (size_t index = 0; index < _members.size(); ++index)
    {
        Player* player = ObjectAccessor::FindConnectedPlayer(_members[index]);
        if (player && player->IsInWorld() && player->IsAlive())
            living[SideOf(index)].emplace_back(index, player);
    }

    if (living[0].empty() || living[1].empty())
    {
        if (living[0].empty() && living[1].empty())
        {
            _result.Outcome = "draw";
            _result.Reason = "nobody was left alive";
        }
        else
            SetWinner(living[0].empty() ? 1 : 0, "");

        EndFight();
        return;
    }

    if (_totalMs - _fightStartMs >= sPlayerbotAIConfig.PvpTestTimeoutSeconds * MsPerSecond)
    {
        _result.Outcome = "timeout";
        _result.Reason =
            Acore::StringFormat("no winner after {} s, ended as a draw", sPlayerbotAIConfig.PvpTestTimeoutSeconds);
        EndFight();
        return;
    }

    for (size_t side = 0; side < living.size(); ++side)
    {
        std::vector<std::pair<size_t, Player*>> const& enemies = living[1 - side];
        for (std::pair<size_t, Player*> const& brawler : living[side])
        {
            Unit* victim = brawler.second->GetVictim();
            bool const aimed =
                victim && std::any_of(enemies.begin(), enemies.end(), [victim](std::pair<size_t, Player*> const& enemy)
                                      { return enemy.second == victim; });
            if (!aimed)
                Aim(brawler.second, enemies[brawler.first % enemies.size()].second);
        }
    }
}

// The counters of the fight into the record: damage and healing done to or for the other bots, killing blows and
// deaths from the PvP kills, one entry per bot
void PvpTestFightJob::CaptureStats()
{
    for (size_t index = 0; index < _members.size(); ++index)
    {
        size_t const side = SideOf(index);
        PvpTestBotRecord& bot = _record.Sides[side].Bots[index - (side == 0 ? 0 : _sideASize)];
        PvpTestFightStats::Totals const totals = PvpTestFightStats::Instance().Get(index);
        bot.Damage = totals.Damage;
        bot.Healing = totals.Healing;
        bot.KillingBlows = totals.KillingBlows;
        bot.Deaths = totals.Deaths;
    }
}

// Every bot that was saved is revived if need be, restored, taken out of combat and sent back to where it was
void PvpTestFightJob::UpdateTearDown()
{
    bool pending = false;
    for (size_t index = 0; index < _members.size(); ++index)
    {
        Origin& origin = _origins[index];
        Player* player = ObjectAccessor::FindConnectedPlayer(_members[index]);
        if (!player || !origin.Saved || origin.Back)
            continue;

        if (!player->IsInWorld() || player->IsBeingTeleported())
        {
            pending = true;
            continue;
        }

        if (!origin.SentBack)
        {
            if (player->duel)
                player->DuelComplete(DUEL_INTERRUPTED);

            if (!player->IsAlive())
            {
                // The next tick sends it back
                player->ResurrectPlayer(1.0f);
                player->SpawnCorpseBones();
                pending = true;
                continue;
            }

            player->CombatStop(true);
            Restore(player);
            player->UpdatePvP(origin.Pvp, true);
            if (PlayerbotAI* botAI = GET_PLAYERBOT_AI(player))
            {
                botAI->Reset();
                botAI->ResetStrategies(!sRandomPlayerbotMgr.IsRandomBot(player));
            }

            player->TeleportTo(origin.MapId, origin.X, origin.Y, origin.Z, origin.O);
            origin.SentBack = true;
            pending = true;
            continue;
        }

        if (player->GetMapId() == origin.MapId && player->GetExactDist(origin.X, origin.Y, origin.Z) < BackRadius)
            origin.Back = true;
        else
            pending = true;
    }

    if (pending && _stageMs < TearDownTimeoutMs)
        return;

    FinishTearDown(pending);
}

void PvpTestFightJob::FinishTearDown(bool timedOut)
{
    for (size_t index = 0; index < _members.size(); ++index)
    {
        Origin const& origin = _origins[index];
        Player* player = ObjectAccessor::FindConnectedPlayer(_members[index]);
        if (!origin.Saved)
            continue;

        if (!player)
        {
            LOG_INFO("playerbots", "PVPTEST {} {} vs {}: {} is offline, not sent back", _params.Kind, _sideA.Label,
                     _sideB.Label, _names[index]);
            continue;
        }

        bool const sameMap = player->GetMapId() == origin.MapId;
        LOG_INFO("playerbots", "PVPTEST {} {} vs {}: {} back on map {} ({}), {:.1f} yards from where it was",
                 _params.Kind, _sideA.Label, _sideB.Label, _names[index], player->GetMapId(),
                 origin.Back ? "back" : "NOT back",
                 sameMap ? player->GetExactDist(origin.X, origin.Y, origin.Z) : -1.0f);
    }

    if (timedOut)
        _result.Reason += Acore::StringFormat("{}tear-down timed out after {} s", _result.Reason.empty() ? "" : "; ",
                                              TearDownTimeoutMs / MsPerSecond);

    if (_result.Outcome.empty())
        _result.Outcome = "ended";

    _result.TotalMs = _totalMs;
    EnterStage(Stage::Done);
}

PvpTestMatchRecord PvpTestFightJob::BuildRecord() const
{
    PvpTestMatchRecord record = _record;
    record.Outcome = _result.Outcome;
    record.Winner = _result.Winner;
    record.WinnerSide = _result.WinnerSide;
    record.Reason = _result.Reason;
    record.FailedStage = _result.FailedStage;
    record.FightMs = _result.FightMs;
    record.TotalMs = _result.TotalMs ? _result.TotalMs : _totalMs;
    if (record.Outcome.empty())
    {
        // Only reached for a job dropped by the server shutdown
        bool const unfinished = _stage != Stage::Done;
        record.Outcome = unfinished ? "stopped" : "ended";
        if (unfinished)
        {
            record.FailedStage = StageName(_stage);
            record.Reason = "server shutdown";
        }
    }

    return record;
}

std::string PvpTestFightJob::Describe() const
{
    char const* phase = "";
    if (_stage == Stage::Fight)
        phase = _phase == Phase::Arrive ? " (arriving)" : _phase == Phase::Challenge ? " (challenge)" : " (fighting)";

    return Acore::StringFormat("PVPTEST {} {} vs {}: stage {}{} for {} s, total {}, map {}, match {}", _params.Kind,
                               _sideA.Label, _sideB.Label, StageName(_stage), phase, _stageMs / MsPerSecond,
                               FormatDuration(_totalMs), _result.Map.empty() ? "(not there yet)" : _result.Map,
                               _params.MatchId);
}

std::string PvpTestFightJob::ResultLine() const
{
    std::string const head = Acore::StringFormat("PVPTEST {} {} vs {}: ", _params.Kind, _sideA.Label, _sideB.Label);
    std::string const tail =
        Acore::StringFormat(", map {}, match {}", _result.Map.empty() ? "-" : _result.Map, _params.MatchId);
    std::string const reason = _result.Reason.empty() ? "" : " (" + _result.Reason + ")";
    if (_result.Outcome == "failed" || _result.Outcome == "stopped")
        return head + _result.Outcome + " in stage " + _result.FailedStage + reason + tail;

    std::string const durations = Acore::StringFormat(" after {} of fighting (total {})",
                                                      FormatDuration(_result.FightMs), FormatDuration(_result.TotalMs));
    if (_result.Outcome == "winner")
        return head + "Winner: " + _result.Winner + durations + reason + tail;

    return head + _result.Outcome + durations + reason + tail;
}

// ---------------------------------------------------------------------------------------------------------------------
// Manager side: reports and commands

void PvpTestMgr::ReportFight(PvpTestFightJob const& job)
{
    _lastResult = job.ResultLine();
    _lastMembers = job.AllMembers();
    PvpTestRecordWriter::Instance().Write(job.BuildRecord());
    LOG_INFO("playerbots", "{}", _lastResult);
    if (Player* issuer = ObjectAccessor::FindConnectedPlayer(job.GetIssuer()))
        ChatHandler(issuer->GetSession()).SendSysMessage(_lastResult);
}

bool PvpTestMgr::HandleFightCommand(ChatHandler* handler, PvpTestFightParams& params)
{
    PvpTestMgr& manager = Instance();
    if (manager._plan)
        return CommandFail(handler, "A pvptest plan runs (.playerbots pvptest plan status | stop)");

    if (!manager._fightJobs.empty())
        return CommandFail(handler, "A pvptest fight runs already (.playerbots pvptest status | stop)");

    std::string error;
    PvpTestSide sideA;
    PvpTestSide sideB;
    if (!PvpTestFightJob::Resolve(params, sideA, sideB, error))
        return CommandFail(handler, error);

    std::set<ObjectGuid> const inUse = manager.MembersInUse();
    for (PvpTestSide const* side : {&sideA, &sideB})
    {
        for (ObjectGuid const& guid : side->Members)
        {
            if (inUse.count(guid))
                return CommandFail(handler, "A bot of this fight is in another pvptest job already");
        }
    }

    params.MatchId = Acore::StringFormat("f-{}-{}-{}", params.Kind, std::time(nullptr), ++manager._singleCounter);
    WorldSession* session = handler->GetSession();
    ObjectGuid const issuer = session && session->GetPlayer() ? session->GetPlayer()->GetGUID() : ObjectGuid::Empty;
    std::unique_ptr<PvpTestFightJob> job =
        std::make_unique<PvpTestFightJob>(params, std::move(sideA), std::move(sideB), issuer);
    handler->PSendSysMessage("{} (the result follows in the log as PVPTEST and in .playerbots pvptest status)",
                             job->Describe());
    manager._fightJobs.push_back(std::move(job));
    return true;
}

bool PvpTestMgr::HandleDuelCommand(ChatHandler* handler, char const* args)
{
    if (!CheckEnabled(handler))
        return false;

    std::vector<std::string> const tokens = Tokenize(args ? args : "");
    if (tokens.size() != 2)
        return CommandFail(handler, DuelUsage);

    PvpTestFightParams params;
    params.Kind = "duel";
    params.SideA = tokens[0];
    params.SideB = tokens[1];
    return HandleFightCommand(handler, params);
}

bool PvpTestMgr::HandleBrawlCommand(ChatHandler* handler, char const* args)
{
    if (!CheckEnabled(handler))
        return false;

    PvpTestFightParams params;
    params.Kind = "brawl";
    bool versus = false;
    for (std::string const& token : Tokenize(args ? args : ""))
    {
        if (ToLower(token) == "vs" && !versus)
        {
            versus = true;
            continue;
        }

        std::string& side = versus ? params.SideB : params.SideA;
        side += side.empty() ? token : "," + token;
    }

    if (!versus || params.SideA.empty() || params.SideB.empty())
        return CommandFail(handler, BrawlUsage);

    return HandleFightCommand(handler, params);
}
