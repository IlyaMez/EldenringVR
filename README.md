# ERVR — motion controls for ELDEN RING VR

ERVR is a companion mod for **Luke Ross's R.E.A.L. VR** that adds **motion‑controller support** to ELDEN RING:
Touch/Index/WMR controllers become the game's gamepad, sword swings trigger attacks, game rumble goes to your hands,
and (experimentally) the camera can follow your hand for bows and spells.

R.E.A.L. already provides stereo rendering and **6DoF head tracking**. ERVR does not touch rendering — it rides on the
VR runtime R.E.A.L. has open inside the game process and feeds the game input.

```
                ┌──────────────────────── eldenring.exe ─────────────────────────┐
  Quest 3 ───►  │ LibOVRRT64_1.dll / openvr_api.dll ◄── RealVR64.dll (stereo, 6DoF)│
  (Link)        │          ▲ session captured by hook                              │
                │          │                                                       │
                │   ERVR.dll ── poll poses/buttons ── gestures ── virtual gamepad  │
                │          │                                                       │
                │   XInputGetState hook ◄──────────────── game input thread        │
                └────────────────────────────────────────────────────────────────┘
```

## Features

| Area | What you get |
|---|---|
| Controllers → gamepad | Every Touch control mapped to XInput (configurable). Works with **no physical gamepad** plugged in. |
| Motion attacks | Swing right hand → light (RB) or heavy (RT) attack by swing speed. Swing left hand → LB (parry/shield bash/offhand). |
| Haptics | Game rumble (`XInputSetState`) forwarded to the controllers; hit‑confirm pulses on gesture attacks. |
| Tap/hold layers | Menu tap = START, hold = BACK (map). Left‑stick click tap = crouch, hold+tilt = D‑pad (quick slots). |
| Movement options | Hand‑relative locomotion (move where the left controller points), snap‑turn pulses. |
| Hand aim (experimental) | Camera yaw/pitch follow the right controller while gripping — needs a camera pointer (see below). |
| Hot reload | Edit `ERVR\ERVR.ini` while playing; changes apply within a second. |

## Requirements

* ELDEN RING (Steam) with **R.E.A.L. VR** installed and working (`RealVR64.dll` in `Game\`).
  EAC must be off — it already is if R.E.A.L. runs for you.
* Meta Quest via Link/Air Link (**Oculus** path, what R.E.A.L. uses by default) *or* SteamVR (**OpenVR** path).
* To build: Visual Studio 2022+ with "Desktop development with C++" (CMake is bundled), internet on first configure
  (fetches MinHook and the OpenVR headers).

## Build & install

```powershell
.\build.ps1          # -> build\dist\{dinput8.dll, ERVR\ERVR.dll, ERVR\ERVR.ini}
.\install.ps1        # copies into ...\ELDEN RING\Game   (-GameDir to override, -Uninstall to remove)
```

Then launch the game exactly as you do for R.E.A.L. VR. `Game\ERVR\ERVR.log` shows what happened:

```
INFO | ERVR 0.1.0 starting ...
INFO | [xinput] hooked xinput9_1_0.dll!XInputGetState
INFO | [oculus] LibOVRRT64_1.dll found ...
INFO | [oculus] Captured ovrSession 0000... from R.E.A.L.
INFO | Active VR backend: oculus
```

Set `LogLevel=debug` to get a 5‑second heartbeat with live trigger/stick values — handy to confirm the controllers
are being read before you blame the mapping.

## Default controls (Quest Touch)

| Controller | Game |
|---|---|
| Left stick | Move (tilt while holding stick‑click = D‑pad) |
| Right stick | Camera (or snap‑turn pulses) |
| A / B | Interact / Dodge‑sprint |
| X / Y | Use item / Event‑two‑hand |
| Right trigger | Light attack (RB) |
| Right grip | Heavy attack (RT) — also the **hand‑aim** modifier |
| Left trigger | Guard (LB) |
| Left grip | Skill (LT) |
| Right stick click | Lock‑on |
| Left stick click (tap) | Crouch |
| ≡ menu (tap / hold) | Menu / Map |
| **Swing right hand** | Light attack; swing harder → heavy |
| **Swing left hand** | LB (parry / bash / offhand attack) |

Everything above is a key in `ERVR.ini`. Speeds are in m/s; the debug heartbeat and `[gesture]` lines in the log tell
you your actual peak speeds so you can tune `LightAttackSpeed` / `HeavyAttackSpeed`.

## How it works (for modders)

* **Loading** – `dinput8.dll` is a tiny proxy (forwards `DirectInput8Create` to System32) that `LoadLibrary`s
  `ERVR\ERVR.dll`. Standard ELDEN RING mod‑loader route; if you already use a `dinput8.dll` loader, drop `ERVR.dll`
  into its mods folder instead.
* **VR runtime** – R.E.A.L. owns the headset session. On the Oculus path only one `ovrSession` per process is allowed,
  so ERVR hooks `ovr_GetTrackingState` / `ovr_GetPredictedDisplayTime` / `ovr_GetSessionStatus` in
  `LibOVRRT64_1.dll`, captures the session handle R.E.A.L. passes, then calls `ovr_GetTrackingState`,
  `ovr_GetInputState` and `ovr_SetControllerVibration` on it. On the OpenVR path it grabs `IVRSystem` from the
  already‑initialised `openvr_api.dll` (or inits as a *background* app) and uses poses + legacy controller state.
  ERVR never shuts either runtime down.
* **Input** – `XInputGetState/Ex/SetState/GetCapabilities` are detoured (MinHook) in `xinput9_1_0`, `xinput1_3`
  and `xinput1_4`. The virtual pad is merged with any real gamepad (buttons OR'd, largest stick wins). With no real
  pad, user 0 is reported as connected.
* **Gestures** – linear velocity from the runtime; a rising edge over threshold arms a 70 ms window; the peak decides
  light vs heavy; per‑hand cooldown; synthesized buttons are held for `PressDuration`.

## First person + hand-tracked weapons (v0.2, ELDEN RING 1.16.2 / exe 2.6.2.0)

`[FirstPerson]` in `ERVR.ini`. ERVR takes over the game camera and the arm bones; R.E.A.L. keeps rendering.

* **Camera** – MinHook on the camera step function (RVA `0x3bc5e0`, erfps2's `CAMERA_STEP_UPDATE`). While first
  person is on the original is *not* called; ERVR writes `CSPersCam1` (`[[FieldArea]+0x20]+0x20`, matrix at +0x10)
  and `ChrCam.pers_cam` (`WorldChrMan+0x1ECE0`) with position = character root (`ChrCtrl+0x230` row 3) +
  `EyeHeight`, rotation = base yaw from the game's own chr-cam (stick / lock-on) — R.E.A.L. composes the HMD on top.
  The HMD look direction is written to `ChrIns+0x120` (`aim_view_mtx`) so spells/bows aim where you look.
* **Head hiding** – erfps2's part list: `PlayerIns+0x648 -> CSChrAsmModelIns+0x28 [27] -> CSModelIns+0x18 ->
  CSModelDispEntity+0x20 bit0` for parts 0,2,6,21..25 (+ sheathed 7,8,11,12) and the model-res masks at
  `PlayerIns+0x640` (+0x58 bit3, +0x49 bit3, +0x50 bit5).
* **Arms** – ERSkeletonMan's pose access: `ChrIns+0x190 -> +0x28 (behavior) -> +0x10 (or +0x60) -> +0x30 =
  hkbCharacter`; skeleton `[[hkb+0x90]+0x28]` (parent indices +0x20, count +0x28, names +0x30, ref pose +0x40);
  live pose `[[hkb+0x38]] + *(int*)(..+0x54)` as `hkQsTransform[bones]` (parent-relative). Timing point:
  `CS::NoUpdateInterface` vtable slot 10 (RTTI-located; slot replaced with a register-preserving stub).
  Each call: read pose, compute world transforms from the physics module root (`+0x70` pos, `+0x50` quat), solve
  2-bone IK per arm toward the controller (VR -> game frame: mirror Z; hand rotation = controller * calibration),
  write the three local quaternions back. Weapons/hitboxes hang off the hand bones and follow.
* Version gate: RVAs are used only when the exe is 2.6.2.0; otherwise erfps2's AOBs are tried. AOB check script:
  see `tools/verify_signatures.py`.

## Troubleshooting notes (learned the hard way)

* **Virtual Desktop**: R.E.A.L. runs on the Oculus path via VD's shim, which is injected as
  `VirtualDesktop.LibOVRRT64_1.dll` (not the stock name) and returns 0 from `ovr_GetConnectedControllerTypes`
  even though input works. ERVR finds the runtime by its exports and infers "connected" from live data.
* **The game imports `XINPUT1_4.dll` by ordinal** (2 = `XInputGetState`, 3 = `XInputSetState`) and R.E.A.L. hooks
  kernel32 `GetProcAddress`/`LoadLibrary`. Inline hooks alone were not on the game's call path; ERVR patches the
  IAT slots of `eldenring.exe` directly (see `[xinput] IAT patched` in the log) and resolves exports by walking
  the PE export table.
* `LogLevel=debug` heartbeat shows `XInputGetState calls N, modified N, fabricated N` — if `calls` is 0 the game is
  not reaching the hook; if the others are 0 the merge produced nothing.
* R.E.A.L.'s LoadLibrary hook refuses `vrclient_x64.dll` loads from foreign threads early on, so the SteamVR
  background fallback is delayed/rate limited.

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
plug them in and check `[handaim]` lines in the log. Leave it off otherwise — it does nothing without valid values.

## Limitations / roadmap

* **Rendering is R.E.A.L.'s.** Head 6DoF, alternate‑eye rendering, FOV, comfort options — all configured in
  R.E.A.L.'s menu, not here. ERVR cannot show your hands or weapons in‑world; the character animates as with a pad.
* Oculus path requires R.E.A.L. to actually have created its session (log shows "Captured ovrSession"). If R.E.A.L.
  runs in a mode that never loads `LibOVRRT64_1.dll` or `openvr_api.dll` in‑process, ERVR has nothing to attach to.
* Motion attacks map to button presses; the game decides the animation. Directional swings → directional attacks would
  need animation‑graph hooks (v2 idea).
* Planned: hand‑aim presets per game version, weapon‑bone tracking (draw the weapon where your hand is via `ChrIns`
  bone overrides), a small in‑headset status overlay.

## Credits & inspiration

* [Halo‑MCC‑VR](https://github.com/pancreations/Halo-MCC-VR) and
  [cyberpunk‑vr‑port](https://github.com/dariulone/cyberpunk-vr-port) for the "controllers → engine input, gestures →
  actions" structure. Those projects also *render* natively; here rendering is intentionally left to R.E.A.L.
* Luke Ross — R.E.A.L. VR.
* [MinHook](https://github.com/TsudaKageyu/minhook), [OpenVR](https://github.com/ValveSoftware/openvr).

## Disclaimer

Offline / EAC‑disabled use only. Anti‑cheat and online play are not supported and never will be.
