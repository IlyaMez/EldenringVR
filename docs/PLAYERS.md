# ERVR players' guide

ERVR puts ELDEN RING in VR: an image for each eye, your head as the camera, the weapons in your hands, and
melee where the swing you make is the attack. This guide has the setup, every control and the fixes. It ships
in the download as `ERVR\README.md`; the newest copy is on
[GitHub](https://github.com/IlyaMez/EldenringVR/blob/main/docs/PLAYERS.md).

Questions and bugs: the `#ervr-help` channel on [Discord](https://discord.gg/x27ZXDpdNt). Release news is
posted in `#ervr-updates` first.

## Before you start

> **Offline or Seamless Co-op only.** ERVR writes the game's memory, so it runs with Easy Anti-Cheat off.
> Never use it with EAC on or in FromSoftware's online play: that risks a ban on your account. The launcher
> starts the game with EAC off for you. [Seamless Co-op](#seamless-co-op) is the multiplayer that works.

You need:

- **ELDEN RING on Steam**, patch 1.16.2, 1.17.1 or 1.17.2 (game exe 2.6.2.0, 2.7.1.0 or 2.7.2.0). A new
  patch needs an ERVR update; the launcher tells you when yours has no address map yet.
- **A PC VR headset with an OpenXR runtime**: Meta Quest through Quest Link or Air Link, Virtual Desktop
  (VDXR), or SteamVR (Index, Vive, Pico, Steam Link and others). Played by the author on Quest 3 and Quest
  Pro; other headsets should work and reports are welcome.
- **A gaming PC.** ERVR renders the game twice per frame, once for each eye, so it costs about twice the GPU
  of playing flat.
- Optional: bHaptics devices and the [bHaptics Player](https://www.bhaptics.com/software/player/) for
  [body haptics](#body-haptics-bhaptics).

ERVR needs no other mod. It does not use R.E.A.L. VR; if that was installed, the launcher parks its leftover
files for you.

## Install and play

1. Download the zip from [Releases](https://github.com/IlyaMez/EldenringVR/releases/latest) and unpack it to a
   folder you can write to, such as `Documents\ERVR`.
2. Start **ERVR Launcher.exe**. If Windows says it protected your PC, choose *More info*, then *Run anyway*.
3. The **Play** tab checks this PC: the game in your Steam libraries and its patch, the OpenXR runtimes, the
   headset (*Check* asks the runtime), ERVR in the game folder, and flat mods in VR's way. Fix anything it
   marks.
4. Connect your headset and press **START VR**. The launcher installs or updates ERVR in the game folder,
   parks the flat render mods, turns off the game's depth of field, motion blur and ray tracing, starts Steam
   if needed, and starts the game with EAC off (through Seamless Co-op when it is installed). `ERVR.log`
   shows on the Play tab as the game runs.

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

### Fighting

| Control | What it does |
|---|---|
| Swing the right hand | Attack with the right weapon. The swing is the hitbox: faster hits harder, a full windup hardest |
| Swing the left hand | Attack with the left weapon |
| Shield raised in front | Block with a shield in the left hand; moving the shield into a hit deflects it |
| Left trigger | Guard |
| Free hand on the hilt, squeeze its grip | Two-hand the weapon; open the grip to let go |
| Right trigger, while two-handing | The weapon's skill (Ash of War), without its animation |
| Left grip | Skill |
| A grip held, hand still | The weapon wheel: that hand's weapons around it; let go over one to switch to it, in the middle to keep yours |
| Cast with a staff, seal or bow | The trigger casts; the spell or arrow flies where the casting hand points |
| X / Y | Use the item in the quick slot / interact (the game's event action) |

With physical melee on, the triggers and grips never press the game's attack buttons: those would play attack
animations that pull your arms. A staff, seal or bow in the attacking hand lets the trigger through, so it
casts.

**Two-handing.** Put your free hand on the hilt of the weapon in your other hand and squeeze its grip. You
stay two-handed for as long as the grip stays squeezed, wherever your hands move, and opening it lets go. The
squeeze must be fresh: a grip already closed when the hand arrives is ignored, so guarding never grabs.

**The Ash of War.** The first time you fire a weapon's skill, ERVR lets the game play it and records what the
skill does; every later press plays that effect at once, with no animation and no lock. An attack skill arms
your next swing instead, so the strike you make is the skill's. Hold the trigger for a moment to play it the
game's way again (after swapping the ash, say).

**The weapon wheel.** Hold a grip with that hand still and the hand's weapon slots open around it, with the
game's own icons. Move over one and let go to switch; let go in the middle to switch nothing. A short squeeze
is still the grip's normal button, and the wheel stays shut while two-handing or in a menu.

### Menus

| Control | What it does |
|---|---|
| Menu button (left controller) | Tap: the menu. Hold: the map |
| Left stick, in a menu | Moves the cursor like a D-pad; keep it tilted to repeat |
| A / B | Confirm / back |
| Left stick click held 0.5 s | Menu navigation by hand on or off, for menus ERVR does not notice opening (grace sites, shops, level up) |

While a game menu is open, combat stands down: no swings, no injected attacks, and the left stick moves the
cursor.

## Settings

The launcher's Options tab covers the settings players change most. Everything else is in
`ELDEN RING\Game\ERVR\ERVR.ini`, and most keys apply within a second while you play.

- **View** (`[VR] StereoMode`): *Full stereo* renders each eye; *Big screen* (`cinema`) shows the flat game
  on a screen in the headset, which is cheap and always works. Menus, loading screens and cutscenes use the big
  screen in either mode.
- **Resolution** (`[VR] GameResScale`, 86% by default): the share of the headset's pixels the game renders.
  Lower it if the log warns about stereo pairs per second.
- **Frame rate follows the headset** (`[VR] LimiterFollowsHeadset`, on): lifts the game's frame cap above the
  headset's refresh so the runtime never drops to half rate. Game speed is untouched.
- **The HUD** (`[HUD]`): health, stamina and the rest float on their own panel at headset resolution.
  `Distance` and `Width` set its size, `Lock` how it follows you. While playing (NumLock on), Numpad 7 turns
  the panel off and on, and Numpad . re-centres it where you look.
- **Turning and moving** (`[Movement]`): smooth or snap turns, turn speed, and what forward is (where you look,
  where the left hand points, or the body).
- **Combat** (`[Combat]`): physical melee, speed-scaled damage, weapon weight, two-handing, blocking, cast aim.
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
| A swing does nothing | Swing faster: a slow push is not an attack. In a menu, combat stands down on purpose. |
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

ERVR has no telemetry, no analytics, no auto-updater and no network code of its own. For body haptics it loads
bHaptics' own library, which talks to the bHaptics Player on your PC; `[bHaptics] Level=0` keeps it from
loading at all.

## Credits and licence

ERVR is made by Ilya Mezerowsky, the maker of [Spidy](https://github.com/IlyaMez/SpidyVR), and is free software
under the GPL-3.0-or-later. It builds on research by erfps2 and fromsoftware-rs (Dasaav, vswarte),
DSAnimStudio (Meowmaritus) and the Souls modding community; ReShade (crosire), MinHook and the OpenXR SDK ship
with it. Licences are in `THIRD_PARTY.md`.

Free fan mod. Not affiliated with or endorsed by FromSoftware or Bandai Namco. ELDEN RING is a trademark of its
owners; ERVR ships no game code or assets.
