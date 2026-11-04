#pragma once
// Stereo orchestrator. The frame loop here runs on the game's render thread, driven from
// ReShade's present event; the eye baton below is how it talks to the game thread.
//
// States:
//   Cinema   - the flat game copied onto a head-locked quad. Proves session, pacing, blit
//              and submission with zero stereo risk, and stays forever as the fallback for
//              menus, cutscenes and anything else where per-eye cameras are wrong.
//   DebugAlt - alternate-eye: each game frame renders one eye and both are submitted as a
//              projection layer. Real stereo depth, but the two eyes are from different
//              simulation times. VALIDATION ONLY - it proves the camera offsets, the FOV
//              crop and the projection submission are right. Not the product.
//   Full     - true stereo, both eyes at one simulation time (M3).
#include "reshade_addon.h"
#include "vrmath.h"

namespace ervr {

enum class StereoMode { Off = 0, Cinema, DebugAlt, Full };

void StereoInit();
void StereoShutdown();

// Render-thread callbacks from reshade_addon.cpp.
void StereoOnPresent(const Dx12Frame& frame);
void StereoOnDeviceLost();
// True while the screen is faded to (near-)black after being normal - a world transition
// (fast travel, death warp) is in progress and the frame pump must not run a second pass.
bool StereoFadeGuardActive();
void StereoOnSwapchainResize();
// Width/height of the one symmetric frustum that holds both eyes, from the runtime's latest
// FOV (0 until a VR frame has been located). The shape of the GameResShape=eye frame.
float StereoHmdFrameAspect();

// ---- the eye baton (render thread <-> game thread) ---------------------------------
//
// The render thread decides which eye the NEXT game frame must render and publishes the
// headset's eye geometry; the game thread latches that once per frame in UpdateCamera,
// applies it, and echoes back what it actually used. The render thread then blits into
// the echoed eye's swapchain and submits poses built from the echoed head pose - so it
// never has to guess how the two threads are phased, and a stalled or skipped game frame
// degrades into "submit the previous pair again" instead of a swapped-eye image.

struct StereoEyeRequest {
    int      eye = 0;        // 0 = left, 1 = right
    Vec3     offset;         // eye position relative to head centre, GAME convention, metres
    Quat     rot;            // eye orientation relative to head centre, GAME convention
    float    fovWrite = 0;   // radians to put in Cam_Fov, already in its own axis. 0 = leave alone
    uint64_t pair = 0;
    // Full stereo (M3): the frame is drawn twice - the game camera gets `offset` for the
    // real pass and `otherOffset` written between passes for the draw-only second pass.
    bool     haveOther = false;
    int      otherEye = 1;
    Vec3     otherOffset;
    Quat     otherRot;
};
// Game thread, once per game frame. False => render mono (cinema / not ready / menus).
bool StereoTakeEyeRequest(StereoEyeRequest& out);

struct StereoEyeReport {
    int      eye = 0;
    uint64_t pair = 0;
    float    fovY = 0;       // vertical FOV the game rendered with, radians. 0 = unknown -> cinema
    Vec3     headPos;        // head pose the frame was built on, XR (stage) convention
    Quat     headRot;
};
// Game thread, at the end of the UpdateCamera that consumed the request.
void StereoReportEye(const StereoEyeReport& r);

struct StereoStats {
    StereoMode mode = StereoMode::Off;
    bool  graphicsBound = false;
    long  frames = 0;         // submitted since the last read
    long  submitFails = 0;
    long  blitFails = 0;
    long  stereoFrames = 0;   // of those, projection-layer (i.e. actually stereo) frames
    long  eyeBlitL = 0, eyeBlitR = 0;   // successful per-eye blits (full mode: should match)
    long  eyeDesync = 0;      // report/pass eye mismatches (FIFO fell out of step)
    long  lateDrops = 0;      // late repeat reports bounced by the consumed-pair watermark
    long  hudFrames = 0;      // frames that carried the UI as its own quad layer
    // Presents that submitted a flat frame because the frame pump stopped producing second
    // passes (menus, cutscenes). Non-zero here while a menu is open is normal and correct;
    // it used to be "submit nothing at all", which froze the headset on the last pair.
    long  monoStalls = 0;
    float lastSubmitMs = 0;
    float cropU = 1, cropV = 1;   // fraction of the back buffer each eye actually uses
    float waitFrameMsAvg = 0, waitFrameMsMax = 0;   // xrWaitFrame on the game's frame thread, since the last read
    float headsetHz = 0;          // 1 / predictedDisplayPeriod; 0 until a frame has been waited
};
StereoStats StereoGetStats();
}
