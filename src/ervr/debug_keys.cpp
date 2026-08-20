#include "debug_keys.h"
#include "frame_probe.h"
#include "vr_backend.h"
#include "config.h"
#include "log.h"
#include <windows.h>
#include <atomic>

namespace ervr {
namespace {

std::atomic<int>   g_presentLag{-1};    // -1 = follow the ini
std::atomic<float> g_pass2Pitch{-1.f};  // <0 = follow the ini
std::atomic<float> g_pass1Pitch{0.f};   // capture probe; no ini, hotkey only
std::atomic<int>   g_gpuSync{-1};       // -1 = follow the ini
std::atomic<int>   g_fullFrame{-1};     // -1 = follow the ini; 0/1 override
int                g_keepIdx = -1;      // -1 = follow the ini
int                g_pitchState = 0;    // 0 off | 1 pitch pass 2 | 2 pitch pass 1

// The candidate sets for the draw-only second pass. The CamWatch census (2026-08-20) found
// the camera-propagation machinery - the once-per-frame function chain (exe+0x680bxx copy +
// CamFinalize 0x680d70) that carries the camera from the game side into what the renderer
// actually consumes - running in group 123 (GameFlowInGame_MoveMap_PostPhysics_1_Core0),
// with 118/124/125 as its family. Without those, pass 2 renders pass 1's captured camera
// no matter what the camera objects hold (measured: pass-1 pitch tilts BOTH eyes).
// 106 = CameraStep (our hook writes the second eye there), 144-158 = Draw_Pre..Draw_Post,
// 164 = Flip.
// Darkness hunt, round 2. The valid post-camera-fix sweep cleared 49/98/107/142; the
// in-game task dump (2026-08-20 20:21) then showed a CS::CSDrawStep task registered in
// group 0 (FrameBegin) - the RENDERER'S frame-begin (per-frame buffer swap / upload-ring
// advance / allocator reset), gated in pass 2 until now. A draw pass without its frame-
// begin reads per-frame GPU data nobody set up: per-object dark + double-buffer flicker,
// exactly the symptom. 168 (FrameEnd) is its counterpart; 161/165 are the resource
// lifecycle (FileStep/DelayDelete). 160 (BeginShiftWorldPosition) and 166 (AI GC) stay
// gated always - double-running a world-origin shift is not survivable.
// Darkness hunt, round 3. Rounds 1-2 cleared 49/98/107/142 and the render frame lifecycle
// (0/168/161/165). New clue: a TORCH lights geometry correctly in BOTH eyes - dynamic light
// computed during the draw works, so what pass 2 is missing is the ENVIRONMENT lighting
// (IBL/ambient/sun) prepared by a per-frame producer outside the draw. Prime named suspect:
// EmkSystem (22-24, 64 tasks - the environment-map system). Sets 3-5 bisect the remaining
// populated gated bands; they may cause brief weirdness (2x sim speed, doubled audio) -
// judge ONLY the dark eye's brightness and cycle on. NEVER kept: 9 PadStep (input replay),
// 11/160 world-origin shift pair, 166 AI GC.
// Darkness hunt, round 5: bisect DOWN. Bottom-up sweeps exhausted every plausible producer
// group (49/98/107/142, frame lifecycle 0/168/161/165, EmkSystem 22-24, Location, Havok,
// managers) - none brightened pass 2. So flip it: keep EVERYTHING except the known-fatal
// groups (7-21 game-flow band - crashed the game; 109 sound; 126 MenuMan; 130-133 AI/net
// flow; 160 world-origin shift; 166 AI GC). Sets 2-3 double-step the world sim: the game
// runs ~2x speed and both eyes desync in time while held - EXPECTED, judge only whether
// the right eye brightens, and do it out of combat.
struct KeepSet { const char* spec; const char* what; };
const KeepSet kKeeps[] = {
    {"106,118,123-125,144-158,164",                     "reference (dark right eye)"},
    {"0-6,22-108,110-125,127-129,134-159,161-165,167,168", "keep ALL safe groups (the decisive test)"},
    {"0-6,22-106,118,123-125,144-158,164",              "all-safe LOW half (0-6,22-105)"},
    {"106,108,110-125,127-129,134-159,161-165,167,168", "all-safe HIGH half (108-168)"},
    {"144-158,164",                                     "bare draw (bright-reference sanity)"},
};
constexpr int kKeepCount = (int)(sizeof(kKeeps) / sizeof(kKeeps[0]));

// A pulse pattern both hands can feel without looking at anything.
void Buzz(int pulses) {
    IVRBackend* vr = ActiveBackend();
    if (!vr || !vr->IsReady()) return;
    for (int i = 0; i < pulses; ++i) {
        vr->Haptic(HandLeft, 0.6f, 0.06f);
        vr->Haptic(HandRight, 0.6f, 0.06f);
        Sleep(i + 1 < pulses ? 130 : 60);
    }
}

bool Pressed(int vk) {
    // Edge detection per key: GetAsyncKeyState's low bit is unreliable when several
    // readers poll it, so track the high bit ourselves.
    static bool s_down[256] = {};
    if (vk < 0 || vk > 255) return false;
    const bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
    const bool edge = down && !s_down[vk];
    s_down[vk] = down;
    return edge;
}

void Announce() {
    const int lag = g_presentLag.load();
    const float pitch = g_pass2Pitch.load();
    const int sync = g_gpuSync.load();
    LOGI("[keys] PresentLag=%s  Pass2Keep=%s  pitch=%s  GpuSync=%s",
         lag < 0 ? "(ini)" : (lag ? "1" : "0"),
         g_keepIdx < 0 ? "(ini)" : kKeeps[g_keepIdx].what,
         g_pitchState == 0 ? (pitch < 0.f ? "(ini)" : "off")
                           : (g_pitchState == 1 ? "pass2 30deg" : "pass1 30deg"),
         sync < 0 ? "(ini)" : (sync == 0 ? "0" : (sync == 1 ? "1" : "2")));
}

} // namespace

int   DebugKeysPresentLag() { return g_presentLag.load(std::memory_order_relaxed); }
float DebugKeysPass2Pitch() { return g_pass2Pitch.load(std::memory_order_relaxed); }
float DebugKeysPass1Pitch() { return g_pass1Pitch.load(std::memory_order_relaxed); }
int   DebugKeysGpuSync()    { return g_gpuSync.load(std::memory_order_relaxed); }
int   DebugKeysPass2FullFrame() { return g_fullFrame.load(std::memory_order_relaxed); }

void DebugKeysTick() {
    if (!Cfg().debugKeys) return;

    if (Pressed(VK_NUMPAD1)) {
        // First press adopts the opposite of whatever the ini currently says, so the very
        // first tap always changes something visible.
        const int cur = g_presentLag.load() < 0 ? (Cfg().vrPresentLag != 0 ? 1 : 0) : g_presentLag.load();
        const int next = cur ? 0 : 1;
        g_presentLag.store(next);
        LOGI("[keys] PresentLag -> %d (%s)", next,
             next ? "pass 2 opens the XR frame (left eye)" : "each present carries its own pass");
        Buzz(next ? 2 : 1);
    }
    if (Pressed(VK_NUMPAD2)) {
        g_keepIdx = (g_keepIdx + 1) % kKeepCount;
        FrameProbeSetPass2Keep(kKeeps[g_keepIdx].spec);
        LOGI("[keys] Pass2Keep -> %s = {%s}", kKeeps[g_keepIdx].what, kKeeps[g_keepIdx].spec);
        Buzz(g_keepIdx + 1);
    }
    if (Pressed(VK_NUMPAD3)) {
        // Three states: off -> pitch pass 2 -> pitch pass 1 -> off. Pass-2 pitch names the
        // eye that receives pass 2's content. Pass-1 pitch is the capture probe: if BOTH
        // eyes tilt, the renderer captures its camera once per sim frame from pass 1 and
        // pass 2 can never have its own camera through the task groups alone; if ONE eye
        // tilts, that eye is the pass-1 content receiver (routing answered directly).
        g_pitchState = (g_pitchState + 1) % 3;
        g_pass2Pitch.store(g_pitchState == 1 ? 30.f : 0.f);
        g_pass1Pitch.store(g_pitchState == 2 ? 30.f : 0.f);
        LOGI("[keys] pitch -> %s",
             g_pitchState == 0 ? "off"
                               : (g_pitchState == 1 ? "PASS 2 tilted 30 deg (which eye tilts?)"
                                                    : "PASS 1 tilted 30 deg (one eye or both?)"));
        Buzz(g_pitchState + 1);
    }
    if (Pressed(VK_NUMPAD4)) {
        // GPU drain between the two engine passes: 0 off, 1 before pass 2 (protects the
        // LEFT eye's data), 2 also after pass 2 (protects the right's). One buzz per level+1.
        const int cur = g_gpuSync.load() < 0 ? Cfg().vrGpuSync : g_gpuSync.load();
        const int next = (cur + 1) % 3;
        g_gpuSync.store(next);
        LOGI("[keys] GpuSync -> %d (%s)", next,
             next == 0 ? "off - passes overlap on the GPU"
                       : (next == 1 ? "drain before pass 2 (left eye protected)"
                                    : "drain before AND after pass 2 (both eyes protected)"));
        Buzz(next + 1);
    }
    if (Pressed(VK_NUMPAD5)) {
        // Full-frame second pass: a complete engine frame at dt~0 instead of the draw-only
        // gated pass. The heavyweight fix for pass-2 environment lighting.
        const int cur = g_fullFrame.load() < 0 ? (Cfg().vrPass2FullFrame ? 1 : 0) : g_fullFrame.load();
        const int next = cur ? 0 : 1;
        g_fullFrame.store(next);
        LOGI("[keys] Pass2FullFrame -> %d (%s)", next,
             next ? "pass 2 = complete engine frame at dt~0" : "pass 2 = draw-only gated pass");
        Buzz(next ? 2 : 1);
    }
    if (Pressed(VK_NUMPAD0)) {
        Announce();
        Buzz(1);
    }
}
}
