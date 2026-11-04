#pragma once
// The game's interaction state across the stereo second pass.
//
// The action-button system - the prompt the interact button answers (doors, levers, graces, items on the
// ground) - updates once per frame and is then asked, prompt by prompt, whether the press was for it
// (ExecuteActionButton). Pass 2 ran that update a second time per frame (measured 2026-11-03: the manager's
// reset write at exe+0xa64e4f fires in both passes, its menu-side reader in pass 1 only), so which prompts
// survived into the next frame's decision depended on which task groups pass 2 kept: with EventMan (25)
// gated, pass 2's update consumed the doors' prompts and the interact button did nothing; with it kept, the
// same press was answered again in pass 2 - an item picked up twice, its card left on screen with nothing to
// close it. With [VR] Pass2ActionButtons=false (default) the update runs in pass 1 only - one decision per
// frame, as in the single-pass game - and every ExecuteActionButton call still reaches the game, in either pass;
// a prompt that answers "pressed" a second time for the same decision is answered "not pressed".
//
// Where prompts are asked is NOT per pass (measured in-headset 2026-11-04): a grace's and an NPC's were asked
// only while pass 2 ran, once per stereo pair, never in pass 1. The first version of this fix answered every
// pass-2 call "not pressed" and nothing in the world could be interacted with.
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
