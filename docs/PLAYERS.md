# ERVR players' guide

ERVR puts ELDEN RING in VR: an image for each eye, your head as the camera, the weapons in your hands, and
melee where the swing you make is the attack. This guide has the setup, every control and the fixes. It ships
in the download as `ERVR\README.md`; the newest copy is on
[GitHub](https://github.com/IlyaMez/EldenringVR/blob/main/docs/PLAYERS.md).

Questions and bugs: the `#ervr-help` channel on [Discord](https://discord.gg/x27ZXDpdNt). Release news is
posted in `#ervr-updates` first.

## Weapon presets

Open **Weapons** in the launcher to browse the actual weapon names and icons from your installed game,
including DLC. Search a weapon, then in **Preset options** choose **New for this weapon** or **New for this
weapon type**, open a group in **Feel & alignment** and check the settings you want to change; hovering a
setting explains it with an example. **Action rules** adds conditional rules for ERVR's contact, stance,
skill, support-grip, guard and cast controls. **Try rules & live VR** previews rule decisions and shows
local VR diagnostics when the updated mod is running.

Use **Save & apply**, then release your controls and hold still between actions to apply the preset in
VR. Unchecked settings inherit your normal ERVR options. **Preset options > Save changes** also updates a
preset already in use; **Make a copy** to experiment independently. **Remove preset from this target**
falls back to a broader weapon-type preset or your global settings.

**Preset options > Copy share code** or **Export preset file** shares one preset. **Import preset** reviews
a code or file, then **Add to my presets** saves a new unused copy; pick it on the matching weapon and apply
it yourself. Your library is `<Game>\ERVR\Presets\library.json`. The catalog is read from your game files;
no weapon assets are included in exports. See [Weapon Workshop](WEAPON_PRESETS.md) for settings,
precedence and supported actions.

## Before you start

> **Offline or Seamless Co-op only.** ERVR writes the game's memory, so it runs with Easy Anti-Cheat off.
> Never use it with EAC on or in FromSoftware's online play: that risks a ban on your account. The launcher
> starts the game with EAC off for you. [Seamless Co-op](#seamless-co-op) is the multiplayer that works.

You need:

- **ELDEN RING on Steam**, patch 1.16.2, 1.17.1 or 1.17.2 (game exe 2.6.2.0, 2.7.1.0 or 2.7.2.0). A new
  patch needs an ERVR update; the launcher tells you when yours has no address map yet.
- **A PC VR headset with an OpenXR runtime**: Meta Quest through Quest Link or Air Link, Virtual Desktop
  (VDXR), or SteamVR (Index, Steam Frame, Vive, Pico, Steam Link and others). Played by the author on Quest 3 and Quest
  Pro; other headsets should work and reports are welcome.
- **A gaming PC.** ERVR renders the game twice per frame, once for each eye, so it costs about twice the GPU
  of playing flat.
- Optional: bHaptics devices and the [bHaptics Player](https://www.bhaptics.com/software/player/) for
  [body haptics](#body-haptics-bhaptics).

ERVR needs no other mod. It does not use R.E.A.L. VR; if that was installed, the launcher parks its leftover
files for you.

## Install and play

1. Download **ERVR-v….zip** from [Releases](https://github.com/IlyaMez/EldenringVR/releases/latest), under
   *Assets*, and unpack it to a folder you can write to, such as `Documents\ERVR`. Not *Source code* and not
   the green *Code* button: those are the source, with no launcher in them.
2. Start **ERVR Launcher.exe**. If Windows says it protected your PC, choose *More info*, then *Run anyway*.
3. The **Play** tab checks this PC: the game in your Steam libraries and its patch, the OpenXR runtimes, the
   headset (*Check* asks the runtime), ERVR in the game folder, and flat mods in VR's way. Fix anything it
   marks.
4. Connect your headset and press **START VR**. The launcher installs or updates ERVR in the game folder,
   parks the flat render mods, turns off the game's depth of field, motion blur and ray tracing, starts Steam
   if needed, and starts the game with EAC off (through Seamless Co-op when it is installed). `ERVR.log`
   shows on the Play tab as the game runs.
5. The first time you play, a panel in the headset asks you to calibrate your body: stand tall, look ahead,
   stretch both arms straight out to the sides (a T-pose) and hold both triggers until the bar fills, about a
   second and a half. The Tarnished's arms then take your arm's length, so his hands sit on your controllers
   with his elbows bending as yours do, and the belt hangs at your waist. B skips it. The launcher's Options >
   Body > *Body calibration* shows your height and arm length; its *Redo* asks again at the next moment of
   play, mid-game too. In the headset, the game's System > ERVR Settings tab has the same *Body calibration*
   row.

**Updates.** From 0.7.2 on, the launcher asks GitHub when it starts whether a newer ERVR is out. If one is, the
Play tab says so: **Update now** downloads it, checks it against the release's SHA-256 and puts it in place of the
launcher's files in a few seconds, then the launcher starts again; START VR copies the new ERVR into the game
folder, keeping your `ERVR.ini`. *Later* asks again next time. About > Updates has *Check now* and turns the
start-up check off. A launcher unpacked into the game folder itself, or built from the source, is updated by hand.

The launcher's other tabs:

- **Options** edits the game folder's `ERVR.ini` in place: the view, resolution, the HUD panel, turning,
  combat, body haptics. ERVR takes each change within a second, mid-game too. Its *Game graphics* card picks
  which of the three game effects START VR turns off (all three by default).
- **Controls** lists the default bindings.
- **About** has *Play flat* (parks ERVR and puts your flat mods back), *Remove ERVR*, and desktop shortcuts.

What it changes: ERVR's own files in the game folder (`dxgi.dll`, `ReShade.ini`, `dinput8.dll`,
`LICENSE-ReShade.txt` and the `ERVR` folder), the flat mods it parks (renamed to `*.ervr-off`, never deleted)
and those three values in the game's own `%APPDATA%\EldenRing\GraphicsConfig.xml` (the original is kept beside
it as `GraphicsConfig_ervr_backup.xml`). Nothing is written while the game runs.

Prefer to install by hand? Unpack the zip straight into `ELDEN RING\Game`, next to `eldenring.exe`, and start
the game offline with EAC off yourself.

## Controls

Defaults for Quest Touch controllers. Every button is a key in `ERVR.ini` (`[Buttons]`, `[Gestures]`,
`[Combat]`), and the launcher's Controls tab shows the same list.

### Moving

| Control | What it does |
|---|---|
| Your head | The camera: look around, lean, duck |
| Left stick | Walk where you look (Options > Moving changes what forward is) |
| Right stick | Turn smoothly, or in snaps (`[Movement] SnapTurn`) |
| B | Dodge; hold to sprint |
| A | Jump; confirm in menus |
| Left stick click | Crouch |
| Left stick click held + tilt | D-pad: switch spells, items and weapons in the quick slots |
| Right stick click | Lock on |
| T-pose, both triggers held | Calibrates your body, when the panel asks; B skips it. Until you let go, the triggers, grips, A and B do nothing in the game |

### Fighting

| Control | What it does |
|---|---|
| Swing the right hand | Attack with the right weapon. The swing is the hitbox: faster hits harder, a full windup hardest. Behind an enemy that can be backstabbed, the swing is the game's backstab |
| Swing the left hand | Attack with the left weapon |
| Shield raised in front | Block with a shield in the left hand; moving the shield into a hit deflects it |
| Left trigger | Guard |
| Free hand on the hilt, squeeze its grip | Two-hand the weapon; open the grip to let go |
| Right trigger | The skill (Ash of War), held for as long as you hold it: stances (swing for the follow-up) and charged skills work. Two-handed, a tap plays the skill's effect without its animation |
| Left grip, quick squeeze | Skill (a tap) |
| A grip held, hand still | The weapon wheel: that hand's weapons around it; let go over one to switch to it, in the middle to keep yours |
| A grip closed at your belt | Take that pouch item; X uses it, opening the hand puts it back |
| Cast with a staff, seal or bow | The trigger casts; the spell or arrow flies where the casting hand points |
| X / Y | Use the item in the quick slot (or the belt item in your hand) / interact (the game's event action) |

With physical melee on, the triggers and grips never press the game's attack buttons: those would play attack
animations that pull your arms. A staff, seal or bow in the attacking hand lets the trigger through, so it
casts (and the right trigger is no skill button in that hand). Two swings are the game's own on purpose: a
swing while you hold the skill (a stance's follow-up), and a swing behind an enemy that can be backstabbed.

**Two-handing.** Put your free hand on the hilt of the weapon in your other hand and squeeze its grip. You
stay two-handed for as long as the grip stays squeezed, wherever your hands move, and opening it lets go. The
squeeze must be fresh: a grip already closed when the hand arrives is ignored, so guarding never grabs.

**The Ash of War.** Hold the right trigger and the game holds the skill with you, for exactly as long as you
hold it: a stance waits for your swing (a real swing is its light follow-up, a hard one its heavy; let the
blade settle before the next one), a charged skill lets go when you do. A stance's projectile, like Moonveil's
wave, flies where your swing aimed: slash across an enemy and it flies at that enemy. Drawing the sword back to
wind up is not a follow-up; the swing after it is. A skill that fires the moment you pull the trigger, like
Reduvia's blood blades, flies where your weapon arm aims. Two-handed, a tap is quicker: the first time you tap a weapon's skill, ERVR lets
the game play it and records what the skill does; every later tap plays that effect at once, with no animation
and no lock. An attack skill arms your next swing instead, so the strike you make is the skill's. Hold the
trigger past a third of a second for the game's own skill, held (after swapping the ash, say).

**Backstabs.** Swing at an enemy's back, close and facing it, and the game plays its backstab, with its own
damage. ERVR checks the game's own critical table for that enemy before it lets the swing be the game's light
attack; an enemy that is mid-attack cannot be backstabbed and takes a normal light attack instead. Ripostes
after a parry are not in yet.

**The weapon wheel.** Hold a grip with that hand still and the hand's weapon slots open around it, with the
game's own icons. Move over one and let go to switch; let go in the middle to switch nothing. A short squeeze
is still the grip's normal button, and the wheel stays shut while two-handing or in a menu.

**The belt.** The four pouch items the game gives a shortcut (the top two rows of the pouch in the equipment
menu) hang around your waist with their icons and counts: the top row in front, the middle row on your hips,
left column on the left. Reach down, close a grip on one to take it, and press X while you hold it: ERVR plays
the game's own pouch shortcut for that item (Y held, then the item's D-pad direction), so the game's rules and
animation apply. Open the hand and the item goes back. X with nothing in your hand still uses the item in your
quick slot. The belt turns with your body and holds still while you reach for it or look down at it. Because
the shortcut holds Y, using a belt item right beside something you can interact with (a grace, an item on the
ground) interacts with it instead, exactly as the pouch does on a gamepad. Where it hangs, how big the items
are and which hands take them are in `[Belt]` in `ERVR.ini`; with a body calibration its height follows yours.

### Menus

| Control | What it does |
|---|---|
| Menu button (left controller) | Tap: the menu. Hold: the map |
| Both stick clicks together | The menu button too, for SteamVR, which keeps a Quest's left menu button for its dashboard |
| Left stick, in a menu | Moves the cursor like a D-pad; keep it tilted to repeat |
| A / B | Confirm / back |
| Left stick click held 0.5 s | Menu navigation by hand on or off, for menus ERVR does not notice opening (grace sites, shops, level up) |

While a game menu is open, combat stands down: no swings, no injected attacks, and the left stick moves the
cursor. ERVR's own settings are a tab of the game's System menu ([Settings](#settings)).

### Valve Index

Everything above, with these differences:

| Control | What it does |
|---|---|
| Left trackpad, pressed | The menu button: tap for the menu, hold for the map (the Index's own menu button belongs to SteamVR) |
| Right trackpad, pressed at an edge | The D-pad in that direction: switch spells, items and weapons |
| A / B on the left controller | X / Y |
| A grip | Counts when you squeeze it, not when your fingers rest on it |

How hard a grip squeeze must be is Options > Body > *Index grip squeeze* (`[Buttons] IndexGripForce`). What
each trackpad does is `[Buttons] LeftPad` / `RightPad` (`MENU`, `DPAD`, a button name or `none`).

### Steam Frame

Everything above, with these differences (contributed and tested by a Frame owner):

| Control | What it does |
|---|---|
| View button (left) or Menu button (right) | The menu button: tap for the menu, hold for the map |
| D-pad (left controller) | The D-pad: switch spells, items and weapons |
| Bumpers | LB / RB, as on a gamepad |
| X / Y on the right controller | X / Y (a Quest has them on the left controller) |
| Either stick, on the title screen, loading screens and character creation | Moves the cursor one step at a time; keep it tilted to repeat |

## Settings

In the game: open the menu, choose **System**, and go to the **ERVR Settings** tab, to the right of Graphics
(its icon is a game pad). Its twelve rows work like the game's own: Walk toward, Turning, Smooth turn speed,
Physical melee, Damage, Weapon weight, Weapon angle, Body calibration, World scale, HUD follows, HUD distance
and Rumble strength. A change applies within a second and is saved in `ERVR.ini`. *Body calibration* shows your
height and arm length: choose *Redo when the menu closes*, close the menu, and the T-pose panel comes up. (Exe
2.7.2.0, patch 1.17.2.)

The launcher's Options tab has those and the other settings players change most. Everything else is in
`ELDEN RING\Game\ERVR\ERVR.ini`, and most keys apply within a second while you play.

- **View** (`[VR] StereoMode`): *Single-frame* (`replay`, the default since 0.9.0) runs the game once per
  headset frame and has the graphics card draw that frame a second time for the other eye: about half full
  stereo's processor work, so a steady 90 fps is in reach. The card still draws both eyes, so if new frames fall
  short of the refresh, lower *Resolution*. *Full stereo* (`full`) runs the game twice a frame, once per eye.
  *Big screen* (`cinema`) shows the flat game on a screen in the headset, which is cheap and always works.
  Menus, loading screens and cutscenes use the big screen in every mode. Updating from 0.8.x or earlier moves a
  full stereo setting to single-frame once; pick *Full stereo* here to go back, and it stays.
- **Resolution** (`[VR] GameResScale`, 86% by default): the share of the headset's pixels the game renders.
  Lower it if the log warns about stereo pairs per second. The game renders a frame shaped like your view
  (`[VR] GameResShape=eye`), so 86% is sharper than 100% was in 0.6.5, for less GPU work.
- **Frame rate follows the headset** (`[VR] LimiterFollowsHeadset`, on): lifts the game's frame cap above the
  headset's refresh so the runtime never drops to half rate. Game speed is untouched.
- **The HUD** (`[HUD]`): health, stamina and the rest float on their own panel at headset resolution.
  `Distance` and `Width` set its size, `Lock` how it follows you. While playing (NumLock on), Numpad 7 turns
  the panel off and on, and Numpad . re-centres it where you look.
- **Turning and moving** (`[Movement]`): smooth or snap turns, turn speed, and what forward is (where you look,
  where the left hand points, or the body).
- **Weapon angle** (`[FirstPerson] HandRotOffsetX`, 45 by default): how the weapon sits in your hand. If you
  have to bend your wrist forward to hold the blade upright, raise it; if the blade leans too far forward,
  lower it. Options > Body has it as a slider.
- **Body calibration** (`[Body]`): your eye height and arm length from the T-pose (above). `FitArms` gives the
  Tarnished your arm's length, `FitBelt` puts the belt at your waist; `EyeHeightMm=0` and `ArmLengthMm=0` (the
  launcher's *Redo*) ask for the T-pose again, as does the in-game tab's *Redo when the menu closes* (which keeps
  the old one until the new one is done). The launcher's *Defaults* keeps your calibration.
- **Combat** (`[Combat]`): physical melee, speed-scaled damage, weapon weight, two-handing, blocking, cast aim.
  *Swing threshold* (`ContactOnSpeed`, 0.8 m/s) is how fast the blade's tip must move to strike: raise it if
  slow moves attack, lower it if gentle swings miss.
- `[General] LogLevel=debug` writes much more to `ERVR.log`: use it when you report a bug.

Copying a fresh `ERVR.ini` over yours resets your settings; the launcher keeps yours when it updates ERVR.

## Seamless Co-op

ERVR runs inside [Seamless Co-op](https://www.nexusmods.com/eldenring/mods/510) sessions, and your partners
play the ordinary game: they need neither ERVR nor a headset. When Seamless Co-op is installed, START VR
starts the game through `ersc_launcher.exe`. Seamless Co-op keeps its own save (`ER0000.co2`); copy your
`ER0000.sl2` to that name if you want your character there. Everyone must be on the same game patch and the
same Seamless Co-op version.

With other players around:

- **What they see.** Your character arrives on their screens through the game's own network sync, with its
  normal animations, not your tracked arms or head. Physical melee plays no attack animation, so to them you
  stand still while the enemy reacts to the hit.
- **Your hits are yours.** ERVR counts only the damage you caused, so a partner's strike on the same enemy
  never spends your swing or buzzes your controller.
- **Death.** While you are down, the game's own camera (Seamless Co-op's spectator view too) shows on the big
  screen until you are back on your feet.
- **Joins and leaves.** A player arriving, leaving or loading briefly drops you to the big screen for about
  four seconds while their character is built into the world, as a loading screen does.
- **PvP.** With invasions on in `ersc_settings.ini`, speed-scaled damage applies to players you hit too.

Not yet verified with a real partner: physical-melee damage on enemies the host owns while you are the
client, and Ash of War replays as seen from the other side. Tell us what you see in `#ervr-help`.

## Body haptics (bHaptics)

With a bHaptics TactSuit, TactSleeves or a TactVisor you feel the game on your body. Install the
[bHaptics Player](https://www.bhaptics.com/software/player/), pair your devices in it and keep it running
(ERVR finds it by itself if you start it later). The devices pulse twice when ERVR has them, and `ERVR.log`
says which devices are playing.

- **Blows taken** land on the side the attacker stands, higher for a blow from above; a heavy one rings in
  the visor and the arms. The vest faces where your body does, not your head.
- **Combat**: a blow you land kicks back in that arm, a block in the shield arm and the chest, a deflect
  sharper with a ring in the head, a spell, arrow or throw leaving the casting hand.
- **Damage**: heals spread warmth from the chest, and below 30% HP the heart races on the chest's left.
- **Movement**: the wind of a long fall round the waist, and the landing as hard as the fall.
- **Rumble**: what the game shakes the pad for (a boss's stomp, a critical, an explosion) on the vest, softly.

`[bHaptics] Level` sets the strength (0 is off), and `Combat`, `Damage`, `Movement` and `Rumble` switch each
group off; the launcher's Options tab has them too. Without the Player nothing changes. Body haptics are new
in this release and not yet tried on a suit: tell us in `#ervr-help` if a side feels mirrored.

## Other mods

- **Flat render mods** (RenoDX HDR, DLSS Enabler or OptiScaler, ReShade effects) break the headset image:
  START VR parks them (renamed to `*.ervr-off`) and *Play flat* puts them back. Nothing is deleted.
- **Your own ReShade**: keep your `ReShade.ini`, but `dxgi.dll` must be ReShade's add-on build, 6.8 or newer.
- **A `dinput8.dll` mod loader** (ELDEN MOD LOADER and the like): keep it and put `ERVR.dll` in its `mods`
  folder. The launcher refuses to overwrite another loader's `dinput8.dll`.
- **R.E.A.L. VR**: not used. Its files (`dxgi.dll`, `RealVR64.dll`, `openvr_api.dll`, `cudart64_12.dll`,
  `RealVR.ini`) must go, since both want to be `dxgi.dll`; the launcher parks them.

## Troubleshooting

| Problem | Fix |
|---|---|
| The launcher says your patch has no address map | ERVR supports patches 1.16.2, 1.17.1 and 1.17.2. A newer patch needs an ERVR update: watch `#ervr-updates`. |
| The headset is not found | Connect it first, pick the runtime it streams through on the Play tab (Automatic picks Virtual Desktop while its streamer runs, SteamVR while it runs, else Windows' active runtime), press *Check*. |
| The picture is washed out, doubled or broken | A flat render mod is in the way. Start from the launcher, which parks them. |
| Low frame rate, or play feels sluggish | Lower *Resolution* in Options and the game's own graphics settings. In Virtual Desktop pick 72, 80 or 90 Hz yourself: games only get its current rate. *Big screen* is the cheap fallback. |
| The stick does nothing in a menu | Hold the left stick click for half a second: menu navigation by hand turns on for menus ERVR did not notice. |
| A swing does nothing | Swing faster: a slow push is not an attack. In a menu, combat stands down on purpose. Options > Combat > *Swing threshold* sets how fast a strike must be. |
| Slow moves, or a blade resting on a wall, keep attacking | Update to 0.7.1 or later: a strike is one swing. If small moves still strike, raise Options > Combat > *Swing threshold* (`[Combat] ContactOnSpeed`, 0.8 m/s; try 1.5): the blade's tip must move faster than that to strike. |
| Stamina never comes back with a shield | Update to 0.7.1 or later: the shield guards only when you raise it in front of you (left hand near chest height). |
| A launcher slider saved a tenth of what you set (90° became 9°) | That was the 0.6.0 and 0.6.5 launcher, for *Smooth turn speed*, *Weapon angle* and *Body haptics*: update and set them again. |
| A door, lever, chest or new grace takes Y and does nothing | That was 0.7.0 (and 0.6.0 before it): update to 0.7.1 or later. ERVR corrects 0.6.0's `Pass2FullFrameKeep` / `Pass2SafeKeep` lines and ignores 0.7.0's `Pass2ActionButtons` by itself. |
| A lift stays put after you pull its lever | Set `[VR] Pass2EventsOnce=true` in `ERVR.ini` (a test) and tell us in `#ervr-help` whether the lift moves, with your `ERVR.log`. |
| The menu button does nothing | On SteamVR the left menu button opens SteamVR's dashboard: click both sticks together instead (tap: menu, hold: map). |
| The picture goes flat, third person, with no head, and stays so | ERVR thinks the game is still loading. Walk a few steps: since 0.7.1 it notices and comes back. If it does not, post your `ERVR.log` in `#ervr-help`. |
| The weapon points too far up, or you bend your wrist to hold it upright | Options > Body > *Weapon angle*. |
| The Tarnished's arms look stretched, or his elbows stay bent with your arms straight | In the game, System > ERVR Settings > *Body calibration* > *Redo when the menu closes* (or the launcher's Options > Body > *Body calibration* > *Redo*), then stand tall in the T-pose when the panel asks. |
| The calibration panel keeps saying *Check your headset's floor* | The headset's floor is off (it measures you under 1.20 m or over 2.20 m tall): set the floor in the headset's own settings, then hold the T-pose again. |
| Your antivirus removes `dxgi.dll` or `ERVR.dll` | See below; restore the file and allow the folder. |
| Anything else | Ask in `#ervr-help` with what happened and your `ERVR.log`. |

## Reporting a bug

Set `[General] LogLevel=debug` (Options > Troubleshooting in the launcher), play until it happens, and post in `#ervr-help`
on [Discord](https://discord.gg/x27ZXDpdNt) or open a
[GitHub issue](https://github.com/IlyaMez/EldenringVR/issues) with:

- what you did and what you saw,
- `ELDEN RING\Game\ERVR\ERVR.log`,
- your headset and how it connects (Link, Air Link, Virtual Desktop, SteamVR).

When a session goes wrong, the launcher's *Copy report* puts a summary for the report on the clipboard.
The log answers in five lines what otherwise takes ten messages.

## Antivirus and verifying the download

Some antivirus programs flag ERVR: `ERVR.dll` injects into a running game, and `dxgi.dll` is ReShade's add-on
build, which is unsigned by design. Both detections are generic, not a finding about this code. Each release
is built by a public GitHub Actions workflow from the tagged commit and carries a signed provenance
attestation and its SHA-256, so you can check the zip is exactly what the repository built:

```
gh attestation verify ERVR-vX.Y.Z.zip --repo IlyaMez/EldenringVR
```

The bundled `dxgi.dll` is byte for byte ReShade 6.8.0's own add-on build; its hash is in `THIRD_PARTY.md`.

ERVR has no telemetry and no analytics, and `ERVR.dll` never goes online. The launcher asks GitHub once at start
whether a newer ERVR is out (About > Updates turns that off) and downloads it only when you press Update now. For
body haptics ERVR loads bHaptics' own library, which talks to the bHaptics Player on your PC; `[bHaptics]
Level=0` keeps it from loading at all.

## Credits and licence

ERVR is made by Ilya Mezerowsky, the maker of [Spidy](https://github.com/IlyaMez/SpidyVR), and is free software
under the GPL-3.0-or-later. It builds on research by erfps2 and fromsoftware-rs (Dasaav, vswarte),
DSAnimStudio (Meowmaritus) and the Souls modding community; ReShade (crosire), MinHook and the OpenXR SDK ship
with it. Licences are in `THIRD_PARTY.md`.

Free fan mod. Not affiliated with or endorsed by FromSoftware or Bandai Namco. ELDEN RING is a trademark of its
owners; ERVR ships no game code or assets.
