# Changelog

All notable changes to ERVR. Versions are tags on `main`; each tag is built by the public
release workflow and published on GitHub Releases and Nexus Mods.

## Unreleased

### Game patch 1.17.2 (exe 2.7.2.0)
* RVA map for 2.7.2.0, derived from 2.7.1.0 by instruction signature (all 15 sites unique) and
  matching every address erfps2 and fromsoftware-rs publish for 1.17.2.
* WorldChrMan fields from +0x1E608 on moved by 0x10 in 1.17.2: the chr-cam pointer and the
  by-distance character list (enemy watcher) follow the build. The chr-cam pointer is now only used
  when it points at a camera, so an unmapped patch can no longer turn a camera write into a stray
  64-byte write.
* Transition null-call guard expects the 1.17 call slot (+0x448), so it stops logging false
  "NOT eaten" errors.

### Seamless Co-op
* Works with Seamless Co-op (`ersc_launcher.exe`); partners do not need ERVR.
* Connecting no longer double-steps the game's network: the second-eye pass stops re-running the
  network and session task groups (FrpgNet, SessionManager, WorldChrMan_ChrNetwork,
  NetFlushSendData...), which crashed a connect at the transition null-call site.
* Co-op connect guard: ERVR watches the game's multiplayer join state machine
  (`CSSessionManagerImp` lobby/protocol state) and logs every transition. While a session is being
  created, joined or left, the second eye holds and ERVR's camera/pose/combat writes stand down,
  as on a loading screen. Verified with a real join and leave.
* Co-op roster guard: another player's character appearing, leaving or loading holds the second
  eye for `[VR] Pass2CoopGraceSeconds` (4 s). Full stereo depth in between (`Pass2CoopSafe`, off:
  the SAFE set gave both eyes the same image in a session).
* The OpenXR session now ends when the game destroys its D3D12 device, instead of staying bound
  through shutdown: Virtual Desktop's runtime crashed during process exit with it still alive.
* Enemy watcher counts only HP drops the player caused (`ChrIns.last_hit_by`), so a partner's or a
  spirit ash's hit no longer spends your swing budget or fires your hit haptics
  (`[Combat] EnemyWatchOwnHits`, default on).

### Fixes
* Singleton lookup accepts a name string packed right after non-text data; `CSGaitem` (Ash of War
  cost lookup) was never found on 1.17.
* Frame-task probe: the readable-range check no longer wraps for sentinel pointers, which logged
  bursts of guarded access violations when the probe ran early in boot.

### Tooling
* `tools/game_mode.ps1`: switch the game folder between VR and flat render mods (RenoDX, DLSS
  Enabler / OptiScaler, ReShade effects) by renaming, nothing deleted.
* `tools/run_game.ps1 -Launcher` (e.g. `ersc_launcher.exe`); the autopilot setting is restored after
  every run.
* `tools/verify_game_version_map.py` checks any pair of mapped builds; `sync_game_launcher.ps1`
  accepts 2.7.2.0.

## 0.4.0 — 2026-09-10 — second public release

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
