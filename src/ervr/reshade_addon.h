#pragma once
// ERVR registers *itself* (ERVR.dll, loaded by the dinput8 proxy) as a ReShade add-on.
//
// ReShade sits in the game's dxgi.dll slot and IS the graphics API the game talks to, so
// this gives us the D3D12 device / queue / swapchain and a render-thread present callback
// without a single hook or AOB scan of our own. reshade.hpp is header-only and finds
// ReShade by scanning loaded modules, returning false when it is absent - so installation
// simply retries from the worker loop, like XInputHooksInstall() does.
#include <cstdint>
#include <dxgiformat.h>

struct ID3D12Device;
struct ID3D12CommandQueue;
struct ID3D12Resource;

namespace ervr {

// Everything the stereo layer needs about the frame being presented. Render thread only.
struct Dx12Frame {
    ID3D12Device*       device = nullptr;
    ID3D12CommandQueue* queue = nullptr;      // the queue ReShade is presenting on == the one XR must bind
    ID3D12Resource*     backBuffer = nullptr;
    uint32_t            width = 0, height = 0;
    DXGI_FORMAT         format = DXGI_FORMAT_UNKNOWN;
};

// Worker thread; idempotent, returns true once registered.
bool ReShadeAddonInstall(void* selfModule);
bool ReShadeAddonInstalled();
void ReShadeAddonShutdown();
// Size of the FIRST swapchain of the session, before any resize - the frame the game built
// its raster projection for, which it keeps after resizing larger. False until one exists.
bool ReShadeFirstSwapchainSize(uint32_t& w, uint32_t& h);

struct ReShadeStats {
    bool     registered = false;
    bool     d3d12 = false;        // false => ReShade is running on a non-D3D12 backend (we do nothing)
    long     presents = 0;
    uint32_t width = 0, height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    // Present timing per engine pass ([0] = pass 1, [1] = pass 2), since the last read: ERVR's
    // own present-event work (eye blits, XR submit, xrWaitFrame) and how long the real DXGI
    // Present then held the frame thread (monitor vsync / flip-queue back-pressure).
    float ervrMsAvg[2] = {}, ervrMsMax[2] = {};
    float dxgiMsAvg[2] = {}, dxgiMsMax[2] = {};
};
ReShadeStats ReShadeAddonGetStats();

// Present tracing for the M2/M3 pairing experiments: records the QPC timestamp and thread
// of every Present until stopped (or the buffer fills). Cheap enough to run for a second.
struct PresentSample { long long qpc; unsigned long tid; };
void ReShadeAddonPresentTraceStart();
int  ReShadeAddonPresentTraceStop(PresentSample* out, int maxOut);
}
