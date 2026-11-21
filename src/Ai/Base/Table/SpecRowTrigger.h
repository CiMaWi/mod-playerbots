/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_SPECROWTRIGGER_H
#define PLAYERBOTS_SPECROWTRIGGER_H

#include <array>
#include <memory>
#include <string>

#include "NamedObjectContext.h"
#include "SpecTable.h"
#include "Trigger.h"

class SpecTableStrategy;
class Unit;
template <class T>
class Value;

// A kind's rule in batch B (INTERFACE §3): a registered trigger taken from the context by name, or an instance the
// row owns, built with the row's spell or effect; neither for a kind without a rule. The one owner of a kind's "when"
namespace KindRule
{
struct Rule
{
    std::string registered;          // a registered trigger name (also a row's Instead)
    std::unique_ptr<Trigger> owned;  // built with the row's spell or effect
    bool missing = false;            // the kind needs a spell or effect and the row has none: never live
};

// spell: the row's effect, else the spell of its action ("" when the action casts none)
Rule Make(PlayerbotAI* botAI, spec::Row const& row, std::string const& spell);
}  // namespace KindRule

// "spec row::<strategy>#<index>": one table row of the bot's combat engine (INTERFACE §6). Live in a tick when the row
// stands, its kind's rule (or Instead) holds and every `when` term holds, in that order
class SpecRowTrigger : public Trigger, public Qualified
{
public:
    SpecRowTrigger(PlayerbotAI* botAI) : Trigger(botAI, "spec row") {}

    using Qualified::Qualify;
    void Qualify(std::string const qual) override;
    std::string const getName() override { return displayName; }
    bool IsActive() override;
    // A Form or Renewal row answers as its rule's class, so the force-rebuff cycle sees it
    bool IsBuffTrigger() override { return rule.owned && rule.owned->IsBuffTrigger(); }
    bool IsDebuffTrigger() override { return rule.owned && rule.owned->IsDebuffTrigger(); }

private:
    void Resolve();
    bool RuleHolds(uint32 now);
    bool TermHolds(spec::Term const& term, uint32 now);

    std::string strategyName;
    size_t index = 0;
    bool parsed = false;
    std::string displayName = "spec row";

    uint32 resolvedBuild = 0;
    bool resolvedOnce = false;
    SpecTableStrategy* strategy = nullptr;
    spec::Row const* row = nullptr;

    KindRule::Rule rule;
    Trigger* shared = nullptr;
    spec::SharedAnswer* sharedAnswer = nullptr;
    bool ownedAnswer = false;

    std::array<std::string, spec::MAX_WHEN> termNames;
    std::array<Trigger*, spec::MAX_WHEN> termTriggers{};
    std::array<spec::SharedAnswer*, spec::MAX_WHEN> termAnswers{};
    Value<Unit*>* currentTarget = nullptr;
    Value<uint8>* combo = nullptr;
    Value<uint8>* attackers = nullptr;
};

#endif
