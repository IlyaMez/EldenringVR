#pragma once
namespace ervr {
// Installs hooks on every XInput module currently loaded (xinput1_4 / xinput1_3 /
// xinput9_1_0). Safe to call repeatedly; returns true once at least one module is hooked.
bool XInputHooksInstall();
void XInputHooksRemove();
// Counters since last call (GetState calls for user 0, how many were modified, how many fabricated).
void XInputHookStats(long& calls, long& merged, long& fabricated);
}
