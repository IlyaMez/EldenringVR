#pragma once
// The game's interaction state across the stereo second pass.
//
// The action-button system - the prompt the interact button answers (doors, levers, graces, items on the
// ground) - updates once per frame and is then asked, prompt by prompt, whether the press was for it
// (ExecuteActionButton). Full-frame pass 2 runs that update a second time per frame (measured 2026-11-03: the
// manager's reset write at exe+0xa64e4f fires in both passes, its menu-side reader in pass 1 only).
//
// What the history says about it (each row a release players ran):
//   0.6.0  update both passes, event manager (25) pass 1 only  -> doors, levers, lifts dead
//   0.6.5  update both passes, event manager both passes       -> doors work; an item card stuck (same press
//                                                                  answered twice, the item handled twice)
//   0.7.0  update pass 1 only, event manager both passes       -> doors, levers, chests dead: the press is
//                                                                  accepted in pass 1 and nothing happens
//                                                                  (players' logs 2026-11-04); their
//                                                                  Pass2ActionButtons=true opened them again
// The event manager holds the door/lever executor (obj_act_exec, fromsoftware-rs event_man.rs): it and this
// update have to step together. So the update runs in both passes again ([VR] Pass2ActionButtonUpdate, default
// true), and the 0.6.5 card is handled at the answer: a prompt (caller + action button id) that answers
// "pressed" again within two updates of its last "pressed" - one stereo pair - is answered "not pressed"
// ([VR] Pass2RepeatPressGuard). Every ExecuteActionButton call still reaches the game, in either pass.
//
// The update's entry also resets the region table that picks the nearest prompt of each kind (action_button_update.h):
// a prompt that asks it counts only if it was the nearest at the previous reset. The event scripts (22-24) run in
// pass 1 only and ask it for boss fog walls and their other area prompts (EcTestActionButtonRegion), so the table
// resets once per frame, in pass 1; pass 2 runs the manager's update alone ([VR] Pass2RegionReset, default false):
//   0.6.0-0.6.5, 0.7.1-0.7.2  table reset in both passes  -> fog walls show no prompt (always compared with pass 2's
//                                                             empty slot; 2026-11-06, co-op client at a boss fog)
//   0.7.0                     whole entry in pass 1 only   -> fog walls fine, doors dead (above)
//   0.7.3                     reset in pass 1 only         -> fog walls fine; graces, plants, items, runes asked in
//                                                             both passes registered in one and failed the strictly-
//                                                             nearer range check in the other, so the other pass's
//                                                             update forgot them: no grace prompt, item prompts
//                                                             flickering (players' logs 2026-11-06, 0/277 per pass)
//   0.7.4                     reset in pass 1, each kind's nearest banked at both pass boundaries and carried into
//                             the reset ([VR] Pass2RegionBank, default true; action_button_update.h)
//
// Where prompts are asked is NOT per pass (measured 2026-11-04): a grace's, an NPC's and a ground item's were
// asked only while pass 2 ran, a door's, a lever's only in pass 1. The first version of the 0.7.0 fix answered
// every pass-2 call "not pressed" and nothing in the world could be interacted with.
//
// Also logged, capped, at INFO ([VR] Pass2Watch): every press the game accepts, with its pass and caller, each
// prompt caller's calls per pass and thread, and what each pass changed in the action-button manager,
// MapItemMan's item-get popup queue and the menu manager's per-element UI states.
//
// Exe signature-checked: each hook needs its function's first bytes. Threads: the boundaries run on the
// game's frame thread around queue_tasks; the hooks on whichever thread calls them; the lookup on its own.
#include <cstdint>

namespace ervr {
void Pass2InteractInit();                     // worker, after MH_Initialize
void Pass2InteractFrameBegin(long frame);     // frame thread, before pass 1
void Pass2InteractBeforePass2();              // frame thread, after pass 1, when pass 2 is about to run
void Pass2InteractAfterPass2();               // frame thread, after pass 2
uint8_t* Pass2InteractAbmTarget(uint32_t* outLen);   // the action-button manager's flag bytes, for CamWatch
}
