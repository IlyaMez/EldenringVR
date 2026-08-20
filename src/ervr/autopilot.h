#pragma once
// Scripted controller input, for unattended test runs.
//
// Exists so a change can be tested without a human driving the game: launch ELDEN RING,
// let this walk through the title/save screens, let the probe or the render path do its
// work, read ERVR.log. It rides the XInput synthesis we already own, which is far more
// reliable than SendInput for a gamepad-driven menu.
//
// Off unless [Experimental] AutoPilotSeconds > 0. It presses buttons in the game, so it is
// never enabled by default.
#include <windows.h>
#include <xinput.h>

namespace ervr {

void AutoPilotInit();
// Called from the XInput hook. ORs scripted buttons into `pad`; true if it touched anything.
bool AutoPilotMerge(XINPUT_GAMEPAD& pad);
// Configured and not yet finished. The capability hook needs this: ELDEN RING asks
// XInputGetCapabilities first and stops polling GetState at all if that says "no pad", so
// without it the autopilot is never even called.
bool AutoPilotActive();
}
