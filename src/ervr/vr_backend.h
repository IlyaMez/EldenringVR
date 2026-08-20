#pragma once
#include "vrmath.h"
#include <cstdint>

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
};

struct VRInputState {
    uint32_t buttons = 0;
    float indexTrigger[2]{};   // 0..1
    float handTrigger[2]{};    // grip 0..1
    float thumb[2][2]{};       // [hand][x,y] -1..1
    bool  controllerConnected[2]{};
};

struct VRTracking {
    Pose head;
    Pose hand[2];
    double time = 0;   // runtime time in seconds
};

class IVRBackend {
public:
    virtual ~IVRBackend() = default;
    virtual const char* Name() const = 0;
    // Try to attach to the runtime already living in this process (R.E.A.L. VR's).
    // Returns true once ready. Called repeatedly from a worker thread until it succeeds.
    virtual bool TryAttach() = 0;
    virtual bool IsReady() const = 0;
    virtual bool Poll(VRTracking& tracking, VRInputState& input) = 0;
    // amplitude 0..1, duration in seconds (best-effort; some runtimes only expose on/off).
    virtual void Haptic(Hand hand, float amplitude, float durationSec) = 0;
    virtual void Detach() = 0;
};

IVRBackend* CreateOculusBackend();
IVRBackend* CreateOpenVRBackend();

// Owned singleton picked from config; may be null until a runtime shows up.
IVRBackend* ActiveBackend();
void BackendsInit(const char* preference);   // "auto" | "oculus" | "openvr"
void BackendsTick();                          // called from worker thread
void BackendsShutdown();

// Thread-safe snapshot for consumers (XInput hook runs on the game's input thread).
bool GetLatestVRState(VRTracking& tracking, VRInputState& input);

// Head-position ownership. When enabled, the backend hides the HMD *position* from the host (R.E.A.L.) - it keeps
// receiving orientation, but sees the head sitting at the tracking origin - so the only positional offset in the
// rendered view is the one ERVR writes into the game camera. ERVR's own polling always sees the raw position.
// Returns false if the active backend cannot intercept the host's pose reads (then the host still adds its own offset).
void VRSetHostHeadPositionHidden(bool hidden);
bool VRHostHeadPositionHideSupported();
}
