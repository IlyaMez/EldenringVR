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
    "Texture2D gTexW : register(t1);\n"     // Alpha::Dual: the same UI captured over white
    "SamplerState gSmp : register(s0);\n"
    "cbuffer Params : register(b0) { float4 gUv; uint gLinearize; uint gAlphaMode; float gAlphaScale; uint gPad; };\n"
    "struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
    "VSOut VSMain(uint id : SV_VertexID) {\n"
    "  VSOut o; float2 t = float2((id << 1) & 2, id & 2);\n"
    "  o.uv = t; o.pos = float4(t.x * 2 - 1, 1 - t.y * 2, 0, 1); return o; }\n"
    "float3 SrgbToLinear(float3 c) {\n"
    "  float3 lo = c / 12.92;\n"
    "  float3 hi = pow(max(c + 0.055, 1e-5) / 1.055, 2.4);\n"
    "  return lerp(hi, lo, step(c, 0.04045)); }\n"
    "float4 PSMain(VSOut i) : SV_Target {\n"
    "  float2 uv = i.uv * gUv.xy + gUv.zw;\n"
    "  float4 c = gTex.Sample(gSmp, uv);\n"
    // Dual: recover coverage from the pair of captures BEFORE anything else touches
    // the colour. B = s*a over black, W = s*a + (1-a) over white, so W - B is exactly
    // (1 - a) - per channel, and the SMALLEST difference is the honest one (a pure red
    // overlay leaves green and blue looking fully transparent). This needs no alpha
    // channel from the game, which is the point: ELDEN RING never writes one.
    "  if (gAlphaMode == 4) {\n"
    "    float3 w = gTexW.Sample(gSmp, uv).rgb;\n"
    "    float3 d = saturate(w - c.rgb);\n"
    "    float a = saturate(1 - min(d.r, min(d.g, d.b)));\n"
    "    float3 s = a > 0.003 ? saturate(c.rgb / a) : 0;\n"
    // Unpremultiply -> convert -> re-premultiply. Linearising the premultiplied value
    // instead would darken every translucent pixel by its own coverage.
    "    if (gLinearize != 0) s = SrgbToLinear(s);\n"
    "    a *= gAlphaScale;\n"
    "    return float4(s * a, a);\n"
    "  }\n"
    // Keep the stored (gamma) brightness: the luma key is a "did anything draw here"
    // test, and reading it off linear light makes every dim UI pixel near-transparent.
    "  float stored = max(c.r, max(c.g, c.b));\n"
    "  if (gLinearize != 0) c.rgb = SrgbToLinear(c.rgb);\n"
    // Opaque (0) is every world blit: the game's alpha is not a coverage mask. The
    // other keys are fallbacks for when the UI cannot be captured twice.
    "  float a = 1;\n"
    "  if (gAlphaMode == 1) a = c.a;\n"
    "  else if (gAlphaMode == 2) a = saturate(stored * 4);\n"
    "  else if (gAlphaMode == 3) a = max(c.a, saturate(stored * 4));\n"
    // Premultiplied: scale colour with coverage, or a faded HUD gets brighter.
    "  if (gAlphaMode != 0) { a *= gAlphaScale; c.rgb *= gAlphaScale; }\n"
    "  return float4(c.rgb, a); }\n";

struct RootConsts { float uv[4]; uint32_t linearize; uint32_t alphaMode; float alphaScale; uint32_t pad; };

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
    range.NumDescriptors = 2;      // t0 = the image, t1 = its over-white twin (Alpha::Dual)
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
        hd.NumDescriptors = kRing * 2;   // two SRVs per in-flight slot (t0, t1)
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
ID3D12PipelineState* Dx12Blitter::PipelineFor(DXGI_FORMAT rtvFormat, bool blend) {
    if (!EnsureShaderRoot()) return nullptr;
    for (int i = 0; i < kPso; ++i)
        if (pso_[i] && psoFormat_[i] == rtvFormat && psoBlend_[i] == blend) return pso_[i];

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
    if (blend) {
        // Premultiplied source-over: the shader already multiplied colour by coverage, which
        // is the same arrangement the compositor uses for the HUD layer.
        auto& rt = pd.BlendState.RenderTarget[0];
        rt.BlendEnable = TRUE;
        rt.SrcBlend = D3D12_BLEND_ONE;
        rt.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
        rt.BlendOp = D3D12_BLEND_OP_ADD;
        rt.SrcBlendAlpha = D3D12_BLEND_ONE;
        rt.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
        rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    }
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
    pso_[slot] = pso; psoFormat_[slot] = rtvFormat; psoBlend_[slot] = blend;
    LOGI("[blit] shader blit pipeline ready (RTV fmt %u%s)", (unsigned)rtvFormat,
         blend ? ", blended" : "");
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
    // The HUD capture target is ours and already SRV-able, so it is sampled in place. That
    // is not just a saved copy: staging is keyed on the source's size AND format, and the
    // HUD blit alternates with the eye blits, so routing it through the ring would tear the
    // staging textures down and rebuild them twice a frame.
    const bool direct = params.srcIsShaderResource;
    if (!direct && !EnsureIntermediate(sd)) return false;
    // The XR image is typically TYPELESS; both the PSO and the RTV need a concrete format,
    // and it has to be the one the swapchain was CREATED with - resolving the typeless
    // resource ourselves cannot tell _UNORM from _UNORM_SRGB, and picking the wrong one
    // drops the hardware sRGB encode, which the runtime then decodes anyway: a dark image.
    const DXGI_FORMAT rtvFmt = params.dstFormat != DXGI_FORMAT_UNKNOWN ? params.dstFormat
                                                                       : RtvFormat(dd.Format);
    ID3D12PipelineState* pso = PipelineFor(rtvFmt, params.blendOver);
    if (!pso) return false;

    ID3D12GraphicsCommandList* l = nullptr;
    if (!BeginList(&l)) return false;
    const int slot = lastSlot_;                 // BeginList already waited for this slot
    ID3D12Resource* stage = direct ? src : inter_[slot];

    // Per-slot descriptors: the GPU reads these at execute time, so a single shared slot
    // would race with frames still in flight.
    D3D12_CPU_DESCRIPTOR_HANDLE srvCpu = srvHeap_->GetCPUDescriptorHandleForHeapStart();
    D3D12_GPU_DESCRIPTOR_HANDLE srvGpu = srvHeap_->GetGPUDescriptorHandleForHeapStart();
    D3D12_CPU_DESCRIPTOR_HANDLE rtv    = rtvHeap_->GetCPUDescriptorHandleForHeapStart();
    srvCpu.ptr += (SIZE_T)slot * 2 * srvStride_;
    srvGpu.ptr += (UINT64)slot * 2 * srvStride_;
    rtv.ptr    += (SIZE_T)slot * rtvStride_;

    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = SrvFormat(sd.Format);
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    dev_->CreateShaderResourceView(stage, &srv, srvCpu);
    // t1 always gets a real descriptor: an unpopulated slot inside a bound table is
    // undefined behaviour even when the shader never samples it, so when there is no
    // second source it simply aliases the first.
    D3D12_CPU_DESCRIPTOR_HANDLE srvCpu2 = srvCpu;
    srvCpu2.ptr += srvStride_;
    dev_->CreateShaderResourceView(params.src2 ? params.src2 : stage, &srv, srvCpu2);

    D3D12_RENDER_TARGET_VIEW_DESC rtvDesc{};
    rtvDesc.Format = rtvFmt;
    rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    dev_->CreateRenderTargetView(dst, &rtvDesc, rtv);

    if (direct) {
        Barrier(l, src, srcState, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        if (params.src2) Barrier(l, params.src2, srcState, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    } else {
        // Stage the back buffer into the SRV-able copy (same format => plain copy).
        Barrier(l, src, srcState, D3D12_RESOURCE_STATE_COPY_SOURCE);
        l->CopyResource(stage, src);
        Barrier(l, stage, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        Barrier(l, src, D3D12_RESOURCE_STATE_COPY_SOURCE, srcState);
    }

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
    rc.alphaMode = (uint32_t)params.alphaMode;
    rc.alphaScale = params.alphaScale;
    l->SetGraphicsRoot32BitConstants(1, 8, &rc, 0);
    l->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    l->DrawInstanced(3, 1, 0, 0);

    // OpenXR expects the released image in RENDER_TARGET, so leave it there; only revert
    // if the caller asked for something else.
    Barrier(l, dst, D3D12_RESOURCE_STATE_RENDER_TARGET, dstState);
    if (direct) {  // hand the caller's own textures back in the state they were lent in
        Barrier(l, src, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, srcState);
        if (params.src2) Barrier(l, params.src2, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, srcState);
    } else          // return this slot's staging texture to COPY_DEST for its next turn
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
    for (int i = 0; i < kPso; ++i) {
        if (pso_[i]) pso_[i]->Release();
        pso_[i] = nullptr; psoFormat_[i] = DXGI_FORMAT_UNKNOWN; psoBlend_[i] = false;
    }
    if (rootSig_) { rootSig_->Release(); rootSig_ = nullptr; }
    if (srvHeap_) { srvHeap_->Release(); srvHeap_ = nullptr; }
    if (rtvHeap_) { rtvHeap_->Release(); rtvHeap_ = nullptr; }
    for (auto& p : inter_) if (p) { p->Release(); p = nullptr; }
    interDesc_ = {};
    dev_ = nullptr; queue_ = nullptr; fenceVal_ = 0; ring_ = 0;
    for (auto& f : ringFence_) f = 0;
}

// ---- debug probe -------------------------------------------------------------------
// Snapshots a small centre region of the back buffer at present time. Submit-now / read-
// later with a 2-deep ring, exactly like the blitter uses the game's queue - the earlier
// version blocked the render thread on a fence wait INSIDE the present event, which crashed
// D3D12Core/ReShade; this never blocks. Returns the luma from a copy submitted a couple of
// presents ago, which is fine for an experiment measuring steady-state brightness.
namespace {
struct SampleRing {
    static constexpr UINT W = 128, H = 128, kRowPitch = W * 4;   // 512, 256-aligned
    static constexpr int  N = 3;
    ID3D12Device*              dev = nullptr;
    ID3D12CommandAllocator*    alloc[N]{};
    ID3D12GraphicsCommandList* list[N]{};
    ID3D12Resource*            rb[N]{};
    ID3D12Fence*               fence = nullptr;
    uint64_t                   fv = 0;
    uint64_t                   slotFence[N]{};
    bool                       slotPending[N]{};
    uint32_t                   slotTag[N]{};
    int                        cur = 0;
    DXGI_FORMAT                fmt = DXGI_FORMAT_UNKNOWN;

    void Destroy() {
        for (auto& l : list) if (l) { l->Release(); l = nullptr; }
        for (auto& a : alloc) if (a) { a->Release(); a = nullptr; }
        for (auto& r : rb) if (r) { r->Release(); r = nullptr; }
        if (fence) { fence->Release(); fence = nullptr; }
        dev = nullptr; fv = 0; cur = 0;
        for (int i = 0; i < N; ++i) { slotFence[i] = 0; slotPending[i] = false; }
    }
    bool Ensure(ID3D12Device* d) {
        if (dev == d) return true;
        Destroy();
        D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_READBACK};
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = kRowPitch * H; rd.Height = 1; rd.DepthOrArraySize = 1;
        rd.MipLevels = 1; rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        bool ok = SUCCEEDED(d->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
        for (int i = 0; i < N && ok; ++i) {
            ok = SUCCEEDED(d->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc[i]))) &&
                 SUCCEEDED(d->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc[i], nullptr, IID_PPV_ARGS(&list[i]))) &&
                 SUCCEEDED(list[i]->Close()) &&
                 SUCCEEDED(d->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                                      D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&rb[i])));
        }
        if (!ok) { Destroy(); return false; }
        dev = d;
        return true;
    }
    static float Luma(const void* mem, DXGI_FORMAT f) {
        const bool tenBit = f == DXGI_FORMAT_R10G10B10A2_UNORM || f == DXGI_FORMAT_R10G10B10A2_TYPELESS;
        const bool bgra = f == DXGI_FORMAT_B8G8R8A8_UNORM || f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        double sr = 0, sg = 0, sb = 0;
        for (UINT y = 0; y < H; ++y) {
            const uint32_t* row = (const uint32_t*)((const uint8_t*)mem + (size_t)y * kRowPitch);
            for (UINT x = 0; x < W; ++x) {
                const uint32_t v = row[x];
                if (tenBit) { sr += (v & 1023) / 1023.0; sg += ((v >> 10) & 1023) / 1023.0; sb += ((v >> 20) & 1023) / 1023.0; }
                else if (bgra) { sb += (v & 255) / 255.0; sg += ((v >> 8) & 255) / 255.0; sr += ((v >> 16) & 255) / 255.0; }
                else { sr += (v & 255) / 255.0; sg += ((v >> 8) & 255) / 255.0; sb += ((v >> 16) & 255) / 255.0; }
            }
        }
        const double n = (double)W * H;
        return (float)((sr + sg + sb) / (3.0 * n));
    }
};
SampleRing g_sr;
D3D12_RESOURCE_STATES g_probeSrcState = D3D12_RESOURCE_STATE_RENDER_TARGET;
} // namespace

D3D12_RESOURCE_STATES Dx12ProbeSrcState() { return g_probeSrcState; }
void Dx12SetProbeSrcState(D3D12_RESOURCE_STATES s) { g_probeSrcState = s; }

bool Dx12SampleBackbuffer(ID3D12CommandQueue* queue, ID3D12Resource* src,
                          uint32_t tagIn, uint32_t* tagOut, float* outLuma) {
    if (!queue || !src || !outLuma) return false;
    ID3D12Device* dev = nullptr;
    if (FAILED(src->GetDevice(IID_PPV_ARGS(&dev)))) return false;
    const bool ok = g_sr.Ensure(dev);
    dev->Release();
    if (!ok) return false;

    const D3D12_RESOURCE_DESC sd = src->GetDesc();
    if (sd.Width < SampleRing::W || sd.Height < SampleRing::H) return false;
    g_sr.fmt = sd.Format;

    const int slot = g_sr.cur;
    g_sr.cur = (g_sr.cur + 1) % SampleRing::N;

    // Read back this slot's PREVIOUS copy (finished long ago; no wait) before overwriting it.
    bool produced = false;
    if (g_sr.slotPending[slot] && g_sr.fence->GetCompletedValue() >= g_sr.slotFence[slot]) {
        void* mem = nullptr;
        D3D12_RANGE rr{0, SampleRing::kRowPitch * SampleRing::H};
        if (SUCCEEDED(g_sr.rb[slot]->Map(0, &rr, &mem))) {
            *outLuma = SampleRing::Luma(mem, g_sr.fmt);
            D3D12_RANGE wr{0, 0};
            g_sr.rb[slot]->Unmap(0, &wr);
            if (tagOut) *tagOut = g_sr.slotTag[slot];
            produced = true;
        }
        g_sr.slotPending[slot] = false;
    }

    // Record + submit a fresh copy into this slot (read next time this slot comes round).
    if (FAILED(g_sr.alloc[slot]->Reset()) || FAILED(g_sr.list[slot]->Reset(g_sr.alloc[slot], nullptr)))
        return produced;
    ID3D12GraphicsCommandList* cl = g_sr.list[slot];
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = src;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    // The state the back buffer is actually in during ReShade's present event. PRESENT
    // (== COMMON) crashed D3D12Core at +0x11f85 twice; ReShade transitions the back buffer
    // to RENDER_TARGET for its own effect pass, and a wrong StateBefore is UB. Overridable
    // so the two candidates can be A/B'd in one run.
    b.Transition.StateBefore = Dx12ProbeSrcState();
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    cl->ResourceBarrier(1, &b);
    const UINT x0 = (UINT)((sd.Width - SampleRing::W) / 2), y0 = (sd.Height - SampleRing::H) / 2;
    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = g_sr.rb[slot];
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Footprint = {sd.Format, SampleRing::W, SampleRing::H, 1, SampleRing::kRowPitch};
    D3D12_TEXTURE_COPY_LOCATION srcLoc{};
    srcLoc.pResource = src;
    srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    srcLoc.SubresourceIndex = 0;
    D3D12_BOX box{x0, y0, 0, x0 + SampleRing::W, y0 + SampleRing::H, 1};
    cl->CopyTextureRegion(&dst, 0, 0, 0, &srcLoc, &box);
    std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
    cl->ResourceBarrier(1, &b);
    if (FAILED(cl->Close())) return produced;
    ID3D12CommandList* lists[] = {cl};
    queue->ExecuteCommandLists(1, lists);
    const uint64_t fv = ++g_sr.fv;
    if (SUCCEEDED(queue->Signal(g_sr.fence, fv))) {
        g_sr.slotFence[slot] = fv;
        g_sr.slotTag[slot] = tagIn;
        g_sr.slotPending[slot] = true;
    }
    return produced;
}
}
