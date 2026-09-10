#pragma once
#include "vrmath.h"
#include <atomic>
#include <cstdint>

namespace ervr {

// Keep the eye-to-head calibration for the skeleton's lifetime. A new stance changes
// the head height, not this reference; otherwise each capture leaves a new body gap.
struct HeadAnchorState {
    float restHeight = -1.f;
    Vec3 offset{};
    bool initialized = false;
    bool riding = false;
    double lastUpdate = 0;
    std::atomic<uint32_t> lastFrame{~0u};

    void Reset() {
        restHeight = -1.f; offset = {}; initialized = false; riding = false; lastUpdate = 0;
        lastFrame.store(~0u);
    }

    Vec3 Update(Vec3 root, Vec3 head, float eyeHeight, float bob, float smoothing,
                bool templatePose, double now, uint32_t frame, bool ridePose = false) {
        if (ridePose) {
            // The rider's model already includes the saddle height. Track its live
            // head exactly, even after a late model update in the same frame. Bob
            // suppression / standing EyeHeight must not lift us above the rider.
            offset = head - root;
            if (restHeight >= 0.f) offset.y += eyeHeight - restHeight;
            // Loading on Torrent must not calibrate standing height from a seated pose.
            initialized = true; riding = true; lastUpdate = now; lastFrame.store(frame);
            return root + offset;
        }
        const float height = head.y - root.y;
        if (restHeight < 0.f) restHeight = height;
        const Vec3 target{head.x - root.x, eyeHeight + bob * (height - restHeight), head.z - root.z};
        if (!initialized || riding) {
            offset = target; initialized = true; lastUpdate = now; lastFrame.store(frame);
        } else if (lastFrame.exchange(frame) != frame) {
            const float dt = Clamp((float)(now - lastUpdate), 0.f, 0.1f); lastUpdate = now;
            const float k = smoothing > 1e-3f ? 1.f - std::exp(-dt / smoothing) : 1.f;
            offset = offset + (target - offset) * k;
        }
        riding = false;
        // A captured VR pose has no animation bob to filter. Follow its height even
        // on a second call in this frame, when a stance capture may have just changed it.
        if (templatePose) offset.y = target.y;
        return root + offset;   // terrain/root motion is never smoothed
    }
};

} // namespace ervr
