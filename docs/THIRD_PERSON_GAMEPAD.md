# Third-person gamepad

This prototype is for seated, offline play on Steam with a regular gamepad and a Quest 3 over Virtual Desktop (VDXR). Elden Ring owns the follow camera, right-stick orbit, lock-on, movement, and combat. Head rotation and lean change the rendered view. Each eye gets its own OpenXR pose and projection, with both views drawn from the same game update.

## Try it

In ERVR Launcher, select **Options → Picture → Camera style → Third-person gamepad** before starting the game. This selects Full stereo. The equivalent setting is `[VR] CameraStyle=third-person` in `ERVR.ini`. Camera style is read at startup.

To center the view, choose **System → ERVR Settings → Recenter view**, face forward in your seated position, and close the menu. NumPad decimal also recenters when debug keys are enabled. BetterVR for Breath of the Wild inspired the explicit play-style choice and the way both eye views derive from the gameplay camera. Moving the camera-style switch into the game menu would be a separate change. [BetterVR Play Style menu](https://github.com/Crementif/BotW-BetterVR/blob/1.0.2/src/rendering/imgui_menus.cpp#L925) · [BetterVR stereo camera](https://github.com/Crementif/BotW-BetterVR/blob/1.0.2/src/game/botw/camera/stereo_view.cpp#L132)

## Camera and input

On Elden Ring 1.17.2 (executable 2.7.2.0), [game_er.cpp](../src/ervr/game_er.cpp) hooks the function that publishes the stock perspective camera to the renderer. It passes an aligned copy with the eye pose and FOV applied. The second, draw-only engine pass uses the same stock camera pose and tracking sample. The game keeps its original camera object for the next update. An earlier attempt to write camera objects after the camera step missed the renderer's second-eye input, as seen in the diagnostic captures.

[third_person_camera.h](../src/ervr/third_person_camera.h) contains the pose math. The mode preserves native gamepad input and bypasses the immersive first-person body, motion-controller and character-facing overrides. Existing INIs that omit `CameraStyle` still select the immersive camera.

## Observations and validation

- On upstream 0.9.3, Single-frame showed left-eye jitter in the initial headset session. That mode replays captured GPU commands for the other eye. The cause of the jitter is unknown.
- Full stereo with First Person enabled, hand tracking and physical combat disabled, and the character's head shown gave a more familiar camera distance. Right-stick orbit and head-driven character facing still felt wrong for seated play. There was a small artifact at foreground edges where visibility differed by eye. Its cause is unknown. That session delivered about 43–56 fresh pairs per second at 72 Hz.
- The prototype passed all 31 Release regression tests, including third-person pose math, gamepad routing, and the recenter menu action.
- The existing Windows desktop `ForcePass2` harness used fixed synthetic eye poses. Scripted gamepad input loaded Limgrave, orbited the camera in both directions, opened and closed the menu, and captured both render passes. A diagnostic 7° tilt appeared only in the intended eye. With the diagnostic tilt disabled, the nearer character shifted about 10 pixels between eyes in an 800-pixel-wide capture. Distant scenery shifted 0–1 pixels. This exercised the game's stereo draw and camera path without a headset runtime.
- The hardware smoke test used a Quest 3 at 72 Hz over VDXR. ADB through the [Machine Control Quest workflow](https://github.com/kzahel/machine-control/tree/main/platforms/quest) woke the attached headset and opened its Virtual Desktop client. Limgrave delivered about 55–62 fresh stereo pairs per second with zero recorded submit or blit failures during the measured gameplay intervals. The test used the headset itself for OpenXR submission. No virtual headset or Meta XR Simulator was used.

A seated playtest is still needed for head rotation and lean, lock-on and combat comfort, and the foreground-edge artifact.

## Source baseline and local build

The starting point was upstream [v0.9.3](https://github.com/IlyaMez/EldenringVR/releases/tag/v0.9.3), commit `b6c95423d97ccdf41b792c32db3cc5c3a0b221a2`. The baseline x64 Release build passed all 30 upstream tests. An offline launch loaded the source-built `dinput8.dll` and `ERVR.dll` (both checked against their build-output SHA-256 hashes) and submitted title-screen stereo frames to the Quest.

The configured Windows build used Visual Studio Community 2022, MSVC 19.44, and portable CMake 3.31.8 with the x64 `Visual Studio 17 2022` generator and `ERVR_BUILD_REGRESSION_TESTS=ON`. Windows SDK 10.0.22621.0 lacked Direct3D 12 interfaces used by ERVR. The build targeted SDK 10.0.26100.0 using Microsoft NuGet packages at version 10.0.26100.9457. The SDK root is the base package's `c/` directory, with the companion package's x64 `um` and `ucrt` library folders under `c/Lib/10.0.26100.0/`. The CMake cache points `FETCHCONTENT_SOURCE_DIR_*` at the seven dependency checkouts in `build/_deps/`, populated at the refs in [CMakeLists.txt](../CMakeLists.txt).

To rebuild and test with that preconfigured build tree:

```powershell
$cmakeBin = 'build/toolchain/cmake-3.31.8-windows-x86_64/bin'
$sdk = (Resolve-Path 'build/toolchain/winsdk-10.0.26100.9457/c').Path
$env:CMAKE_WINDOWS_KITS_10_DIR = $sdk
$env:WindowsSdkDir_10 = "$sdk\"
$env:UniversalCRTSdkDir_10 = "$sdk\"
$env:UCRTContentRoot = "$sdk\"
$env:TargetUniversalCRTVersion = '10.0.26100.0'
& "$cmakeBin/cmake.exe" --build build --config Release --parallel 6
& "$cmakeBin/ctest.exe" --test-dir build -C Release --output-on-failure --no-tests=error
```

The package is assembled in `build/dist/`. This build had no bHaptics application credentials, so bHaptics body output was unavailable.

## Local deployment record

On 2026-11-28 the source-built launcher was staged in `~/Documents/ERVR Source/` and took over the **ELDEN RING VR (ERVR)** desktop shortcut. The published launcher remained in `~/Documents/ERVR/`. The baseline deployment preserved the game INI and offline VDXR launcher settings. The prototype was then deployed with third-person selected.

Under `~/Documents/ERVR/Source Build Rollback/`, `third-person-2026-11-28_100950/` holds the source baseline backup and `2026-11-28_093242/` holds the published build backup. To restore a snapshot, close the game and launcher, then restore its `dinput8.dll` and `ERVR/ERVR.dll` to their corresponding paths under the game's install directory. Restore the desktop shortcut from the published build snapshot if returning to that launcher.
