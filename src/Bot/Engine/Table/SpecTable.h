/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_SPECTABLE_H
#define PLAYERBOTS_SPECTABLE_H

// A spec's combat as one ordered constexpr table of rows (the combat foundation's spec table). Pure: no core
// header, so tests/spec-table-selftest.cpp compiles it alone.

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace spec
{
// The seventeen kinds; a kind selects the band and the shared "when" (its rule, owned by the base)
enum class Kind : uint8_t
{
    Defensive,
    Dispel,
    Interrupt,
    MoveTool,
    Opener,
    Heal,
    Form,
    Renewal,
    Proc,
    Cooldown,
    Recover,
    Aoe,
    Control,
    Pet,
    Builder,
    Spender,
    Filler
};
constexpr size_t KIND_COUNT = 17;

// In the order a table lists its rows, top to bottom
enum class Band : uint8_t
{
    Survive,
    Dispel,
    Interrupt,
    Move,
    Heal,
    Rotation,
    Filler
};
constexpr size_t BAND_COUNT = 7;

enum class Role : uint8_t
{
    Melee,
    Ranged,
    Healer,
    Tank
};

enum class HealthTier : uint8_t
{
    Critical,
    Low,
    Medium,
    AlmostFull
};

// None: a Recover row of another power, which carries Instead
enum class ManaTier : uint8_t
{
    None,
    Low,
    Medium,
    High
};

enum class HealScope : uint8_t
{
    One,
    Group,
    GroupHard
};

enum class DispelOn : uint8_t
{
    Self,
    Party,
    Enemy
};

// The core's DispelType values (SharedDefines.h), checked where the core is included
enum class DispelType : uint8_t
{
    Magic = 1,
    Curse = 2,
    Disease = 3,
    Poison = 4
};

// A Renewal row's subject; Totem and Pet are reserved (Instead only)
enum class Subject : uint8_t
{
    Self,
    Party,
    Target,
    TargetMine,
    Totem,
    Pet
};

enum class Who : uint8_t
{
    Target,
    EnemyHealer
};

enum class Purpose : uint8_t
{
    Close,
    Escape
};

enum class Centre : uint8_t
{
    Self,
    Target,
    Ground,
    Totem,
    Pet
};

enum class Owner : uint8_t
{
    Any,
    Mine
};
inline constexpr Owner Any = Owner::Any;
inline constexpr Owner Mine = Owner::Mine;

// A `when` term: O(1), read every tick
enum class Atom : uint8_t
{
    None,
    HealthBelow,
    TargetHealthBelow,
    PowerAtLeast,
    PowerBelow,
    Combo,
    Has,
    Lacks,
    TargetHas,
    TargetLacks,
    Attackers,
    Solo,
    Grouped,
    Trigger
};

struct Term
{
    Atom atom = Atom::None;
    uint32_t number = 0;    // a percentage, power points, combo points, stacks or attackers
    std::string_view name;  // an aura or a registered trigger
    Owner owner = Owner::Any;
};

constexpr Term MakeTerm(Atom atom, uint32_t number, std::string_view name = {}, Owner owner = Owner::Any)
{
    Term term;
    term.atom = atom;
    term.number = number;
    term.name = name;
    term.owner = owner;
    return term;
}

constexpr Term HealthBelow(uint32_t percent) { return MakeTerm(Atom::HealthBelow, percent); }
constexpr Term TargetHealthBelow(uint32_t percent) { return MakeTerm(Atom::TargetHealthBelow, percent); }
// Rage, energy and runic power in points, mana in percent
constexpr Term PowerAtLeast(uint32_t amount) { return MakeTerm(Atom::PowerAtLeast, amount); }
constexpr Term PowerBelow(uint32_t amount) { return MakeTerm(Atom::PowerBelow, amount); }
constexpr Term Combo(uint32_t points) { return MakeTerm(Atom::Combo, points); }
constexpr Term Has(std::string_view aura, uint32_t stacks = 1) { return MakeTerm(Atom::Has, stacks, aura); }
constexpr Term Lacks(std::string_view aura) { return MakeTerm(Atom::Lacks, 0, aura); }
constexpr Term TargetHas(std::string_view aura, Owner owner) { return MakeTerm(Atom::TargetHas, 0, aura, owner); }
constexpr Term TargetLacks(std::string_view aura, Owner owner) { return MakeTerm(Atom::TargetLacks, 0, aura, owner); }
constexpr Term Attackers(uint32_t count) { return MakeTerm(Atom::Attackers, count); }
constexpr Term Solo() { return MakeTerm(Atom::Solo, 0); }
constexpr Term Grouped() { return MakeTerm(Atom::Grouped, 0); }
constexpr Term Trigger(std::string_view name) { return MakeTerm(Atom::Trigger, 0, name); }

constexpr bool TermNeedsName(Atom atom)
{
    return atom == Atom::Has || atom == Atom::Lacks || atom == Atom::TargetHas || atom == Atom::TargetLacks ||
           atom == Atom::Trigger;
}

// A standing requirement, refreshed with the row's known state, not every tick
enum class Need : uint8_t
{
    None,
    Knows,
    NotKnows,
    Wields
};

enum class Gear : uint8_t
{
    None,
    OffHandWeapon,
    Shield,
    TwoHand,
    Wand,
    Ranged
};

struct Requirement
{
    Need need = Need::None;
    std::string_view spell;
    Gear gear = Gear::None;
};

constexpr Requirement MakeRequirement(Need need, std::string_view spell, Gear gear)
{
    Requirement requirement;
    requirement.need = need;
    requirement.spell = spell;
    requirement.gear = gear;
    return requirement;
}

constexpr Requirement Knows(std::string_view spell) { return MakeRequirement(Need::Knows, spell, Gear::None); }
constexpr Requirement NotKnows(std::string_view spell) { return MakeRequirement(Need::NotKnows, spell, Gear::None); }
constexpr Requirement Wields(Gear gear) { return MakeRequirement(Need::Wields, {}, gear); }

constexpr size_t MAX_WHEN = 3;
constexpr size_t MAX_REQUIRES = 2;

struct Row
{
    Kind kind = Kind::Filler;
    std::string_view action;  // a registered action name
    std::string_view effect;  // the aura the kind's rule looks for; empty: the action's spell
    uint32_t stacks = 1;
    HealthTier health = HealthTier::Critical;
    ManaTier mana = ManaTier::None;
    HealScope scope = HealScope::One;
    DispelOn dispelOn = DispelOn::Self;
    DispelType dispelType = DispelType::Magic;
    Subject subject = Subject::Self;
    Who who = Who::Target;
    Purpose purpose = Purpose::Close;
    Centre centre = Centre::Target;
    uint32_t count = 0;
    std::array<Term, MAX_WHEN> when{};
    uint8_t whenCount = 0;
    std::array<Requirement, MAX_REQUIRES> needs{};
    uint8_t needCount = 0;
    std::string_view instead;  // a registered trigger that replaces the kind's rule
    std::string_view option;   // a strategy that must be on the combat engine
    bool overflow = false;     // more terms or requirements than the row holds

    template <typename... Terms>
    constexpr Row When(Terms const&... terms) const
    {
        Row row = *this;
        (row.AddTerm(terms), ...);
        return row;
    }

    template <typename... Requirements>
    constexpr Row Requires(Requirements const&... requirements) const
    {
        Row row = *this;
        (row.AddRequirement(requirements), ...);
        return row;
    }

    constexpr Row Instead(std::string_view trigger) const
    {
        Row row = *this;
        row.instead = trigger;
        return row;
    }

    constexpr Row Option(std::string_view strategy) const
    {
        Row row = *this;
        row.option = strategy;
        return row;
    }

    constexpr Row Effect(std::string_view aura) const
    {
        Row row = *this;
        row.effect = aura;
        return row;
    }

    constexpr void AddTerm(Term const& term)
    {
        if (whenCount < MAX_WHEN)
            when[whenCount++] = term;
        else
            overflow = true;
    }

    constexpr void AddRequirement(Requirement const& requirement)
    {
        if (needCount < MAX_REQUIRES)
            needs[needCount++] = requirement;
        else
            overflow = true;
    }
};

constexpr Row MakeRow(Kind kind, std::string_view action)
{
    Row row;
    row.kind = kind;
    row.action = action;
    return row;
}

// One builder per kind, with the kind's own inputs
constexpr Row Defensive(std::string_view action, HealthTier tier)
{
    Row row = MakeRow(Kind::Defensive, action);
    row.health = tier;
    return row;
}

constexpr Row Dispel(std::string_view action, DispelOn on, DispelType type)
{
    Row row = MakeRow(Kind::Dispel, action);
    row.dispelOn = on;
    row.dispelType = type;
    return row;
}

constexpr Row Interrupt(std::string_view action, Who who = Who::Target)
{
    Row row = MakeRow(Kind::Interrupt, action);
    row.who = who;
    return row;
}

constexpr Row MoveTool(std::string_view action, Purpose purpose)
{
    Row row = MakeRow(Kind::MoveTool, action);
    row.purpose = purpose;
    return row;
}

constexpr Row Opener(std::string_view action) { return MakeRow(Kind::Opener, action); }

constexpr Row Heal(std::string_view action, HealthTier tier, HealScope scope = HealScope::One)
{
    Row row = MakeRow(Kind::Heal, action);
    row.health = tier;
    row.scope = scope;
    return row;
}

constexpr Row Form(std::string_view action) { return MakeRow(Kind::Form, action); }

constexpr Row Renewal(std::string_view action, Subject subject)
{
    Row row = MakeRow(Kind::Renewal, action);
    row.subject = subject;
    return row;
}

constexpr Row Proc(std::string_view action, std::string_view effect, uint32_t stacks = 1)
{
    Row row = MakeRow(Kind::Proc, action);
    row.effect = effect;
    row.stacks = stacks;
    return row;
}

constexpr Row Cooldown(std::string_view action) { return MakeRow(Kind::Cooldown, action); }

constexpr Row Recover(std::string_view action, ManaTier tier = ManaTier::None)
{
    Row row = MakeRow(Kind::Recover, action);
    row.mana = tier;
    return row;
}

constexpr Row Aoe(std::string_view action, Centre centre, uint32_t count)
{
    Row row = MakeRow(Kind::Aoe, action);
    row.centre = centre;
    row.count = count;
    return row;
}

constexpr Row Control(std::string_view action) { return MakeRow(Kind::Control, action); }
constexpr Row Pet(std::string_view action) { return MakeRow(Kind::Pet, action); }
constexpr Row Builder(std::string_view action) { return MakeRow(Kind::Builder, action); }
constexpr Row Spender(std::string_view action) { return MakeRow(Kind::Spender, action); }
constexpr Row Filler(std::string_view action) { return MakeRow(Kind::Filler, action); }

// Rows and row groups flattened into one array, in place
template <size_t N>
struct RowList
{
    std::array<Row, N> rows{};

    static constexpr size_t Size() { return N; }
    constexpr Row const& operator[](size_t index) const { return rows[index]; }
};

namespace detail
{
template <typename Part>
struct PartSize;

template <>
struct PartSize<Row>
{
    static constexpr size_t value = 1;
};

template <size_t N>
struct PartSize<RowList<N>>
{
    static constexpr size_t value = N;
};

template <size_t N>
constexpr void Append(RowList<N>& out, size_t& at, Row const& row)
{
    out.rows[at++] = row;
}

template <size_t N, size_t M>
constexpr void Append(RowList<N>& out, size_t& at, RowList<M> const& group)
{
    for (Row const& row : group.rows)
        out.rows[at++] = row;
}
}  // namespace detail

template <typename... Parts>
constexpr auto Rows(Parts const&... parts)
{
    RowList<(detail::PartSize<Parts>::value + ... + 0)> out{};
    size_t at = 0;
    (detail::Append(out, at, parts), ...);
    return out;
}

// Bands: a row's relevance is (top - 1 - ROW_STEP * k) hundredths, k = the table's rows of its band above it, and
// stays above the band's floor (the engine constant it sits on)
struct BandData
{
    uint32_t top;    // hundredths
    uint32_t floor;  // hundredths
    std::string_view word;
};

constexpr uint32_t ROW_STEP = 5;
constexpr std::array<BandData, BAND_COUNT> BANDS = {{{9500, 9000, "survive"},
                                                     {5500, 5000, "dispel"},
                                                     {4500, 4000, "interrupt"},
                                                     {4000, 3500, "move"},
                                                     {3500, 3000, "heal"},
                                                     {1000, 600, "rotation"},
                                                     {600, 500, "filler"}}};

constexpr BandData const& BandInfo(Band band) { return BANDS[static_cast<size_t>(band)]; }

// The rows a band holds: the last one sits ROW_STEP - 1 hundredths above the floor at most
constexpr uint32_t BandCapacity(Band band) { return (BandInfo(band).top - 1 - BandInfo(band).floor) / ROW_STEP + 1; }

constexpr uint32_t RowHundredths(Band band, uint32_t k) { return BandInfo(band).top - 1 - ROW_STEP * k; }
constexpr float RowRelevance(Band band, uint32_t k) { return static_cast<float>(RowHundredths(band, k)) / 100.0f; }

constexpr bool BandsNest()
{
    for (size_t band = 0; band < BAND_COUNT; ++band)
    {
        if (BANDS[band].floor >= BANDS[band].top)
            return false;
        if (band > 0 && BANDS[band].top > BANDS[band - 1].floor)
            return false;
        if (RowHundredths(static_cast<Band>(band), BandCapacity(static_cast<Band>(band)) - 1) <= BANDS[band].floor)
            return false;
    }
    return true;
}
static_assert(BandsNest(), "each band lies below the one above it and its last row above its floor");

constexpr Band BandOf(Kind kind)
{
    switch (kind)
    {
        case Kind::Defensive:
            return Band::Survive;
        case Kind::Dispel:
            return Band::Dispel;
        case Kind::Interrupt:
            return Band::Interrupt;
        case Kind::MoveTool:
        case Kind::Opener:
            return Band::Move;
        case Kind::Heal:
            return Band::Heal;
        case Kind::Filler:
            return Band::Filler;
        default:
            return Band::Rotation;
    }
}

constexpr std::array<std::string_view, KIND_COUNT> KIND_WORDS = {
    "defensive", "dispel",  "interrupt", "move tool", "opener", "heal",    "form",    "renewal", "proc",
    "cooldown",  "recover", "aoe",       "control",   "pet",    "builder", "spender", "filler"};

constexpr std::string_view KindWord(Kind kind) { return KIND_WORDS[static_cast<size_t>(kind)]; }

// A reserved kind or subject has no rule yet: its row carries Instead
constexpr bool IsReserved(Row const& row)
{
    return row.kind == Kind::Opener || row.kind == Kind::Control || row.kind == Kind::Pet ||
           (row.kind == Kind::Renewal && (row.subject == Subject::Totem || row.subject == Subject::Pet));
}

constexpr uint32_t AOE_MIN = 2;
constexpr uint32_t AOE_MAX = 4;

// The compile-time checks; Table refuses a table with a fault
enum class Fault : uint8_t
{
    None,
    Empty,
    NoAction,
    EmptyName,
    TooManyTerms,
    ReservedWithoutInstead,
    ProcWithoutEffect,
    AoeCount,
    MissingInput,
    BandOrder,
    OverCapacity,
    NoFinalFiller,
    FinalFillerConditional
};

constexpr Fault CheckRow(Row const& row)
{
    if (row.action.empty())
        return Fault::NoAction;
    if (row.overflow)
        return Fault::TooManyTerms;
    for (uint8_t i = 0; i < row.whenCount; ++i)
        if (row.when[i].atom == Atom::None || (TermNeedsName(row.when[i].atom) && row.when[i].name.empty()))
            return Fault::EmptyName;
    for (uint8_t i = 0; i < row.needCount; ++i)
        if (row.needs[i].need == Need::None || (row.needs[i].need == Need::Wields && row.needs[i].gear == Gear::None) ||
            (row.needs[i].need != Need::Wields && row.needs[i].spell.empty()))
            return Fault::EmptyName;
    if (IsReserved(row) && row.instead.empty())
        return Fault::ReservedWithoutInstead;
    if (row.kind == Kind::Proc && (row.effect.empty() || row.stacks == 0))
        return Fault::ProcWithoutEffect;
    if (row.kind == Kind::Aoe && (row.count < AOE_MIN || row.count > AOE_MAX))
        return Fault::AoeCount;
    if (row.kind == Kind::Recover && row.mana == ManaTier::None && row.instead.empty())
        return Fault::MissingInput;
    return Fault::None;
}

constexpr Fault Check(Row const* rows, size_t count)
{
    if (!count)
        return Fault::Empty;

    std::array<uint32_t, BAND_COUNT> perBand{};
    for (size_t i = 0; i < count; ++i)
    {
        if (Fault const fault = CheckRow(rows[i]); fault != Fault::None)
            return fault;

        Band const band = BandOf(rows[i].kind);
        if (i > 0 && band < BandOf(rows[i - 1].kind))
            return Fault::BandOrder;
        if (++perBand[static_cast<size_t>(band)] > BandCapacity(band))
            return Fault::OverCapacity;
    }

    Row const& last = rows[count - 1];
    if (last.kind != Kind::Filler)
        return Fault::NoFinalFiller;
    if (last.whenCount || last.needCount || !last.option.empty())
        return Fault::FinalFillerConditional;
    return Fault::None;
}

template <size_t N>
constexpr Fault Check(RowList<N> const& list)
{
    return Check(list.rows.data(), N);
}

// What the engine side reads; the rows live in the table's static storage
struct TableView
{
    std::string_view name;
    Role role = Role::Melee;
    uint32_t type = 0;
    Row const* rows = nullptr;
    size_t count = 0;
};

// Never defined: a fault makes the consteval Table call it, which does not compile
void TableFailsItsStaticCheck(Fault fault);

template <size_t N>
struct Table
{
    std::string_view name;  // the strategy name it is registered under
    Role role;
    uint32_t type;  // the GetType() value of the strategy it replaces
    RowList<N> rows;

    consteval Table(std::string_view tableName, Role tableRole, uint32_t tableType, RowList<N> const& tableRows)
        : name(tableName), role(tableRole), type(tableType), rows(tableRows)
    {
        if (Fault const fault = Check(rows); fault != Fault::None)
            TableFailsItsStaticCheck(fault);
    }

    constexpr TableView View() const { return TableView{name, role, type, rows.rows.data(), N}; }
};

// The nodes the generator emits: one per row, whatever the bot knows, at the row's relevance
inline constexpr std::string_view ROW_TRIGGER = "spec row";

struct Node
{
    std::string trigger;  // "spec row::<strategy>#<index>"
    std::string_view action;
    uint32_t hundredths = 0;
    float relevance = 0.0f;
};

inline std::string RowQualifier(std::string_view strategy, size_t index)
{
    std::string text(strategy);
    text.push_back('#');
    text += std::to_string(index);
    return text;
}

// "<strategy>#<index>" back into its parts; false for anything else
inline bool ParseRowQualifier(std::string_view qualifier, std::string_view& strategy, size_t& index)
{
    size_t const hash = qualifier.rfind('#');
    if (hash == std::string_view::npos || !hash || hash + 1 == qualifier.size())
        return false;

    size_t value = 0;
    for (char const digit : qualifier.substr(hash + 1))
    {
        if (digit < '0' || digit > '9')
            return false;
        value = value * 10 + static_cast<size_t>(digit - '0');
    }

    strategy = qualifier.substr(0, hash);
    index = value;
    return true;
}

inline std::vector<Node> Nodes(TableView const& table)
{
    std::vector<Node> nodes;
    nodes.reserve(table.count);
    std::array<uint32_t, BAND_COUNT> above{};
    for (size_t i = 0; i < table.count; ++i)
    {
        Band const band = BandOf(table.rows[i].kind);
        uint32_t const k = above[static_cast<size_t>(band)]++;
        Node node;
        node.trigger = std::string(ROW_TRIGGER) + "::" + RowQualifier(table.name, i);
        node.action = table.rows[i].action;
        node.hundredths = RowHundredths(band, k);
        node.relevance = RowRelevance(band, k);
        nodes.push_back(std::move(node));
    }
    return nodes;
}

// The row an executed action at this relevance came from, or -1. The engine's follow-up pushes add at most 0.003 to
// a row's relevance and rows of a band lie ROW_STEP hundredths apart, so a window of half a hundredth names one row
constexpr float ROW_MATCH_BELOW = 0.25f;  // hundredths: float rounding under the row's own value
constexpr float ROW_MATCH_ABOVE = 0.5f;   // hundredths: the follow-up pushes above it

inline int RowOfRelevance(TableView const& table, std::string_view action, float relevance)
{
    float const hundredths = relevance * 100.0f;
    std::array<uint32_t, BAND_COUNT> above{};
    for (size_t i = 0; i < table.count; ++i)
    {
        Band const band = BandOf(table.rows[i].kind);
        float const row = static_cast<float>(RowHundredths(band, above[static_cast<size_t>(band)]++));
        if (hundredths >= row - ROW_MATCH_BELOW && hundredths < row + ROW_MATCH_ABOVE && table.rows[i].action == action)
            return static_cast<int>(i);
    }
    return -1;
}

// The startup checks of a table (CF4's name check runs them once per class): every name a row gives resolves, the
// table is in a sibling group, and no hand-written node of the class sits on a row's relevance. The resolvers are
// callbacks, so the selftest runs them on fakes
enum class SpellName : uint8_t
{
    Unknown,  // no spell of the spell store has the name
    Passive,  // only passive spells have it
    Active
};

struct RowResolvers
{
    std::function<bool(std::string_view)> hasTrigger;
    std::function<bool(std::string_view)> hasStrategy;
    std::function<SpellName(std::string_view)> spell;
    std::function<std::string(std::string_view)> actionSpell;  // a CastSpellAction's spell, "" for another action
    std::function<bool(std::string_view)> inSiblingGroup;      // the table's strategy name
    std::set<uint32_t> handWritten;                            // the class's hand-written relevances, in hundredths
};

struct RowFinding
{
    std::string_view kind;  // the known file's entry kind: row-trigger, row-option, row-spell, row-clash, row-group
    std::string name;
};

inline std::vector<RowFinding> RowChecks(TableView const& table, RowResolvers const& resolve)
{
    std::vector<RowFinding> findings;
    auto const spellNamed = [&](std::string_view name, bool castable)
    {
        SpellName const found = resolve.spell(name);
        if (found == SpellName::Unknown || (castable && found == SpellName::Passive))
            findings.push_back({"row-spell", std::string(name)});
    };

    if (!resolve.inSiblingGroup(table.name))
        findings.push_back({"row-group", std::string(table.name)});

    std::vector<Node> const nodes = Nodes(table);
    for (size_t i = 0; i < table.count; ++i)
    {
        Row const& row = table.rows[i];
        if (!row.instead.empty() && !resolve.hasTrigger(row.instead))
            findings.push_back({"row-trigger", std::string(row.instead)});
        if (!row.option.empty() && !resolve.hasStrategy(row.option))
            findings.push_back({"row-option", std::string(row.option)});
        if (!row.effect.empty())
            spellNamed(row.effect, false);
        for (uint8_t t = 0; t < row.whenCount; ++t)
        {
            Term const& term = row.when[t];
            if (term.atom == Atom::Trigger && !resolve.hasTrigger(term.name))
                findings.push_back({"row-trigger", std::string(term.name)});
            else if (term.atom != Atom::Trigger && TermNeedsName(term.atom))
                spellNamed(term.name, false);
        }
        for (uint8_t n = 0; n < row.needCount; ++n)
            if (row.needs[n].need == Need::Knows || row.needs[n].need == Need::NotKnows)
                spellNamed(row.needs[n].spell, true);
        if (std::string const spell = resolve.actionSpell(row.action); !spell.empty())
            spellNamed(spell, false);
        if (resolve.handWritten.count(nodes[i].hundredths))
            findings.push_back({"row-clash", RowQualifier(table.name, i)});
    }
    return findings;
}

// The standing state of a row (refreshed on an engine rebuild and every SPELL_ID_REFRESH_MS, one flag per tick)
enum class Standing : uint8_t
{
    Known,
    NoAction,
    UnknownSpell,
    Requirement,
    OptionOff
};

constexpr std::array<std::string_view, 5> STANDING_WORDS = {"known", "no action", "unknown spell", "requirement",
                                                            "option off"};

constexpr std::string_view StandingWord(Standing standing) { return STANDING_WORDS[static_cast<size_t>(standing)]; }

constexpr uint32_t SPELL_ID_REFRESH_MS = 20000;

struct StandingFacts
{
    bool actionExists = true;
    bool castsSpell = false;  // the action is a CastSpellAction
    bool spellKnown = true;   // the cached "spell id" value is not 0
    bool requirementsHold = true;
    bool optionOn = true;  // the option, if any, is on the combat engine
};

constexpr Standing StandingOf(StandingFacts const& facts)
{
    if (!facts.actionExists)
        return Standing::NoAction;
    if (facts.castsSpell && !facts.spellKnown)
        return Standing::UnknownSpell;
    if (!facts.requirementsHold)
        return Standing::Requirement;
    if (!facts.optionOn)
        return Standing::OptionOff;
    return Standing::Known;
}

// fact: the bot knows the requirement's spell (Knows, NotKnows) or wields its gear (Wields)
constexpr bool Holds(Requirement const& requirement, bool fact)
{
    return requirement.need == Need::NotKnows ? !fact : (requirement.need == Need::None || fact);
}

template <typename Fact>
constexpr bool RequirementsHold(Row const& row, Fact&& fact)
{
    for (uint8_t i = 0; i < row.needCount; ++i)
        if (!Holds(row.needs[i], fact(row.needs[i])))
            return false;
    return true;
}

// A row is live in a tick: standing, then the kind's rule (or Instead), then every `when` term; each later check
// runs only when the earlier ones hold
template <typename Rule, typename TermCheck>
constexpr bool IsLive(Row const& row, Standing standing, Rule&& rule, TermCheck&& term)
{
    if (standing != Standing::Known)
        return false;
    if (!rule())
        return false;
    for (uint8_t i = 0; i < row.whenCount; ++i)
        if (!term(row.when[i]))
            return false;
    return true;
}

// One registered trigger shared by rows: one answer, asked again when its interval has passed (interval 0: once
// per tick, i.e. when the tick's time differs)
struct SharedAnswer
{
    bool answer = false;
    bool valid = false;
    uint32_t atMs = 0;

    template <typename Ask>
    bool Read(uint32_t nowMs, uint32_t intervalMs, Ask&& ask)
    {
        if (!valid || (intervalMs ? nowMs - atMs >= intervalMs : nowMs != atMs))
        {
            answer = ask();
            atMs = nowMs;
            valid = true;
        }
        return answer;
    }
};

// Fresh pushes. A basket holds its relevance and kept, the highest relevance a persistent push gave it (0 for
// none). A persistent push raises both, a fresh one only the relevance; at the end of every tick a basket without
// kept goes and any other falls back to kept. With persistent pushes only, kept == relevance always.
struct BasketRelevance
{
    float relevance = 0.0f;
    float kept = 0.0f;
};

constexpr BasketRelevance PushOf(float relevance, bool fresh) { return {relevance, fresh ? 0.0f : relevance}; }

constexpr void Merge(BasketRelevance& basket, BasketRelevance const& push)
{
    if (basket.kept < push.kept)
        basket.kept = push.kept;
    if (basket.relevance < push.relevance)
        basket.relevance = push.relevance;
}

// false: the basket goes
constexpr bool EndOfTick(BasketRelevance& basket)
{
    if (basket.kept <= 0.0f)
        return false;
    basket.relevance = basket.kept;
    return true;
}
}  // namespace spec

#endif
