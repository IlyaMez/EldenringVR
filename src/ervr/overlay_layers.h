#pragma once
// The small quads ERVR draws itself (VROverlay in vr_backend.h): the belt's items, the weapon wheel and its
// cursor. Their owners compose a square image on the CPU (overlay_canvas.h) and say every update where it hangs;
// the render thread uploads whatever changed and fills the XR layers. One D3D12 command list carries every copy.
#include "vr_backend.h"
#include <cstdint>
#include <vector>

namespace ervr {
struct Dx12Frame;

// New content for overlay `which`: `side` px square, 8-bit sRGB of premultiplied linear colour
// (r | g << 8 | b << 16 | a << 24). Taken by swap. Any thread.
void OverlayPublishImage(int which, int side, std::vector<uint32_t>& rgba);
// Where it hangs now, in stage space (the image faces +Z), `sizeM` across. Call every update, hidden or not: an
// overlay whose owner stops refreshing it (controllers gone, a stall) disappears after 300 ms. Any thread.
void OverlayPublishPose(int which, bool visible, const Pose& pose, float sizeM);
// Render thread, before each xrEndFrame.
void OverlayLayersCompose(IVRBackend* vr, const Dx12Frame& f, VRSubmitLayer& layer);
void OverlayLayersReleaseGpu();   // render thread: the device is going away

}
