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

struct ID3D12CommandQueue;

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
bool FramePumpReady();
bool FramePumpSafeSetActive();   // pass 2 currently runs the SAFE keep set (finalize replay needed)      // task table found and the frame-driver hook is installed
// Which pass the engine is currently executing: 1 = real frame, 2 = draw-only second pass.
// Read from the present callback - presents are synchronous with the pass (measured: 58/58
// presents inside their pass windows, all on the main-loop thread).
int  FramePumpCurrentPass();
// The main-loop thread queue_tasks (both passes) runs on; 0 before the first frame.
unsigned long FramePumpFrameThread();
// The task runner's current_concurrent_task_group field ([[CSTask+0x78]+0x18]), below -1 when unreadable.
// NOT the running group's index on 2.7.2.0: it read 18 inside the world step's action-button update and 103
// inside a menu read (2026-11-03) - a raw tag that tells call sites apart. Safe from an exception handler.
int  FramePumpCurrentTaskGroup();
// stereo.cpp calls this from any present arriving while the pass is 2 - the proof the
// engine really ran the second pass to Flip. Feeds the wedged-runner watchdog.
void FramePumpNotePass2Present();
// Heartbeat: second passes executed since the last read, and the last pass-2 duration.
long  FramePumpTakePasses();
float FramePumpLastMs();
// Heartbeat: wall time of each engine pass's queue_tasks call since the last read (Flip and
// any blocking inside it included). [0] = pass 1 (the real frame), [1] = pass 2.
struct PumpPassTimes { float msAvg[2] = {}, msMax[2] = {}; };
PumpPassTimes FramePumpTakePassTimes();
// The game's D3D12 present queue, from the ReShade present handler (null on device loss).
// [VR] GpuSync drains it with a fence between the two engine passes so pass 2's constant
// uploads cannot overwrite per-frame GPU data pass 1's draws are still reading.
void  FramePumpSetGpuQueue(ID3D12CommandQueue* q);
float FramePumpGpuSyncLastMs();   // CPU time the last drain cost, ms

// Content-probe experiment ([Experimental] TaskContentProbe): samples the back buffer at
// each Present while active. Called from the ReShade present handler; cheap no-op when idle.
struct Dx12Frame;
void FrameProbeOnPresent(const Dx12Frame& f);

// Overrides [VR] Pass2Keep at runtime (numpad hotkeys). Empty string restores the ini value.
void FrameProbeSetPass2Keep(const char* spec);
// The engine's frame-timing dt block (hw-bp captured; null until the pump has run once).
// [Experimental] CamWatchDt points the CamWatch census here instead of the camera.
uint8_t* FramePumpDtWatchTarget(uint32_t* outLen);
// The task-runner's staged frame-state block (steps/flags/dt staging), hw-bp captured.
uint8_t* FramePumpStepsDstWatchTarget(uint32_t* outLen);
const TaskGroupTable& FrameProbeGetTable(); // valid only after the probe has succeeded
int  FrameProbeIndexOf(const char* name);   // -1 if absent
bool FrameProbeGetActive(int index, bool& out);
bool FrameProbeSetActive(int index, bool active);        // writes the byte directly
// Calls CS::CSTask::set_task_group_state (vtable slot 2) - the engine's own entry point, which
// does whatever bookkeeping the raw byte write misses.
bool FrameProbeSetActiveApi(int index, bool active);
}
