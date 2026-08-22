#include "stereo.h"
#include "debug_keys.h"
#include "dx12_blit.h"
#include "frame_probe.h"
#include "hud.h"
#include "vr_backend.h"
#include "config.h"
#include "log.h"
#include <windows.h>
#include <atomic>
#include <cmath>
#include <mutex>

namespace ervr {
namespace {

std::mutex     g_mtx;          // serialises present against device-loss teardown
Dx12Blitter    g_blit;
bool           g_bound = false;
ID3D12Device*  g_boundDevice = nullptr;

std::atomic<long>  g_frames{0}, g_submitFails{0}, g_blitFails{0}, g_stereoFrames{0};
std::atomic<long>  g_eyeBlits[2]{};   // successful per-eye blits (L, R)
std::atomic<long>  g_eyeDesync{0};    // report popped with the wrong eye for the pass
std::atomic<long>  g_hudFrames{0};    // HUD layers submitted since the last read
std::atomic<long>  g_monoStalls{0};   // presents that fell back because pass 2 stopped
bool               g_pumpStalled = false;   // render thread only; drives the log line
std::atomic<float> g_lastSubmitMs{0.f};
std::atomic<int>   g_mode{(int)StereoMode::Off};
std::atomic<float> g_cropU{1.f}, g_cropV{1.f};

// ---- the baton ---------------------------------------------------------------------
std::mutex        g_batonMtx;
bool              g_reqActive = false;
uint64_t          g_reqPair = 0;
Vec3              g_reqOffset[2];    // game convention, world-scaled
Quat              g_reqRot[2];
float             g_reqFovWrite = 0; // radians, already in Cam_Fov's own axis
int               g_nextEye = 0;     // toggled by the GAME thread, so alternation is exactly
                                     // one eye per game frame however the threads are phased
uint64_t          g_lastReportTick = 0;

// Reports come back through a FIFO, not a single slot. ELDEN RING's game thread runs ahead
// of present by a frame or more, so "the newest report" is not the frame being presented -
// reading it would put left-eye images into the right eye's swapchain, every frame, forever.
// A queue keeps frames in the order they were rendered, which is the order they present in.
constexpr int kRepQ = 4;
StereoEyeReport g_repQ[kRepQ];
int  g_repHead = 0, g_repCount = 0;

// Eye geometry as located for each pair, kept in a small ring so a game frame that lands a
// frame or two late is still matched with the geometry it was actually told to use.
constexpr int kPairRing = 8;
VREyeView g_pairViews[kPairRing][2]{};

// Poses/FOVs each eye's CURRENT swapchain content was rendered with. OpenXR keeps the last
// released image of a swapchain alive, so alternate-eye submits a fresh image for one eye
// and the previous one for the other - which is exactly what these echo. Render thread only.
VREyeView g_submit[2]{};
bool      g_submitValid[2]{};

// Full stereo: the XR frame opened at pass 1's present, closed at pass 2's. Same thread as
// every present (the main-loop thread), so plain bools are fine.
bool        g_frameOpen = false;
VRFrameInfo g_openFi;
// When the last pass-2 present arrived. The frame pump only runs a second pass on frames
// where the game stashed a second-eye camera, and a full-screen menu stops it doing that -
// so pass 2 simply stops coming. The closing branch below used to wait for it forever,
// submitting NOTHING in the meantime, which the compositor answers by reprojecting the last
// frame: a frozen world and no menu, for as long as the menu is open. This is how it
// notices instead.
ULONGLONG   g_lastOpenTick = 0;
constexpr ULONGLONG kPumpStallMs = 120;   // ~7 pairs at 60 Hz; well past a dropped frame

double NowMs() {
    static LARGE_INTEGER f = [] { LARGE_INTEGER x; QueryPerformanceFrequency(&x); return x; }();
    LARGE_INTEGER c; QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
}

StereoMode ParseMode(const std::string& s) {
    if (s == "off") return StereoMode::Off;
    if (s == "debugalt") return StereoMode::DebugAlt;
    if (s == "full") return StereoMode::Full;
    return StereoMode::Cinema;   // "cinema", "auto", anything else
}

// VR (right-handed, -Z forward) -> game (left-handed, +Z forward): mirror Z. Same mapping
// game_er.cpp uses; it is its own inverse.
Vec3 ToGame(const Vec3& v) { return {v.x, v.y, -v.z}; }
Quat ToGame(const Quat& q) { return {-q.x, -q.y, q.z, q.w}; }

// The rectangle of the game's image that matches the runtime's per-eye frustum.
//
// The game renders one symmetric frustum at the window's aspect. The headset wants an
// asymmetric, roughly square one. As long as the game's frustum is the WIDER of the two in
// every direction, the headset's frustum is just a sub-rectangle of the rendered image - so
// we crop to it and submit the exact asymmetric FOV. No projection-matrix patching, no
// aspect-ratio patching of the game, and correct by construction.
struct Crop {
    float su = 1, sv = 1, ou = 0, ov = 0;
    VRFov fov;
};
// The vertical FOV the game has to render so that ONE image contains a whole eye's field of
// view, at the render target's aspect. Derived from the runtime rather than configured: the
// headset is the only thing that knows what it needs, and on a Quest 3 it is ~110 deg
// vertical - two and a half times ELDEN RING's own 43, which is why the un-widened image
// looked like a small screen floating in the middle of the view.
float RequiredFovY(const VREyeView rel[2], float aspect) {
    float tv = 0.f, th = 0.f;
    for (int i = 0; i < 2; ++i) {
        tv = std::fmax(tv, std::fmax(std::fabs(std::tan(rel[i].fov.angleUp)),
                                     std::fabs(std::tan(rel[i].fov.angleDown))));
        th = std::fmax(th, std::fmax(std::fabs(std::tan(rel[i].fov.angleLeft)),
                                     std::fabs(std::tan(rel[i].fov.angleRight))));
    }
    const float t = std::fmax(tv, th / std::fmax(aspect, 0.1f)) * 1.02f;   // 2% edge margin
    return 2.f * std::atan(std::fmin(t, 8.f));
}

Crop MakeCrop(float gameFovY, float aspect, const VRFov& want) {
    const float tanV = std::tan(gameFovY * 0.5f);
    const float tanH = tanV * aspect;
    // Clamp the request to what the game actually rendered. When the game's frustum is the
    // narrower one we submit the narrower FOV rather than stretching to fill: a correct
    // tunnel-vision image beats a warped full-width one, and the log says so.
    const float l = std::fmax(std::tan(want.angleLeft),  -tanH);
    const float r = std::fmin(std::tan(want.angleRight),  tanH);
    const float u = std::fmin(std::tan(want.angleUp),     tanV);
    const float d = std::fmax(std::tan(want.angleDown),  -tanV);
    const float u0 = 0.5f + 0.5f * l / tanH, u1 = 0.5f + 0.5f * r / tanH;
    const float v0 = 0.5f - 0.5f * u / tanV, v1 = 0.5f - 0.5f * d / tanV;
    Crop c;
    c.ou = u0; c.su = u1 - u0;
    c.ov = v0; c.sv = v1 - v0;
    c.fov = {std::atan(l), std::atan(r), std::atan(u), std::atan(d)};
    return c;
}

// Tear down GPU-side state. Caller holds g_mtx.
void ReleaseLocked() {
    FramePumpSetGpuQueue(nullptr);
    g_blit.Release();
    g_bound = false;
    g_boundDevice = nullptr;
    g_frameOpen = false;
    g_lastOpenTick = 0;
    std::lock_guard<std::mutex> b(g_batonMtx);
    g_reqActive = false;
    g_repHead = g_repCount = 0;
}

} // namespace

void StereoInit() {
    g_mode.store((int)ParseMode(Cfg().vrStereoMode));
    LOGI("[stereo] mode %s", Cfg().vrStereoMode.c_str());
}

void StereoShutdown() {
    std::lock_guard<std::mutex> g(g_mtx);
    ReleaseLocked();
}

void StereoOnDeviceLost() {
    std::lock_guard<std::mutex> g(g_mtx);
    LOGI("[stereo] device/swapchain lost; releasing GPU state");
    ReleaseLocked();
}

void StereoOnSwapchainResize() {
    std::lock_guard<std::mutex> g(g_mtx);
    g_blit.WaitIdle();   // the quad swapchain is re-created lazily on the next present
}

// ---- baton, game-thread side --------------------------------------------------------
// The eye advances HERE, on the take, so it advances exactly once per game frame no matter
// how far ahead of present the game thread is running.
bool StereoTakeEyeRequest(StereoEyeRequest& out) {
    std::lock_guard<std::mutex> g(g_batonMtx);
    if (!g_reqActive) return false;
    out.eye = g_nextEye;
    out.offset = g_reqOffset[g_nextEye];
    out.rot = g_reqRot[g_nextEye];
    out.fovWrite = g_reqFovWrite;
    out.pair = g_reqPair;
    if ((StereoMode)g_mode.load(std::memory_order_relaxed) == StereoMode::Full) {
        // Full mode: both eyes belong to THIS frame (left in the real pass, right written
        // for the draw-only second pass), so the eye does not alternate across frames.
        out.eye = 0;
        out.offset = g_reqOffset[0];
        out.rot = g_reqRot[0];
        out.haveOther = true;
        out.otherEye = 1;
        out.otherOffset = g_reqOffset[1];
        out.otherRot = g_reqRot[1];
    } else {
        out.haveOther = false;
        g_nextEye ^= 1;
    }
    return true;
}

// Called from every UpdateCamera in the frame; they all carry the frame's latched pair, so
// repeats coalesce onto one queue entry holding the last (latest) camera state.
void StereoReportEye(const StereoEyeReport& r) {
    std::lock_guard<std::mutex> g(g_batonMtx);
    g_lastReportTick = GetTickCount64();
    if (g_repCount > 0) {
        StereoEyeReport& tail = g_repQ[(g_repHead + g_repCount - 1) % kRepQ];
        if (tail.pair == r.pair && tail.eye == r.eye) { tail = r; return; }
    }
    if (g_repCount == kRepQ) { g_repHead = (g_repHead + 1) % kRepQ; --g_repCount; }   // presenting has fallen behind
    g_repQ[(g_repHead + g_repCount) % kRepQ] = r;
    ++g_repCount;
}

// ---- shared pieces of the present path -----------------------------------------------

// Pops the oldest game-frame report, or resets everything when the game thread has gone
// quiet (menu, loading, no player) so we drop back to cinema instead of freezing stereo.
// True while the game thread is actively producing stereo reports (used to tell "mono
// frame" from "stereo pair in flight" without consuming anything from the queue).
bool ReportsLive() {
    std::lock_guard<std::mutex> b(g_batonMtx);
    return g_repCount > 0 && GetTickCount64() - g_lastReportTick <= 500;
}

// Throws away every queued report. Used when the pump stops: the game thread keeps
// reporting cameras even with no second pass, so the queue would otherwise stay
// permanently "live" and mis-phase the first pair after it resumes.
void ResetReports() {
    std::lock_guard<std::mutex> b(g_batonMtx);
    g_repHead = g_repCount = 0;
    g_submitValid[0] = g_submitValid[1] = false;
}

bool PopReport(StereoEyeReport& rep) {
    std::lock_guard<std::mutex> b(g_batonMtx);
    if (GetTickCount64() - g_lastReportTick > 500) {
        g_repHead = g_repCount = 0;
        g_submitValid[0] = g_submitValid[1] = false;
        return false;
    }
    if (g_repCount == 0) return false;
    rep = g_repQ[g_repHead];
    g_repHead = (g_repHead + 1) % kRepQ;
    --g_repCount;
    return true;
}

// Pops the oldest report for `eye`, discarding mismatched reports queued ahead of it. A
// mismatch means the present/report phasing slipped (mode toggle mid-stream, a dropped
// pass); eating the stale head re-synchronises within a frame. The first PresentLag=1
// attempt refused mismatched heads instead - one bad pop put it into a mono path that
// consumed one report per frame while the game pushed two, and the queue never recovered
// (the observed permanent desync storm).
bool PopReportForEye(int eye, StereoEyeReport& rep) {
    for (int guard = 0; guard <= kRepQ; ++guard) {
        if (!PopReport(rep)) return false;
        if ((rep.eye & 1) == eye) return true;
        g_eyeDesync.fetch_add(1, std::memory_order_relaxed);
    }
    return false;
}

// Crops the back buffer to the runtime's per-eye frustum and blits it into that eye's
// swapchain image, echoing the pose/FOV the image was really rendered with into g_submit.
bool BlitEyeFromReport(IVRBackend* vr, const Dx12Frame& f, const BlitParams& bpBase,
                       const StereoEyeReport& rep) {
    const VREyeView& want = g_pairViews[rep.pair % kPairRing][rep.eye & 1];
    if ((want.fov.angleRight - want.fov.angleLeft) < 0.01f) return false;
    if (rep.fovY <= 0.01f) return false;

    const int e = rep.eye & 1;
    const float aspect = f.height ? (float)f.width / (float)f.height : 1.777f;
    float fovY = rep.fovY;
    if (Cfg().vrFovAxis == "horizontal") fovY = 2.f * std::atan(std::tan(fovY * 0.5f) / aspect);
    const Crop cr = MakeCrop(fovY, aspect, want.fov);
    g_cropU.store(cr.su, std::memory_order_relaxed);
    g_cropV.store(cr.sv, std::memory_order_relaxed);
    // Re-logged whenever the rendered FOV really moves, so the moment the widened Cam_Fov
    // takes effect is visible - and so is the case where the write did not stick, which
    // would otherwise look identical to a wrong FovAxis.
    static float s_loggedFov = 0.f;
    if (std::fabs(fovY - s_loggedFov) > 0.02f) {
        s_loggedFov = fovY;
        const float ipd = (want.pose.pos * 2.f).Length();
        LOGI("[stereo] game fov %.1f deg (%s axis, raw %.3f rad) at aspect %.2f, ipd %.1f mm; "
             "runtime wants L%.1f R%.1f U%.1f D%.1f deg -> crop %.0f%%x%.0f%% of %ux%u = "
             "%.0fx%.0f px per eye%s",
             Rad2Deg(fovY), Cfg().vrFovAxis.c_str(), rep.fovY, aspect, ipd * 1000.f,
             Rad2Deg(want.fov.angleLeft), Rad2Deg(want.fov.angleRight),
             Rad2Deg(want.fov.angleUp), Rad2Deg(want.fov.angleDown),
             cr.su * 100.f, cr.sv * 100.f, f.width, f.height,
             cr.su * f.width, cr.sv * f.height,
             (cr.su > 0.995f && cr.sv > 0.995f)
                 ? " -- CLAMPED: the game's frustum is still narrower than the headset's; the "
                   "Cam_Fov write is not reaching the renderer" : "");
    }

    // Size the eye images to the pixels we can actually deliver. Asking for the runtime's
    // recommendation (3072x3264 on a Quest 3) would upscale a ~1100x1440 crop and pay full
    // fill-rate for pixels carrying no extra information.
    const float ss = Clamp(Cfg().vrRenderScale, 0.3f, 2.0f);
    vr->EnsureEyeSwapchains((uint32_t)(cr.su * f.width * ss), (uint32_t)(cr.sv * f.height * ss));

    bool ok = false;
    if (ID3D12Resource* dst = vr->AcquireEyeImage((VREye)e)) {
        BlitParams ep = bpBase;
        ep.uvScaleX = cr.su; ep.uvOffsetX = cr.ou;
        ep.uvScaleY = cr.sv; ep.uvOffsetY = cr.ov;
        ep.dstFormat = vr->EyeFormat();
        if (!g_blit.Copy(f.queue, f.backBuffer, D3D12_RESOURCE_STATE_PRESENT,
                         dst, D3D12_RESOURCE_STATE_RENDER_TARGET, ep))
            g_blitFails.fetch_add(1, std::memory_order_relaxed);
        else {
            g_submit[e].pose.pos = rep.headPos + rep.headRot.Rotate(want.pose.pos);
            g_submit[e].pose.rot = (rep.headRot * want.pose.rot).Normalized();
            g_submit[e].fov = cr.fov;
            g_submitValid[e] = true;
            g_eyeBlits[e].fetch_add(1, std::memory_order_relaxed);
            ok = true;
        }
        vr->ReleaseEyeImage((VREye)e);
    } else {
        g_blitFails.fetch_add(1, std::memory_order_relaxed);
    }
    return ok;
}

// Locates this frame's eye geometry and publishes the request the game thread will latch:
// eye offsets in game convention, the Cam_Fov value that makes one image contain a whole
// eye, and the pair index that ties reports back to this geometry.
void PublishEyeRequest(IVRBackend* vr, const Dx12Frame& f, const VRFrameInfo& fi) {
    VREyeView rel[2];
    if (!vr->LocateEyeOffsets(fi.displayTimeNs, rel)) return;
    const float aspect = f.height ? (float)f.width / (float)f.height : 1.777f;
    float fovWrite = RequiredFovY(rel, aspect);
    if (Cfg().vrFovAxis == "horizontal") fovWrite = 2.f * std::atan(std::tan(fovWrite * 0.5f) * aspect);
    if (Cfg().vrFovDeg > 1.f) fovWrite = Deg2Rad(Cfg().vrFovDeg);   // explicit override
    fovWrite = Clamp(fovWrite, 0.1f, 3.1f);                         // the game's own sanity range

    // Split the eyes symmetrically about the game's camera, using the pair's OWN midpoint
    // rather than trusting the runtime's VIEW space to sit exactly between the eyes.
    const Vec3 mid = (rel[0].pose.pos + rel[1].pose.pos) * 0.5f;
    const float measuredIpd = (rel[1].pose.pos - rel[0].pose.pos).Length();
    float ws = Clamp(Cfg().vrWorldScale, 0.25f, 4.f);
    if (Cfg().vrIpdMm > 40.f && measuredIpd > 1e-4f)
        ws *= (Cfg().vrIpdMm * 0.001f) / measuredIpd;   // override what the runtime reports

    std::lock_guard<std::mutex> b(g_batonMtx);
    for (int i = 0; i < 2; ++i) {
        g_pairViews[fi.pairIndex % kPairRing][i] = rel[i];
        g_reqOffset[i] = ToGame(rel[i].pose.pos - mid) * ws;
        g_reqRot[i] = ToGame(rel[i].pose.rot);
    }
    static bool s_ipdLogged = false;
    if (!s_ipdLogged) {
        s_ipdLogged = true;
        LOGI("[stereo] runtime ipd %.1f mm (eye offsets L%.1f R%.1f mm), VIEW-space centre off by "
             "(%.1f, %.1f, %.1f) mm%s", measuredIpd * 1000.f,
             rel[0].pose.pos.x * 1000.f, rel[1].pose.pos.x * 1000.f,
             mid.x * 1000.f, mid.y * 1000.f, mid.z * 1000.f,
             Cfg().vrIpdMm > 40.f ? " [overridden by [VR] IpdMm]" : "");
    }
    g_reqFovWrite = fovWrite;
    g_reqPair = fi.pairIndex;
    g_reqActive = true;
}

// Blits the whole back buffer onto the head-locked quad (cinema and mono fallback).
bool BlitQuad(IVRBackend* vr, const Dx12Frame& f, BlitParams bp) {
    if (ID3D12Resource* dst = vr->AcquireQuadImage()) {
        bp.dstFormat = vr->QuadFormat();
        if (!g_blit.Copy(f.queue, f.backBuffer, D3D12_RESOURCE_STATE_PRESENT,
                         dst, D3D12_RESOURCE_STATE_RENDER_TARGET, bp))
            g_blitFails.fetch_add(1, std::memory_order_relaxed);
        vr->ReleaseQuadImage();
        return true;
    }
    g_blitFails.fetch_add(1, std::memory_order_relaxed);
    return false;
}

// ---- the HUD layer ---------------------------------------------------------------------
// hud.cpp has already taken the UI out of the frame the eyes are cropped from and left it,
// alone and over transparency, in a texture of its own. All that is left here is to move it
// into the runtime's swapchain and say where in the world it hangs.
//
// Note the ORDER this runs in: the placement fields are filled into the same VRSubmitLayer
// the eyes (or the cinema quad) already own, so one xrEndFrame carries the world and the UI
// together and the compositor blends them at headset resolution rather than at the game's.
BlitParams::Alpha HudAlphaMode(const HudFrame& hf) {
    const std::string& k = Cfg().hudKey;
    if (k == "alpha") return BlitParams::Alpha::Src;
    if (k == "luma") return BlitParams::Alpha::Luma;
    if (k == "max") return BlitParams::Alpha::Max;
    // "dual" - but only when the pair was actually captured. hud.cpp declines to replay the
    // UI draws if any of them depth-tests, and then there is only the one image to key on.
    return hf.texOverWhite ? BlitParams::Alpha::Dual : BlitParams::Alpha::Max;
}

int HudLock() {
    const std::string& l = Cfg().hudLock;
    if (l == "head") return QuadLockHead;
    if (l == "world") return QuadLockWorld;
    return QuadLockBody;
}

void ComposeHud(IVRBackend* vr, const Dx12Frame& f, VRSubmitLayer& layer) {
    HudFrame hf;
    if (!HudTakeFrame(hf)) return;                     // HUD off, or this pass drew no UI
    if (!vr->EnsureHudSwapchain(hf.width, hf.height)) return;
    ID3D12Resource* dst = vr->AcquireHudImage();
    if (!dst) { g_blitFails.fetch_add(1, std::memory_order_relaxed); return; }
    BlitParams bp;
    bp.dstFormat = vr->HudFormat();
    // The capture target is ours and SRV-able, so it is sampled where it lies - no staging
    // copy, and the staging ring stays keyed to the back buffer for the eye blits.
    bp.srcIsShaderResource = true;
    bp.linearizeSrc = Cfg().vrColorFix == "none"
                          ? false
                          : (Cfg().vrColorFix == "srgb" || Dx12Blitter::StorageIsGammaEncoded(hf.format));
    bp.alphaMode = HudAlphaMode(hf);
    bp.src2 = hf.texOverWhite;
    bp.alphaScale = Clamp(Cfg().hudOpacity, 0.f, 1.f);
    const bool ok = g_blit.Copy(f.queue, hf.tex, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                dst, D3D12_RESOURCE_STATE_RENDER_TARGET, bp);
    vr->ReleaseHudImage();
    if (!ok) { g_blitFails.fetch_add(1, std::memory_order_relaxed); return; }

    layer.hudValid = true;
    layer.hudDistanceM = Clamp(Cfg().hudDistance, 0.3f, 20.f);
    layer.hudWidthM = Clamp(Cfg().hudWidth, 0.2f, 40.f);
    layer.hudHeightOffsetM = Cfg().hudHeightOffset;
    layer.hudYawDeg = Cfg().hudYawDeg;
    layer.hudPitchDeg = Cfg().hudPitchDeg;
    layer.hudLock = HudLock();
    layer.hudFollowDeg = Clamp(Cfg().hudFollowDeg, 1.f, 180.f);
    layer.hudFollowRadiusM = Clamp(Cfg().hudFollowRadius, 0.05f, 5.f);
    layer.hudRecenter = HudTakeRecenter();
    g_hudFrames.fetch_add(1, std::memory_order_relaxed);
}

// ---- full stereo: two engine passes, two presents, one XR frame ------------------------
// The frame pump runs a draw-only second engine pass after each real frame; Present is
// synchronous with each pass (measured), so pass 1's present carries the LEFT eye and
// pass 2's the RIGHT. The XR frame opens at pass 1 and closes (submits) at pass 2. Frames
// with no second pass (menus, loading, stereo not requested) close as a cinema quad on the
// next pass-1 present.
// Fade guard: every dangerous world transition (fast travel, death warp, cutscene load)
// fades the screen to black BEFORE tearing the world down - and both fast-travel crashes
// (2026-08-21: a pass-2 DL_PANIC, then a raw INT_OVERFLOW no assert-eater can catch) hit in
// exactly that window, before any other observable signal. So the guard watches the image
// itself: a sharp drop from normal brightness to near-black = fade-out -> suspend pass 2
// until brightness returns (mono while the screen is black is invisible; the player-
// inactive grace takes over during the load screen). A statically dark scene cannot
// trigger it: the HUD alone keeps luma above the threshold, and arming requires the screen
// to have been NORMAL within the last 3 s.
std::atomic<bool> g_fadeGuard{false};

void FadeGuardSample(const Dx12Frame& f) {
    static int s_n = 0;
    if (++s_n % 32) return;   // ~4 samples/s at 144 presents - catches a ~1 s fade early
    uint32_t tagOut = 0; float luma = 0.f;
    if (!Dx12SampleBackbuffer(f.queue, f.backBuffer, 0, &tagOut, &luma)) return;
    static ULONGLONG s_lastNormal = 0;
    const ULONGLONG now = GetTickCount64();
    if (luma > 0.03f) {
        s_lastNormal = now;
        if (g_fadeGuard.exchange(false))
            LOGI("[stereo] fade guard released (luma %.3f) - pass 2 resumes", luma);
        return;
    }
    if (luma < 0.004f && s_lastNormal && now - s_lastNormal < 3000 &&
        !g_fadeGuard.exchange(true))
        LOGI("[stereo] fade-out detected (luma %.4f) - pass 2 suspended until the screen returns",
             luma);
}

bool StereoFadeGuardActive() { return g_fadeGuard.load(std::memory_order_relaxed); }

void FullPresent(IVRBackend* vr, const Dx12Frame& f, const BlitParams& bp) {
    // [VR] FadeGuard, DEFAULT OFF: the luma readback inside the present event is the
    // D3D12Core+0x11f85 crash (2026-08-21: 100% VR startup crash ~3 s in). See config.h.
    if (Cfg().vrFadeGuard) FadeGuardSample(f);
    // ER's renderer may be pipelined one present deep - the image a Present carries can be
    // the PREVIOUS pass's render, in which case eye content is cross-routed and both eyes
    // shimmer (each image is submitted with the other camera's pose). PresentLag selects
    // which pass's present opens the XR frame with the left eye and which closes it with
    // the right:
    //   0: pass 1 opens (its present carries its own render)
    //   1: pass 2 opens (presents lag one pass; verify with [VR] DebugPass2Pitch)
    // Hot-reloads, so the right value can be found live in the headset.
    const int keyLag = DebugKeysPresentLag();
    const int lag = keyLag >= 0 ? keyLag : (Cfg().vrPresentLag != 0 ? 1 : 0);
    const int pass = FramePumpCurrentPass();
    const bool opens = (lag == 0) ? (pass == 1) : (pass == 2);
    const double t0 = NowMs();

    if (opens) {
        if (g_frameOpen) {
            // A stereo pair started (left eye blitted) but its pass 2 never presented -
            // rare stall. Close the dangling XR frame with whatever we have.
            VRSubmitLayer layer;
            // The pair never completed, so there is no stereo to submit - fall back to the
            // flat screen rather than an empty frame. (This used to read a g_quadPending
            // flag that nothing ever set, i.e. it always submitted nothing.)
            if (BlitQuad(vr, f, bp)) layer.quadValid = true;
            layer.quadDistanceM = Cfg().vrCinemaDistance;
            layer.quadWidthM = Cfg().vrCinemaWidth;
            ComposeHud(vr, f, layer);
            if (!vr->EndFrame(layer)) g_submitFails.fetch_add(1, std::memory_order_relaxed);
            else g_frames.fetch_add(1, std::memory_order_relaxed);
            g_frameOpen = false;
        }
        if (g_pumpStalled) {
            g_pumpStalled = false;
            LOGI("[stereo] second pass is back; stereo pairs resume");
        }
        g_lastOpenTick = GetTickCount64();
        VRFrameInfo fi;
        if (!vr->WaitBeginFrame(fi)) return;
        g_openFi = fi;
        PublishEyeRequest(vr, f, fi);

        StereoEyeReport rep;
        bool leftBlitted = false;
        if (fi.shouldRender && PopReportForEye(0, rep))
            leftBlitted = BlitEyeFromReport(vr, f, bp, rep);
        if (leftBlitted) {
            // The pair completes at pass 2's present; hold the XR frame open until then.
            g_frameOpen = true;
        } else {
            // Mono frame (menu, loading, warm-up): begin-blit-end within THIS present,
            // exactly like cinema mode. Holding the frame open until the next present made
            // every quad submission one present late - a ~16 ms head-locked lag that read
            // as jitter the moment the head moved, even in the main menu.
            VRSubmitLayer layer;
            if (fi.shouldRender && BlitQuad(vr, f, bp)) layer.quadValid = true;
            layer.quadDistanceM = Cfg().vrCinemaDistance;
            layer.quadWidthM = Cfg().vrCinemaWidth;
            ComposeHud(vr, f, layer);
            if (!vr->EndFrame(layer)) g_submitFails.fetch_add(1, std::memory_order_relaxed);
            else g_frames.fetch_add(1, std::memory_order_relaxed);
        }
    } else {
        if (!g_frameOpen) {
            // With PresentLag=1 the closing pass is pass 1 - the ONLY pass menus and mono
            // frames ever present from - so the immediate cinema treatment lives here.
            if (lag == 1) {
                // Normally a live report queue means "a pair is in flight, pass 2 will open
                // it" and this present has nothing to do. But if pass 2 has not arrived for
                // kPumpStallMs there is no pair coming - the game has stopped stashing a
                // second-eye camera (a full-screen menu does exactly this) - and continuing
                // to wait submits nothing at all. Fall through to the flat path instead.
                const ULONGLONG now = GetTickCount64();
                const bool pumpStalled = !g_lastOpenTick || now - g_lastOpenTick > kPumpStallMs;
                if (ReportsLive() && !pumpStalled) return;
                if (pumpStalled) {
                    // Stale reports would otherwise keep the queue permanently "live" and
                    // mis-phase the first pair when the pump comes back.
                    ResetReports();
                    if (!g_pumpStalled) {
                        g_pumpStalled = true;
                        LOGI("[stereo] no second pass for %llu ms - the game is not producing "
                             "stereo pairs (menu, cutscene); submitting flat frames until it "
                             "does. The HUD layer keeps going.", now - g_lastOpenTick);
                    }
                    g_monoStalls.fetch_add(1, std::memory_order_relaxed);
                }
                VRFrameInfo fi;
                if (!vr->WaitBeginFrame(fi)) return;
                PublishEyeRequest(vr, f, fi);
                VRSubmitLayer layer;
                if (fi.shouldRender && BlitQuad(vr, f, bp)) layer.quadValid = true;
                layer.quadDistanceM = Cfg().vrCinemaDistance;
                layer.quadWidthM = Cfg().vrCinemaWidth;
                ComposeHud(vr, f, layer);
                if (!vr->EndFrame(layer)) g_submitFails.fetch_add(1, std::memory_order_relaxed);
                else g_frames.fetch_add(1, std::memory_order_relaxed);
                g_lastSubmitMs.store((float)(NowMs() - t0), std::memory_order_relaxed);
            }
            return;
        }
        StereoEyeReport rep;
        if (g_openFi.shouldRender && PopReportForEye(1, rep))
            BlitEyeFromReport(vr, f, bp, rep);
        VRSubmitLayer layer;
        const bool didStereo = g_submitValid[0] && g_submitValid[1];
        if (didStereo) {
            layer.projectionValid = true;
            layer.views[0] = g_submit[0];
            layer.views[1] = g_submit[1];
        } else {
            // Half a pair is not a stereo frame; show the flat screen rather than nothing.
            if (BlitQuad(vr, f, bp)) layer.quadValid = true;
            layer.quadDistanceM = Cfg().vrCinemaDistance;
            layer.quadWidthM = Cfg().vrCinemaWidth;
        }
        ComposeHud(vr, f, layer);
        if (!vr->EndFrame(layer)) g_submitFails.fetch_add(1, std::memory_order_relaxed);
        else {
            g_frames.fetch_add(1, std::memory_order_relaxed);
            if (didStereo) g_stereoFrames.fetch_add(1, std::memory_order_relaxed);
        }
        g_frameOpen = false;
    }
    g_lastSubmitMs.store((float)(NowMs() - t0), std::memory_order_relaxed);
}

// ---- frame loop, render-thread side -------------------------------------------------
void StereoOnPresent(const Dx12Frame& f) {
    const StereoMode mode = (StereoMode)g_mode.load(std::memory_order_relaxed);
    if (mode == StereoMode::Off) return;

    IVRBackend* vr = ActiveBackend();
    if (!vr || !vr->IsReady()) return;

    std::lock_guard<std::mutex> g(g_mtx);

    if (!g_bound || g_boundDevice != f.device) {
        if (g_boundDevice && g_boundDevice != f.device) ReleaseLocked();
        if (!vr->BindGraphics(f.device, f.queue)) return;
        if (!g_blit.Ensure(f.device)) return;
        g_boundDevice = f.device;
        g_bound = true;
        FramePumpSetGpuQueue(f.queue);   // the pump's GpuSync fences this queue between passes
        uint32_t ew = 0, eh = 0;
        vr->GetRecommendedEyeSize(ew, eh);
        LOGI("[stereo] bound: back buffer %ux%u fmt %u, eye target %ux%u",
             f.width, f.height, (unsigned)f.format, ew, eh);
    }

    if (!vr->EnsureQuadSwapchain(f.width, f.height, f.format)) return;

    // There is a live session with graphics bound, so there IS a layer to hang the UI on.
    // hud.cpp only redirects the UI out of the game's frame while this keeps ticking.
    HudNoteSubmitLive();

    // The game's back buffer holds gamma-encoded pixels; every XR swapchain format we pick
    // is read as linear (FLOAT) or hardware-decoded (_SRGB). Let the blit shader convert.
    BlitParams bp;
    bp.linearizeSrc = Cfg().vrColorFix == "none"
                          ? false
                          : (Cfg().vrColorFix == "srgb" || Dx12Blitter::StorageIsGammaEncoded(f.format));

    // Full stereo: two engine passes -> two presents -> one XR frame, in FullPresent.
    if (mode == StereoMode::Full && FramePumpReady()) {
        FullPresent(vr, f, bp);
        return;
    }
    if (mode == StereoMode::Full) {
        static bool s_warned = false;
        if (!s_warned) { s_warned = true; LOGW("[stereo] full: frame pump not armed yet; running debugalt until it is"); }
    }
    const bool wantStereo = mode == StereoMode::DebugAlt || mode == StereoMode::Full;

    const double t0 = NowMs();
    VRFrameInfo fi;
    if (!vr->WaitBeginFrame(fi)) return;   // not running / not visible yet

    VRSubmitLayer layer;
    bool didStereo = false;
    if (wantStereo && fi.shouldRender) {
        PublishEyeRequest(vr, f, fi);
        // What did the game thread render for the frame we are presenting? Oldest first:
        // frames present in the order they were rendered.
        StereoEyeReport rep;
        if (PopReport(rep)) BlitEyeFromReport(vr, f, bp, rep);
        didStereo = g_submitValid[0] && g_submitValid[1];
        if (didStereo) {
            layer.projectionValid = true;
            layer.views[0] = g_submit[0];
            layer.views[1] = g_submit[1];
        }
    } else {
        std::lock_guard<std::mutex> b(g_batonMtx);
        g_reqActive = false;                       // cinema: the game renders mono
        g_repHead = g_repCount = 0;
        g_submitValid[0] = g_submitValid[1] = false;
    }

    // Cinema, and the warm-up frames before both eyes hold content.
    if (!didStereo && fi.shouldRender) {
        if (BlitQuad(vr, f, bp)) layer.quadValid = true;
        layer.quadDistanceM = Cfg().vrCinemaDistance;
        layer.quadWidthM = Cfg().vrCinemaWidth;
    }

    ComposeHud(vr, f, layer);
    if (!vr->EndFrame(layer)) g_submitFails.fetch_add(1, std::memory_order_relaxed);
    else {
        g_frames.fetch_add(1, std::memory_order_relaxed);
        if (didStereo) g_stereoFrames.fetch_add(1, std::memory_order_relaxed);
    }

    g_lastSubmitMs.store((float)(NowMs() - t0), std::memory_order_relaxed);
}

StereoStats StereoGetStats() {
    StereoStats s;
    s.mode = (StereoMode)g_mode.load();
    s.graphicsBound = g_bound;
    s.frames = g_frames.exchange(0);
    s.submitFails = g_submitFails.exchange(0);
    s.blitFails = g_blitFails.exchange(0);
    s.stereoFrames = g_stereoFrames.exchange(0);
    s.eyeBlitL = g_eyeBlits[0].exchange(0);
    s.eyeBlitR = g_eyeBlits[1].exchange(0);
    s.eyeDesync = g_eyeDesync.exchange(0);
    s.hudFrames = g_hudFrames.exchange(0);
    s.monoStalls = g_monoStalls.exchange(0);
    s.lastSubmitMs = g_lastSubmitMs.load();
    s.cropU = g_cropU.load();
    s.cropV = g_cropV.load();
    return s;
}
}
