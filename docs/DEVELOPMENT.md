# ERVR development notes

How ERVR works inside the game, for modders and contributors: the hooks, the offsets, the reasoning behind
each choice and the knobs in `ERVR.ini` that steer them. Players want the [README](../README.md) and the
[players' guide](PLAYERS.md); every build's changes are in the [changelog](../CHANGELOG.md).

- [Architecture](#architecture)
- [Building and installing by hand](#building-and-installing-by-hand)
- [First person and hand-tracked weapons](#first-person-and-hand-tracked-weapons)
- [The HUD on its own floating quad](#the-hud-on-its-own-floating-quad-hud)
- [Physical melee](#physical-melee-swings-invoke-the-games-attack-event)
- [Two-handing and the Ash of War](#two-handing-by-grabbing-the-weapon-and-the-ash-of-war-without-its-animation)
- [Cast aim](#cast-aim-spells-ranged-skills-and-bows-fly-where-the-casting-hand-points)
- [Body haptics](#body-haptics-internals)
- [Hand aim (experimental)](#hand-aim-experimental)
- [Troubleshooting notes](#troubleshooting-notes-learned-the-hard-way)
- [Limitations](#limitations)
- [Verifying a release](#verifying-a-release)

## Architecture

```
                ┌──────────────────────── eldenring.exe ──────────────────────────┐
  Quest 3 ◄───  │ OpenXR runtime (Link / VDXR / SteamVR) ◄── ERVR.dll (own XrSession)│
  (Link)        │      ▲ poses, buttons, haptics              ▲ one image per eye  │
                │      │                                      │                    │
                │   dxgi.dll = ReShade (bundled) ── present ── ERVR add-on:        │
                │      capture D3D12 frame ── stereo pass ── HUD quad ── xrEndFrame │
                │   dinput8.dll (proxy) ── loads ERVR.dll ── XInput hook ── game    │
                └─────────────────────────────────────────────────────────────────┘
```

* **Loading**: `dinput8.dll` is a tiny proxy (forwards `DirectInput8Create` to System32) that `LoadLibrary`s
  `ERVR\ERVR.dll`. Standard ELDEN RING mod-loader route; if you already use a `dinput8.dll` loader, drop `ERVR.dll`
  into its mods folder instead.
* **VR runtime**: ERVR creates and owns its own OpenXR session (`vr_openxr.cpp`), bound to the game's D3D12 device
  and the queue ReShade presents on. Poses, buttons and haptics are polled on a worker thread at `PollHz`; the
  frame loop (`xrWaitFrame` / `xrLocateViews` / `xrEndFrame`) runs on the render thread. Whatever is the system's
  active OpenXR runtime (Oculus, VDXR, SteamVR) is what ERVR talks to.
* **Rendering**: `ERVR.dll` registers itself as a [ReShade](https://reshade.me) add-on (the bundled `dxgi.dll`).
  ReShade's `present` event hands it the game's back buffer, device and queue every frame; ERVR blits that into an
  OpenXR swapchain image and submits it. `StereoMode=cinema` shows the flat frame on a head-locked quad;
  `StereoMode=full` runs a second engine pass per frame from the other eye's camera with the simulation frozen
  (`Pass2*` in `[VR]`), so both eyes see the same instant. Menus and cutscenes fall back to the cinema quad.
* **Input**: `XInputGetState/Ex/SetState/GetCapabilities` are detoured (MinHook) in `xinput9_1_0`, `xinput1_3`
  and `xinput1_4`. The virtual pad is merged with any real gamepad (buttons OR'd, largest stick wins). With no real
  pad, user 0 is reported as connected.
* **Gestures**: linear velocity from the runtime; a rising edge over threshold arms a 70 ms window; the peak decides
  light vs heavy; per-hand cooldown; synthesized buttons are held for `PressDuration`.

## Building and installing by hand

Needs Visual Studio 2022 or newer with "Desktop development with C++" (CMake is bundled) and internet on the
first configure (it fetches MinHook, the ReShade headers, the OpenXR SDK and bHaptics' C/C++ SDK).

```powershell
.\build.ps1          # -> build\dist\{ERVR Launcher.exe, dxgi.dll, ReShade.ini, dinput8.dll, ERVR\ERVR.dll, ERVR\ERVR.ini}
.\install.ps1        # copies into ...\ELDEN RING\Game   (-GameDir to override, -Uninstall to remove)
```

`install.ps1` keeps an existing `ERVR.ini` and `ReShade.ini`, backs up a foreign `dxgi.dll` to `dxgi.dll.ervr-bak`,
and refuses to overwrite another mod loader's `dinput8.dll` (it tells you where to put `ERVR.dll` instead).
`build\dist\ERVR Launcher.exe` does the same from its window (it installs the `build\dist` beside it; started from
`build\Release` it finds `build\dist` too). The launcher's sources are in `src/launcher/`; `-DERVR_BUILD_LAUNCHER=OFF`
skips it.
For a manual DLL update, replace only `ERVR\ERVR.dll`; copying the bundled `ERVR.ini` resets your settings.

Then launch the game (offline, EAC disabled) and put the headset on. `Game\ERVR\ERVR.log` shows what happened:

```
INFO | ERVR 0.x.y starting ...
INFO | [xinput] hooked xinput9_1_0.dll!XInputGetState
INFO | VR runtime: openxr
INFO | [xr] runtime "Oculus" 1.x.x
INFO | Active VR backend: openxr
INFO | [reshade] registered as add-on (API version 20)
INFO | ReShade add-on live (D3D12 capture + XR submission active)
```

`Game\ReShade.log` is the other half: its first line must say `Initializing crosire's ReShade version '6.8.0.2155'`.
If ERVR.log never prints the `[reshade]` lines, `dxgi.dll` is not the bundled one.

Set `LogLevel=debug` to get a 5-second heartbeat with live trigger/stick values, handy to confirm the controllers
are being read before you blame the mapping.

**Already using ReShade?** Keep your own `ReShade.ini`, but the `dxgi.dll` must be an add-on-support build (6.8+).
**Already using a `dinput8.dll` mod loader** (ELDEN MOD LOADER etc.)? Keep it and put `ERVR.dll` in its `mods\` folder.
**Using flat render mods** (RenoDX HDR, DLSS Enabler / OptiScaler, ReShade effects)? They break the headset image:
RenoDX re-encodes the frame as HDR10, the DLSS Enabler wraps DXGI under ReShade, and effects run once per eye.
`.\tools\game_mode.ps1 -Mode vr` parks them (renames to `*.ervr-off`) and `-Mode flat` puts them back and parks
ERVR instead; `-Mode status` shows what is active. Nothing is deleted.

## First person and hand-tracked weapons

`[FirstPerson]` in `ERVR.ini`. ERVR takes over the game camera and the arm bones and renders the result through its
own OpenXR session.

* **Camera**: MinHook on the camera step function (RVA `0x3bc5e0`, erfps2's `CAMERA_STEP_UPDATE`). ERVR writes
  `CSPersCam1` (`[[FieldArea]+0x20]+0x20`, matrix at +0x10) and `ChrCam.pers_cam` (`WorldChrMan+0x1ECE0`) with
  position = the character's **head bone** (`CameraAnchor=head`, low-passed by `CameraAnchorSmoothing`, vertical
  = root + `EyeHeight` + `CameraBob` × the bone's own excursion), so neck, shoulders and arms are under the view
  whatever the run/turn animation does, instead of the skeleton being dragged under a root-anchored camera every
  tick (`CameraAnchor=root` + `BodyFollowsHead`, the old way, which the run animation fights and wins on some
  frames). Rotation = ERVR's yaw with the headset rotation composed on top (`CameraRotation=hmd`).
  The HMD look direction is written to `ChrIns+0x120` (`aim_view_mtx`) so spells/bows aim where you look.
  Our writes always happen *after* the game's own chr-cam / camera step has run, so the game never gets a
  first-person matrix as the input to its next update.
* **Play-space origin** (`Recenter`, off by default: it makes body and hands slide for `RecenterSeconds` after every real head move): the runtime reports head/hand positions relative to *its* origin, and
  everything we place from them is rotated by the camera yaw. Half a metre of "I'm standing off to the side"
  therefore becomes a camera that swings sideways every time you turn with the stick, arm targets that miss by
  the same amount, and a `BodyFollowsHead` shift pinned against its clamp. ERVR keeps its own origin that follows
  your head with a `RecenterSeconds` time constant and a `RecenterMaxRadius` hard limit: leaning and ducking still
  move the view, walking around the room is absorbed.
* **Body pose** (`PoseMode=vr`, default): on foot, the rig never plays a game animation. A standing pose is captured from
  the animation system once (at skeleton discovery, re-captured whenever you stand still for a second, so
  idle/crouch stances still come through as discrete poses) and written over the live Havok body pose on
  every pose-hook call. Stance changes preserve the initial head-to-eye height offset and update camera height
  immediately, so a new pose does not leave the body vertically detached. Terrain movement is never smoothed.
  The pose is yawed to the body yaw; the arm IK and spine twist go on top. Root motion is untouched:
  rolls, attacks and running still *displace* the character (and the camera rides along 1:1), they just never pose
  the rig or whip the camera around, because the head bone the camera anchors to no longer moves relative to the
  root. Finger joints retain the game's current equipment grip instead of freezing an occasional open-hand
  animation until the next stance capture. Menus such as Sites of Grace suspend stance capture until three seconds
  after closing, preserving the saved walking pose through sitting and stand-up. The VR body and its head anchor
  keep using that pose throughout. `PoseMode=game` is the old layered behaviour (animations play, IK fights them).
* **Torrent** (`mounted_rider.h`): `FindMountedTorrent` resolves the horse by the first identity that holds - the
  rider's pair handle (`CSRideNode` counter party), Torrent's own pair link back to the rider, or the last mount
  being ridden - because the all-links-at-once resolver found nothing on 1.17.2. "Seated" is the riding node
  state (5), or the mounted flag outside the mount/dismount states (3/7). A miss logs everything it read.
  `PoseMode=vr` takes a seated pose from the rider's own seat once it holds still (`SeatSettle`, judged upright
  against the reference pose's head-over-pelvis height) and keeps it across reloads of the same character. The
  seated body lives in the rider's own model matrix, upright about the pelvis (`UprightSeatRoot`), so camera and
  body share the game's placement on the saddle; the camera rides the seated head (never the gallop/jump
  animation's), low-passed in Torrent's physics frame (`RideCameraStabilizer`) and advanced only by the frame's
  final camera write (the model-pos post). Full-frame pass 2 re-places the rider on the horse at dt 0, so the
  second eye solves in the pass-1 placement and rebases into the pass-2 matrix, predicted from the last frame
  before that pass's rebuild (`RiderPassHold`). Controller arm IK and physical leaning remain active, and Torrent's
  own animation is unchanged. Mount/dismount transitions and `PoseMode=game` use the live rider pose. Standing eye
  height is not added above the saddle. The walking template is preserved and restored after dismounting; loading
  a save on Torrent never captures the seated pose as a walking stance. Spine twist, player facing pins and direct
  locomotion stand down throughout the ride.
  Right-stick turns and, with `BodyYaw=head`, physical turns (`RideHeadFollow`, `RideTurnsWithHead`) rotate Torrent
  and the seated rider together, once per simulation frame. The active horse's draw visibility and opacity are
  requested separately from `NearDither`; logs also report model position, scale and alpha for investigating
  missing geometry. Enabled flags alone do not prove that the mesh or shadow was rendered. Horse writes require a
  validated current rider/horse pair and stand down during summon/dismount animations and mount death. Combat
  sends no two-hand chords or learn presses from the saddle (their attack button is a mounted attack there);
  `[Combat] RideAttacks=buttons` turns a swing into the game's own mounted attack.
* **Body facing** (`FaceCamera` + `FaceCameraMode`): keeps movement in strafe mode so the arms can always reach.
  `strafe` (default with `PoseMode=vr`) only sets the ChrCtrl "strafing" flag and leaves the game's lock-on state
  alone. The old way (`lock`) forced `Chr_IsLockedOn` onto a fake far-away point every frame, which engages the
  game's **lock-on camera**, a camera writer ERVR does not hook: it orbits the view around the character while
  strafing, swallows right-stick turns, and follows animations. If the log warns about "foreign camera yaw", an
  unhooked writer like that is still active. `LockTargetDistance` only matters in `lock` mode. The flags are
  re-asserted from the pose hook as well, since the camera hook only runs at one point in the frame.
* **Follow-cam neutralization** (erfps2's full first-person set): while first person runs, every
  `ChrExFollowCam::follow` call gets `reset_camera=1` (no position interpolation), `camera_auto_rotation=0` (no
  turning behind a moving character), `locked_on_cam_offset=0` (no lateral lock-on framing), and the global camera
  **wall-recovery** flag is cleared each frame (the game otherwise pushes the camera out of geometry near walls:
  another writer that moved the view on its own).
* **Head hiding**: erfps2's part list: `PlayerIns+0x648 -> CSChrAsmModelIns+0x28 [27] -> CSModelIns+0x18 ->
  CSModelDispEntity+0x20 bit0` for parts 0,2,6,21..25 (+ sheathed 7,8,11,12) and the model-res masks at
  `PlayerIns+0x640` (+0x58 bit3, +0x49 bit3, +0x50 bit5).
* **Arms**: ERSkeletonMan's pose access: `ChrIns+0x190 -> +0x28 (behavior) -> +0x10 (or +0x60) -> +0x30 =
  hkbCharacter`; skeleton `[[hkb+0x90]+0x28]` (parent indices +0x20, count +0x28, names +0x30, ref pose +0x40);
  live pose `[[hkb+0x38]] + *(int*)(..+0x54)` as `hkQsTransform[bones]` (parent-relative). Timing point:
  `CS::NoUpdateInterface` vtable slot 10 (RTTI-located; slot replaced with a register-preserving stub), on
  every call (`ArmSolveEveryCall`): it fires several times per rendered frame and a later animation evaluation
  otherwise throws our pose away, most visibly while running.
  Each call: read pose, compute world transforms from the character root, solve
  2-bone IK per arm toward the controller (VR -> game frame: mirror Z; hand rotation = controller * calibration),
  write the three local quaternions back. Weapons/hitboxes hang off the hand bones and follow.
  The root position is the `ChrCtrl` model matrix (`ArmRootSource=model`): that is the transform the game draws
  the skeleton with, and while running it and the physics position are a frame of motion apart. The root
  rotation is the physics one (local -Z = facing, verified against the lock-on point; `modelmatrix` uses the
  matrix basis too). `BodyRotate` yaws the root bones by (body yaw - that facing); the residual animation yaw is
  taken out by the Spine twist, measured on the L/R clavicle line whose side is auto-calibrated against the root
  facing on the untouched pose (the Master bone's own axes are *not* the body's forward; aligning them spun the
  body 90 degrees).
  The completed VR root pose is converted into the native model-matrix frame, keeping head movement and hand
  targets consistent even while its rotation lags the physics facing. The engine retains control of the model
  matrix and its derived movement transforms.
  `BodyFollowsHead` shifts the root bones by the (clamped, re-centred) HMD offset so shoulders stay under the
  headset when you lean or step; the physics root stays put, hands get the unclamped offset. The shift is applied
  against the animation's own translation, remembered per bone, so running twice in a frame is a no-op instead of
  walking the skeleton away from the character; with `BodyAnchor=head` the head bone is measured on that
  *unshifted* pose first (measuring it on our own previous output made the shift alternate between full and
  zero on back-to-back calls).
* **Turning / movement**: ERVR owns the camera controller (`OwnChrCam`, `CameraYawSource=ervr`,
  `CameraTurnByErvr`): the game's follow cam keeps its own yaw state (it is what turned the view whenever the
  character walked at an angle) and writes it into the chr-cam from several places per frame (skipping one of
  them made the view flip-flop between its yaw and ours), so our matrix is re-asserted right after every game
  write we hook (follow-cam function, chr-cam update, camera step), and the debug dump counts any foreign yaw
  still found before a write. The view yaw is integrated from the right stick
  (smooth or snap), the chr-cam matrix is anchor + that yaw, the game moves the character relative to it, and the
  game's own camera step still runs on top. Locked on, the yaw follows the target. `Forward=head|hand|body`
  rotate the move stick if you want forward to be something other than the view yaw. `CameraYawSource=game`
  is the old read-it-back mode. The character itself is still driven through the game (stick, strafe flags,
  its own animation/collision/root motion); nothing of the locomotion is re-implemented.
  Dash and roll follow the movement joystick, using the same `Forward` reference as walking. The physics-facing
  pin yields to the game's turn for the action and its recovery; the visible VR body still follows `BodyYaw`.
* **Locomotion**: `[Locomotion] Mode=game` (default): the game moves the character from the virtual stick.
  `Mode=direct` (experimental, opt-in): ERVR integrates the horizontal position itself from the processed stick in
  the view frame (`RunSpeed`/`SprintSpeed`, sprint = B held), keeps the game's height, and turns the physics
  body to the view yaw, only while the stick is pushed, so rolls/attacks/ladders still displace normally. The
  game's collision is bypassed for that motion. `FeedStick` keeps handing the stick to the game so the run/strafe
  animations play (its own displacement is overwritten).
* **Body yaw** (`BodyYaw=camera`): the character body (fake lock-on facing, whole-skeleton yaw, spine twist,
  elbow poles) sits on the view yaw, instantly. `head` makes it chase the headset yaw with a
  `BodyYawFollowSeconds` time constant instead (shoulders come round when you turn your real body); `stick` is
  for `CameraYawSource=ervr` + `RightStick=body`. Locked on, the game aims the body.
* Version gate: separate RVA maps cover exe 2.6.2.0 (game 1.16.2), 2.7.1.0 (game 1.17.1) and 2.7.2.0
  (game 1.17.2). Each 1.17 map was derived from the previous executable by instruction signature and
  checked with `tools/verify_game_version_map.py OLD_EXE NEW_EXE`; the 2.7.2.0 one also agrees with every
  address erfps2 and fromsoftware-rs publish for 1.17.2. 1.17.2 also moved WorldChrMan's fields from
  +0x1E608 on by 0x10 (`chr_cam` +0x1ECE0 → +0x1ECF0), which `GameWorldChrManOffset` applies. Other versions use the existing AOB fallbacks and cannot register
  frame tasks through a guessed address. Keep any renamed game launcher synchronized with Steam's game files:
  running the old executable with 1.17's Torrent appearance assets can leave the horse missing even with its
  visibility flags enabled. Startup logs warn when the launcher differs from the adjacent `eldenring.exe`.
  AOB check script:
  see `tools/verify_signatures.py`.

## The HUD on its own floating quad (`[HUD]`)

**The problem.** In full stereo ERVR renders **one** very wide game frame per pass (the camera is widened to
roughly 110° vertical so that a single image can contain a whole eye's field of view), and each eye is a *crop* of
it. The HUD, though, is laid out in **screen space**, so the engine paints it along the edges of that widened
frame: 50°+ off-axis, half of it outside the eye crop, the rest smeared across the periphery at the wrong depth.
No camera setting fixes this. The HUD is not in the world; it is on the film.

**The fix.** Take it off the film. ERVR is already a ReShade add-on, which means it sees ELDEN RING's own D3D12
command stream. The moment the first UI draw of a frame is issued, ERVR swaps the render target underneath it for
a texture of its own, cleared transparent:

* the game's frame comes out with **no UI in it**: the eye crops are pure world, and nothing about the stereo
  path changes;
* ERVR's texture comes out holding **the UI alone, over transparency**;
* that texture is submitted as a second `XrCompositionLayerQuad`, blended over the projection layer by the
  compositor with `XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT`, at *headset* resolution, not the
  game's, so text is sharper on the panel than it ever was in the frame.

No shader is patched, no draw is skipped, no game code is hooked. The whole mechanism is one `OMSetRenderTargets`
per UI draw batch.

### Finding the UI draws

ERVR does not need to know which shaders draw the HUD. It counts draws **per render target**: everything after
the first `FirstDraw` draws into the frame's final target is UI. On ELDEN RING that first draw is the
tonemap/output pass and everything after it is Scaleform plus the sprite HUD, so `FirstDraw=1` is the default.

Which target that *is* changes mid-session, measured on 1.16.2 (2026-08-21) with `Probe=true`:

| | back-buffer draws / frame | where the UI is |
|---|---|---|
| boot logos, title screen | 3 | straight onto the back buffer (1 output blit + 2 UI batches) |
| in game | 1 | a full-size offscreen `R8G8B8A8` target takes 11 to 24 batches (133 with a menu open), then one copy to the back buffer |

`Target=auto` (the default) follows that: it starts on the back buffer, and after ~1 s with no UI found it
re-derives the target as the last full-size one the frame drew into, in both directions, so quitting to the
title moves it back.

If a game patch (or another add-on in the ReShade chain) moves things around, three knobs find it again without
leaving the headset:

| | |
|---|---|
| `[HUD] Probe=true` | Logs the frame's render-target census once a second: which target took how many draws, in what order, and which of them are back buffers. Read `FirstDraw` straight off it. |
| **Numpad 8** | Cycles `FirstDraw` 0 to 8 live. Buzzes *value + 1* times so you can count the current setting blind. |
| **Numpad 7** | HUD layer on/off: the instant undo if detection ever grabs a post-processing pass instead of the HUD. |
| **Numpad .** | Re-centres the panel on where you are looking (the only thing that moves it under `Lock=world`). |

### Getting the coverage right

Lifting the UI out is only half of it: the panel also has to know **which of its pixels are
UI**, and ELDEN RING gives no help at all: its UI pipelines never write the alpha channel.
Keyed on the captured alpha the whole HUD is invisible; keyed on brightness, every *black*
part of it is: text outlines, icon frames, the character portrait box, the dimming behind a
full-screen menu. That is why the first build made the inventory menu look like it simply
never appeared.

So `Key=dual` (the default) does not key at all; it **solves** for coverage. The same UI is
captured twice in the same frame: once over a transparent-black clear, once over an
opaque-white one (each UI draw is issued a second time into the second target). For any
src-over blend,

```
over black:  B = s·a            over white:  W = s·a + (1 − a)
       ⇒     a = 1 − (W − B)    and the premultiplied colour is B
```

which is exact, needs nothing from the game, and degrades sensibly for the other blend modes
the UI uses (an additive overlay comes out additive, a multiply-dim comes out as the dim it
is). Opaque black is opaque; half-transparent is half-transparent. It costs one extra
full-size render target and one extra draw call per UI draw, around 24 of them in the
world, 133 with a menu open, against a ~700-draw frame.

`luma`, `alpha` and `max` remain as fallbacks.

### Seeing it without a headset

`[HUD] MirrorPanel=true` composites the reconstructed panel back onto the game's own frame,
with the same premultiplied blend the compositor performs. What lands on the monitor is what
the headset gets, which makes a screenshot a regression test for the entire capture, and is
the only way an unattended run can check any of this. `[HUD] RedirectWithoutVR=true` goes
with it, since the redirect otherwise stands down when there is no session to submit into.

### If a menu does not appear

Worth knowing, because it looks like a HUD bug and is not one. The frame pump only runs a
second engine pass on frames where the game stashed a second-eye camera, and a full-screen
menu stops it doing that, so pass 2 simply stops arriving. With `PresentLag=1` the closing
branch used to wait for a pair that was never coming and submit **nothing at all**, which the
compositor answers by reprojecting the last frame: a frozen world, and no menu, for as long
as the menu was open. It now gives up after 120 ms and submits a flat frame plus the HUD
layer. The heartbeat counts these as `flat frames (no second pass)`, and the first one logs
`[stereo] no second pass for N ms`; both are normal while a menu is open.

### Placing it

`Distance` and `Width` together *are* the angular size: 2.4 m wide at 2.0 m away spans about 62°, against the
~110° the raw frame spans, which is the entire point. `Lock=body` (the default) is world-locked with a lazy
follow: the panel stays put while you look around and re-places itself once you turn past `FollowDeg` or walk
past `FollowRadius`. World-locked is deliberate: the compositor reprojects world-locked content perfectly at any
submit rate, while head-locked content has to move *with* the head between submits, and submissions run at one XR
frame per stereo pair. `Lock=head` (welded to the view) and `Lock=world` (dropped and left) are there if you want
them.

`Key=` controls where the panel's transparency comes from. The UI is captured over a transparent clear, so in
principle its own alpha is the coverage mask, but the game's back buffer is `R10G10B10A2`, i.e. **two bits** of
alpha, far too coarse for soft text edges. `luma` treats anything not black as UI, and the default `max` takes
whichever of the two claims more coverage.

`[HUD] Mode` is read **once, at startup**: watching the draw stream costs something on every draw the game
issues, so a session that does not want the HUD layer installs no hooks at all and pays nothing. Changing `Mode`
needs a restart; everything else in the section hot-reloads as usual, and Numpad 7 toggles the panel within a
session that started with `Mode=quad`.

**One expected side effect:** the monitor mirror loses the HUD as well, because the HUD genuinely is no longer in
the frame. That is the feature working. The redirect only runs while there *is* a live VR session to submit the
panel into, though: start the game with the headset not streaming and the UI is left in the frame where it is at
least visible, with one line in the log saying so.

## Physical melee: swings invoke the game's attack event

`[Combat]` in `ERVR.ini`. Elden Ring's melee is animation-driven: an attack animation carries a TAE event
(`AttackBehavior`, event id 1) that, every frame it is active, makes the game look up
`BehaviorParam_PC[1_0000_0000 + weaponVariationId*1000 + judgeId] -> AtkParam_Pc`, sweep the AtkParam's capsule
between two dummy-polys on the weapon model, and apply damage/poise/status/hitstop with a per-attack hit list.
The hitbox is not "on the weapon"; the event creates it. Since ERVR already moves the arm bones (and thus the weapon
dummy-polys) with the controllers, physical combat is:

1. **Never press R1/R2** (no attack animation), and
2. **call the AttackBehavior event ourselves** every frame while a swing is in progress.

Implementation (`src/ervr/combat.cpp`):

* Player TAE object: `ChrIns+0x190 -> +0x18 (CSChrTimeActModule) -> +0x18 = CS::CSChrTaeAnimEvent*` (fromsoftware-rs
  layouts; the object's vtable is checked against RTTI `CS::CSChrTaeAnimEvent` / RVA `0x2a34280` before use).
* Event execution: `CSChrTaeAnimEvent` vtable: slot 1 runs slot 2 then slot 3; slot 2 handles generic events and
  is a **no-op for events 1/2/5**; slot 3 dispatches the behavior events (event 1 → handler RVA `0x4266d0`, found by
  disassembling the exe). ERVR hooks and calls slot 3. Args = `{event_id, args*, from, to, is_first_execution,
  +0x1a u16 "tae version" (must be ≥ 0x1a or `Source` is ignored), +0x20 float* animation time}`; the
  `AttackBehaviorArgs` pointer is passed into the damage module as the attack-instance key, so it is kept in
  stable per-hand storage for the whole swing. For event 1 `args` is
  `{AttackType s32, AttackIndex s32, BehaviorJudgeID s32, DirectionType u8, Source u8 (1 = right hand, 2 = left), StateInfo s16}`
  (DSAnimStudio `TAE.Template.ER.xml`). ERVR calls the original slot-2 function once per rendered frame from the
  skeleton hook (`CS::NoUpdateInterface[10]`, after the arm pose is written) with `is_first_execution=1` on the
  first frame of a swing (new attack instance: hit list reset, BehaviorParam stamina cost) and 0 afterwards.
* Judge ids observed in 1.16 (from the learn log): R1 chain `0/10/20`, R2 `100` (`105` on some charged
  variants), left hand `0` with `Source=2`; these are the defaults, so nothing needs learning. `Source` is always
  forced to the swinging hand (the game's "default" source follows whichever hand HKS last attacked with, which
  goes stale after weapon swaps). While physical combat is active RB/RT are never sent (trigger/grip would play
  attack animations that move your arms); menus still get them.
* Judge ids can still be **learned** (`Learn=true`, opt-in `SaveLearned=true` writes them to the ini): slot 2 is MinHooked; while in `Mode=buttons` (or `auto` before anything is learned) a
  swing still presses RB/RT/LB, and the first `AttackBehavior` the game fires for the player within a second of that
  press is captured verbatim (all six fields) as the light/heavy/left template. `auto` then switches to physical.
  Values are logged (`[combat] learned ...`) so they can be pinned in the ini (`LightJudgeId=` etc.).
* Contact detection (worker thread, 250 Hz) uses the rendered tip displacement along the calibrated weapon
  shaft, including two-hand steering. The fallback uses controller velocity plus angular velocity crossed
  with the same shaft lever. With `KineticContinuous=true`, contacts start immediately at `ContactOnSpeed`
  (0.8 m/s) and end below `ContactOffSpeed` (0.25 m/s); wrist-driven arcs count, and short contacts survive
  until a game frame can process them. Damage still scales with speed, and the existing damage budget
  bounds each swing. Enemy and world contacts use the game's own attack capsules; this does not add
  rigid-body wall blocking to the weapon. The gated model uses `SwingOnSpeed`, `MinHandSpeed`, `ArmWindow`
  for light/heavy selection, and `Cooldown`. Rendered inertia does not add another weight-based startup delay.
* Weight keeps the main grip at the controller and resists bending the shaft around it. Head-heavy weapons
  resist more than balanced ones, two hands provide more torque, and settling does not spring past the target.
  Shaft twist stays under the main wrist's control. `InertiaMaxLagAngle` defaults to 0.6 radians (about 34 degrees),
  with a 45-degree hard cap for older INIs. The old translational spring settings are ignored.
* Not done yet: directional thrust vs slash, charged (hold) skills.

## Two-handing by grabbing the weapon, and the Ash of War without its animation

`[Combat] TwoHand=grab` (default). Put your free hand on the hilt of the weapon in your other hand (the controller
within `TwoHandGrabRadius` (0.18 m) of the weapon axis, between `TwoHandGrabMin` and `TwoHandGrabMax` metres along
it) and *squeeze its grip*. That's the whole gesture: the grip alone is the hold: you stay two-handed for exactly
as long as it stays squeezed, wherever the hands move, and opening it is the only release. The squeeze must be
fresh (a grip already closed when the hand arrives is ignored: open it and squeeze again), so guarding or holding
your own weapon never grabs. Closing the *right* hand on the *left* weapon two-hands that one instead.

**How the switch is made.** ERVR never writes `ChrAsm.arm_style` (`PlayerIns+0x638 → +0x08`: 0 empty, 1 one-handed,
2 left both hands, 3 right both hands; fromsoftware-rs). It sends the game's *own* chord: `TwoHandModifierButton` +
`LightAttackButton` for the right weapon, + `GuardButton` for the left (Y + RB / Y + LB in the default bindings),
modifier down 100 ms before the key, played on the virtual pad *after* the attack-button block, and reads
`arm_style` back as the authority. The game therefore does everything it does when you press the chord yourself:
the strength bonus, the two-hand attack rows, HKS state, the weapon re-attached. A chord the game refuses is retried
three times 0.9 s apart, then logged once and left alone until the wish changes (`[combat] two-hand: ...` and
`[combat] arm style A -> B` in the log). A manual Y + RB still works; ERVR only takes you *out* of two-handing when it
put you in. While two-handed:

* the free hand's arm **rides the weapon shaft** where it took hold (`TwoHandPinHand`): its target is the weapon
  hand's tracked grip plus the grab distance along the rendered weapon axis, so the hand stays on the
  haft while the greatsword catches up. `TwoHandPinOrient=true` keeps the free hand's wrist angle relative
  to the weapon when the grab registers, then turns it with the weapon. Release and re-grab to choose a
  new angle, or set it to `false` to follow that controller's live orientation;
* `TwoHandAim=true` lets the supporting position bend the weapon while the main wrist controls its twist
  around the shaft. Taking hold preserves the initial alignment, and release blends back to wrist aim;
* the free hand never swings on its own, and its grip button is the grab, not the skill (LT) it is mapped to;
* swings use the **two-handed attack rows**. They differ from the one-handed ones (`SlotLight2H`/`SlotHeavy2H`):
  with `Mode=auto` + `Learn=true` the first two-handed light / heavy swing presses the button once so the game plays
  its two-handed attack and the hook learns the row (`[combat] learned light-2h attack: judgeId ...`), pinnable via
  `TwoHandLightJudgeId` / `TwoHandHeavyJudgeId`; an unknown row falls back to the one-handed one.

**The Ash of War.** With both hands on the weapon, `TwoHandSkillButton` (default `RTrigger`: RB is blocked in
physical combat anyway, so the right trigger is free) fires the weapon's skill. `TwoHandSkill=game` just sends the
skill button. `TwoHandSkill=effect` (default) gives you **the effect without the animation**, and this is where
"effect" has a precise meaning: a skill's effect is a handful of TAE events its animation carries:
`AddSpEffect_WeaponArts` (event 331, `{SpEffect if enough FP, SpEffect if not}`: the weapon buff or the fizzle),
`AddSpEffect` (67 / 66: self-buffs), `BulletBehavior` (2: projectiles, spawned from the weapon's own dummy-polys,
which follow your hands), the FFX spawns (96/110/112/120), ids from DSAnimStudio's `TAE.Template.ER.xml`. ERVR
replays exactly those, at their recorded times, through the same `CSChrTaeAnimEvent` slots physical melee already
calls (slot 2 for generic events, slot 3 for behaviors), with a stable args copy per instance and
`is_first_execution` on the first frame. No animation plays, nothing locks you, no root motion.

* **Where the events come from.** The first time you fire a given weapon + ash, the press goes to the game
  (`SkillButton`, forced through the block) and both TAE hooks record every event the player runs for
  `SkillRecordSeconds`: id, a copy of the args, first/last execution time, `*animTime`. That recording is kept for
  the session (`[combat] skill recorded for weapon W arts A: ...` lists them; `Debug=true` prints each one as it is
  seen). Every later press replays it. Hold the trigger `TwoHandSkillRelearnHold` s to play it the game's way again
  and re-record (after swapping the ash, or if a replay misses something). Only ids in `SkillReplayEvents` are
  replayed; add one from the `skill rec:` lines if a particular skill needs it.
* **Attack skills** (Stamp, Lion's Claw, Square Off...). The skill's own `AttackBehavior` is *not* replayed at its
  recorded time: you are not swinging then. It **arms your next swing** for `SkillSwingWindow` seconds with the
  skill's attack row (`[combat] skill strike: judge ...`), so the strike you make *is* the skill's strike, kinetic
  scaling and all.
* **Costs are the game's.** The skill is resolved through the applied gem (`ChrAsm` gaitem handle → `CSGaitem`
  singleton table → the weapon gaitem's gem slot → the gem item → `EquipParamGem.swordArtsParamId`), else the
  weapon's own `swordArtsParamId`; `SwordArtsParam` (holder 82) gives the L2 FP / stamina cost (`+0x12` / `+0x09`).
  `SkillChargeFP` charges them after the first frame's events ran (331 decides "enough FP" itself) and refuses with
  a long buzz when FP is short; if the row is unreadable the FP the game took during the recording is used.
* Charged (hold) skills replay their tap version; stance skills' R1/R2 follow-ups are the plain rows.

## Cast aim: spells, ranged skills and bows fly where the casting hand points

Unlocked, ELDEN RING aims a bullet from the player's **aim view matrix** (`ChrIns+0x120`, which `[FirstPerson]
WriteAim` fills from your head) and the **lock-on target point**, and the cast animation's `SetBulletAimAngle`
TAE event (238) clamps that aim with limits and deadzones sized for a camera a few degrees off the body. erfps2
drives the same three things for its first-person camera; ERVR drives them from the **controller**:

- **What arms it.** Only the game's own aim events: `SetBulletAimAngle` (238) at a cast's start and
  `BulletBehavior` (2) at every bullet the player fires; the aim stays live `CastAimSeconds` after the last one.
  A press never arms it: guard and grip-mapped buttons are presses too, and forcing the lock on them made the
  game turn the body after the hand. Outside a live cast the head aims exactly as before.
- **Which hand casts.** The hand whose attack/skill button the game was handed last, within 3 s (RB/RT = the right
  weapon, LB = the left weapon unless it is a shield, LT = the skill; two-handed = that weapon's hand); else the
  hand holding a ranged weapon; else the right. That is a *live* cast. Your visible body stays
  where your head is: the game turns the physics character toward the lock target underneath, which PoseMode=vr
  hides (`CastAimTurnBody=true` steers the rig and the camera anchor after the arm instead; it swung and snapped
  back on every cast, so it is off). Outside a press the head aims as before: a merely held catalyst never aims (its ray as the
  lock target made the game turn the body after the hand while walking). The `CastAimWeaponTypes` classes (staff,
  seal, bow, crossbow, perfume bottle, throwing blade) only route buttons: a ranged class in the attacking hand
  lets RB/RT through `BlockAttackButtons`: with a staff, the trigger casts.
- **Where.** The aim matrix looks along the controller's ray *from the hand*, the fake lock target sits down the
  same ray (pitch included) with the lock-on state forced, and for the player's own 238 / 700
  (`EnableTwistModifier`) events the deadzones are zeroed and the limits raised to `CastAimLimitDeg`. The game's
  lock-target update (erfps2's `UPDATE_LOCK_TGT`, 0x716260) rewrites the player's lock state every frame, so a
  lock forced at the camera step is gone by the time the bullet spawns; ERVR hooks that update and re-forces the
  state right after it, during a live cast. A real lock-on still wins: the game aims at the enemy.
- **Knobs** (`[Combat]`): `CastAim`, `CastAimHand=auto|right|left`, `CastAimSeconds`, `CastAimAxis=aim|handle`
  (where the controller points, or up its handle, the staff's own direction), `CastAimPitchOffsetDeg`,
  `CastAimTurnBody`, `CastAimLimitDeg`, `CastAimWeaponTypes`, and `CastAimForceLock` (with `FaceCameraMode=strafe`
  only): forces the lock-on state onto the ray's far point during a live cast (on by default; `lock` mode has the
  state forced anyway).
- **Log.** `[combat] cast aim: the right hand holds a ranged weapon and aims`, `cast aim: right hand aims (light
  attack)`, with `Debug=true` the 238 rewrite (`SetBulletAimAngle limits ... -> ...`), and the heartbeat's
  `cast aim right hand (cast live)`.

## Body haptics internals

The player-facing side (what each device plays, the `[bHaptics]` switches) is in the
[players' guide](PLAYERS.md#body-haptics-bhaptics).

`ervr_haptics_test.exe` (built beside ERVR.dll, `build\Release`) plays all of it without the game: `map` lights
each part of each device in turn (tell us if a side is mirrored), `effects` plays every pattern with what it should
feel like, `--dry` prints the frames as text with no Player at all.

ERVR uses bHaptics' own C/C++ SDK 2.7.1: `bhaptics_library.dll` ships unmodified beside ERVR.dll and is loaded only
while `Level` > 0; see [THIRD_PARTY.md](../THIRD_PARTY.md). The Player knows ERVR by the workspace ID and API key of
its app on bHaptics' Developer Portal, built in from the `ERVR_BHAPTICS_APP_ID` / `ERVR_BHAPTICS_API_KEY`
environment variables or a git-ignored `bhaptics-app.local` (`app_id=...` and `api_key=...` lines); a build without
them logs `no app` and plays nothing.

## Hand aim (experimental)

`[Experimental] HandAim=true` rotates the game camera by the delta of the right controller's yaw/pitch while the right
grip is held. It needs the address of the camera's yaw/pitch floats, which changes with game patches, so it's driven
by config:

```
CameraAOB=<IDA-style pattern of an instruction that references the camera manager pointer>
CameraAOBOffset=3         ; where the rel32 lives inside that instruction
CameraAOBInstrLen=7
CameraChain=0x20,0x60     ; pointer chain from the manager to the camera struct
CameraYawOffset=..        ; float yaw (radians) inside that struct
CameraPitchOffset=..
```

Community tools (Cheat Engine tables, the Practice Tool's `FieldArea`/`CSCam` chain) publish these for each patch;
plug them in and check `[handaim]` lines in the log. Leave it off otherwise: it does nothing without valid values.

## Troubleshooting notes (learned the hard way)

* **The game imports `XINPUT1_4.dll` by ordinal** (2 = `XInputGetState`, 3 = `XInputSetState`), and overlays and
  wrappers commonly hook kernel32 `GetProcAddress`/`LoadLibrary`. Inline hooks alone were not on the game's call path; ERVR patches the
  IAT slots of `eldenring.exe` directly (see `[xinput] IAT patched` in the log) and resolves exports by walking
  the PE export table.
* `LogLevel=debug` heartbeat shows `XInputGetState calls N, modified N, fabricated N`: if `calls` is 0 the game is
  not reaching the hook; if the others are 0 the merge produced nothing.

## Limitations

* **Full stereo is expensive.** The engine's simulation step is a fixed 1/60 s, so a loop under 60 Hz slows the
  whole game down, and the second eye pass roughly doubles the GPU cost. Lower the game's resolution/settings
  before blaming the headset; `StereoMode=cinema` is the cheap fallback and always works.
* Visual/comfort options live in `[VR]` (`RenderScale`, `WorldScale`, `FovDeg`, `IpdMm`, snap turn). There is no
  in-headset settings menu yet: edit `ERVR.ini` or the launcher's Options tab; both hot-reload.
* While any game menu is open, physical combat stands down completely (`[Combat] MenuGate`, default on):
  no swing detection, no injected attacks, no guard hold, no two-hand chords, and the live param edits
  are restored. The equipment menu refuses weapon changes while the game sees combat state
  (“cannot change weapons now”); each strike also uses a fresh event-args identity so the game's
  attack bookkeeping never carries one address across strikes. With `Debug=true` a 1 Hz
  `[combat] menudiag` line dumps the action-request / damage-module state while a menu is up.
* Physical melee has no attack animation at all: enemy AI/hitstop cues that key off the player's *attack state*
  (not the hit) are absent, and the game may still allow rolling mid-swing. Damage scales with swing speed
  through the live param edits (`[Combat] Kinetic*`); the weapon's own R1/R2 rows are the reference.
* Game version: only exe 2.6.2.0 (patch 1.16.2), 2.7.1.0 (patch 1.17.1) and 2.7.2.0 (patch 1.17.2) have verified
  address maps. On any other build the AOB fallbacks cover part of the feature set and the rest logs a warning
  and stays off. After a Steam update, keep a renamed `start_protected_game.exe` in step with
  `tools\sync_game_launcher.ps1` (it refuses builds without a map).
  Headsets other than Quest 3 (Link / Air Link / Virtual Desktop) are untested by the author.
* Planned: a small in-headset status overlay, directional thrust vs slash, charged (hold) skills.

## Verifying a release

**Your antivirus may flag it.** `ERVR.dll` injects into a running game process, and `dxgi.dll` is ReShade's add-on
build, which is unsigned by design (it is the variant anti-cheat vendors do not whitelist). Both detections are
generic, not a finding about this code. You do not have to take my word for it. Release builds are produced by
[a public GitHub Actions workflow](../.github/workflows/release.yml) from the tagged commit and carry a signed
provenance attestation, so you can check that the download is exactly what this repository built:

```powershell
gh attestation verify ERVR-vX.Y.Z.zip --repo IlyaMez/EldenringVR
```

The bundled `dxgi.dll` is byte-for-byte the `ReShade64.dll` from ReShade's own `ReShade_Setup_6.8.0_Addon.exe`; its
SHA256 is in [THIRD_PARTY.md](../THIRD_PARTY.md) so you can compare it against a fresh download from
[reshade.me](https://reshade.me). Each release also publishes its own SHA256. Or skip all of it and build from
source: it takes a minute.
