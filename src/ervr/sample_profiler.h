#pragma once
// [Experimental] ProfileSample: a sampling profiler for the engine's own frame work.
//
// Finds the game's busy threads (the frame thread, the task runners, the render threads),
// then every ~0.5 ms suspends each one for a moment, unwinds its call stack and resumes it.
// Every sample is tagged with the engine pass running at that instant (1 = the real frame,
// 2 = the second eye's draw-only pass, 0 = between them), so the log can say which functions
// a pass spends its milliseconds in - inclusive (anywhere on the stack) and self (the top).
// Cost: one core while on, and each sampled thread loses a few microseconds per sample.
namespace ervr {

void SampleProfilerStart();   // no-op unless [Experimental] ProfileSample=true; idempotent

}
