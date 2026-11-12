# Weapon Workshop

Implemented in the native launcher and VR runtime, 11 November 2026. Open the **Weapons** tab in `ERVR Launcher.exe`.

## Catalog

The launcher reads `regulation.bin`, weapon-name FMGs and item-icon archives from the installation selected on Play. It does not ship game assets or depend on a web catalog. Reading and icon decoding run on a background worker; visible icons are uploaded on the UI thread. The catalog currently displays English game names.

The verified installation yields **487 named weapons and shields**, including Shadow of the Erdtree equipment. This count is discovered, not hardcoded. Ammunition and unnamed/internal error rows are excluded. Reinforcement levels and affinities resolve through `originEquipWep` to one base weapon entry. Search names or numeric IDs, filter by family or customized assignments, or show equipped weapons when the updated VR runtime is connected. Missing icons have a visible placeholder and an error tooltip.

The reader supports the installed game's BND4/PARAM/FMG layouts and ZSTD, DFLT and KRAK DCX containers. It reports unsupported layouts instead of substituting a demonstration catalog. Names and icons reflect the selected installation, including loose message files when present. They do not promise to include modifications that exist only in a running mod loader's virtual filesystem.

## Editing and activation

1. Select a weapon. Create a weapon preset or a preset for its family; keep multiple named versions or duplicate one.
2. Check settings to override them. Unchecked values use global ERVR settings and category geometry estimates. Angles and grip positions are corrections to global calibration. Geometry estimates are not extracted meshes.
3. In **Actions & triggers**, add enabled rules with an event, condition, action, hand and priority. Use **Test & live** to evaluate a draft against a chosen context without issuing game input.
4. Choose either hand, left or right for the assignment, then **Save & activate**. Release controls and hold still between actions to let the running mod apply it.
5. **Revert draft** reloads the saved version. **Clear this assignment** removes the selected scope/hand assignment; a less-specific assignment may then apply.

**Save only** creates an inactive preset if new. Saving a revision of an already assigned preset updates those assignments at the next neutral boundary. **Duplicate** creates an independent preset for experiments. Switching weapons with an unsaved draft asks before discarding it. Reloading a concurrently edited library preserves the draft.

An assignment selects one preset, in this order: exact weapon + hand, exact weapon + either hand, family + hand, family + either hand. The selected preset's unset fields inherit global settings. Presets do not merge their rule lists or tuning with other assigned presets.

## Supported tuning

| Group | Settings |
| --- | --- |
| Fit | Pitch/yaw/roll corrections; local grip X/Y/Z; grip-to-tip reach; balance point fraction |
| Weight | Angular response, acceleration, braking, maximum lag and two-hand assistance |
| Contact | Contact start/end speed, slowdown dwell, stroke duration and recovery after a hit |
| Gesture | Stance/gated swing start/end, heavy threshold, minimum hand speed, classification window and recovery |
| Grip | Support-hand radius and start/end of the hilt segment |
| Skill | Hold duration, stance settling time, follow-up gap and swing-aim limit |
| Guard | Shield height, forward distance, release delay and deflect speed |
| Feedback | Combat controller-haptic intensity |

Global feature switches remain authoritative. For example, inertia tuning requires inertia enabled, support grip requires grab mode, and shield tuning requires physical blocking enabled; its pose detector currently monitors the left-hand shield. Contact and gated/stance thresholds are separate controls. Heavy thresholds apply to gated/stance classification; kinetic melee continues to use its energy/arc classifier unless a rule explicitly selects light or heavy.

Fit affects visual hand/weapon calibration, support-grip geometry, projectile aiming and tip measurement. A newly applied preset resets inertial motion history so its pose correction does not count as a swung blade.

## Actions and triggers

| Existing detector / input | Actions |
| --- | --- |
| Blade contact or gated swing | Normal behavior, suppress, light, heavy |
| Swing while holding a stance | Normal behavior, suppress, light follow-up, heavy follow-up |
| Configured skill control | Normal behavior, suppress, game skill, recorded effect skill |
| Support-hand grab | Normal behavior, suppress |
| Shield-raised pose | Normal behavior, suppress |
| Cast/shoot buttons | Normal behavior, suppress |

Conditions are always, one-handed, two-handed, mounted or on foot, with an optional hand restriction. The highest matching priority wins. Equal-priority overlaps are rejected rather than depending on insertion order. Unsupported trigger/action pairs cannot be selected or imported.

The existing combat adapters still own input gating, stamina, damage and game animations. Light/heavy overrides select a right-hand or two-handed attack; an independent left-hand swing retains its left-hand attack. Recorded effect skills retain their existing two-handed/learning requirements. A rule does not enable a disabled global feature or create an unsupported spell/animation.

## Sharing and storage

**Copy share code** copies an `ERVR1:` envelope containing UTF-8 JSON and a SHA-256 checksum. **Export file** writes that JSON to a chosen file. No hosting account or network service is involved. The checksum detects corruption; it is not an author signature.

**Import preset** accepts a pasted code, pasted JSON, or a file. Review the target, author, overrides and rules, then **Save inactive copy**. Import always generates a new local ID and never changes assignments. Unknown targets can be stored but cannot be activated through this catalog. Game assets, paths, telemetry and local assignments are not included in a shared preset.

Presets and assignments live in:

```text
<Game>/ERVR/Presets/library.json
```

The schema uses `ervr.weapon-preset` / `ervr.weapon-library`, version 1. Fields are typed, range-checked and sparse. Imports reject unknown keys, unsupported actions/schema versions, duplicate keys, oversized documents and excessive nesting. Single presets are limited to 32 rules and 64 KiB JSON; libraries to 1,024 presets, 2,048 assignments and 4 MiB.

Saving takes an exclusive file lock, checks the library revision, flushes a temporary file and atomically replaces the old file. Concurrent changes report a conflict; reload before retrying. A malformed library is not overwritten by the editor. The runtime keeps its last valid snapshot if a later file is invalid.

## Runtime integration

- The game thread publishes validated canonical weapon IDs and families.
- A background worker reads the local library every 500 ms and compiles fixed-size settings/rules.
- The pad worker applies revisions only with still tracked hands, released controls and no active swing or held skill. An equipment swap involving a preset cancels held/queued actions and waits for neutral before rearming.
- With no assignment, existing ERVR settings and equipment behavior remain the baseline.
- Rendering and combat read immutable snapshots without file IO, allocation or waiting on the editor.
- Read-only diagnostics in `<Game>/ERVR/weapon-live.txt` expose equipped IDs, measured speeds and the melee rule decision. The editor treats data older than three seconds as disconnected. Diagnostics are local and are not included in sharing.

**Test & live** is a draft rule evaluator plus local runtime diagnostics. It is not a no-damage rehearsal mode, a game command bridge or confirmation that a hit landed. Detector tuning requires saving/activating and testing in VR.

## Validation and remaining extensions

`test_weapon_workshop` covers bounded imports, checksum corruption, threshold/rule validation, assignment precedence, concurrent editor conflicts, atomic snapshot publication, live reload, neutral activation, equipment cancellation, inactive imports, malformed-library retention and FMG parsing. An optional game-directory argument verifies the real catalog and representative icon decoding; a third argument checks every catalog icon.

The original broader design also proposed an in-headset editor, mesh/grip visualizer, recorded gesture rehearsal, pose-zone gestures, loadout bundles and an online gallery. Those are future extensions. This implementation provides the native launcher editor, local sharing, existing-detector rules and their runtime adapters. It has not been validated in a headset during this implementation session.
