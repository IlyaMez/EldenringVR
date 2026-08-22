#pragma once
// Copies the game's back buffer into an OpenXR swapchain image, on the game's own queue.
//
// Two paths, chosen per call:
//   * fast path  - a plain CopyResource / CopyTextureRegion, taken only when the XR image
//                  format is copy-compatible with the back buffer AND no colour or
//                  rectangle conversion is needed.
//   * shader blit - otherwise: sample the back buffer and draw it into the XR image. This
//                  is what does the format conversion (ELDEN RING's R10G10B10A2 against a
//                  runtime that only offers R8G8B8A8 / R16F), the sRGB->linear colour
//                  conversion, and the FOV crop.
//
// All work is submitted on the queue OpenXR is bound to, so the runtime's own
// synchronisation covers it and no CPU stall is needed before xrEndFrame.
#include <cstdint>
#include <d3d12.h>

namespace ervr {

// What the shader path does to the image on its way into the XR swapchain.
struct BlitParams {
    // Sub-rectangle of the source to stretch over the whole destination, in UV. Identity
    // copies the whole image; stereo uses it to crop the game's wide symmetric frustum
    // down to the runtime's asymmetric per-eye FOV (see stereo.cpp).
    float uvScaleX = 1.f, uvScaleY = 1.f;
    float uvOffsetX = 0.f, uvOffsetY = 0.f;
    // Source pixels are gamma (sRGB) encoded and the destination is interpreted as linear.
    // Without this the compositor gamma-encodes an already-encoded image and everything
    // comes out washed out - the classic "why is VR paler than the monitor".
    bool  linearizeSrc = false;
    // The format the destination must be VIEWED as. OpenXR hands back swapchain images
    // declared TYPELESS, so the resource itself does not say whether it is sRGB - only the
    // format the swapchain was CREATED with does, and guessing wrong silently skips (or
    // doubles) the hardware sRGB encode. Always pass the swapchain's declared format.
    DXGI_FORMAT dstFormat = DXGI_FORMAT_UNKNOWN;
    // Where the destination's alpha comes from. The world layers are opaque (the game's
    // alpha channel is not a coverage mask), but the HUD layer IS a coverage mask: it is
    // the UI drawn over a transparent clear, and the compositor blends it over the world.
    //   Opaque - force 1 (every projection/cinema blit)
    //   Src    - pass the source alpha through
    //   Luma   - derive it from brightness: anything not black is UI. Needed because the
    //            game's back-buffer format is R10G10B10A2 - two bits of alpha - so the
    //            redirected UI draws leave almost no usable coverage behind.
    //   Max    - max(src alpha, luma). Catches dark UI only if it has alpha, which on
    //            ELDEN RING it does not - the UI pipelines never write the channel.
    //   Dual   - the exact answer, and the HUD's default. The same UI is captured TWICE,
    //            once over a black clear and once over a white one (hud.cpp replays each
    //            draw into a second target). For any src-over blend:
    //                B = s*a          W = s*a + (1-a)
    //            so coverage is a = 1 - (W - B) and the premultiplied colour is B, with no
    //            alpha channel needed from the game at all. Opaque black UI - menu panels,
    //            text outlines - comes out opaque instead of invisible.
    enum class Alpha { Opaque = 0, Src, Luma, Max, Dual };
    Alpha alphaMode = Alpha::Opaque;
    // Multiplies the result. Applied to RGB as well, because a HUD captured over a
    // transparent clear is premultiplied - scaling only the alpha would brighten it.
    float alphaScale = 1.f;
    // The source is already a shader-readable texture we own (the HUD capture target), so
    // the staging copy can be skipped: the SRV is created straight on it. Also keeps the
    // staging ring from thrashing between two source formats every frame.
    bool  srcIsShaderResource = false;
    // Blend the result over what is already in the destination, premultiplied
    // (ONE, INV_SRC_ALPHA), instead of overwriting it. Used by [HUD] MirrorPanel to draw
    // the reconstructed panel onto the game's own frame exactly as the compositor would.
    bool  blendOver = false;
    // Alpha::Dual only: the same image captured over a white clear. Must match `src` in
    // size and format, and is assumed to be in the same resource state.
    ID3D12Resource* src2 = nullptr;
    bool  Identity() const {
        return !linearizeSrc && alphaMode == Alpha::Opaque && alphaScale == 1.f && !src2 &&
               !blendOver &&
               uvScaleX == 1.f && uvScaleY == 1.f && uvOffsetX == 0.f && uvOffsetY == 0.f;
    }
};

// Debug probe: synchronously copies a small centre region of `src` (expected in PRESENT
// state; R10G10B10A2 or R8G8B8A8/B8G8R8A8) to a readback buffer on `queue`, waits, and
// returns the mean RGB in [0,1] storage (gamma) space. Costs a queue flush per call - for
// experiments only, never the shipping path.
// `tagIn` is stored with the submitted copy and echoed back as `*tagOut` alongside the luma
// it produced, so the caller can pair each luma with the pass it was actually captured on
// despite the submit-now/read-later latency. Returns true only when a luma is produced.
bool Dx12SampleBackbuffer(ID3D12CommandQueue* queue, ID3D12Resource* src,
                          uint32_t tagIn, uint32_t* tagOut, float* outLuma);
// Which state the probe assumes the back buffer is in during the present event.
D3D12_RESOURCE_STATES Dx12ProbeSrcState();
void Dx12SetProbeSrcState(D3D12_RESOURCE_STATES s);

class Dx12Blitter {
public:
    bool Ensure(ID3D12Device* dev);
    // Copies src into dst, transitioning both from the given states and restoring them
    // (the shader path leaves dst in RENDER_TARGET, which is what OpenXR expects on release).
    bool Copy(ID3D12CommandQueue* queue,
              ID3D12Resource* src, D3D12_RESOURCE_STATES srcState,
              ID3D12Resource* dst, D3D12_RESOURCE_STATES dstState,
              const BlitParams& params = {});
    void WaitIdle();
    void Release();
    ~Dx12Blitter() { Release(); }

    static bool FormatsCopyCompatible(DXGI_FORMAT a, DXGI_FORMAT b);
    // True for the LDR UNORM formats a swapchain back buffer uses, whose bits are gamma
    // encoded. FLOAT formats hold linear light; _SRGB formats are decoded by the hardware.
    static bool StorageIsGammaEncoded(DXGI_FORMAT f);

private:
    ID3D12PipelineState* PipelineFor(DXGI_FORMAT rtvFormat, bool blend);
    bool EnsureShaderRoot();
    bool EnsureIntermediate(const D3D12_RESOURCE_DESC& srcDesc);
    bool ShaderBlit(ID3D12Resource* src, D3D12_RESOURCE_STATES srcState,
                    ID3D12Resource* dst, D3D12_RESOURCE_STATES dstState,
                    const BlitParams& params);
    bool BeginList(ID3D12GraphicsCommandList** out);
    void SubmitList();

    static constexpr int kRing = 3;
    static constexpr int kPso  = 6;    // one per (destination format, blend) in play
    ID3D12Device*              dev_ = nullptr;
    ID3D12CommandQueue*        queue_ = nullptr;      // set for the duration of one Copy
    ID3D12CommandAllocator*    alloc_[kRing]{};
    ID3D12GraphicsCommandList* list_ = nullptr;
    ID3D12Fence*               fence_ = nullptr;
    HANDLE                     event_ = nullptr;
    uint64_t                   fenceVal_ = 0;
    uint64_t                   ringFence_[kRing]{};
    int                        ring_ = 0;
    int                        lastSlot_ = 0;

    // Shader-blit pipeline (created lazily on first use).
    //
    // Descriptors and the staging texture are ringed alongside the command allocators:
    // the GPU reads a descriptor when the command list *executes*, so overwriting a
    // single slot every frame would race against frames still in flight.
    ID3D12RootSignature*  rootSig_ = nullptr;
    ID3D12PipelineState*  pso_[kPso]{};              // cached by RTV format, so alternating
    DXGI_FORMAT           psoFormat_[kPso]{};        // eye/quad blits do not thrash the PSO
    bool                  psoBlend_[kPso]{};
    ID3D12DescriptorHeap* srvHeap_ = nullptr;   // shader-visible, kRing slots
    ID3D12DescriptorHeap* rtvHeap_ = nullptr;   // kRing slots
    UINT                  srvStride_ = 0;
    UINT                  rtvStride_ = 0;
    ID3D12Resource*       inter_[kRing]{};      // SRV-able staging copy of the back buffer
    D3D12_RESOURCE_DESC   interDesc_{};
};
}
