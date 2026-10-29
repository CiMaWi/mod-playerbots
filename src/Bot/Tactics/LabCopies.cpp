/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "LabCopies.h"

#include <algorithm>
#include <optional>
#include <sstream>

#include "BotAttributes.h"
#include "BotProfileMgr.h"
#include "CharacterCache.h"
#include "Chat.h"
#include "LadderMgr.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "PvpGearMgr.h"
#include "RandomPlayerbotMgr.h"
#include "StringConvert.h"
#include "StringFormat.h"
#include "Talentspec.h"
#include "ThreadCheck.h"

namespace
{
// A connected bot by name or guid; the pointer is not kept
Player* FindOnlineBot(ChatHandler* handler, std::string const& target, std::string& name)
{
    ObjectGuid guid = ObjectGuid::Empty;
    name = target;
    if (std::optional<uint32> const number = Acore::StringTo<uint32>(target))
        guid = ObjectGuid::Create<HighGuid::Player>(*number);
    else if (normalizePlayerName(name))
        guid = sCharacterCache->GetCharacterGuidByName(name);

    if (guid.IsEmpty() || !sCharacterCache->GetCharacterNameByGuid(guid, name))
    {
        handler->PSendSysMessage("ATTRIBUTES no character {}.", target);
        return nullptr;
    }

    Player* player = ObjectAccessor::FindConnectedPlayer(guid);
    if (!player || !GET_PLAYERBOT_AI(player))
    {
        handler->PSendSysMessage("ATTRIBUTES {} is not an online bot.", name);
        return nullptr;
    }

    return player;
}

std::vector<std::string> SplitList(std::string const& list)
{
    std::vector<std::string> names;
    std::stringstream stream(list);
    std::string name;
    while (std::getline(stream, name, ','))
    {
        if (!name.empty())
            names.push_back(name);
    }

    return names;
}

// The active spec's talent link. TalentSpec(Player*) reads the ranks by HasSpell, which sees only the talents that go
// into the spell book (the passive ones are only in the talent map), so the ranks come from HasTalent here
std::string TalentLink(Player* bot)
{
    TalentSpec spec(bot->getClassMask());
    for (TalentSpec::TalentListEntry& entry : spec.talents)
    {
        for (uint8 rank = MAX_TALENT_RANK; rank > 0; --rank)
        {
            uint32 const spellId = entry.talentInfo->RankID[rank - 1];
            if (spellId && bot->HasTalent(spellId, bot->GetActiveSpec()))
            {
                entry.rank = rank;
                break;
            }
        }
    }

    return spec.GetTalentLink();
}

// The active spec's talents from a link of the same class (no gold, no glyphs). ApplyTalents learns them in tree,
// row, column order, so every legal set passes LearnTalent's points-per-row rule
void LearnTalentLink(Player* bot, std::string const& link)
{
    TalentSpec spec(bot, link);
    bot->resetTalents(true);
    spec.ApplyTalents(bot, nullptr);
    bot->SendTalentsInfoData(false);
}

// The copy's own talents again; restored: they were learned. "restored", "unchanged" or the failure
char const* RestoreTalents(Player* copy, std::string const& ownTalents, bool talentsChanged, bool& restored)
{
    restored = talentsChanged && TalentLink(copy) != ownTalents;
    if (restored)
        LearnTalentLink(copy, ownTalents);

    return TalentLink(copy) != ownTalents ? "NOT the own ones after the restore" : restored ? "restored" : "unchanged";
}

// The loadout the bot wears in a match (a copy's: the template's), as a count and a hash of the item entries
std::string LoadoutSummary(uint32 guid)
{
    PvpGearMgr::Loadout const* loadout = PvpGearMgr::Instance().Find(guid);
    if (!loadout)
        return "none";

    uint32 items = 0;
    uint64 hash = 1469598103934665603ULL;
    for (PvpGearMgr::LoadoutSlot const& slot : *loadout)
    {
        if (slot.Item)
            ++items;

        hash = (hash ^ slot.Item) * 1099511628211ULL;
    }

    return Acore::StringFormat("{} items, entries hash {:016x}", items, hash);
}
}  // namespace

bool LabCopyMgr::Pin(Player* templatePlayer, Player* copy, std::string& error, std::string& detail)
{
    PLAYERBOT_CHECK_WORLD_THREAD("LabCopyMgr::Pin");
    error.clear();
    PlayerbotAI* templateAI = GET_PLAYERBOT_AI(templatePlayer);
    PlayerbotAI* copyAI = GET_PLAYERBOT_AI(copy);
    uint32 const templateGuid = templatePlayer->GetGUID().GetCounter();
    uint32 const copyGuid = copy->GetGUID().GetCounter();
    auto const pinned = _pins.find(copyGuid);
    if (!IsLabServer())
        error = "this server is not a lab (Playerbots.Ladder.ServerId lab*): copies only on a lab";
    else if (!templateAI || !copyAI)
        error = "not an online bot";
    else if (copyGuid == templateGuid)
        error = "a bot cannot be a copy of itself";
    else if (TemplateOf(templateGuid))
        error = "the template is a copy itself";
    else if (pinned != _pins.end() && pinned->second.Template != templateGuid)
        error = Acore::StringFormat("a copy of guid {} already (unpin it first)", pinned->second.Template);
    else if (IsTemplate(copyGuid))
        error = "the template of other copies";
    else if (copy->getClass() != templatePlayer->getClass())
        error = Acore::StringFormat("class {} is not the template's class {}", copy->getClass(),
                                    templatePlayer->getClass());
    else if (copy->getRace() != templatePlayer->getRace())
        error =
            Acore::StringFormat("race {} is not the template's race {}", copy->getRace(), templatePlayer->getRace());
    else if (copy->GetLevel() != templatePlayer->GetLevel())
        error = Acore::StringFormat("level {} is not the template's level {}", copy->GetLevel(),
                                    templatePlayer->GetLevel());
    else if (copyAI->HasGameClientMaster())
        error = "with a real player";
    else if (copy->InBattleground() || copy->InBattlegroundQueue())
        error = "in a battleground, an arena or a queue (talents change only outside)";
    else if (LadderMgr::Instance().IsAway(copyGuid))
        error = "not hosted on this server (its ladder team is away)";
    else if (LadderMgr::Instance().IsAway(templateGuid))
        error = "the template is not hosted on this server (its ladder team is away)";

    if (!error.empty())
        return false;

    PinRecord& record = _pins[copyGuid];
    if (!record.Template)
    {
        record.Template = templateGuid;
        record.OwnTalents = TalentLink(copy);
    }

    std::string const templateTalents = TalentLink(templatePlayer);
    bool const talentsCopied = TalentLink(copy) != templateTalents;
    if (talentsCopied)
    {
        LearnTalentLink(copy, templateTalents);
        record.TalentsChanged = true;
    }

    bool const loadout = PvpGearMgr::Instance().PinLoadout(copyGuid, templateGuid);
    // The template's guid draws the attributes and the gate in SetPvpSkill, with the template's skill
    copyAI->SetAttributeTemplate(templateGuid);
    copyAI->SetPvpSkill(templateAI->GetPvpSkill());
    if (talentsCopied)
        copyAI->ResetStrategies();

    bool const talentsEqual = TalentLink(copy) == templateTalents;
    detail = Acore::StringFormat("skill {}, talents {}, loadout {}", templateAI->GetPvpSkill(),
                                 !talentsCopied ? "already the template's"
                                 : talentsEqual ? "copied"
                                                : "NOT equal after the copy",
                                 loadout ? "pinned" : "own (the template has no stored loadout)");
    LOG_INFO("playerbots", "ATTRIBUTES pin {} (guid {}) to {} (guid {}): {}", copy->GetName(), copyGuid,
             templatePlayer->GetName(), templateGuid, detail);
    return true;
}

bool LabCopyMgr::Unpin(uint32 copyGuid, std::string& error, std::string& detail)
{
    PLAYERBOT_CHECK_WORLD_THREAD("LabCopyMgr::Unpin");
    error.clear();
    auto const pinned = _pins.find(copyGuid);
    if (pinned == _pins.end())
    {
        error = "not a copy";
        return false;
    }

    PvpGearMgr::Instance().UnpinLoadout(copyGuid);
    Player* copy = ObjectAccessor::FindConnectedPlayer(ObjectGuid::Create<HighGuid::Player>(copyGuid));
    PlayerbotAI* copyAI = copy ? GET_PLAYERBOT_AI(copy) : nullptr;
    if (!copyAI)
    {
        // A new login draws its own attributes anyway; only the talents wait for the bot
        error = "offline: loadout pin dropped, the talents are learned back at an unpin once it is online";
        return false;
    }

    copyAI->SetAttributeTemplate(0);
    std::optional<BotProfile> const profile = BotProfileMgr::instance().Find(copyGuid);
    copyAI->SetPvpSkill(profile ? profile->Skill : 0);
    bool restored = false;
    char const* talents = RestoreTalents(copy, pinned->second.OwnTalents, pinned->second.TalentsChanged, restored);
    if (restored)
        copyAI->ResetStrategies();

    detail = Acore::StringFormat("skill {}, talents {}", copyAI->GetPvpSkill(), talents);
    LOG_INFO("playerbots", "ATTRIBUTES unpin {} (guid {}): {}", copy->GetName(), copyGuid, detail);
    _pins.erase(pinned);
    return true;
}

void LabCopyMgr::OnLogout(Player* bot)
{
    if (_pins.empty())
        return;

    auto const pinned = _pins.find(bot->GetGUID().GetCounter());
    if (pinned == _pins.end())
        return;

    // Before the logout's save, so the character rows get the own talents and the next login is the bot itself
    PLAYERBOT_CHECK_WORLD_THREAD("LabCopyMgr::OnLogout");
    PvpGearMgr::Instance().UnpinLoadout(pinned->first);
    if (PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot))
        botAI->SetAttributeTemplate(0);

    bool restored = false;
    char const* talents = RestoreTalents(bot, pinned->second.OwnTalents, pinned->second.TalentsChanged, restored);
    LOG_INFO("playerbots", "ATTRIBUTES unpin {} (guid {}) at its logout: talents {}, loadout pin dropped",
             bot->GetName(), pinned->first, talents);
    _pins.erase(pinned);
}

void LabCopyMgr::UnpinAll()
{
    PLAYERBOT_CHECK_WORLD_THREAD("LabCopyMgr::UnpinAll");
    for (uint32 guid : Copies())
    {
        std::string error;
        std::string detail;
        if (!Unpin(guid, error, detail))
            LOG_WARN("playerbots", "ATTRIBUTES unpin guid {} at the shutdown: {}", guid, error);
    }
}

bool LabCopyMgr::IsLabServer() { return sPlayerbotAIConfig.LadderServerId.rfind("lab", 0) == 0; }

uint32 LabCopyMgr::TemplateOf(uint32 copyGuid) const
{
    auto const pinned = _pins.find(copyGuid);
    return pinned == _pins.end() ? 0 : pinned->second.Template;
}

bool LabCopyMgr::IsTemplate(uint32 guid) const
{
    return std::any_of(_pins.begin(), _pins.end(), [guid](auto const& pin) { return pin.second.Template == guid; });
}

std::vector<uint32> LabCopyMgr::Copies() const
{
    std::vector<uint32> copies;
    for (auto const& pin : _pins)
        copies.push_back(pin.first);

    std::sort(copies.begin(), copies.end());
    return copies;
}

bool LabCopyMgr::HandlePinCommand(ChatHandler* handler, char const* args)
{
    std::istringstream stream(args ? args : "");
    std::string templateName;
    std::string list;
    stream >> templateName >> list;
    if (templateName.empty() || list.empty())
    {
        handler->PSendSysMessage("Usage: .playerbots attributes pin <template> <bot>[,<bot>...]");
        return false;
    }

    std::string name;
    Player* templatePlayer = FindOnlineBot(handler, templateName, name);
    if (!templatePlayer)
        return false;

    uint32 pinnedCount = 0;
    for (std::string const& target : SplitList(list))
    {
        std::string copyName;
        Player* copy = FindOnlineBot(handler, target, copyName);
        if (!copy)
            continue;

        std::string error;
        std::string detail;
        if (Instance().Pin(templatePlayer, copy, error, detail))
        {
            ++pinnedCount;
            handler->PSendSysMessage("ATTRIBUTES pinned {} to {}: {}", copyName, name, detail);
        }
        else
            handler->PSendSysMessage("ATTRIBUTES refused {}: {}", copyName, error);
    }

    handler->PSendSysMessage("ATTRIBUTES pin: {} copies of {} (guid {})", pinnedCount, name,
                             templatePlayer->GetGUID().GetCounter());
    return pinnedCount > 0;
}

bool LabCopyMgr::HandleUnpinCommand(ChatHandler* handler, char const* args)
{
    std::string const list = args ? args : "";
    std::vector<uint32> guids;
    if (list == "all")
        guids = Instance().Copies();
    else
    {
        for (std::string const& target : SplitList(list))
        {
            std::string name = target;
            ObjectGuid guid = ObjectGuid::Empty;
            if (std::optional<uint32> const number = Acore::StringTo<uint32>(target))
                guid = ObjectGuid::Create<HighGuid::Player>(*number);
            else if (normalizePlayerName(name))
                guid = sCharacterCache->GetCharacterGuidByName(name);

            if (guid.IsEmpty())
                handler->PSendSysMessage("ATTRIBUTES no character {}.", target);
            else
                guids.push_back(guid.GetCounter());
        }
    }

    if (guids.empty())
    {
        handler->PSendSysMessage("Usage: .playerbots attributes unpin <bot>[,<bot>...]|all (no copy named)");
        return false;
    }

    uint32 unpinned = 0;
    for (uint32 guid : guids)
    {
        std::string error;
        std::string detail;
        if (Instance().Unpin(guid, error, detail))
        {
            ++unpinned;
            handler->PSendSysMessage("ATTRIBUTES unpinned guid {}: {}", guid, detail);
        }
        else
            handler->PSendSysMessage("ATTRIBUTES unpin guid {}: {}", guid, error);
    }

    handler->PSendSysMessage("ATTRIBUTES unpin: {} of {}", unpinned, guids.size());
    return true;
}

bool LabCopyMgr::HandleShowCommand(ChatHandler* handler, char const* args)
{
    std::string const target = args ? args : "";
    if (target.empty())
    {
        std::string text;
        for (uint32 guid : Instance().Copies())
            text += Acore::StringFormat("{}{} -> {}", text.empty() ? "" : ", ", guid, Instance().TemplateOf(guid));

        handler->PSendSysMessage("ATTRIBUTES copies (copy -> template guid): {}", text.empty() ? "none" : text);
        return true;
    }

    std::string name;
    Player* bot = FindOnlineBot(handler, target, name);
    if (!bot)
        return false;

    PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
    uint32 const guid = bot->GetGUID().GetCounter();
    uint32 const templateGuid = botAI->GetAttributeTemplate();
    std::string templateName;
    if (templateGuid)
        sCharacterCache->GetCharacterNameByGuid(ObjectGuid::Create<HighGuid::Player>(templateGuid), templateName);

    handler->PSendSysMessage(
        "ATTRIBUTES {} (guid {}, class {}, race {}, level {}): skill {}, {}", name, guid, bot->getClass(),
        bot->getRace(), bot->GetLevel(), botAI->GetPvpSkill(),
        templateGuid ? Acore::StringFormat("copy of {} (guid {})", templateName, templateGuid) : std::string("own"));
    handler->PSendSysMessage("  attributes: {}", FormatBotAttributes(botAI->GetAttributes()));
    handler->PSendSysMessage("  talents {}", TalentLink(bot));
    handler->PSendSysMessage("  loadout {}", LoadoutSummary(guid));
    return true;
}

bool LabCopyMgr::HandleReapplyCommand(ChatHandler* handler, char const* args)
{
    PLAYERBOT_CHECK_WORLD_THREAD("LabCopyMgr::HandleReapplyCommand");
    std::string const target = args ? args : "";
    uint32 count = 0;
    if (!target.empty() && target != "all")
    {
        std::string name;
        Player* bot = FindOnlineBot(handler, target, name);
        if (!bot)
            return false;

        PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
        botAI->SetPvpSkill(botAI->GetPvpSkill());
        count = 1;
    }
    else
    {
        // The skill each bot has now (its own, a forced or a template's) drawn again with the current options
        for (auto const& entry : ObjectAccessor::GetPlayers())
        {
            PlayerbotAI* botAI = entry.second ? GET_PLAYERBOT_AI(entry.second) : nullptr;
            if (!botAI)
                continue;

            botAI->SetPvpSkill(botAI->GetPvpSkill());
            ++count;
        }
    }

    handler->PSendSysMessage("ATTRIBUTES reapply: {} bots drew their attributes again ({})", count,
                             AttributesAtDefaults(sPlayerbotAIConfig.SkillAttributes) ? "defaults" : "drawn");
    return true;
}

bool LabCopyMgr::HandleLogoutCommand(ChatHandler* handler, char const* args)
{
    PLAYERBOT_CHECK_WORLD_THREAD("LabCopyMgr::HandleLogoutCommand");
    if (!IsLabServer())
    {
        handler->PSendSysMessage("ATTRIBUTES logout: only on a lab (Playerbots.Ladder.ServerId lab*)");
        return false;
    }

    std::string name;
    Player* bot = FindOnlineBot(handler, args ? args : "", name);
    if (!bot)
        return false;

    ObjectGuid const guid = bot->GetGUID();
    if (!sRandomPlayerbotMgr.GetPlayerBot(guid))
    {
        handler->PSendSysMessage("ATTRIBUTES logout: {} is not a random bot", name);
        return false;
    }

    bool const copy = Instance().TemplateOf(guid.GetCounter()) != 0;
    sRandomPlayerbotMgr.LogoutPlayerBot(guid);
    handler->PSendSysMessage("ATTRIBUTES logout {} (guid {}){}", name, guid.GetCounter(),
                             copy ? ": a copy, unpinned before the save" : "");
    return true;
}
