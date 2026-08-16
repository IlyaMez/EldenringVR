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
};
GameERStatus GameERGetStatus();
}
