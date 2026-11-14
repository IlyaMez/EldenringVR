#pragma once
#include "vrmath.h"
#include <cstdint>
#include <dxgiformat.h>

struct ID3D12Device;
struct ID3D12CommandQueue;
struct ID3D12Resource;

namespace ervr {

enum Hand { HandLeft = 0, HandRight = 1 };

// Controller buttons, normalized across runtimes (Oculus Touch naming).
enum VRButton : uint32_t {
    VRB_A          = 1u << 0,   // right, lower
    VRB_B          = 1u << 1,   // right, upper
    VRB_X          = 1u << 2,   // left,  lower
    VRB_Y          = 1u << 3,   // left,  upper
    VRB_LThumb     = 1u << 4,   // left stick click
    VRB_RThumb     = 1u << 5,   // right stick click
    VRB_Menu       = 1u << 6,   // left "≡" button
    VRB_LTrigger   = 1u << 7,   // digital trigger (derived from analog)
    VRB_RTrigger   = 1u << 8,
    VRB_LGrip      = 1u << 9,
    VRB_RGrip      = 1u << 10,
    VRB_DpadUp     = 1u << 11,  // Steam Frame left controller D-pad
    VRB_DpadDown   = 1u << 12,
    VRB_DpadLeft   = 1u << 13,
    VRB_DpadRight  = 1u << 14,
    VRB_LBumper    = 1u << 15,  // Steam Frame bumpers (shoulder buttons)
    VRB_RBumper    = 1u << 16,
};

struct VRInputState {
    uint32_t buttons = 0;
    float indexTrigger[2]{};   // 0..1
    float handTrigger[2]{};    // grip 0..1
    float thumb[2][2]{};       // [hand][x,y] -1..1
    // Trackpads (Valve Index): where the thumb is ([hand][x,y] -1..1) and how hard it presses (0..1).
    // Zero on controllers without one; what counts as a press is the virtual pad's call ([Buttons] PadForce).
    float pad[2][2]{};
    float padForce[2]{};
    bool  controllerConnected[2]{};
    bool  frameController = false;   // the runtime picked the Steam Frame controller profile
};

struct VRTracking {
    Pose head;
    Pose hand[2];
    double time = 0;   // runtime time in seconds
};

// ---- frame plane -------------------------------------------------------------
// Only stereo.cpp calls any of this; the game-side subsystems never see it.

enum VREye { EyeLeft = 0, EyeRight = 1 };

// Signed half-angles from the view axis, radians - XrFovf's shape (left/down negative).
struct VRFov { float angleLeft = 0, angleRight = 0, angleUp = 0, angleDown = 0; };

// One eye's view, in the ervr convention (right-handed, Y-up, -Z forward), in stage space.
struct VREyeView { Pose pose; VRFov fov; };

struct VRFrameInfo {
    int64_t  displayTimeNs = 0;    // XrTime: what LocateViews and the camera must predict to
    double   displayTimeSec = 0;
    bool     shouldRender = false; // XrFrameState::shouldRender
    uint64_t pairIndex = 0;
    int64_t  displayPeriodNs = 0;  // XrFrameState::predictedDisplayPeriod = the headset's refresh interval
    float    waitMs = 0.f;         // how long xrWaitFrame held the calling (game) thread
};

// Where a quad hangs. Cinema and the HUD both use this.
//   Body  - world-locked with a lazy follow: it stays put for small head movements and
//           re-anchors when the head has turned or walked away far enough. The compositor
//           reprojects world-locked content perfectly at any submit rate, which view-locked
//           content cannot be, so this is the comfortable default.
//   Head  - view space, rigidly in front of the eyes.
//   World - anchored once and left there, until the player recentres it.
enum VRQuadLock { QuadLockBody = 0, QuadLockHead, QuadLockWorld };

// Small quads ERVR draws itself (overlay_layers.h), placed in stage space by their owner - the belt's
// four pouch items at the waist or in the hand holding one, the weapon wheel at the hand that opened
// it, the cursor showing where that hand is over it, and the body calibration's panel with a ring at
// each controller. Alpha-blended like the HUD (premultiplied), submitted after it in this order, so the
// wheel draws over a belt item and the calibration over everything.
enum VROverlay {
    OverlayBelt0 = 0, OverlayBelt1, OverlayBelt2, OverlayBelt3, OverlayWheel, OverlayWheelCursor,
    OverlayCalibPanel, OverlayCalibRingL, OverlayCalibRingR, OverlayCount
};
struct VROverlayQuad {
    bool  valid = false;
    Pose  pose;                     // centre and orientation; the image faces +Z
    float widthM = 0.f, heightM = 0.f;
};

// What EndFrame composes. A frame may carry a projection layer, the flat cinema/menu quad,
// the HUD quad, the overlays, or any combination - submitted in that order, back to front.
struct VRSubmitLayer {
    bool projectionValid = false;   // stereo: views[] are the poses/fovs the eyes were RENDERED with
    VREyeView views[2];
    bool  quadValid = false;        // flat layer: cinema mode, menus, mono fallback
    float quadDistanceM = 2.5f;
    float quadWidthM = 4.0f;        // height follows the quad image's aspect

    // The UI, lifted out of the frame by hud.cpp and composited on top of everything else.
    // Its image has real coverage in the alpha channel, so unlike every other layer here it
    // is submitted with source-alpha blending.
    bool  hudValid = false;
    float hudDistanceM = 2.0f;
    float hudWidthM = 2.4f;         // height follows the HUD image's aspect
    float hudHeightOffsetM = 0.f;   // raise/lower the quad relative to eye level
    float hudYawDeg = 0.f, hudPitchDeg = 0.f;
    int   hudLock = QuadLockBody;
    float hudFollowDeg = 25.f;      // Body lock: head turn that re-anchors the quad
    float hudFollowRadiusM = 0.5f;  // Body lock: head travel that re-anchors it
    bool  hudRecenter = false;      // one-shot: drop the anchor where the head is looking now

    VROverlayQuad overlays[OverlayCount];

    // Every image this layer references has already FINISHED on the GPU (its producer saw the fence). Only
    // meaningful for a session bound with its own submit queue (BindGraphics ownSubmitQueue): the runtime syncs
    // its compositor to the bound queue, so a finished layer is submitted with nothing to wait on; an unfinished
    // one first has the submit queue wait for everything the game's queue holds - the plain binding's behaviour.
    bool gpuComplete = false;
};

class IVRBackend {
public:
    virtual ~IVRBackend() = default;
    virtual const char* Name() const = 0;

    // -- lifecycle (worker thread; retried until it succeeds, like XInputHooksInstall) --
    virtual bool TryAttach() = 0;      // instance + system only; no graphics yet
    virtual bool IsReady() const = 0;
    virtual void Detach() = 0;

    // -- graphics binding (render thread, once the D3D12 device is known) --
    // Creates the session and swapchains. Safe to call repeatedly; a no-op once bound.
    // ownSubmitQueue: bind the session to a queue of our own on `device` rather than to `queue`. At every
    // xrEndFrame the runtime fences the BOUND queue and makes its compositor wait for that fence (VDXR, 2026-11-12)
    // - on the game's queue that is all the work the game has queued by then, the next frame's included, so the
    // headset waits on frames it is not even showing. `queue` stays the game's: unfinished layers bridge to it.
    virtual bool BindGraphics(ID3D12Device* device, ID3D12CommandQueue* queue, bool ownSubmitQueue = false) = 0;
    virtual bool GraphicsBound() const = 0;
    virtual bool HasOwnSubmitQueue() const { return false; }
    // The game is about to destroy this D3D12 device. A session bound to it must end now,
    // while the device and its queue still exist; BindGraphics re-creates it on the next one.
    virtual void UnbindGraphics(ID3D12Device* device) { (void)device; }
    virtual bool GetRecommendedEyeSize(uint32_t& w, uint32_t& h) const = 0;
    // The formats the swapchains were CREATED with. The D3D12 images the runtime hands back
    // are typically TYPELESS, so these are the only authority on sRGB-ness and the blitter
    // needs them to build a correct render-target view.
    virtual DXGI_FORMAT EyeFormat() const = 0;
    virtual DXGI_FORMAT QuadFormat() const = 0;
    // Per-eye images, sized to what we can actually deliver rather than to the runtime's
    // recommendation: the game renders one wide frustum and only a sub-rectangle of it
    // belongs to an eye, so asking for the recommended size would upscale a crop and pay
    // for pixels that carry no information. No-op unless the size really changed.
    virtual bool EnsureEyeSwapchains(uint32_t w, uint32_t h) = 0;
    // Cinema quad, sized to the game window. No-op once created.
    virtual bool EnsureQuadSwapchain(uint32_t w, uint32_t h, DXGI_FORMAT backBufferFormat) = 0;
    // HUD quad. Separate from the cinema quad because it needs an ALPHA channel the
    // compositor will honour, and because in full stereo both can be live at once
    // (the cinema quad during a menu-only mono frame, the HUD quad over the world).
    virtual bool EnsureHudSwapchain(uint32_t w, uint32_t h) = 0;
    virtual DXGI_FORMAT HudFormat() const = 0;
    // Overlay quads (VROverlay). 8-bit sRGB preferred, so an image composed on the CPU copies
    // straight in; OverlayFormat says what the runtime actually gave.
    virtual bool EnsureOverlaySwapchain(int which, uint32_t w, uint32_t h) = 0;
    virtual DXGI_FORMAT OverlayFormat(int which) const = 0;

    // -- frame loop (render thread only; stereo.cpp is the sole caller) --
    virtual bool WaitBeginFrame(VRFrameInfo& out) = 0;                       // xrWaitFrame + xrBeginFrame + event pump
    // The panel's refresh rate in Hz (XR_FB_display_refresh_rate), 0 = unknown. Unlike the
    // predicted display period it does not halve when the runtime throttles a slow app.
    virtual float DisplayRefreshHz() const { return 0.f; }
    virtual bool LocateViews(int64_t displayTimeNs, VREyeView out[2]) = 0;   // BOTH eyes at the SAME time: the true-stereo contract
    // The same two views expressed RELATIVE TO THE HEAD (VIEW space) - i.e. the headset's
    // IPD geometry, with where the head happens to be divided out. This is what the game
    // thread needs: it owns its own head pose (latched on its own cycle) and just has to
    // displace the camera by the eye offset, so the two threads never have to agree on an
    // instant in time.
    virtual bool LocateEyeOffsets(int64_t displayTimeNs, VREyeView out[2]) = 0;
    virtual ID3D12Resource* AcquireEyeImage(VREye eye) = 0;                  // acquire + wait; null on failure
    virtual void ReleaseEyeImage(VREye eye) = 0;
    virtual ID3D12Resource* AcquireQuadImage() = 0;
    virtual void ReleaseQuadImage() = 0;
    virtual ID3D12Resource* AcquireHudImage() = 0;
    virtual void ReleaseHudImage() = 0;
    virtual ID3D12Resource* AcquireOverlayImage(int which) = 0;
    virtual void ReleaseOverlayImage(int which) = 0;
    // While on, Release*Image keeps the image acquired and queues the release; FlushDeferredReleases performs
    // them. A producer whose images are written on a queue the session is not bound to releases them only once
    // its own fence says they are finished - a released image is what the next xrEndFrame shows. An image
    // still held makes the next Acquire* of that swapchain fail rather than break the runtime's call order.
    virtual void SetDeferredRelease(bool on) { (void)on; }
    virtual void FlushDeferredReleases() {}
    virtual bool EndFrame(const VRSubmitLayer& layer) = 0;

    // -- pose/input plane (worker thread; unchanged, consumers untouched) --
    virtual bool Poll(VRTracking& tracking, VRInputState& input) = 0;
    // amplitude 0..1, duration in seconds (best-effort; some runtimes only expose on/off).
    virtual void Haptic(Hand hand, float amplitude, float durationSec) = 0;
};

IVRBackend* CreateOpenXRBackend();

// Owned singleton picked from config; may be null until a runtime shows up.
IVRBackend* ActiveBackend();
void BackendsInit(const char* preference);   // "openxr" (only value today; "auto" accepted)
void BackendsTick();                          // called from worker thread
void BackendsShutdown();
void BackendsOnDeviceDestroyed(ID3D12Device* device);   // any thread; see IVRBackend::UnbindGraphics

// Thread-safe snapshot for consumers (XInput hook runs on the game's input thread).
bool GetLatestVRState(VRTracking& tracking, VRInputState& input);
}
