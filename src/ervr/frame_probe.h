#pragma once
// M2: ELDEN RING's frame task-group table - the lever true stereo has to pull.
//
// The frame is a strictly ordered list of 169 NAMED task groups, all simulation upstream of
// `Draw_Pre`(144) and the scene drawn by `DrawStep`(153) .. `DrawEnd`(157).
//
// Confirmed on eldenring.exe 2.6.2.0: 169 groups, stride 136, id at +0, name at +4 (wchar[64]),
// active at +132, and ids are `0x90000000 | index` rather than plain indices. Writing the
// `active` byte does NOTHING (measured): the engine consumes it when it BUILDS the frame's
// queue, so gating must go through `CS::CSTask::set_task_group_state` - vtable slot 3 on the
// CSTask instance (slot 0 is the destructor; the CSTaskImp singleton at RTTI "CS::CSTaskImp"
// is only a handle { vptr, CSTask* } and its vtable must not be used for these calls).
#include <cstdint>

namespace ervr {

struct TaskGroupTable {
    bool     valid = false;
    uint8_t* owner = nullptr;    // the CSTask instance - `this` for the CSTask vtable calls
    void**   vtable = nullptr;   // the CSTask's own vtable (slot 3 = set_task_group_state)
    uint8_t* imp = nullptr;      // the CS::CSTaskImp singleton - `this` for register_task
    uint8_t* entry0 = nullptr;
    size_t   stride = 0;
    int      count = 0;
};

void FrameProbeTick();                      // worker thread; spawns its own thread, no-op unless enabled

// ---- production surface for full stereo (M3) -------------------------------------------
// The frame pump: after every real engine frame, run a second draw-only engine frame (sim
// task lists gated, Draw_Pre..Draw_Post + Flip kept) so both eyes render at one sim time.
// Armed automatically when [VR] StereoMode=full; the second pass only actually runs on
// frames where the game stashed a second-eye camera (GameERApplySecondEyeCamera).
bool FramePumpReady();      // task table found and the frame-driver hook is installed
// Which pass the engine is currently executing: 1 = real frame, 2 = draw-only second pass.
// Read from the present callback - presents are synchronous with the pass (measured: 58/58
// presents inside their pass windows, all on the main-loop thread).
int  FramePumpCurrentPass();
// Heartbeat: second passes executed since the last read, and the last pass-2 duration.
long  FramePumpTakePasses();
float FramePumpLastMs();
const TaskGroupTable& FrameProbeGetTable(); // valid only after the probe has succeeded
int  FrameProbeIndexOf(const char* name);   // -1 if absent
bool FrameProbeGetActive(int index, bool& out);
bool FrameProbeSetActive(int index, bool active);        // writes the byte directly
// Calls CS::CSTask::set_task_group_state (vtable slot 2) - the engine's own entry point, which
// does whatever bookkeeping the raw byte write misses.
bool FrameProbeSetActiveApi(int index, bool active);
}
