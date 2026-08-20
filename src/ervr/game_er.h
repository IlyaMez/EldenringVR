#pragma once
#include "vrmath.h"

namespace ervr {

// ELDEN RING integration: first-person camera takeover, head hiding, and
// controller-driven arm bones. Layouts/RVAs target exe 2.6.2.0 (game 1.16.2);
// hooks are only installed when the version matches or the AOB fallbacks hit.
bool GameERInit();
void GameERShutdown();

// Called from the ERVR worker at poll rate: refreshes VR pose snapshot used by the
// game-thread hooks and applies hot-reloaded config.
void GameERTick();

struct GameERStatus {
    bool versionOk = false;
    bool cameraHooked = false;
    bool skeletonHooked = false;
    bool playerFound = false;
    int  boneCount = 0;
    bool armsActive = false;
    long hkCalls = 0, hkSolves = 0, camCalls = 0;   // since last query
    double hkMs = 0, camMs = 0;                     // total ms spent in the hooks since last query
};
GameERStatus GameERGetStatus();
// Cheap "are we actually in the world?" check. Unlike GameERGetStatus it resets no counters,
// so it is safe to call from the input hook every frame.
bool GameERPlayerActive();
// Full stereo: write the second eye's camera between the two engine passes of one frame.
// No-op (false) unless UpdateCamera stashed a second-eye request this frame.
bool GameERApplySecondEyeCamera();
// Content-probe experiment: rewrite all camera slots as the last camera pitched by N degrees.
bool GameERDebugPitchCamera(float pitchDeg);
// The render camera's matrix+fov range (CSPersCam via FieldArea->GameRend), for the
// CamWatch guard-page probe. Null until in-game.
uint8_t* GameERCamWatchTarget(uint32_t* outLen);

// ---- camera ownership -------------------------------------------------------------
// With CameraYawSource=ervr the view yaw is ours, not the game's: virtual_pad feeds turn input in here and
// everything (camera matrix, body facing, movement basis) is derived from it, so nothing the game's own
// follow / lock-on camera does can drag the view around.
void GameERAddCameraYaw(float radians);
void GameERAddBodyYaw(float radians);     // RightStick=body: turn the character body only, the view stays put

struct MoveBasis {
    bool  active = false;    // first person is running and we have a player
    bool  lockedOn = false;  // a real enemy is locked on (the game owns the yaw then)
    float camYaw = 0;        // where the view points
    float chrYaw = 0;        // where the character actually faces (lags camYaw while it turns)
    float bodyYaw = 0;       // where ERVR is turning the body toward (head yaw by default)
    float headYaw = 0;       // HMD yaw relative to the tracking origin, game convention (positive = right)
    bool  headValid = false;
};
MoveBasis GameERGetMoveBasis();
}
