#pragma once
// The ERVR tab in the game's own Settings (pause menu > System), to the right of Graphics: the settings players
// change while playing (settings_tab_logic.h), on a page the game builds with its own code.
//
// The Settings dialog (CS::OptionSettingTopDialog) puts its tabs, CS::MenuOptionCategory items, into a list of
// 10 as it opens; it has 9 in game. A hook on that list's push_back adds ERVR's before Quit Game (after Graphics
// where there is no Quit Game). A tab names the page it opens by a kind, 0 to 9; ERVR's is kind 4, the console
// Brightness page, which has no tab on PC, and a hook on that page's factory builds ERVR's page instead: the
// game's own page code, with a row builder of ERVR's that adds each row with the game's own helpers (an OFF/ON
// row, a list row). Every row edits a byte of ERVR's; the worker sees the change and writes it into ERVR.ini,
// which hot-reloads.
//
// Two things the layout (menu/win/02_040_optionsetting.gfx) has no room for, done by hand: a tenth tab icon (the
// bar has 9; its icons follow the cursor grid's scroll) and a window of ERVR's own (the page uses Sound's, and
// the two pages never live at once).
//
// Exe 2.7.2.0 only (image offsets checked against their first bytes, vtables against their RTTI names); any
// other build keeps the game's Settings as they are, with one log line.
//
// Threads: the hooks and the row builder run on the game's main thread as the Settings open and a tab is
// chosen; the rest on the worker.
namespace ervr {
void SettingsTabInit();             // worker, after GameVersionInit and MH_Initialize
void SettingsTabTick(double now);   // worker loop: the tab's changes go into ERVR.ini
void SettingsTabIniChanged();       // worker: ERVR.ini was read again
}
