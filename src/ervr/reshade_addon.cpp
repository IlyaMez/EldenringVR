#include "reshade_addon.h"
#include "config.h"
#include "dx12_blit.h"
#include "frame_probe.h"
#include "hud.h"
#include "patches_er.h"
#include "replay_stereo.h"
#include "stereo.h"
#include "vr_backend.h"
#include "log.h"
#include <string>
#include <windows.h>
#include <d3d12.h>
#include <atomic>
#include <cstdio>
#include <mutex>
#include <unordered_set>
#include <reshade.hpp>

namespace ervr {
namespace {

std::atomic<bool> g_registered{false};
std::atomic<bool> g_eventsUp{false};
HMODULE g_self = nullptr;

// Written on the render thread inside ReShade callbacks, read for logging elsewhere.
std::atomic<ID3D12Device*>       g_device{nullptr};
std::atomic<ID3D12CommandQueue*> g_queue{nullptr};
std::atomic<bool> g_isD3D12{false};
std::atomic<long> g_presents{0};
std::atomic<int> g_screenshots{0};   // ReShadeAddonRequestScreenshot
std::atomic<uint32_t> g_width{0}, g_height{0};
std::atomic<uint32_t> g_format{0};

std::atomic<bool> g_trace{false};
PresentSample g_traceBuf[512];
std::atomic<int> g_traceN{0};

// Present timing, per engine pass. present and finish_present fire back to back on the frame
// thread around the real Present, so the gap between our event returning and finish_present
// is exactly what DXGI held the thread for. Summed here, read and reset by the heartbeat.
struct PassPresentTiming {
    std::atomic<long long> ervrUs{0}, ervrMaxUs{0}, dxgiUs{0}, dxgiMaxUs{0};
    std::atomic<long> ervrN{0}, dxgiN{0};
};
PassPresentTiming g_passTiming[2];
thread_local long long t_presentEnd = 0;   // QPC when our present event returned; 0 = none pending
thread_local int t_presentPass = 0;

long long QpcNow() { LARGE_INTEGER q; QueryPerformanceCounter(&q); return q.QuadPart; }
long long QpcToUs(long long d) {
    static const long long f = [] { LARGE_INTEGER x; QueryPerformanceFrequency(&x); return x.QuadPart; }();
    return d * 1000000 / f;
}
void AddSample(std::atomic<long long>& sum, std::atomic<long long>& mx, std::atomic<long>& n, long long us) {
    sum.fetch_add(us, std::memory_order_relaxed);
    if (us > mx.load(std::memory_order_relaxed)) mx.store(us, std::memory_order_relaxed);
    n.fetch_add(1, std::memory_order_relaxed);
}

// Times our whole present event, whichever way it returns.
struct PresentEventClock {
    long long t0 = QpcNow();
    int pass = FramePumpCurrentPass();
    ~PresentEventClock() {
        const long long t1 = QpcNow();
        t_presentEnd = t1;
        t_presentPass = pass;
        if (pass == 1 || pass == 2) {
            PassPresentTiming& pt = g_passTiming[pass - 1];
            AddSample(pt.ervrUs, pt.ervrMaxUs, pt.ervrN, QpcToUs(t1 - t0));
        }
    }
};

// ReShade's own screenshot path, when an unattended run asked for one (ReShadeAddonRequestScreenshot): the next two
// presents, named by the pass presenting them - with the second pass running, one image of each pass of the same
// moment (flat ForcePass2 draws both from one camera, so they must look alike).
void OnReShadePresent(reshade::api::effect_runtime* runtime) {
    int left = g_screenshots.load(std::memory_order_relaxed);
    while (left > 0 && !g_screenshots.compare_exchange_weak(left, left - 1)) {}
    if (left > 0) runtime->save_screenshot(FramePumpCurrentPass() == 2 ? "ervr pass2" : "ervr pass1");
}

void OnFinishPresent(reshade::api::command_queue*, reshade::api::swapchain* sc) {
    ReplayStereoPresentScope(false);   // opened in OnPresent: ReShade's work and the real Present are over
    if (!t_presentEnd || sc->get_device()->get_api() != reshade::api::device_api::d3d12) return;
    const long long us = QpcToUs(QpcNow() - t_presentEnd);
    const int pass = t_presentPass;
    t_presentEnd = 0;
    if (pass == 1 || pass == 2) {
        PassPresentTiming& pt = g_passTiming[pass - 1];
        AddSample(pt.dxgiUs, pt.dxgiMaxUs, pt.dxgiN, us);
    }
}

void OnInitDevice(reshade::api::device* dev) {
    // Only the game's D3D12 device counts. Others come and go in the game process: overlays, and the
    // VR runtime itself (Meta Quest Link's made a D3D11 device as its session started, 2026-11-02 log) -
    // one of those used to clear the flag and log that ERVR would stay idle, which it did not.
    if (dev->get_api() != reshade::api::device_api::d3d12) {
        LOGI("[reshade] ignoring a device that is not D3D12 (api 0x%x: an overlay's or the VR runtime's)", (unsigned)dev->get_api());
        return;
    }
    g_isD3D12.store(true);
    auto* native = reinterpret_cast<ID3D12Device*>(dev->get_native());
    g_device.store(native);
    LOGI("[reshade] D3D12 device %p", (void*)native);
    ReplayStereoOnInitDevice(native);
}

void OnDestroyDevice(reshade::api::device* dev) {
    // Only the game's D3D12 device: overlays make and drop D3D11 devices of their own.
    if (dev->get_api() != reshade::api::device_api::d3d12) return;
    g_device.store(nullptr);
    g_queue.store(nullptr);
    HudOnDeviceLost();
    StereoOnDeviceLost();
    // End the OpenXR session while the device and its queue still exist. Left bound through
    // the game's shutdown, VDXR 1.0.10 crashed during process exit (2026-10-31: an indirect
    // call through freed memory in its LibOVR shim, CFG fail-fast, ~12 s after the device went).
    BackendsOnDeviceDestroyed(reinterpret_cast<ID3D12Device*>(dev->get_native()));
}

// The FIRST swapchain's size, before any resize. The game builds its raster projection for
// this boot frame and never rebuilds it when the late settings-apply resizes the window
// larger (measured 2026-08-22 by flat A/B: identical px-per-degree at 3072x3264 vs
// 2560x1440) - so this size is the reference the eye-crop geometry needs.
std::atomic<uint32_t> g_firstW{0}, g_firstH{0};

void OnInitSwapchain(reshade::api::swapchain* sc, bool resize) {
    reshade::api::device* dev = sc->get_device();
    if (dev->get_api() != reshade::api::device_api::d3d12) return;
    g_isD3D12.store(true);
    const reshade::api::resource bb = sc->get_current_back_buffer();
    const reshade::api::resource_desc d = dev->get_resource_desc(bb);
    g_width.store(d.texture.width);
    g_height.store(d.texture.height);
    g_format.store((uint32_t)d.texture.format);
    // A resize is by definition not the boot swapchain - recording one here is exactly the
    // mistake that neutered the raster-reference model in the 19:04 session.
    if (!resize) {
        uint32_t expect = 0;
        if (g_firstH.compare_exchange_strong(expect, d.texture.height))
            g_firstW.store(d.texture.width);
    }
    LOGI("[reshade] swapchain %ux%u fmt %u buffers %u%s", d.texture.width, d.texture.height,
         (unsigned)d.texture.format, sc->get_back_buffer_count(), resize ? " (resize)" : "");
    // Once the oversized back buffer exists, the desktop-size lies have done their job and
    // must stop (they cause the vertical head-motion warp when left standing).
    PatchesNoteBackbufferSize(d.texture.width, d.texture.height);
    if (resize) StereoOnSwapchainResize();
}

bool FirstSwapchainSizeImpl(uint32_t& w, uint32_t& h) {
    const uint32_t fh = g_firstH.load(std::memory_order_relaxed);
    if (!fh) return false;
    w = g_firstW.load(std::memory_order_relaxed);
    h = fh;
    return true;
}

void OnInitResource(reshade::api::device* dev, const reshade::api::resource_desc& desc,
                    const reshade::api::subresource_data*, reshade::api::resource_usage, reshade::api::resource res) {
    if (dev->get_api() != reshade::api::device_api::d3d12) return;
    ReplayStereoOnInitResource(reinterpret_cast<ID3D12Resource*>(res.handle), (int)desc.heap,
                               desc.type == reshade::api::resource_type::buffer);
}

void OnDestroyResource(reshade::api::device* dev, reshade::api::resource res) {
    if (dev->get_api() != reshade::api::device_api::d3d12) return;
    ReplayStereoOnDestroyResource(reinterpret_cast<ID3D12Resource*>(res.handle));
}

void OnDestroySwapchain(reshade::api::swapchain* sc, bool resize) {
    if (!resize && sc->get_device()->get_api() == reshade::api::device_api::d3d12) StereoOnDeviceLost();
}

// The render thread. ReShade hands us the queue it is presenting on - exactly the queue
// OpenXR must be bound to - so there is no queue-guessing heuristic to get wrong.
void OnPresent(reshade::api::command_queue* queue, reshade::api::swapchain* sc,
               const reshade::api::rect*, const reshade::api::rect*, uint32_t, const reshade::api::rect*) {
    reshade::api::device* dev = sc->get_device();
    // Determine D3D12-ness from the live device, NOT from a flag set in init_device:
    // ERVR registers as an add-on after the game already created its device, so
    // init_device never fires for us and that flag would stay false forever.
    if (!g_isD3D12.load(std::memory_order_relaxed)) {
        if (dev->get_api() != reshade::api::device_api::d3d12) return;
        g_isD3D12.store(true);
        LOGI("[reshade] D3D12 detected at present (init_device was missed - registered mid-run)");
    }
    PresentEventClock clock;

    Dx12Frame f;
    f.device = reinterpret_cast<ID3D12Device*>(dev->get_native());
    f.queue  = reinterpret_cast<ID3D12CommandQueue*>(queue->get_native());
    const reshade::api::resource bb = sc->get_current_back_buffer();
    f.backBuffer = reinterpret_cast<ID3D12Resource*>(bb.handle);   // D3D12: the handle is the native pointer
    if (!f.device || !f.queue || !f.backBuffer) return;

    const reshade::api::resource_desc d = dev->get_resource_desc(bb);
    f.width  = d.texture.width;
    f.height = d.texture.height;
    f.format = (DXGI_FORMAT)d.texture.format;   // reshade::api::format values match DXGI_FORMAT
    // First-size capture HERE too, not only in the init event: the add-on registers ~1 s
    // into the session and can miss the boot swapchain's init entirely (measured 19:04
    // session - the first init it saw was the +10 s RESIZE, so the "boot size" recorded as
    // 3264 and the raster-reference model neutered itself). Presents run from registration
    // on, a good ~8 s before the resize, so the first present is a reliable boot witness.
    if (!g_firstH.load(std::memory_order_relaxed)) {
        uint32_t expect = 0;
        if (g_firstH.compare_exchange_strong(expect, d.texture.height)) {
            g_firstW.store(d.texture.width);
            LOGI("[reshade] boot frame recorded: %ux%u (raster reference height %u)",
                 d.texture.width, d.texture.height, d.texture.height);
        }
    }

    if (g_queue.exchange(f.queue) != f.queue)
        LOGI("[reshade] present queue %p (type %u)", (void*)f.queue, (unsigned)queue->get_type());
    g_device.store(f.device);
    g_presents.fetch_add(1, std::memory_order_relaxed);

    if (g_trace.load(std::memory_order_relaxed)) {
        const int i = g_traceN.fetch_add(1, std::memory_order_relaxed);
        if (i < 512) {
            LARGE_INTEGER q; QueryPerformanceCounter(&q);
            g_traceBuf[i] = {q.QuadPart, GetCurrentThreadId()};
        }
    }

    // One-shot raw back-buffer dump ([Experimental] DumpBackbufferAt): desktop screenshots
    // are post-DWM window scaling, which normalizes away exactly what a raster probe asks
    // (which aspect the projection really rasterized with). This is the buffer itself.
    {
        static ULONGLONG s_t0 = GetTickCount64();
        static bool s_dumped = false;
        const float at = Cfg().expDumpBackbufferAt;
        if (!s_dumped && at > 0.f && GetTickCount64() - s_t0 > (ULONGLONG)(at * 1000.f)) {
            s_dumped = true;
            std::wstring p = Cfg().iniPath;
            const size_t sl = p.find_last_of(L"\\/");
            p = (sl == std::wstring::npos ? std::wstring() : p.substr(0, sl + 1)) + L"backbuffer.ppm";
            if (Dx12DumpBackbuffer(f.queue, f.backBuffer, p.c_str()))
                LOGI("[reshade] back buffer dumped: %ux%u fmt %u -> backbuffer.ppm",
                     f.width, f.height, (unsigned)f.format);
            else
                LOGW("[reshade] back buffer dump FAILED (%ux%u fmt %u)",
                     f.width, f.height, (unsigned)f.format);
        }
    }
    // The present scope opens first: lists ERVR creates or records from here on (the replay's own, at the bench
    // levels inside ReplayStereoOnPresent) are ERVR's, never shadowed as the game's.
    ReplayStereoPresentScope(true);
    ReplayStereoOnPresent(f);
    // Before the stereo path consumes the capture: optionally paint the panel onto the
    // game's own frame, so the monitor shows what the headset gets ([HUD] MirrorPanel).
    HudMirrorPanel(f);
    StereoOnPresent(f);
    // After the layer has been composed: closes this pass's UI capture and re-arms for the
    // next one, so a pass that draws no UI reports "no HUD" instead of re-submitting a
    // stale image. Cheap no-op when the HUD layer is off.
    HudOnPresent(sc);
    ReplayStereoPresentEnd(f);   // single-frame stereo: a frame the stereo layer did not replay is released
    // The present scope stays open until finish_present: ReShade's own work and the real Present follow.
}

// ---- shader dump (bench: ERVR_SHADER_DUMP set in the environment) ------------------------------------------------------
// Every pixel and compute shader the game creates, written once by the FNV-1a 64 hash of its bytecode to shaderdump\ next
// to the ini, and shaderdump\pipelines.csv: pipeline, layout, vertex/pixel/compute shader hashes. Offline: dxc -dumpbin.
std::mutex g_sdMtx;
std::unordered_set<uint64_t> g_sdSeen;
FILE* g_sdCsv = nullptr;
std::wstring g_sdDir;
bool g_sdOn = false;

uint64_t Fnv64(const void* p, size_t n) {
    uint64_t h = 1469598103934665603ull;
    const auto* b = static_cast<const uint8_t*>(p);
    for (size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 1099511628211ull; }
    return h;
}

void OnInitPipeline(reshade::api::device*, reshade::api::pipeline_layout layout, uint32_t n,
                    const reshade::api::pipeline_subobject* so, reshade::api::pipeline pipe) {
    using T = reshade::api::pipeline_subobject_type;
    uint64_t hs[3] = {};   // vertex, pixel, compute
    for (uint32_t i = 0; i < n && so; ++i) {
        const int k = so[i].type == T::vertex_shader ? 0 : so[i].type == T::pixel_shader ? 1 : so[i].type == T::compute_shader ? 2 : -1;
        if (k < 0 || !so[i].data || !so[i].count) continue;
        const auto* sd = static_cast<const reshade::api::shader_desc*>(so[i].data);
        if (!sd->code || !sd->code_size) continue;
        hs[k] = Fnv64(sd->code, sd->code_size);
        if (k == 0) continue;   // vertex shaders: hash only
        std::lock_guard<std::mutex> g(g_sdMtx);
        if (!g_sdSeen.insert(hs[k]).second) continue;
        wchar_t name[64];
        swprintf(name, 64, L"%016llx.%s", (unsigned long long)hs[k], k == 1 ? L"ps" : L"cs");
        if (FILE* f = _wfopen((g_sdDir + name).c_str(), L"wb")) { fwrite(sd->code, 1, sd->code_size, f); fclose(f); }
    }
    std::lock_guard<std::mutex> g(g_sdMtx);
    if (g_sdCsv) {
        fprintf(g_sdCsv, "%llx,%llx,%016llx,%016llx,%016llx\n", (unsigned long long)pipe.handle,
                (unsigned long long)layout.handle, (unsigned long long)hs[0], (unsigned long long)hs[1], (unsigned long long)hs[2]);
        fflush(g_sdCsv);
    }
}

// Root signatures, for the pass log's tables: layouts.csv - layout, parameter, kind (0 table, 1 constants, 2+ root
// view), then per range: offset in the table, register, space, count, descriptor type (ReShade's numbering).
FILE* g_sdLayouts = nullptr;
void OnInitPipelineLayout(reshade::api::device*, uint32_t n, const reshade::api::pipeline_layout_param* params,
                          reshade::api::pipeline_layout layout) {
    using P = reshade::api::pipeline_layout_param_type;
    std::lock_guard<std::mutex> g(g_sdMtx);
    if (!g_sdLayouts || !params) return;
    for (uint32_t i = 0; i < n; ++i) {
        const auto& p = params[i];
        fprintf(g_sdLayouts, "%llx,%u,%u", (unsigned long long)layout.handle, i, (unsigned)p.type);
        if (p.type == P::push_constants) {
            fprintf(g_sdLayouts, ",b%u space %u x%u", p.push_constants.dx_register_index, p.push_constants.dx_register_space,
                    p.push_constants.count);
        } else if (p.type == P::push_descriptors) {
            fprintf(g_sdLayouts, ",reg %u space %u type %u", p.push_descriptors.dx_register_index,
                    p.push_descriptors.dx_register_space, (unsigned)p.push_descriptors.type);
        } else if (p.type == P::descriptor_table || p.type == P::descriptor_table_with_flags ||
                   p.type == P::push_descriptors_with_ranges || p.type == P::push_descriptors_with_ranges_and_flags) {
            const bool flags = p.type == P::descriptor_table_with_flags || p.type == P::push_descriptors_with_ranges_and_flags;
            const uint32_t cnt = flags ? p.descriptor_table_with_flags.count : p.descriptor_table.count;
            for (uint32_t r = 0; r < cnt; ++r) {
                const reshade::api::descriptor_range& d =
                    flags ? p.descriptor_table_with_flags.ranges[r] : p.descriptor_table.ranges[r];
                fprintf(g_sdLayouts, ",[at %u reg %u space %u x%u type %u]", d.binding, d.dx_register_index, d.dx_register_space,
                        d.count, (unsigned)d.type);
            }
        }
        fputc('\n', g_sdLayouts);
    }
    fflush(g_sdLayouts);
}

bool g_sdLayoutsOnly = false;   // ERVR_SHADER_DUMP=layouts: root signatures only (shader events slow the boot down a lot)

void ShaderDumpStart() {
    wchar_t mode[16] = {};
    if (GetEnvironmentVariableW(L"ERVR_SHADER_DUMP", mode, 16) == 0) return;
    g_sdLayoutsOnly = _wcsicmp(mode, L"layouts") == 0;
    std::wstring dir = Cfg().iniPath;
    const size_t sl = dir.find_last_of(L"\\/");
    dir = (sl == std::wstring::npos ? std::wstring() : dir.substr(0, sl + 1)) + L"shaderdump\\";
    CreateDirectoryW(dir.c_str(), nullptr);
    g_sdDir = dir;
    g_sdCsv = _wfopen((dir + L"pipelines.csv").c_str(), L"a");
    g_sdLayouts = _wfopen((dir + L"layouts.csv").c_str(), L"a");
    if (!g_sdLayoutsOnly) reshade::register_event<reshade::addon_event::init_pipeline>(&OnInitPipeline);
    reshade::register_event<reshade::addon_event::init_pipeline_layout>(&OnInitPipelineLayout);
    g_sdOn = true;
    LOGI("[reshade] shader dump on: pixel and compute shaders -> shaderdump\\");
}

} // namespace

bool ReShadeAddonInstall(void* selfModule) {
    if (g_registered.load()) return true;
    g_self = (HMODULE)selfModule;
    if (!reshade::register_addon(g_self)) return false;   // ReShade not loaded (yet)
    g_registered.store(true);
    LOGI("[reshade] registered as add-on (API version %d)", RESHADE_API_VERSION);

    reshade::register_event<reshade::addon_event::init_device>(&OnInitDevice);
    reshade::register_event<reshade::addon_event::destroy_device>(&OnDestroyDevice);
    reshade::register_event<reshade::addon_event::init_swapchain>(&OnInitSwapchain);
    reshade::register_event<reshade::addon_event::destroy_swapchain>(&OnDestroySwapchain);
    reshade::register_event<reshade::addon_event::present>(&OnPresent);
    reshade::register_event<reshade::addon_event::finish_present>(&OnFinishPresent);
    reshade::register_event<reshade::addon_event::reshade_present>(&OnReShadePresent);
    if (Cfg().replayCensus || ReplayStereoLevel() >= 2) {   // single-frame stereo groundwork: CPU-visible buffers by GPU address
        reshade::register_event<reshade::addon_event::init_resource>(&OnInitResource);
        reshade::register_event<reshade::addon_event::destroy_resource>(&OnDestroyResource);
    }
    HudRegisterEvents();
    ShaderDumpStart();
    g_eventsUp.store(true);
    return true;
}

bool ReShadeAddonInstalled() { return g_registered.load(); }

bool ReShadeFirstSwapchainSize(uint32_t& w, uint32_t& h) { return FirstSwapchainSizeImpl(w, h); }

void ReShadeAddonShutdown() {
    if (!g_registered.exchange(false)) return;
    if (g_eventsUp.exchange(false)) {
        reshade::unregister_event<reshade::addon_event::init_device>(&OnInitDevice);
        reshade::unregister_event<reshade::addon_event::destroy_device>(&OnDestroyDevice);
        reshade::unregister_event<reshade::addon_event::init_swapchain>(&OnInitSwapchain);
        reshade::unregister_event<reshade::addon_event::destroy_swapchain>(&OnDestroySwapchain);
        reshade::unregister_event<reshade::addon_event::present>(&OnPresent);
        reshade::unregister_event<reshade::addon_event::finish_present>(&OnFinishPresent);
        reshade::unregister_event<reshade::addon_event::reshade_present>(&OnReShadePresent);
        if (Cfg().replayCensus || ReplayStereoLevel() >= 2) {
            reshade::unregister_event<reshade::addon_event::init_resource>(&OnInitResource);
            reshade::unregister_event<reshade::addon_event::destroy_resource>(&OnDestroyResource);
        }
        HudUnregisterEvents();
        if (g_sdOn) {
            if (!g_sdLayoutsOnly) reshade::unregister_event<reshade::addon_event::init_pipeline>(&OnInitPipeline);
            reshade::unregister_event<reshade::addon_event::init_pipeline_layout>(&OnInitPipelineLayout);
        }
    }
    reshade::unregister_addon(g_self);
    LOGI("[reshade] add-on unregistered");
}

void ReShadeAddonRequestScreenshot() { g_screenshots.fetch_add(2); }

void ReShadeAddonPresentTraceStart() {
    g_traceN.store(0);
    g_trace.store(true);
}

int ReShadeAddonPresentTraceStop(PresentSample* out, int maxOut) {
    g_trace.store(false);
    int n = g_traceN.load();
    if (n > 512) n = 512;
    if (n > maxOut) n = maxOut;
    for (int i = 0; i < n; ++i) out[i] = g_traceBuf[i];
    return n;
}

ReShadeStats ReShadeAddonGetStats() {
    ReShadeStats s;
    s.registered = g_registered.load();
    s.d3d12  = g_isD3D12.load();
    s.presents = g_presents.exchange(0);
    s.width  = g_width.load();
    s.height = g_height.load();
    s.format = (DXGI_FORMAT)g_format.load();
    for (int p = 0; p < 2; ++p) {
        PassPresentTiming& pt = g_passTiming[p];
        const long en = pt.ervrN.exchange(0), dn = pt.dxgiN.exchange(0);
        const long long eus = pt.ervrUs.exchange(0), emax = pt.ervrMaxUs.exchange(0);
        const long long dus = pt.dxgiUs.exchange(0), dmax = pt.dxgiMaxUs.exchange(0);
        if (en > 0) { s.ervrMsAvg[p] = (float)eus / 1000.f / (float)en; s.ervrMsMax[p] = (float)emax / 1000.f; }
        if (dn > 0) { s.dxgiMsAvg[p] = (float)dus / 1000.f / (float)dn; s.dxgiMsMax[p] = (float)dmax / 1000.f; }
    }
    return s;
}
}
