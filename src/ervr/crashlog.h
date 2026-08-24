#pragma once

// Fatal-exception reporter. ELDEN RING installs its own crash handling and dies silently as
// far as our log is concerned - every warp crash so far has ended the log mid-frame with no
// clue as to WHERE. This registers a log-only vectored handler that names the faulting
// instruction, the pass it happened in, and the call stack, then gets out of the way
// (EXCEPTION_CONTINUE_SEARCH always - it never changes what the game does).
namespace ervr {
void CrashLogInstall();
void CrashLogSelfTest();   // proves the handler fires; logs PASSED/FAILED
void CrashLogShutdown();
}
