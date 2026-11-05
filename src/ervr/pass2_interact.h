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
