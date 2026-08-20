#include "dx12_blit.h"
#include "log.h"
#include <windows.h>
#include <d3dcompiler.h>
#include <algorithm>
#pragma comment(lib, "d3dcompiler.lib")

namespace ervr {
namespace {

DXGI_FORMAT CopyFamily(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: case DXGI_FORMAT_R8G8B8A8_UINT:
    case DXGI_FORMAT_R8G8B8A8_SNORM: case DXGI_FORMAT_R8G8B8A8_SINT:
        return DXGI_FORMAT_R8G8B8A8_TYPELESS;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        return DXGI_FORMAT_B8G8R8A8_TYPELESS;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: case DXGI_FORMAT_R10G10B10A2_UNORM:
    case DXGI_FORMAT_R10G10B10A2_UINT:
        return DXGI_FORMAT_R10G10B10A2_TYPELESS;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_UNORM: case DXGI_FORMAT_R16G16B16A16_UINT:
    case DXGI_FORMAT_R16G16B16A16_SNORM: case DXGI_FORMAT_R16G16B16A16_SINT:
        return DXGI_FORMAT_R16G16B16A16_TYPELESS;
    default: return f;
    }
}
// Concrete format for a view onto a possibly-typeless resource. OpenXR runtimes hand
// back swapchain images declared TYPELESS so the app can choose sRGB-ness, and D3D12
// rejects both an RTV and a PSO render-target format that is typeless.
DXGI_FORMAT SrvFormat(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:    return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:    return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:return DXGI_FORMAT_R16G16B16A16_FLOAT;
    default: return f;
    }
}
DXGI_FORMAT RtvFormat(DXGI_FORMAT f) { return SrvFormat(f); }
void Barrier(ID3D12GraphicsCommandList* l, ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b) {
    if (a == b) return;
    D3D12_RESOURCE_BARRIER br{};
    br.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    br.Transition.pResource = r;
    br.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    br.Transition.StateBefore = a;
    br.Transition.StateAfter = b;
    l->ResourceBarrier(1, &br);
}

// Fullscreen-triangle blit.
//
// The shader always OUTPUTS LINEAR LIGHT, because that is what every destination wants:
// a FLOAT swapchain stores linear and OpenXR reads it as linear, and an _SRGB render
// target has the hardware encode linear->sRGB on write. So the only question is whether
// the SOURCE needed decoding first, which gLinearize answers.
const char kHLSL[] =
    "Texture2D gTex : register(t0);\n"
    "SamplerState gSmp : register(s0);\n"
    "cbuffer Params : register(b0) { float4 gUv; uint gLinearize; uint3 gPad; };\n"
    "struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
    "VSOut VSMain(uint id : SV_VertexID) {\n"
    "  VSOut o; float2 t = float2((id << 1) & 2, id & 2);\n"
    "  o.uv = t; o.pos = float4(t.x * 2 - 1, 1 - t.y * 2, 0, 1); return o; }\n"
    "float3 SrgbToLinear(float3 c) {\n"
    "  float3 lo = c / 12.92;\n"
    "  float3 hi = pow(max(c + 0.055, 1e-5) / 1.055, 2.4);\n"
    "  return lerp(hi, lo, step(c, 0.04045)); }\n"
    "float4 PSMain(VSOut i) : SV_Target {\n"
    "  float4 c = gTex.Sample(gSmp, i.uv * gUv.xy + gUv.zw);\n"
    "  if (gLinearize != 0) c.rgb = SrgbToLinear(c.rgb);\n"
    "  return float4(c.rgb, 1); }\n";   // opaque: the game's alpha channel is not a coverage mask

struct RootConsts { float uv[4]; uint32_t linearize; uint32_t pad[3]; };

} // namespace

bool Dx12Blitter::FormatsCopyCompatible(DXGI_FORMAT a, DXGI_FORMAT b) {
    return a == b || CopyFamily(a) == CopyFamily(b);
}

bool Dx12Blitter::StorageIsGammaEncoded(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8X8_UNORM: case DXGI_FORMAT_R10G10B10A2_UNORM:
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
        return true;
    default: return false;   // FLOAT = linear already, _SRGB = hardware-decoded on read
    }
}

bool Dx12Blitter::Ensure(ID3D12Device* dev) {
    if (dev_ == dev && list_) return true;
    if (dev_ != dev) Release();
    dev_ = dev;
    if (!dev_) return false;
    for (int i = 0; i < kRing; ++i)
        if (FAILED(dev_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc_[i])))) {
            LOGE("[blit] CreateCommandAllocator failed"); Release(); return false;
        }
    if (FAILED(dev_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc_[0], nullptr, IID_PPV_ARGS(&list_)))) {
        LOGE("[blit] CreateCommandList failed"); Release(); return false;
    }
    list_->Close();
    if (FAILED(dev_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_)))) {
        LOGE("[blit] CreateFence failed"); Release(); return false;
    }
    event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!event_) { LOGE("[blit] CreateEvent failed"); Release(); return false; }
    LOGI("[blit] ready on device %p", (void*)dev_);
    return true;
}

bool Dx12Blitter::BeginList(ID3D12GraphicsCommandList** out) {
    const int slot = ring_;
    ring_ = (ring_ + 1) % kRing;
    if (fence_->GetCompletedValue() < ringFence_[slot]) {
        if (SUCCEEDED(fence_->SetEventOnCompletion(ringFence_[slot], event_)))
            WaitForSingleObject(event_, 1000);
    }
    if (FAILED(alloc_[slot]->Reset())) return false;
    if (FAILED(list_->Reset(alloc_[slot], nullptr))) return false;
    lastSlot_ = slot;
    *out = list_;
    return true;
}

void Dx12Blitter::SubmitList() {
    if (FAILED(list_->Close())) return;
    ID3D12CommandList* lists[] = {list_};
    queue_->ExecuteCommandLists(1, lists);
    ringFence_[lastSlot_] = ++fenceVal_;
    queue_->Signal(fence_, fenceVal_);
}

bool Dx12Blitter::Copy(ID3D12CommandQueue* queue,
                       ID3D12Resource* src, D3D12_RESOURCE_STATES srcState,
                       ID3D12Resource* dst, D3D12_RESOURCE_STATES dstState,
                       const BlitParams& params) {
    if (!list_ || !queue || !src || !dst) return false;
    queue_ = queue;
    const D3D12_RESOURCE_DESC sd = src->GetDesc();
    const D3D12_RESOURCE_DESC dd = dst->GetDesc();

    // A raw bit copy preserves the encoding, so it is only correct when nothing has to be
    // converted and the two images are the same size.
    if (params.Identity() && FormatsCopyCompatible(sd.Format, dd.Format) &&
        sd.Width == dd.Width && sd.Height == dd.Height) {
        ID3D12GraphicsCommandList* l = nullptr;
        if (!BeginList(&l)) return false;
        Barrier(l, src, srcState, D3D12_RESOURCE_STATE_COPY_SOURCE);
        Barrier(l, dst, dstState, D3D12_RESOURCE_STATE_COPY_DEST);
        l->CopyResource(dst, src);
        Barrier(l, dst, D3D12_RESOURCE_STATE_COPY_DEST, dstState);
        Barrier(l, src, D3D12_RESOURCE_STATE_COPY_SOURCE, srcState);
        SubmitList();
        return true;
    }
    return ShaderBlit(src, srcState, dst, dstState, params);
}

bool Dx12Blitter::EnsureShaderRoot() {
    if (rootSig_) return true;
    D3D12_DESCRIPTOR_RANGE range{};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = 1;
    range.BaseShaderRegister = 0;
    D3D12_ROOT_PARAMETER param[2]{};
    param[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    param[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    param[0].DescriptorTable.NumDescriptorRanges = 1;
    param[0].DescriptorTable.pDescriptorRanges = &range;
    param[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    param[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    param[1].Constants.ShaderRegister = 0;
    param[1].Constants.Num32BitValues = 8;
    D3D12_STATIC_SAMPLER_DESC samp{};
    samp.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    samp.AddressU = samp.AddressV = samp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC rs{};
    rs.NumParameters = 2; rs.pParameters = param;
    rs.NumStaticSamplers = 1; rs.pStaticSamplers = &samp;
    rs.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    ID3DBlob* sig = nullptr; ID3DBlob* err = nullptr;
    if (FAILED(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err))) {
        LOGE("[blit] SerializeRootSignature failed: %s", err ? (const char*)err->GetBufferPointer() : "?");
        if (sig) sig->Release(); if (err) err->Release(); return false;
    }
    HRESULT hr = dev_->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&rootSig_));
    sig->Release(); if (err) err->Release();
    if (FAILED(hr)) { LOGE("[blit] CreateRootSignature failed 0x%08lx", hr); return false; }

    if (!srvHeap_) {
        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.NumDescriptors = kRing;
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(dev_->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&srvHeap_)))) { LOGE("[blit] SRV heap failed"); return false; }
        srvStride_ = dev_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }
    if (!rtvHeap_) {
        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        hd.NumDescriptors = kRing;
        if (FAILED(dev_->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtvHeap_)))) { LOGE("[blit] RTV heap failed"); return false; }
        rtvStride_ = dev_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    }
    return true;
}

// One PSO per destination format, cached: stereo alternates eye (sRGB) and quad (FLOAT16)
// targets, and recreating the pipeline on every switch would stall the render thread.
ID3D12PipelineState* Dx12Blitter::PipelineFor(DXGI_FORMAT rtvFormat) {
    if (!EnsureShaderRoot()) return nullptr;
    for (int i = 0; i < kPso; ++i)
        if (pso_[i] && psoFormat_[i] == rtvFormat) return pso_[i];

    static ID3DBlob* s_vs = nullptr; static ID3DBlob* s_ps = nullptr;
    if (!s_vs || !s_ps) {
        ID3DBlob* err = nullptr;
        if (FAILED(D3DCompile(kHLSL, sizeof(kHLSL) - 1, "ervr_blit", nullptr, nullptr, "VSMain", "vs_5_0", 0, 0, &s_vs, &err))) {
            LOGE("[blit] VS compile failed: %s", err ? (const char*)err->GetBufferPointer() : "?");
            if (err) err->Release(); return nullptr;
        }
        if (err) { err->Release(); err = nullptr; }
        if (FAILED(D3DCompile(kHLSL, sizeof(kHLSL) - 1, "ervr_blit", nullptr, nullptr, "PSMain", "ps_5_0", 0, 0, &s_ps, &err))) {
            LOGE("[blit] PS compile failed: %s", err ? (const char*)err->GetBufferPointer() : "?");
            if (err) err->Release(); return nullptr;
        }
        if (err) err->Release();
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = rootSig_;
    pd.VS = {s_vs->GetBufferPointer(), s_vs->GetBufferSize()};
    pd.PS = {s_ps->GetBufferPointer(), s_ps->GetBufferSize()};
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.SampleMask = UINT_MAX;
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1;
    pd.RTVFormats[0] = rtvFormat;
    pd.SampleDesc.Count = 1;
    ID3D12PipelineState* pso = nullptr;
    if (FAILED(dev_->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pso)))) {
        LOGE("[blit] CreateGraphicsPipelineState failed (fmt %u)", (unsigned)rtvFormat); return nullptr;
    }
    int slot = 0;
    while (slot < kPso && pso_[slot]) ++slot;
    if (slot == kPso) { slot = 0; pso_[0]->Release(); }   // never happens with 2 formats in play
    pso_[slot] = pso; psoFormat_[slot] = rtvFormat;
    LOGI("[blit] shader blit pipeline ready (RTV fmt %u)", (unsigned)rtvFormat);
    return pso;
}

bool Dx12Blitter::EnsureIntermediate(const D3D12_RESOURCE_DESC& srcDesc) {
    if (inter_[0] && interDesc_.Width == srcDesc.Width && interDesc_.Height == srcDesc.Height &&
        interDesc_.Format == srcDesc.Format)
        return true;
    for (auto& p : inter_) if (p) { p->Release(); p = nullptr; }
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd = srcDesc;
    rd.Flags = D3D12_RESOURCE_FLAG_NONE;   // SRV-able, no RTV/UAV/DS
    for (int i = 0; i < kRing; ++i) {
        if (FAILED(dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                                 D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&inter_[i])))) {
            LOGE("[blit] staging texture %d create failed (%llux%u fmt %u)", i, rd.Width, rd.Height, (unsigned)rd.Format);
            return false;
        }
    }
    interDesc_ = rd;
    LOGI("[blit] staging ring ready: %d x %llux%u fmt %u", kRing, rd.Width, rd.Height, (unsigned)rd.Format);
    return true;
}

bool Dx12Blitter::ShaderBlit(ID3D12Resource* src, D3D12_RESOURCE_STATES srcState,
                             ID3D12Resource* dst, D3D12_RESOURCE_STATES dstState,
                             const BlitParams& params) {
    const D3D12_RESOURCE_DESC sd = src->GetDesc();
    const D3D12_RESOURCE_DESC dd = dst->GetDesc();
    if (!EnsureIntermediate(sd)) return false;
    // The XR image is typically TYPELESS; both the PSO and the RTV need a concrete format,
    // and it has to be the one the swapchain was CREATED with - resolving the typeless
    // resource ourselves cannot tell _UNORM from _UNORM_SRGB, and picking the wrong one
    // drops the hardware sRGB encode, which the runtime then decodes anyway: a dark image.
    const DXGI_FORMAT rtvFmt = params.dstFormat != DXGI_FORMAT_UNKNOWN ? params.dstFormat
                                                                       : RtvFormat(dd.Format);
    ID3D12PipelineState* pso = PipelineFor(rtvFmt);
    if (!pso) return false;

    ID3D12GraphicsCommandList* l = nullptr;
    if (!BeginList(&l)) return false;
    const int slot = lastSlot_;                 // BeginList already waited for this slot
    ID3D12Resource* stage = inter_[slot];

    // Per-slot descriptors: the GPU reads these at execute time, so a single shared slot
    // would race with frames still in flight.
    D3D12_CPU_DESCRIPTOR_HANDLE srvCpu = srvHeap_->GetCPUDescriptorHandleForHeapStart();
    D3D12_GPU_DESCRIPTOR_HANDLE srvGpu = srvHeap_->GetGPUDescriptorHandleForHeapStart();
    D3D12_CPU_DESCRIPTOR_HANDLE rtv    = rtvHeap_->GetCPUDescriptorHandleForHeapStart();
    srvCpu.ptr += (SIZE_T)slot * srvStride_;
    srvGpu.ptr += (UINT64)slot * srvStride_;
    rtv.ptr    += (SIZE_T)slot * rtvStride_;

    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = SrvFormat(sd.Format);
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    dev_->CreateShaderResourceView(stage, &srv, srvCpu);

    D3D12_RENDER_TARGET_VIEW_DESC rtvDesc{};
    rtvDesc.Format = rtvFmt;
    rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    dev_->CreateRenderTargetView(dst, &rtvDesc, rtv);

    // Stage the back buffer into the SRV-able copy (same format => plain copy).
    Barrier(l, src, srcState, D3D12_RESOURCE_STATE_COPY_SOURCE);
    l->CopyResource(stage, src);
    Barrier(l, stage, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    Barrier(l, src, D3D12_RESOURCE_STATE_COPY_SOURCE, srcState);

    // Draw the fullscreen triangle into the XR image.
    Barrier(l, dst, dstState, D3D12_RESOURCE_STATE_RENDER_TARGET);
    l->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    D3D12_VIEWPORT vp{0, 0, (float)dd.Width, (float)dd.Height, 0, 1};
    D3D12_RECT sc{0, 0, (LONG)dd.Width, (LONG)dd.Height};
    l->RSSetViewports(1, &vp);
    l->RSSetScissorRects(1, &sc);
    l->SetGraphicsRootSignature(rootSig_);
    l->SetPipelineState(pso);
    ID3D12DescriptorHeap* heaps[] = {srvHeap_};
    l->SetDescriptorHeaps(1, heaps);
    l->SetGraphicsRootDescriptorTable(0, srvGpu);
    RootConsts rc{};
    rc.uv[0] = params.uvScaleX; rc.uv[1] = params.uvScaleY;
    rc.uv[2] = params.uvOffsetX; rc.uv[3] = params.uvOffsetY;
    rc.linearize = params.linearizeSrc ? 1u : 0u;
    l->SetGraphicsRoot32BitConstants(1, 8, &rc, 0);
    l->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    l->DrawInstanced(3, 1, 0, 0);

    // OpenXR expects the released image in RENDER_TARGET, so leave it there; only revert
    // if the caller asked for something else.
    Barrier(l, dst, D3D12_RESOURCE_STATE_RENDER_TARGET, dstState);
    // Return this slot's staging texture to COPY_DEST for its next turn.
    Barrier(l, stage, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    SubmitList();

    // A blown descriptor or barrier shows up here as device removal; say so plainly
    // rather than letting the runtime fail with a generic error a few frames later.
    const HRESULT rr = dev_->GetDeviceRemovedReason();
    if (FAILED(rr)) { LOGE("[blit] D3D12 DEVICE REMOVED (0x%08lx)", rr); return false; }
    return true;
}

void Dx12Blitter::WaitIdle() {
    if (!fence_ || !event_ || fenceVal_ == 0) return;
    if (fence_->GetCompletedValue() < fenceVal_) {
        if (SUCCEEDED(fence_->SetEventOnCompletion(fenceVal_, event_)))
            WaitForSingleObject(event_, 2000);
    }
}

void Dx12Blitter::Release() {
    WaitIdle();
    if (list_)    { list_->Release();    list_ = nullptr; }
    for (auto& a : alloc_) if (a) { a->Release(); a = nullptr; }
    if (fence_)   { fence_->Release();   fence_ = nullptr; }
    if (event_)   { CloseHandle(event_); event_ = nullptr; }
    for (int i = 0; i < kPso; ++i) { if (pso_[i]) pso_[i]->Release(); pso_[i] = nullptr; psoFormat_[i] = DXGI_FORMAT_UNKNOWN; }
    if (rootSig_) { rootSig_->Release(); rootSig_ = nullptr; }
    if (srvHeap_) { srvHeap_->Release(); srvHeap_ = nullptr; }
    if (rtvHeap_) { rtvHeap_->Release(); rtvHeap_ = nullptr; }
    for (auto& p : inter_) if (p) { p->Release(); p = nullptr; }
    interDesc_ = {};
    dev_ = nullptr; queue_ = nullptr; fenceVal_ = 0; ring_ = 0;
    for (auto& f : ringFence_) f = 0;
}
}
