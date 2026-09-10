# Changelog

All notable changes to ERVR. Versions are tags on `main`; each tag is built by the public
release workflow and published on GitHub Releases and Nexus Mods.

## 0.4.0 — 2026-09-10 — first public release

Native VR for ELDEN RING as a ReShade add-on with its own OpenXR session. Offline / EAC-disabled
only. Verified on exe 2.6.2.0 (patch 1.16.2) and 2.7.1.0 (patch 1.17.1).

### Rendering
* Own OpenXR session bound to the game's D3D12 device; Oculus (Link / Air Link), Virtual Desktop
  (VDXR) and SteamVR runtimes.
* `StereoMode=full`: true stereo, both eyes at the same simulation instant (second engine pass at
  frozen dt). `cinema`: the flat game on a head-locked quad, also the fallback for menus and cutscenes.
* HUD lifted out of the frame and composited as its own world-space quad at headset resolution
  (`[HUD]`, dual-capture coverage solve so black UI and menu dimming survive).
* Near-camera dither fade (`NearDither`) and an in-process 60 fps cap override (`[Patches] FpsTarget`).

### First person and body
* Head-bone camera anchor with headset rotation on top, ERVR-owned camera yaw (smooth or snap turn),
  follow-cam neutralization, head hiding, HMD look direction written to the game's aim matrix.
* `PoseMode=vr`: standing pose captured from the game and held, arms solved by 2-bone IK to the
  controllers, spine twist, body-follows-head shift, finger grips preserved from equipment.
* Torrent: seated pose, rider and camera anchored to the horse, stick turns rotate horse and view together.
* Movement facing: dash and roll follow the movement stick; strafe-flag body facing that leaves the
  game's lock-on camera alone.

### Combat
* Physical melee: swings fire the game's own `AttackBehavior` event on the character, so the weapon in
  your hand is the hitbox and damage, poise, status and stamina all come from the game.
* Kinetic damage: live param-row edits scale damage, poise and stamina with swing speed; continuous
  contact model, swing budget, flinch-once.
* Weapon weight and inertia: head-heavy weapons lag the wrist, two hands add torque.
* Two-handing by grabbing the hilt with the free hand (game's own chord, `arm_style` read back), free
  hand pinned to the shaft, two-hand aim and grip orientation.
* Ash of War without the animation: TAE event record and replay through the game's own event slots,
  FP/stamina costs from `SwordArtsParam`.
* Cast aim: spells, ranged skills and bows fly along the casting hand.
* Menu gate: physical combat stands down completely while any game menu is open.

### Input
* Every Touch control mapped to XInput with tap/hold layers; works with no physical gamepad.
* IAT patch of the game's ordinal XInput imports so overlays cannot sit between ERVR and the game.
* Left-stick corruption filter for Quest Pro / VDXR streams.
* Menu navigation: left stick as a held D-pad while a menu is open, menu state read from the game.
* Haptics: game rumble forwarded, hit-confirm and deflect pulses.

### Tooling
* Crash log with RTTI dump, `xrprobe.exe` OpenXR/D3D12 diagnostic, unattended run script,
  regression tests for camera anchoring, poses, action queue, virtual pad and game-version maps.
* Version gate with separate RVA maps per game build; other builds fall back to AOB scans.
