# Weapon Workshop

Implemented in the native launcher and VR runtime, 11 November 2026. Open the **Weapons** tab in `ERVR Launcher.exe`.

## Catalog

The launcher reads `regulation.bin`, weapon-name FMGs and item-icon archives from the installation selected on Play. It does not ship game assets or depend on a web catalog. Reading and icon decoding run on a background worker; visible icons are uploaded on the UI thread. The catalog currently displays English game names.

The verified installation yields **487 named weapons and shields**, including Shadow of the Erdtree equipment. This count is discovered, not hardcoded. Ammunition and unnamed/internal error rows are excluded. Reinforcement levels and affinities resolve through `originEquipWep` to one base weapon entry. Search names or numeric IDs, filter by family or customized assignments, or show equipped weapons when the updated VR runtime is connected. Missing icons have a visible placeholder and an error tooltip.

The reader supports the installed game's BND4/PARAM/FMG layouts and ZSTD, DFLT and KRAK DCX containers. It reports unsupported layouts instead of substituting a demonstration catalog. Names and icons reflect the selected installation, including loose message files when present. They do not promise to include modifications that exist only in a running mod loader's virtual filesystem.

## Editing and activation

1. Select a weapon. A preset is a named set of settings for that weapon. Use **Preset options** to create one for the weapon or its entire weapon type, or make a copy to experiment.
2. Open a group in **Feel & alignment**. Check only the settings you want to change. Unchecked values use your global ERVR settings and weapon-type estimates. Hover a setting or value for its meaning, your default, and a practical example. Angles and grip positions adjust your global calibration; estimated length does not resize the model or game hitbox.
3. **Action rules** are optional. Choose **When**, **Only if**, and **Do this**, then a hand if needed. Hover trigger and action choices for examples. **If rules overlap** contains priority: the highest matching number wins. Use **Try rules & live VR** to see the chosen action and matching rule without sending game input.
4. Check **Applies to**, choose either hand, left or right, then **Save & apply**. Release controls and hold still between actions to let the running mod apply it.
5. Expand **Currently used & preset details** to see the saved preset used by each hand, rename your preset, or add optional author credit. **Preset options > Discard changes** restores the saved version being edited. **Remove preset from this target** removes the selected target/hand assignment; a broader preset may then apply.

**Preset options > Save changes** saves without changing which weapons use the preset. New presets stay unused; weapons already using a preset receive its saved changes when controls are released and hands are still. **Make a copy** creates an independent preset for experiments. Switching weapons with unsaved changes offers save, discard, or keep editing. **Reload presets** preserves the current draft.

For example, if small wrist movements cause unwanted strikes, open **Physical strikes**, check **Speed to start a strike**, and try raising 0.8 to 1.2 m/s. If the weapon feels slow to catch up with your hand, open **Weight & handling**, check **Follow your hand**, and try raising 8 to 12 Hz. These are starting points to compare in VR, not required values. Global features such as weapon inertia and physical blocking must still be enabled for their settings to take effect.

An assignment selects one preset, in this order: exact weapon + hand, exact weapon + either hand, family + hand, family + either hand. The selected preset's unset fields inherit global settings. Presets do not merge their rule lists or tuning with other assigned presets.

## Supported tuning

| Group | Settings |
| --- | --- |
| Weapon alignment | Tilt, turn and twist; grip position; estimated hand-to-tip distance and balance |
| Weight & handling | How closely the weapon follows your hand, turning and stopping help, trailing angle and two-hand assistance |
| Physical strikes | Speed to start a strike, resting speed, slowdown time, longest strike and pause after a landed strike |
| Stance & swing detection | Start/reset speeds, heavy attack speed, minimum hand movement, time to judge a swing and gated recovery |
| Two-hand grip | How close to grab and the start/end of the grab zone |
| Weapon skills | Hold duration, stance hold animation, wait after entering stance, pause between follow-ups and aim limit |
| Shield blocking | Shield height and forward distance, release delay and deflect speed |
| Controller vibration | Combat vibration strength |

**Shield blocking** and the **Raise the shield** rule trigger appear only for weapon types recognized as shields by your ERVR settings (`ShieldWepTypes`). They do not appear for daggers or other non-shield weapons. Existing saved shield rules on a non-shield preset remain visible with an explanation so you can change or remove them.

**Weapon skills > Stance hold animation** lets each preset **Use default**, **Show while held**, or **Hide while held**. The default follows `[Combat] StanceHoldAnimation`. Hiding the animation arms a stance's follow-ups without holding the game's stance pose: for example, hold Moonveil's skill control freely, then swing to enter the stance and perform Transient Moonlight. Each follow-up adds the **Wait after entering stance** delay. This requires stance swings while on foot; charged skills and skills without follow-ups keep their normal behavior. Save and apply with the matching updated ERVR runtime, then release controls and hold still before trying it.

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

In **Preset options**, **Copy share code** copies an `ERVR1:` envelope containing UTF-8 JSON and a SHA-256 checksum. **Export preset file** writes that JSON to a chosen file. Both include the current draft. No hosting account or network service is involved. The checksum detects corruption; it is not an author signature.

**Import preset** accepts a pasted code, pasted JSON, or a file. Review the target, author, settings and rules, then **Add to my presets**. Import always generates a new local ID and never changes assignments. Unknown targets can be stored but cannot be activated through this catalog. Game assets, paths, telemetry and local assignments are not included in a shared preset.

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

**Try rules & live VR** is a draft rule evaluator plus local runtime readings. It is not a no-damage rehearsal mode, a game command bridge or confirmation that a hit landed. Detector tuning requires saving/applying and testing in VR.

## Validation and remaining extensions

`test_weapon_workshop` covers bounded imports, checksum corruption, threshold/rule validation, assignment precedence, concurrent editor conflicts, atomic snapshot publication, live reload, neutral activation, equipment cancellation, inactive imports, malformed-library retention and FMG parsing. An optional game-directory argument verifies the real catalog and representative icon decoding; a third argument checks every catalog icon.

The original broader design also proposed an in-headset editor, mesh/grip visualizer, recorded gesture rehearsal, pose-zone gestures, loadout bundles and an online gallery. Those are future extensions. This implementation provides the native launcher editor, local sharing, existing-detector rules and their runtime adapters. It has not been validated in a headset during this implementation session.
