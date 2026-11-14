#include "hud.h"
#include "hud_target.h"
#include "config.h"
#include "dx12_blit.h"
#include "game_er.h"
#include "patches_er.h"
#include "reshade_addon.h"
#include "vrmath.h"
#include "log.h"
#include <windows.h>
#include <d3d12.h>
#include <atomic>
#include <mutex>
#include <reshade.hpp>

namespace ervr {
namespace {

namespace rs = reshade::api;

// ---- per-command-list state ------------------------------------------------------------
// A D3D12 command list carries its own binding state, and ELDEN RING records on more than
// one. ReShade's private-data store hangs a struct off each list object, which is both
// lock-free and correct when two lists are recorded at once - a shared "currently bound
// render target" global would mix them up and redirect the wrong draws.
struct __declspec(uuid("6E9A1C40-0E4E-4C2F-9E11-6B4B2A7C51D2")) CmdState {
    rs::resource      rt{};      // resource behind render target 0, as the GAME bound it
    rs::resource_view dsv{};     // whatever depth target came with it, passed through
    bool              ours = false;   // our capture RTV is bound on this list right now
    bool              foreign = false;   // not a D3D12 list: never watched (see NewState)
};

// Only D3D12 lists are watched. The VR runtime and overlays make D3D11 devices in the game's
// process (VDXR and Meta Quest Link as their sessions start), and ReShade raises the same
// bind and draw events for their contexts. Census entries from one of those are D3D11
// resources, and a D3D11 list once set the device the census is read with. On 2026-11-12
// (0.8.0, VDXR, Radeon 7900 XT) RelearnTarget asked the D3D11 device about the game's D3D12
// targets: D3D11's GetType is vtable slot 7, which on an ID3D12Resource is GetDevice(riid,
// ppv), and with ppv aliasing the resource the failed QueryInterface wrote null over its
// vtable pointer. The next relearn crashed in ReShade at dxgi.dll+0x1248cb reading 0x38.
CmdState* NewState(rs::command_list* cl) {
    CmdState* st = cl->create_private_data<CmdState>();
    st->foreign = cl->get_device()->get_api() != rs::device_api::d3d12;
    return st;
}

CmdState* StateOf(rs::command_list* cl) {
    CmdState* st = cl->get_private_data<CmdState>();
    // ERVR registers as an add-on after the game is already running, so init_command_list
    // never fires for the lists that already exist; create on first sight instead.
    if (!st) st = NewState(cl);
    return st->foreign ? nullptr : st;
}

// ---- the capture target ----------------------------------------------------------------
// The UI is captured TWICE per frame: [0] over a transparent-black clear, [1] over an
// opaque-white one. Coverage is then a = 1 - ([1] - [0]) exactly, for any src-over blend -
// see BlitParams::Alpha::Dual. It is the only way to get a coverage mask out of ELDEN RING,
// whose UI pipelines never write the alpha channel: keyed on brightness instead, every
// black menu panel and every text outline came out fully transparent.
struct Capture {
    rs::device*       dev = nullptr;
    rs::resource      tex[2]{};
    rs::resource_view rtv[2]{};
    uint32_t          w = 0, h = 0;
    rs::format        fmt = rs::format::unknown;
    bool              pair = false;   // [1] exists, so the dual reconstruction is available

    void Destroy() {
        if (dev)
            for (int i = 0; i < 2; ++i) {
                if (rtv[i].handle) dev->destroy_resource_view(rtv[i]);
                if (tex[i].handle) dev->destroy_resource(tex[i]);
            }
        for (int i = 0; i < 2; ++i) { tex[i] = {}; rtv[i] = {}; }
        w = h = 0; fmt = rs::format::unknown; pair = false; dev = nullptr;
    }
};

std::mutex  g_mtx;              // guards the capture target and the census
Capture     g_cap;
bool        g_events = false;

// The swapchain's back buffers. The default detection target: the frame's final image.
constexpr int kMaxBb = 8;
uint64_t      g_bb[kMaxBb]{};
// Written under g_mtx, read lock-free from the draw hook. The count is published with a
// release store AFTER the handles are in place, so a reader can never walk a half-filled
// array - the draw hook runs thousands of times a frame and is not going to take a lock
// for this.
std::atomic<int> g_bbCount{0};
uint32_t      g_bbW = 0, g_bbH = 0;

// Which resource this pass counts draws into.
enum class TargetMode { BackBuffer, Learned };
std::atomic<int>      g_targetMode{(int)TargetMode::BackBuffer};
std::atomic<uint64_t> g_learned{0};       // Learned mode: the resource, from last pass's census
std::atomic<int>      g_targetDraws{0};   // draws into the target so far THIS pass
std::atomic<bool>     g_captured{false};  // a UI draw has been redirected this pass
std::atomic<int>      g_missStreak{0};    // consecutive passes with no UI draws

std::atomic<int>  g_enabledOverride{-1};  // Numpad 7
std::atomic<int>  g_firstDrawOverride{-1};// Numpad 8
// HudEnabled() compares ini strings, and the draw hook runs thousands of times a frame -
// so the draw path reads these instead, refreshed once per present.
//   g_active   - run the hook body at all: count draws, and record the census for [HUD] Probe
//   g_redirect - actually swap the render target out from under the UI
// They are separate so that the PROBE still works when there is no VR session to submit a
// layer into. Diagnosing "the menu never reaches the panel" needs the census from a flat,
// unattended run, and it would be unavailable if standing down also switched off counting.
std::atomic<bool> g_active{false};
std::atomic<bool> g_redirect{false};
std::atomic<rs::device*> g_dev{nullptr};
std::atomic<long> g_captures{0}, g_redirects{0}, g_misses{0};
std::atomic<int>  g_lastDraws{0};
std::atomic<bool> g_announced{false};
// Did any UI-phase draw arrive with a depth-stencil view bound? Reported by [HUD] Probe as
// information about the UI pass; it does NOT gate the dual capture. ELDEN RING does leave a
// DSV bound for its UI draws, so an earlier version that refused to replay whenever it saw
// one disabled the exact-coverage path permanently. What actually matters is whether those
// draws WRITE depth - which cannot be observed from here, so it is settled by looking:
// [HUD] MirrorPanel puts the reconstructed panel on the monitor, and a replay that lost its
// fight with the depth test shows up there instantly as a black silhouette.
std::atomic<bool> g_targetHasDsv{false};
// Composites the panel back onto the game's frame for MirrorPanel.
Dx12Blitter g_mirror;
std::atomic<bool> g_dualUsed{false};   // the pair really was captured this pass
std::atomic<bool> g_recenter{false};
// Last tick at which the stereo path had a live session to submit a layer into.
std::atomic<unsigned long long> g_lastLive{0};
// Last tick at which an XR frame went out as a stereo pair (a projection layer).
std::atomic<unsigned long long> g_lastStereo{0};

// The UI is only worth taking out of the frame if something is going to put it back on
// screen. One second of grace, so a dropped frame or a session state blip does not flicker
// the HUD between the panel and the frame.
bool SubmitLive() {
    if (Cfg().hudRedirectWithoutVR) return true;   // test switch, see config.h
    const unsigned long long t = g_lastLive.load(std::memory_order_relaxed);
    return t && GetTickCount64() - t <= 1000;
}

// ...and only while the eyes are crops of a stereo frame. A flat frame goes onto the cinema
// screen WHOLE, its UI already where it belongs - and at the title screen, the boot screens
// and the loading screens that UI is the entire picture: the title is 7 draws onto the UI
// target over a black clear, nothing of it world. Lifted out there, the frame is pure black
// (measured flat, 2026-10-31) and the picture rests on the HUD layer alone; that day three
// VR boots in a row showed the player nothing but black at the title until they quit. Half a
// second of grace: pairs run at 40+ Hz, so a gap that long - on top of the pump's own 120 ms
// stall test - is a real flat stretch (the map, a load, a cutscene), not a hitch.
constexpr unsigned long long kStereoHoldMs = 500;
bool StereoLive() {
    if (Cfg().hudRedirectWithoutVR) return true;   // test switch, see config.h
    const unsigned long long t = g_lastStereo.load(std::memory_order_relaxed);
    return t && GetTickCount64() - t <= kStereoHoldMs;
}

// ---- the render-target census ----------------------------------------------------------
// One entry per contiguous run of draws that shared a render target, in submission order.
// This is what [HUD] Probe prints and what "learn the last multi-draw target" reads; it is
// also the only way to answer "why did nothing get captured" without a GPU debugger.
// Passes with no UI found before the target is re-derived. ~1 s at a 60 Hz pair rate: long
// enough that a genuinely HUD-less stretch (a loading screen, a cutscene) does not thrash it.
constexpr int kRelearnAfter = 60;
// A RING of the last kMaxRuns runs, not the first: the UI is at the END of the frame, so a
// buffer that fills up and stops recording drops the only part worth looking at. (It did,
// on the first menu investigation - 48 runs of scene setup and nothing else.)
constexpr int kMaxRuns = 32;
struct Run { uint64_t res; int draws; };
Run g_runs[kMaxRuns]{};
int g_runCount = 0;      // runs held, <= kMaxRuns
int g_runHead = 0;       // index of the oldest held run
int g_runTotal = 0;      // runs seen this frame, including those rotated out
int g_drawTotal = 0;     // draws seen this frame, all targets

Run& RunAt(int i) { return g_runs[(g_runHead + i) % kMaxRuns]; }

bool IsBackBuffer(uint64_t h) {
    const int n = g_bbCount.load(std::memory_order_acquire);
    for (int i = 0; i < n; ++i) if (g_bb[i] == h) return true;
    return false;
}

int FirstDrawCfg() {
    const int ov = g_firstDrawOverride.load(std::memory_order_relaxed);
    if (ov >= 0) return ov;
    return Cfg().hudFirstDraw < 0 ? 0 : Cfg().hudFirstDraw;
}

// The dual capture replays every UI draw into the second target, which is only safe while
// nothing depth-tests: a replayed depth-writing draw would win the test and make the real
// one fail. Measured on 1.16.2 - 112 of 112 frames reported no DSV bound for any UI draw -
// but it is checked at runtime rather than assumed, and falls back to the brightness key.
bool WantDual() { return Cfg().hudKey == "dual"; }

bool IsTarget(uint64_t res) {
    if (!res) return false;
    return (TargetMode)g_targetMode.load(std::memory_order_relaxed) == TargetMode::BackBuffer
               ? IsBackBuffer(res)
               : res == g_learned.load(std::memory_order_relaxed);
}

// Creates (or resizes) the texture the UI is redirected into. Format and size are copied
// from the target being replaced, because the game's UI pipelines were created against
// THAT format - handing OMSetRenderTargets a different one is undefined behaviour.
bool EnsureCapture(rs::device* dev, rs::resource target) {
    const rs::resource_desc td = dev->get_resource_desc(target);
    if (td.type != rs::resource_type::texture_2d || !td.texture.width || !td.texture.height)
        return false;
    const bool wantPair = WantDual();
    if (g_cap.tex[0].handle && g_cap.w == td.texture.width && g_cap.h == td.texture.height &&
        g_cap.fmt == td.texture.format && g_cap.pair == wantPair)
        return true;

    g_cap.Destroy();
    rs::resource_desc rd(td.texture.width, td.texture.height, 1, 1, td.texture.format, 1,
                         rs::memory_heap::gpu_only,
                         rs::resource_usage::render_target | rs::resource_usage::shader_resource |
                             rs::resource_usage::copy_source);
    g_cap.dev = dev;
    for (int i = 0; i < (wantPair ? 2 : 1); ++i) {
        if (!dev->create_resource(rd, nullptr, rs::resource_usage::render_target, &g_cap.tex[i]) ||
            !dev->create_resource_view(g_cap.tex[i], rs::resource_usage::render_target,
                                       rs::resource_view_desc(td.texture.format), &g_cap.rtv[i])) {
            LOGE("[hud] could not create capture target %d (%ux%u fmt %u)", i,
                 td.texture.width, td.texture.height, (unsigned)td.texture.format);
            g_cap.Destroy();
            return false;
        }
    }
    g_cap.w = td.texture.width;
    g_cap.h = td.texture.height;
    g_cap.fmt = td.texture.format;
    g_cap.pair = wantPair;
    LOGI("[hud] capture target%s %ux%u fmt %u ready - the UI now renders here instead of the "
         "frame%s", wantPair ? "s (over black + over white)" : "", g_cap.w, g_cap.h,
         (unsigned)g_cap.fmt,
         wantPair ? "; coverage is reconstructed from the pair" : " (key: brightness only)");
    return true;
}

void RecordRun(uint64_t res) {
    ++g_drawTotal;
    if (g_runCount && RunAt(g_runCount - 1).res == res) { ++RunAt(g_runCount - 1).draws; return; }
    ++g_runTotal;
    if (g_runCount < kMaxRuns) {
        RunAt(g_runCount) = {res, 1};
        ++g_runCount;
    } else {                                  // rotate the oldest out, keep the tail
        g_runs[g_runHead] = {res, 1};
        g_runHead = (g_runHead + 1) % kMaxRuns;
    }
}

// One draw, in the form needed to issue it a second time. All other state - pipeline, root
// arguments, vertex and index buffers, viewport, scissor - is already bound and unchanged,
// so replaying is just the draw call itself against a different render target.
struct DrawCall {
    int      kind = 0;          // 0 = draw, 1 = draw_indexed, 2 = indirect
    uint32_t a = 0, b = 0, c = 0, e = 0;
    int32_t  d = 0;
    rs::indirect_command itype{};
    rs::resource buffer{};
    uint64_t offset = 0;
};

void Replay(rs::command_list* cl, const DrawCall& dc) {
    switch (dc.kind) {
    case 0: cl->draw(dc.a, dc.b, dc.c, dc.e); break;
    case 1: cl->draw_indexed(dc.a, dc.b, dc.c, dc.d, dc.e); break;
    default: cl->draw_or_dispatch_indirect(dc.itype, dc.buffer, dc.offset, dc.a, dc.b); break;
    }
}

// The heart of it: called for every draw the game issues.
void NoteDraw(rs::command_list* cl, CmdState* st, const DrawCall& dc) {
    const uint64_t res = st->rt.handle;
    if (!res) return;

    if (Cfg().hudProbe || (TargetMode)g_targetMode.load(std::memory_order_relaxed) == TargetMode::Learned ||
        g_missStreak.load(std::memory_order_relaxed) > 0) {
        std::lock_guard<std::mutex> g(g_mtx);
        RecordRun(res);
    }
    if (!IsTarget(res)) return;

    const int n = g_targetDraws.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= FirstDrawCfg()) return;   // still the frame's own resolve/tonemap draws
    if (st->dsv.handle) g_targetHasDsv.store(true, std::memory_order_relaxed);
    if (!g_redirect.load(std::memory_order_relaxed)) return;   // counting/census only

    std::lock_guard<std::mutex> g(g_mtx);
    if (!EnsureCapture(cl->get_device(), st->rt)) return;
    if (!g_captured.exchange(true)) {
        // First UI draw of the pass. The two clears are the two known backgrounds the
        // coverage is solved against; without the pair, just the transparent one.
        const float black[4] = {0.f, 0.f, 0.f, 0.f};
        cl->clear_render_target_view(g_cap.rtv[0], black);
        if (g_cap.pair) {
            const float white[4] = {1.f, 1.f, 1.f, 1.f};
            cl->clear_render_target_view(g_cap.rtv[1], white);
        }
    }
    if (g_cap.pair) {
        g_dualUsed.store(true, std::memory_order_relaxed);
        // Draw it into the white target first, then leave the black one bound for the
        // game's own draw. Every draw needs this, so the "already redirected" shortcut
        // below does not apply here.
        cl->bind_render_targets_and_depth_stencil(1, &g_cap.rtv[1], st->dsv);
        Replay(cl, dc);
        cl->bind_render_targets_and_depth_stencil(1, &g_cap.rtv[0], st->dsv);
        st->ours = true;
        g_redirects.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (st->ours) return;              // already redirected, nothing to rebind
    cl->bind_render_targets_and_depth_stencil(1, &g_cap.rtv[0], st->dsv);
    st->ours = true;
    g_redirects.fetch_add(1, std::memory_order_relaxed);
}

// ---- ReShade callbacks -----------------------------------------------------------------

void OnInitCmdList(rs::command_list* cl) { NewState(cl); }
void OnDestroyCmdList(rs::command_list* cl) { cl->destroy_private_data<CmdState>(); }
void OnResetCmdList(rs::command_list* cl) {
    CmdState* st = StateOf(cl);
    if (st) *st = CmdState{};
}

void OnBindRenderTargets(rs::command_list* cl, uint32_t count, const rs::resource_view* rtvs,
                         rs::resource_view dsv) {
    CmdState* st = StateOf(cl);
    if (!st) return;
    st->ours = false;    // the game just took its own target back
    st->dsv = dsv;
    // g_dev is NOT taken from here: it is the swapchain's device, set where the back buffers
    // are learned. Any list's device used to land in it, a foreign one included (NewState).
    rs::device* dev = cl->get_device();
    st->rt = (count && rtvs && rtvs[0].handle) ? dev->get_resource_from_view(rtvs[0])
                                               : rs::resource{};
}

bool OnBeginRenderPass(rs::command_list* cl, uint32_t count, const rs::render_pass_render_target_desc* rts,
                       const rs::render_pass_depth_stencil_desc* ds, rs::render_pass_flags) {
    CmdState* st = StateOf(cl);
    if (st) {
        st->ours = false;
        st->dsv = ds ? ds->view : rs::resource_view{};
        st->rt = (count && rts && rts[0].view.handle)
                     ? cl->get_device()->get_resource_from_view(rts[0].view)
                     : rs::resource{};
    }
    return false;
}

bool OnDraw(rs::command_list* cl, uint32_t verts, uint32_t insts, uint32_t v0, uint32_t i0) {
    if (!g_active.load(std::memory_order_relaxed)) return false;
    if (CmdState* st = StateOf(cl)) NoteDraw(cl, st, DrawCall{0, verts, insts, v0, i0});
    return false;
}
bool OnDrawIndexed(rs::command_list* cl, uint32_t idx, uint32_t insts, uint32_t i0, int32_t v0,
                   uint32_t inst0) {
    if (!g_active.load(std::memory_order_relaxed)) return false;
    if (CmdState* st = StateOf(cl)) NoteDraw(cl, st, DrawCall{1, idx, insts, i0, inst0, v0});
    return false;
}
bool OnDrawIndirect(rs::command_list* cl, rs::indirect_command type, rs::resource buffer,
                    uint64_t offset, uint32_t count, uint32_t stride) {
    if (type == rs::indirect_command::dispatch || type == rs::indirect_command::dispatch_mesh ||
        type == rs::indirect_command::dispatch_rays)
        return false;
    if (!g_active.load(std::memory_order_relaxed)) return false;
    DrawCall dc{2, count, stride};
    dc.itype = type; dc.buffer = buffer; dc.offset = offset;
    if (CmdState* st = StateOf(cl)) NoteDraw(cl, st, dc);
    return false;
}

// Caller holds g_mtx.
void LearnBackBuffersLocked(rs::swapchain* sc) {
    rs::device* dev = sc->get_device();
    if (dev->get_api() != rs::device_api::d3d12) return;
    g_dev.store(dev, std::memory_order_relaxed);
    g_bbCount.store(0, std::memory_order_release);
    int count = 0;
    const uint32_t n = sc->get_back_buffer_count();
    for (uint32_t i = 0; i < n && count < kMaxBb; ++i) {
        const rs::resource bb = sc->get_back_buffer(i);
        if (!bb.handle) continue;
        g_bb[count++] = bb.handle;
        if (i == 0) {
            const rs::resource_desc d = dev->get_resource_desc(bb);
            g_bbW = d.texture.width;
            g_bbH = d.texture.height;
        }
    }
    g_bbCount.store(count, std::memory_order_release);
    LOGI("[hud] tracking %d back buffers (%ux%u)", count, g_bbW, g_bbH);
    // Off 16:9 (and no letterbox patch) the game renders a 16:9 frame INSIDE the back buffer,
    // so every target - the UI's included - is a few px narrower or shorter than it and
    // RelearnTarget's full-size test never matches; the bar draws then pass for UI and the
    // real UI stays in the eye images (2026-10-31: a 3632x2040 window rendered at 3628x2040,
    // the menu outside both eye crops, the map magnified). Say so: it is invisible otherwise.
    static uint32_t s_warnedW = 0, s_warnedH = 0;
    if (g_bbH && (uint64_t)g_bbW * 9 != (uint64_t)g_bbH * 16 && !PatchesLetterboxRemoved() &&
        (g_bbW != s_warnedW || g_bbH != s_warnedH)) {
        s_warnedW = g_bbW; s_warnedH = g_bbH;
        LOGW("[hud] back buffer %ux%u is not exactly 16:9: the game renders a 16:9 frame inside "
             "it with black bars, and the HUD layer cannot find that frame's UI - menus and HUD "
             "stay in the eye images. Use [VR] GameRes=auto or an exact 16:9 size (3200x1800).",
             g_bbW, g_bbH);
    }
}

void OnInitSwapchainHud(rs::swapchain* sc, bool) {
    std::lock_guard<std::mutex> g(g_mtx);
    LearnBackBuffersLocked(sc);
    // The old capture target may be the wrong size now; it is rebuilt on the next UI draw.
    g_cap.Destroy();
}

void OnDestroySwapchainHud(rs::swapchain*, bool) {
    std::lock_guard<std::mutex> g(g_mtx);
    g_bbCount.store(0, std::memory_order_release);
    g_cap.Destroy();
}

// Once a second while [HUD] Probe is on: the frame's render-target census. Reading it is
// how FirstDraw gets picked - find the run that owns the final image and count how many
// draws land on it before the UI starts.
void ProbeLog() {
    static ULONGLONG s_last = 0;
    const ULONGLONG now = GetTickCount64();
    if (now - s_last < 1000) return;
    s_last = now;
    rs::device* dev = g_dev.load(std::memory_order_relaxed);
    char line[1600];
    int off = 0;
    for (int i = 0; i < g_runCount && off < (int)sizeof(line) - 96; ++i) {
        const Run& r = RunAt(i);
        char what[48] = "BB";
        if (!IsBackBuffer(r.res)) {
            // Size and format are what separate a UI target from a half-res post-process
            // one, and they are the first thing anyone reading this log needs.
            what[0] = 0;
            if (dev) {
                const rs::resource_desc d = dev->get_resource_desc(rs::resource{r.res});
                _snprintf_s(what, sizeof(what), _TRUNCATE, "%ux%u/%u",
                            d.texture.width, d.texture.height, (unsigned)d.texture.format);
            }
        }
        off += _snprintf_s(line + off, sizeof(line) - off, _TRUNCATE, "%s%llx[%s]:%d",
                           i ? " " : "", (unsigned long long)(r.res & 0xffffffull), what, r.draws);
    }
    line[off < 0 ? 0 : off] = 0;
    LOGI("[hud] census: %d draws in %d runs (last %d shown), target=%s, firstDraw=%d, "
         "drawsIntoTarget=%d, uiDsv=%d | %s",
         g_drawTotal, g_runTotal, g_runCount,
         (TargetMode)g_targetMode.load() == TargetMode::BackBuffer ? "back buffer" : "learned",
         FirstDrawCfg(), g_lastDraws.load(),
         (int)g_targetHasDsv.load(std::memory_order_relaxed), line);
}

// Nothing has been captured for a while: work out which render target the UI is on now.
//
// This is not a fallback for a broken guess - the answer genuinely CHANGES during a session.
// Measured on 1.16.2 (2026-08-21): the title screen and the boot logos paint their UI
// straight onto the back buffer (3 draws: the output blit plus two UI batches), but once a
// save is loaded the engine tonemaps into a full-size offscreen R8G8B8A8 target, draws ~55
// HUD/Scaleform batches onto THAT, and only then copies the result to the back buffer - one
// single draw. So the target moves when a save loads, and moves back on quit-to-title.
//
// The rule: scan the frame's census backwards for the last full-size target that took more
// than one draw and is in a format a UI can be on (HudPickUiTarget). A back buffer is
// accepted as readily as an offscreen one, so this can walk back as well as forward.
void RelearnTarget(rs::device* dev) {
    // The census holds D3D12 handles only; another API's get_resource_desc on one corrupts it.
    if (!dev || dev->get_api() != rs::device_api::d3d12) return;
    const int want = FirstDrawCfg() + 1 > 2 ? FirstDrawCfg() + 1 : 2;
    HudRun runs[kMaxRuns];
    for (int i = 0; i < g_runCount; ++i) {
        HudRun& r = runs[i];
        r.res = RunAt(i).res;
        r.draws = RunAt(i).draws;
        r.backBuffer = IsBackBuffer(r.res);
        if (r.backBuffer || r.draws < want) continue;
        const rs::resource_desc d = dev->get_resource_desc(rs::resource{r.res});
        r.texture2d = d.type == rs::resource_type::texture_2d;
        r.w = d.texture.width;
        r.h = d.texture.height;
        r.fmt = (uint32_t)d.texture.format;
    }
    int rejected = -1;
    const int i = HudPickUiTarget(runs, g_runCount, want, g_bbW, g_bbH, &rejected);
    static uint64_t s_toldRejected = 0;
    if (rejected >= 0 && runs[rejected].res != s_toldRejected) {
        s_toldRejected = runs[rejected].res;
        LOGI("[hud] passed over a full-size fmt %u target (@%llx, %d draws) while looking for "
             "the UI: a scene buffer, not a UI target", runs[rejected].fmt,
             (unsigned long long)(runs[rejected].res & 0xffffffull), runs[rejected].draws);
    }
    if (i < 0) return;
    const uint64_t res = runs[i].res;
    const bool bb = runs[i].backBuffer;
    const int mode = bb ? (int)TargetMode::BackBuffer : (int)TargetMode::Learned;
    const int prevMode = g_targetMode.exchange(mode);
    const uint64_t prevRes = g_learned.exchange(bb ? 0 : res);
    if (prevMode != mode || prevRes != (bb ? 0 : res))
        LOGI("[hud] UI target moved to %s (@%llx, fmt %u, %d draws in that frame) after %d "
             "passes with no UI. Set [HUD] Target=backbuffer to pin the old behaviour, or "
             "Probe=true to watch the whole census.",
             bb ? "the back buffer" : "a full-size offscreen target",
             (unsigned long long)(res & 0xffffffull), runs[i].fmt, runs[i].draws,
             g_missStreak.load(std::memory_order_relaxed));
}

} // namespace

bool HudEnabled() {
    const int ov = g_enabledOverride.load(std::memory_order_relaxed);
    if (ov >= 0) return ov != 0;
    return Cfg().hudMode == "quad" && Cfg().vrStereoMode != "off";
}

void HudSetEnabledOverride(int on) {
    if (on > 0 && !g_events) {
        // The draw-stream hooks are only installed when the ini asked for the HUD layer at
        // startup - see HudRegisterEvents - so there is nothing to turn on here.
        LOGW("[hud] cannot enable the HUD layer this session: it was off in ERVR.ini when the "
             "game started, so the draw hooks were never installed. Set [HUD] Mode=quad and "
             "restart.");
        return;
    }
    g_enabledOverride.store(on);
}
int  HudGetEnabledOverride() { return g_enabledOverride.load(); }
int  HudFirstDraw() { return FirstDrawCfg(); }

void HudStepFirstDraw(int delta) {
    int v = FirstDrawCfg() + delta;
    if (v < 0) v = 0;
    if (v > 16) v = 16;
    g_firstDrawOverride.store(v);
    LOGI("[hud] FirstDraw -> %d (draws into the frame's final target that stay in the frame; "
         "everything after them goes to the HUD quad - bake the winner into [HUD] FirstDraw)", v);
}

// Composites this pass's panel back onto the game's own frame: same reconstruction, same
// premultiplied blend the compositor performs - so what lands on the monitor is what the
// headset will show, and a screenshot becomes a regression test for the whole capture.
void HudMirrorPanel(const Dx12Frame& f) {
    if (!g_events || !Cfg().hudMirrorPanel) return;
    HudFrame hf;
    if (!HudTakeFrame(hf)) return;
    if (!g_mirror.Ensure(f.device)) return;
    BlitParams bp;
    bp.dstFormat = f.format;
    bp.srcIsShaderResource = true;
    bp.src2 = hf.texOverWhite;
    bp.blendOver = true;
    // Capture and destination are both the game's own targets in the same encoding, so
    // there is no colour conversion here - unlike the trip into an XR swapchain.
    bp.linearizeSrc = false;
    bp.alphaMode = hf.texOverWhite ? BlitParams::Alpha::Dual : BlitParams::Alpha::Max;
    bp.alphaScale = Clamp(Cfg().hudOpacity, 0.f, 1.f);
    g_mirror.Copy(f.queue, hf.tex, D3D12_RESOURCE_STATE_RENDER_TARGET,
                  f.backBuffer, D3D12_RESOURCE_STATE_PRESENT, bp);
}

void HudNoteSubmitLive() { g_lastLive.store(GetTickCount64(), std::memory_order_relaxed); }
void HudNoteStereoFrame() { g_lastStereo.store(GetTickCount64(), std::memory_order_relaxed); }

void HudRequestRecenter() { g_recenter.store(true); LOGI("[hud] panel re-centred on the current view"); }
bool HudTakeRecenter() { return g_recenter.exchange(false); }

void HudOnPresent(void* reshadeSwapchain) {
    if (!g_events) return;    // hooks never installed: nothing to close, nothing to arm
    const bool want = HudEnabled(), live = SubmitLive(), stereo = StereoLive();
    // The setting this pass's draws ran under (armed by the previous present), before it moves.
    const bool wasRedirecting = g_redirect.load(std::memory_order_relaxed);
    g_active.store(want, std::memory_order_relaxed);
    g_redirect.store(want && live && stereo, std::memory_order_relaxed);
    if (want) {
        static bool s_live = true;
        if (s_live != live) {
            s_live = live;
            LOGI("[hud] %s", live
                 ? "VR session live again - the UI goes onto its own layer whenever the eyes are stereo"
                 : "no VR session to submit a layer into (headset not streaming?) - leaving "
                   "the UI in the frame, where it is at least visible");
        }
        // Flips with every map, load and cutscene, so only the first few say so out loud.
        static int s_stereo = -1, s_told = 0;
        if (live && s_stereo != (int)stereo) {
            s_stereo = (int)stereo;
            Log(s_told++ < 4 ? LogLevel::Info : LogLevel::Debug, "[hud] %s", stereo
                ? "stereo frames: the UI goes onto its own layer"
                : "flat frames (title, loading, map, cutscene): the UI stays in the frame, which "
                  "the cinema screen shows whole");
        }
    }
    if (auto* sc = reinterpret_cast<rs::swapchain*>(reshadeSwapchain)) {
        std::lock_guard<std::mutex> g(g_mtx);
        static rs::swapchain* s_known = nullptr;
        if (!g_bbCount.load(std::memory_order_relaxed) || s_known != sc) {
            s_known = sc;
            LearnBackBuffersLocked(sc);
        }
    }
    const bool captured = g_captured.exchange(false);
    g_dualUsed.store(false, std::memory_order_relaxed);
    const int draws = g_targetDraws.exchange(0);
    g_lastDraws.store(draws, std::memory_order_relaxed);

    if (!(want && live)) {
        // Standing down (feature off, or nothing to submit a layer into) is not a detection
        // failure - counting it as one raised the "no UI draw found" alarm on every flat run.
        g_missStreak.store(0, std::memory_order_relaxed);
        std::lock_guard<std::mutex> g(g_mtx);
        // The census still prints: a flat unattended run is exactly where the render-target
        // question gets investigated, and it would be useless if standing down muted it.
        if (Cfg().hudProbe) ProbeLog();
        g_runCount = g_runHead = g_runTotal = g_drawTotal = 0;
        return;
    }
    // Flat frames leave the UI where it is but still watch for it, so the target keeps
    // following the game (it moves when a save loads) and the first stereo pair after a load
    // or the map lifts the HUD at once instead of after a relearn.
    const bool found = captured || (!wasRedirecting && draws > FirstDrawCfg());
    const bool absent = !found && HudUiAbsentNotMoved(
        (TargetMode)g_targetMode.load(std::memory_order_relaxed) == TargetMode::Learned, draws);
    static int s_absent = 0, s_toldAbsent = 0;
    if (!absent) {
        s_absent = 0;
    } else if (++s_absent == kRelearnAfter) {
        // Every cutscene does this, so only the first few say so out loud.
        Log(s_toldAbsent++ < 4 ? LogLevel::Info : LogLevel::Debug,
            "[hud] %d passes with no UI, but the frame still lands on the UI target (a "
            "cutscene, or the HUD hidden): target kept", kRelearnAfter);
    }
    if (found || absent) {
        g_missStreak.store(0, std::memory_order_relaxed);
        if (captured) {
            g_captures.fetch_add(1, std::memory_order_relaxed);
            if (!g_announced.exchange(true))
                LOGI("[hud] UI redirected onto its own layer (%d draws into the frame's final "
                     "target, the first %d kept)", draws, FirstDrawCfg());
        }
    } else {
        g_misses.fetch_add(1, std::memory_order_relaxed);
        const int streak = g_missStreak.fetch_add(1, std::memory_order_relaxed) + 1;
        // A few empty passes are normal (loading screens, the HUD faded out). A sustained
        // run means detection is wrong, so say so once, loudly, with the way out.
        if (streak == 240)
            LOGW("[hud] 240 passes with no UI draw found (%d draws into the frame's final "
                 "target, FirstDraw=%d, Target=%s). Turn on [HUD] Probe to see the "
                 "render-target census, step FirstDraw with Numpad 8, or set [HUD] Mode=off.",
                 draws, FirstDrawCfg(), Cfg().hudTarget.c_str());
    }

    std::lock_guard<std::mutex> g(g_mtx);
    if (Cfg().hudProbe) ProbeLog();
    // Re-derive the target after a second or so of silence, then again every second while it
    // stays silent - never just once, and never only in one direction: the UI moves off the
    // back buffer when a save loads and back onto it on quit-to-title.
    const int streak = g_missStreak.load(std::memory_order_relaxed);
    if (Cfg().hudTarget == "auto" && streak >= kRelearnAfter && streak % kRelearnAfter == 0) {
        // Not while a loading screen or a cutscene/menu camera is up: the UI is absent then, not moved, and the
        // frame's last full-size multi-draw targets are the scene's. Both boss cinematics that broke the view
        // (2026-11-01, 2026-11-07: "normal maps everywhere until I died") relearned with the loading byte up.
        // The streak keeps counting; the next 60-pass mark after the world is back relearns as before.
        if (GameERLoadingScreen() || GameERForeignCamActive()) {
            static int s_told = 0;
            if (s_told++ < 4)
                LOGI("[hud] %d passes with no UI during a loading screen or cutscene: target kept until it ends",
                     streak);
        } else {
            RelearnTarget(g_dev.load(std::memory_order_relaxed));
        }
    }
    g_runCount = g_runHead = g_runTotal = g_drawTotal = 0;
}

bool HudTakeFrame(HudFrame& out) {
    out = {};
    if (!HudEnabled()) return false;
    std::lock_guard<std::mutex> g(g_mtx);
    // g_captured is cleared by HudOnPresent, which runs AFTER the layer is composed, so a
    // pass that drew no UI is reported as "no HUD" rather than re-submitting a stale image.
    if (!g_captured.load(std::memory_order_relaxed) || !g_cap.tex[0].handle) return false;
    out.valid = true;
    out.tex = reinterpret_cast<ID3D12Resource*>(g_cap.tex[0].handle);
    out.texOverWhite = (g_cap.pair && g_dualUsed.load(std::memory_order_relaxed))
                          ? reinterpret_cast<ID3D12Resource*>(g_cap.tex[1].handle)
                          : nullptr;
    out.format = (DXGI_FORMAT)g_cap.fmt;   // reshade::api::format values match DXGI_FORMAT
    out.width = g_cap.w;
    out.height = g_cap.h;
    return true;
}

void HudOnDeviceLost() {
    std::lock_guard<std::mutex> g(g_mtx);
    g_cap.Destroy();
    g_mirror.Release();
    g_bbCount.store(0, std::memory_order_release);
    g_runCount = g_runHead = g_runTotal = g_drawTotal = 0;
    g_captured.store(false);
    g_targetDraws.store(0);
    g_announced.store(false);
    g_dev.store(nullptr, std::memory_order_relaxed);
}

void HudRegisterEvents() {
    if (g_events) return;
    // Registering the draw events costs something on EVERY draw the game issues - ReShade
    // has to walk its handler list - so a player who does not want the HUD layer should not
    // pay for it at all. That makes [HUD] Mode the one setting here which is NOT hot: it is
    // read once, at startup, and turning it on later needs a restart. Everything else about
    // the panel (placement, size, FirstDraw, the key) still hot-reloads.
    if (!HudEnabled()) {
        LOGI("[hud] [HUD] Mode=%s - the UI stays in the frame and no draw hooks are installed "
             "(set Mode=quad and restart to lift it onto its own layer)", Cfg().hudMode.c_str());
        return;
    }
    g_events = true;
    g_active.store(true, std::memory_order_relaxed);
    g_redirect.store(false, std::memory_order_relaxed);   // armed by the first live present
    reshade::register_event<reshade::addon_event::init_swapchain>(&OnInitSwapchainHud);
    reshade::register_event<reshade::addon_event::destroy_swapchain>(&OnDestroySwapchainHud);
    reshade::register_event<reshade::addon_event::init_command_list>(&OnInitCmdList);
    reshade::register_event<reshade::addon_event::destroy_command_list>(&OnDestroyCmdList);
    reshade::register_event<reshade::addon_event::reset_command_list>(&OnResetCmdList);
    reshade::register_event<reshade::addon_event::bind_render_targets_and_depth_stencil>(&OnBindRenderTargets);
    reshade::register_event<reshade::addon_event::begin_render_pass>(&OnBeginRenderPass);
    reshade::register_event<reshade::addon_event::draw>(&OnDraw);
    reshade::register_event<reshade::addon_event::draw_indexed>(&OnDrawIndexed);
    reshade::register_event<reshade::addon_event::draw_or_dispatch_indirect>(&OnDrawIndirect);
    LOGI("[hud] draw-stream events registered (mode=%s target=%s firstDraw=%d)",
         Cfg().hudMode.c_str(), Cfg().hudTarget.c_str(), FirstDrawCfg());
}

void HudUnregisterEvents() {
    if (!g_events) return;
    g_events = false;
    g_active.store(false, std::memory_order_relaxed);
    g_redirect.store(false, std::memory_order_relaxed);
    reshade::unregister_event<reshade::addon_event::init_swapchain>(&OnInitSwapchainHud);
    reshade::unregister_event<reshade::addon_event::destroy_swapchain>(&OnDestroySwapchainHud);
    reshade::unregister_event<reshade::addon_event::init_command_list>(&OnInitCmdList);
    reshade::unregister_event<reshade::addon_event::destroy_command_list>(&OnDestroyCmdList);
    reshade::unregister_event<reshade::addon_event::reset_command_list>(&OnResetCmdList);
    reshade::unregister_event<reshade::addon_event::bind_render_targets_and_depth_stencil>(&OnBindRenderTargets);
    reshade::unregister_event<reshade::addon_event::begin_render_pass>(&OnBeginRenderPass);
    reshade::unregister_event<reshade::addon_event::draw>(&OnDraw);
    reshade::unregister_event<reshade::addon_event::draw_indexed>(&OnDrawIndexed);
    reshade::unregister_event<reshade::addon_event::draw_or_dispatch_indirect>(&OnDrawIndirect);
    HudOnDeviceLost();
}

HudStats HudGetStats() {
    HudStats s;
    s.captures = g_captures.exchange(0);
    s.redirects = g_redirects.exchange(0);
    s.misses = g_misses.exchange(0);
    s.lastDrawsIntoTarget = g_lastDraws.load();
    s.firstDraw = FirstDrawCfg();
    s.targetFound = g_cap.tex[0].handle != 0;
    return s;
}
}
