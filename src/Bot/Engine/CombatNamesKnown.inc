// The names the check lists today per class (combat-foundation CF4). Generated from tests/combat-names-known.txt (the
// offline selftest checks the two are identical); G2 empties it.
static char const* const COMBAT_NAMES_KNOWN = R"KNOWN(
# Names the combat-names check lists today, per class section and the shared section (combat-foundation CF4).
# Maintenance: written from the server's output (playerbots combat names, the ' <- referrer' part stripped), never by
# hand. Lines only leave (the selftest and the in-server check fail on a stale one); only an upstream sync adds a line,
# for a class file or an instance script, never for a base file.
hunter action:explosive shot
priest trigger:boost
deathknight action:mind freeze on enemy healer
deathknight trigger:rune strike
shaman action:grounding totem
mage trigger:high threat
warlock trigger:chaos bolt
warlock trigger:conflagrate
# shared: names every class's general set holds. The two Uld names are WD's and WPE's; food is WD's (mod-dungeon-clear
# DungeonClearStrategy.cpp:415 and DungeonClearMultiplier.cpp:338 say food, the action is eat; CF.17).
shared action:food
shared action:thorim fall from floor action
shared trigger:yogg-saron shadow resistance trigger
)KNOWN";
