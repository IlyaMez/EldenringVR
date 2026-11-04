#include "overlay_layers.h"
#include "overlay_canvas.h"
#include "log.h"
#include "reshade_addon.h"
#include <windows.h>
#include <d3d12.h>
#include <cstring>
#include <mutex>

namespace ervr {
namespace {

// What the owners published.
struct Published {
    bool visible = false;
    ULONGLONG tick = 0;            // when the owner last refreshed it
    Pose pose;
    float widthM = 0.f, heightM = 0.f;
    int w = 0, h = 0;              // of the image, px
    uint32_t version = 0;          // bumped with every new image
    std::vector<uint32_t> img;
};
std::mutex g_mtx;
Published g_pub[OverlayCount];

struct Gpu {
    ID3D12Device* dev = nullptr;
    ID3D12CommandAllocator* alloc[3] = {};
    uint64_t allocFence[3] = {};
    int ring = 0;
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12Fence* fence = nullptr;
    HANDLE ev = nullptr;
    uint64_t fenceVal = 0;
    // Two upload buffers per overlay: the copy of the previous frame may still be reading one while
    // new content goes into the other. Every frame copies the current one into a fresh swapchain image.
    struct Up { ID3D12Resource* buf = nullptr; uint8_t* map = nullptr; uint64_t fence = 0; };
    struct Ov {
        Up up[2];
        int w = 0, h = 0;                          // what the buffers were made for
        int cur = -1;                              // the buffer holding the current content
        uint32_t version = 0;                      // content version staged in up[cur]
        DXGI_FORMAT staged = DXGI_FORMAT_UNKNOWN;  // swapchain format it was converted for
    } ov[OverlayCount];
} g_gpu;

UINT RowPitch(int w) { return (UINT)((w * 8 + 255) & ~255); }   // room for 8 bytes/pixel, 256-aligned

void WaitFence(uint64_t v) {
    if (!v || !g_gpu.fence || g_gpu.fence->GetCompletedValue() >= v) return;
    if (SUCCEEDED(g_gpu.fence->SetEventOnCompletion(v, g_gpu.ev))) WaitForSingleObject(g_gpu.ev, 500);
}

void ReleaseUpload(Gpu::Ov& o) {
    for (auto& u : o.up) if (u.buf) { u.buf->Release(); u = {}; }
    o.w = o.h = 0; o.cur = -1;
}

void ReleaseGpu() {
    if (g_gpu.fence) WaitFence(g_gpu.fenceVal);
    for (auto& a : g_gpu.alloc) if (a) { a->Release(); a = nullptr; }
    if (g_gpu.list) { g_gpu.list->Release(); g_gpu.list = nullptr; }
    if (g_gpu.fence) { g_gpu.fence->Release(); g_gpu.fence = nullptr; }
    if (g_gpu.ev) { CloseHandle(g_gpu.ev); g_gpu.ev = nullptr; }
    for (auto& o : g_gpu.ov) ReleaseUpload(o);
    g_gpu = Gpu{};
}

bool EnsureGpu(ID3D12Device* dev) {
    if (g_gpu.dev == dev && g_gpu.list) return true;
    ReleaseGpu();
    if (!dev) return false;
    g_gpu.dev = dev;
    bool ok = true;
    for (auto& a : g_gpu.alloc) ok = ok && SUCCEEDED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&a)));
    ok = ok && SUCCEEDED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_gpu.alloc[0], nullptr, IID_PPV_ARGS(&g_gpu.list)));
    if (ok) g_gpu.list->Close();
    ok = ok && SUCCEEDED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_gpu.fence)));
    g_gpu.ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    ok = ok && g_gpu.ev;
    if (!ok) {
        static bool s_said = false;
        if (!s_said) { s_said = true; LOGE("[overlay] could not create the command resources; the weapon wheel and the belt are not shown"); }
        ReleaseGpu();
        return false;
    }
    return true;
}

// The overlay's two upload buffers, (re)made for a `w` x `h` px image.
bool EnsureUpload(int i, int w, int h) {
    Gpu::Ov& o = g_gpu.ov[i];
    if (o.w == w && o.h == h && o.up[0].buf && o.up[1].buf) return true;
    for (auto& u : o.up) WaitFence(u.fence);
    ReleaseUpload(o);
    D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_UPLOAD;
    bool ok = true;
    for (auto& u : o.up) {
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = (UINT64)RowPitch(w) * h;
        rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1; rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ok = SUCCEEDED(g_gpu.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_GENERIC_READ,
                                                          nullptr, IID_PPV_ARGS(&u.buf))) &&
             SUCCEEDED(u.buf->Map(0, nullptr, (void**)&u.map));
        if (!ok) break;
    }
    if (!ok) {
        static bool s_said = false;
        if (!s_said) { s_said = true; LOGE("[overlay] could not create the upload buffers for overlay %d; it is not shown", i); }
        ReleaseUpload(o);
        return false;
    }
    o.w = w; o.h = h;
    return true;
}

uint16_t Half(float f) {
    uint32_t x; std::memcpy(&x, &f, 4);
    const uint32_t sign = (x >> 16) & 0x8000u;
    const int e = (int)((x >> 23) & 0xFF) - 127 + 15;
    if (e <= 0) return (uint16_t)sign;
    if (e >= 31) return (uint16_t)(sign | 0x7C00u);
    return (uint16_t)(sign | ((uint32_t)e << 10) | ((x >> 13) & 0x3FFu));
}

// The CPU image (8-bit sRGB of premultiplied linear) into the upload buffer, in the swapchain's format.
void Stage(int i, const std::vector<uint32_t>& img, DXGI_FORMAT fmt) {
    Gpu::Ov& o = g_gpu.ov[i];
    const int next = o.cur < 0 ? 0 : (o.cur + 1) & 1;
    Gpu::Up& u = o.up[next];
    WaitFence(u.fence);
    const int w = o.w;
    const UINT pitch = RowPitch(w);
    CanvasInit();
    for (int y = 0; y < o.h; ++y) {
        const uint32_t* src = img.data() + (size_t)y * w;
        uint8_t* dst = u.map + (size_t)y * pitch;
        switch (fmt) {
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
            std::memcpy(dst, src, (size_t)w * 4);
            break;
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
            for (int x = 0; x < w; ++x) {
                const uint32_t v = src[x];
                ((uint32_t*)dst)[x] = (v & 0xFF00FF00u) | ((v >> 16) & 0xFFu) | ((v & 0xFFu) << 16);
            }
            break;
        case DXGI_FORMAT_R16G16B16A16_FLOAT:   // linear light, read as such
            for (int x = 0; x < w; ++x) {
                const uint32_t v = src[x];
                uint16_t* out = (uint16_t*)dst + 4 * x;
                out[0] = Half(SrgbToLinear(v & 255)); out[1] = Half(SrgbToLinear((v >> 8) & 255));
                out[2] = Half(SrgbToLinear((v >> 16) & 255)); out[3] = Half((v >> 24) / 255.f);
            }
            break;
        default: {   // 8-bit UNORM, which the runtimes read as linear (as the eye and HUD blits assume)
            const bool bgra = fmt == DXGI_FORMAT_B8G8R8A8_UNORM;
            for (int x = 0; x < w; ++x) {
                const uint32_t v = src[x];
                const uint32_t r = (uint32_t)std::lround(SrgbToLinear(v & 255) * 255.f), g = (uint32_t)std::lround(SrgbToLinear((v >> 8) & 255) * 255.f);
                const uint32_t b = (uint32_t)std::lround(SrgbToLinear((v >> 16) & 255) * 255.f);
                ((uint32_t*)dst)[x] = (bgra ? (b | (r << 16)) : (r | (b << 16))) | (g << 8) | (v & 0xFF000000u);
            }
            break;
        }
        }
    }
    o.cur = next;
    o.staged = fmt;
}

void Barrier(ID3D12GraphicsCommandList* l, ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b) {
    D3D12_RESOURCE_BARRIER br{};
    br.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    br.Transition.pResource = r;
    br.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    br.Transition.StateBefore = a; br.Transition.StateAfter = b;
    l->ResourceBarrier(1, &br);
}

} // namespace

void OverlayPublishImage(int which, int w, int h, std::vector<uint32_t>& rgba) {
    if (which < 0 || which >= OverlayCount || w <= 0 || h <= 0 || rgba.size() != (size_t)w * h) return;
    std::lock_guard<std::mutex> g(g_mtx);
    Published& p = g_pub[which];
    p.img.swap(rgba);
    p.w = w; p.h = h;
    ++p.version;
}

void OverlayPublishPose(int which, bool visible, const Pose& pose, float widthM, float heightM) {
    if (which < 0 || which >= OverlayCount) return;
    std::lock_guard<std::mutex> g(g_mtx);
    Published& p = g_pub[which];
    p.visible = visible;
    p.tick = GetTickCount64();
    if (visible) { p.pose = pose; p.widthM = widthM; p.heightM = heightM; }
}

void OverlayLayersCompose(IVRBackend* vr, const Dx12Frame& f, VRSubmitLayer& layer) {
    struct Want { bool on = false; Pose pose; float widthM = 0.f, heightM = 0.f; int w = 0, h = 0; uint32_t version = 0; } want[OverlayCount];
    bool any = false;
    {
        std::lock_guard<std::mutex> g(g_mtx);
        const ULONGLONG now = GetTickCount64();
        for (int i = 0; i < OverlayCount; ++i) {
            const Published& p = g_pub[i];
            // An owner that stopped refreshing (controllers gone, a stall) must not leave its quad hanging.
            if (!p.visible || now - p.tick > 300 || p.w <= 0 || p.h <= 0 || p.widthM <= 0.f || p.heightM <= 0.f) continue;
            want[i].on = true; want[i].pose = p.pose; want[i].widthM = p.widthM; want[i].heightM = p.heightM;
            want[i].w = p.w; want[i].h = p.h; want[i].version = p.version;
            any = true;
        }
    }
    if (!any || !vr || !f.device || !f.queue) return;
    if (!EnsureGpu(f.device)) return;
    for (int i = 0; i < OverlayCount; ++i) {
        if (!want[i].on) continue;
        if (!vr->EnsureOverlaySwapchain(i, (uint32_t)want[i].w, (uint32_t)want[i].h) || !EnsureUpload(i, want[i].w, want[i].h)) {
            want[i].on = false;
            continue;
        }
        const DXGI_FORMAT fmt = vr->OverlayFormat(i);
        Gpu::Ov& o = g_gpu.ov[i];
        if (o.cur >= 0 && o.version == want[i].version && o.staged == fmt) continue;
        // New content is copied out under the lock and staged outside it: staging may wait on the GPU.
        std::vector<uint32_t> img;
        uint32_t version = 0;
        {
            std::lock_guard<std::mutex> g(g_mtx);
            const Published& p = g_pub[i];
            if (p.w == want[i].w && p.h == want[i].h && p.img.size() == (size_t)p.w * p.h) { img = p.img; version = p.version; }
        }
        if (!img.empty()) { Stage(i, img, fmt); o.version = version; }
    }
    ID3D12Resource* img[OverlayCount] = {};
    bool anyImg = false;
    for (int i = 0; i < OverlayCount; ++i)
        if (want[i].on && g_gpu.ov[i].cur >= 0 && (img[i] = vr->AcquireOverlayImage(i)) != nullptr) anyImg = true;
    if (!anyImg) return;
    const int slot = g_gpu.ring;
    g_gpu.ring = (g_gpu.ring + 1) % 3;
    WaitFence(g_gpu.allocFence[slot]);
    bool recorded = false;
    if (SUCCEEDED(g_gpu.alloc[slot]->Reset()) && SUCCEEDED(g_gpu.list->Reset(g_gpu.alloc[slot], nullptr))) {
        ID3D12GraphicsCommandList* l = g_gpu.list;
        for (int i = 0; i < OverlayCount; ++i) {
            if (!img[i]) continue;
            const Gpu::Ov& o = g_gpu.ov[i];
            // OpenXR hands D3D12 colour images over in RENDER_TARGET and wants them back that way.
            Barrier(l, img[i], D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_DEST);
            D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
            dst.pResource = img[i]; dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; dst.SubresourceIndex = 0;
            src.pResource = o.up[o.cur].buf; src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            src.PlacedFootprint.Offset = 0;
            src.PlacedFootprint.Footprint.Format = img[i]->GetDesc().Format;   // the image's own (maybe typeless) format: a bit copy
            src.PlacedFootprint.Footprint.Width = (UINT)o.w;
            src.PlacedFootprint.Footprint.Height = (UINT)o.h;
            src.PlacedFootprint.Footprint.Depth = 1;
            src.PlacedFootprint.Footprint.RowPitch = RowPitch(o.w);
            l->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            Barrier(l, img[i], D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_RENDER_TARGET);
        }
        if (SUCCEEDED(l->Close())) {
            ID3D12CommandList* lists[] = {l};
            f.queue->ExecuteCommandLists(1, lists);
            f.queue->Signal(g_gpu.fence, ++g_gpu.fenceVal);
            g_gpu.allocFence[slot] = g_gpu.fenceVal;
            for (int i = 0; i < OverlayCount; ++i) if (img[i]) g_gpu.ov[i].up[g_gpu.ov[i].cur].fence = g_gpu.fenceVal;
            recorded = true;
        }
    }
    for (int i = 0; i < OverlayCount; ++i) if (img[i]) vr->ReleaseOverlayImage(i);
    if (!recorded) return;
    for (int i = 0; i < OverlayCount; ++i) {
        if (!img[i]) continue;
        VROverlayQuad& q = layer.overlays[i];
        q.valid = true; q.pose = want[i].pose; q.widthM = want[i].widthM; q.heightM = want[i].heightM;
    }
}

void OverlayLayersReleaseGpu() { ReleaseGpu(); }

} // namespace ervr
