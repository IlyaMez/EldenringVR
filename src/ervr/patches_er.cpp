#include "patches_er.h"
#include "game_memory.h"
#include "config.h"
#include "log.h"
#include <windows.h>
#include <cmath>
#include <cstring>

namespace ervr {
namespace {

// SafeWrite is a plain guarded memcpy and .text is read-execute, so patches need their own
// protection dance. Restore the old protection and flush the icache like any code patcher.
bool PatchWrite(uint8_t* at, const void* bytes, size_t n) {
    DWORD old = 0;
    if (!VirtualProtect(at, n, PAGE_EXECUTE_READWRITE, &old)) return false;
    memcpy(at, bytes, n);
    VirtualProtect(at, n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), at, n);
    return true;
}

// Patterns from uberhalit/EldenRingFpsUnlockAndMore GameData.cs (MIT, (c) uberhalit).
//
// FRAMELOCK: `mov [reg+disp], 0x3C888889` - the 1/60 the game stores as its per-frame delta
// time every frame. This is the SAME constant our registered probe tasks observed coming
// back as FD4TaskData::dt (fixed 0.01667 even when the real frame rate sagged to 40), so
// patching it moves the limiter target AND the simulation step together: the game runs at
// correct speed exactly when it actually reaches the target rate, and in slow motion when
// the GPU cannot keep up. That is inherent to the game's fixed-step design, not a bug here.
constexpr const char* kPatFramelock = "C7 ?? ?? ?? 88 88 3C EB";
constexpr int         kOffFramelock = 3;   // the imm32 float (its low byte varies: 88/89/90)
constexpr const char* kPatFramelockFuzzy = "89 73 ?? C7 ?? ?? ?? ?? ?? ?? EB ?? 89 73";
constexpr int         kOffFramelockFuzzy = 6;

// HERTZLOCK: on every resolution change the game writes "60 Hz" + a change flag, clamping
// exclusive-fullscreen modes back to 60. uberhalit's patch zeroes both immediates.
constexpr const char* kPatHertzlock = "EB ?? C7 ?? ?? 3C 00 00 00 C7 ?? ?? 01 00 00 00";
constexpr int         kOffHertzBase = 2;    // the first mov
constexpr int         kOffHertzInt1 = 3;    // +base: the 0x3C (60) immediate
constexpr int         kOffHertzInt2 = 10;   // +base: the 1 (change flag) immediate

// Framelock imm resolve, shared by the fps unlock and the full-frame pass-2 dt swap.
uint8_t* ResolveFrameDtImm() {
    static uint8_t* s_at = nullptr;
    static bool s_tried = false;
    if (!s_tried) {
        s_tried = true;
        uint8_t* fl = AobScan(kPatFramelock);
        int off = kOffFramelock;
        if (!fl) { fl = AobScan(kPatFramelockFuzzy); off = kOffFramelockFuzzy; }
        if (fl) s_at = fl + off;
    }
    return s_at;
}

} // namespace

float* PatchesFrameDtImm() { return (float*)ResolveFrameDtImm(); }

bool PatchesWriteFrameDt(float dt) {
    uint8_t* at = ResolveFrameDtImm();
    if (!at) return false;
    return PatchWrite(at, &dt, 4);
}

void PatchesInit() {
    const int fps = Cfg().patchFpsTarget;
    if (fps <= 0) return;
    // uberhalit's own clamp. Below 30 the game misbehaves; above 360 nothing sensible.
    const int target = fps < 30 ? 30 : (fps > 360 ? 360 : fps);
    auto* base = (uint8_t*)GetModuleHandleW(nullptr);

    uint8_t* flimm = ResolveFrameDtImm();
    if (!flimm) {
        LOGW("[patch] FpsTarget=%d: framelock pattern not found (game patch changed it?); no unlock", fps);
        return;
    }
    float before = 0.f;
    memcpy(&before, flimm, 4);
    const float dt = 1.0f / (float)target;
    if (!PatchWrite(flimm, &dt, 4)) {
        LOGW("[patch] framelock found at rva 0x%llx but the write failed", (unsigned long long)(flimm - base));
        return;
    }
    LOGI("[patch] fps unlock: frame tick at rva 0x%llx: %.1f -> %d fps (dt %.5f). The game "
         "runs slow-motion whenever it cannot actually reach this rate.",
         (unsigned long long)(flimm - base),
         before > 1e-6f ? 1.f / before : 0.f, target, dt);

    if (uint8_t* hz = AobScan(kPatHertzlock)) {
        uint8_t* b = hz + kOffHertzBase;
        const int32_t zero = 0;
        const bool ok1 = PatchWrite(b + kOffHertzInt1, &zero, 4);
        const bool ok2 = PatchWrite(b + kOffHertzInt2, &zero, 4);
        if (ok1 && ok2)
            LOGI("[patch] hertz lock removed at rva 0x%llx (display modes no longer forced to 60 Hz)",
                 (unsigned long long)(b - base));
        else
            LOGW("[patch] hertz lock write failed");
    } else {
        // Only matters for exclusive fullscreen; borderless/windowed never hits that path.
        LOGW("[patch] hertz-lock pattern not found; exclusive fullscreen may stay 60 Hz");
    }
}
}
