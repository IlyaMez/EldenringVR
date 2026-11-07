# Changelog

All notable changes to ERVR. Versions are tags on `main`; each tag is built by the public
release workflow and published on GitHub Releases and Nexus Mods.

## 0.7.5 (unreleased)

The second eye's pass only draws: new graces, NPCs and every other prompt answer the button as in the game
without ERVR, two co-op crashes are gone, and a skipped boss cinematic no longer wrecks the view.

### Fixes
* New graces: the prompt showed and the press did nothing, or the touch animation played and "Rest" never
  worked, and a death sent you back to the old grace (Paolo, MasterXploda, Willthethrill on 0.7.4). Graces, NPCs
  and merchants run on the game's talk scripts, and ERVR's second-eye pass stepped them a second time every frame,
  at zero time step: a press the second pass took was lost. Their logs showed it exactly - every press taken in the
  first pass worked, every grace press taken in the second was lost. Talk scripts now step once per frame, in the
  first pass. `[VR] Pass2TalkScripts=true` puts the old behaviour back, to compare.
* Why interactions kept breaking, and the fix for all of them: ERVR draws the right eye by running the game's frame
  a second time, and every interaction bug since 0.6.0 - doors (0.6.0, 0.7.0), the stuck item card (0.6.5), lifts
  and fog walls (0.7.1-0.7.3), graces (0.7.3, 0.7.4) - was one more game system stepping in that second run. 0.7.5
  stops it at the root: the second pass keeps only what drawing the same moment needs (the "draw-only set"). Game
  flow, characters, damage, AI, events, menus and talk scripts step once per frame, as in the game without ERVR;
  the world update stays (it makes the environment lighting), and so does the camera family that carries the
  second eye's camera. Measured flat with screenshot pairs of both passes from one camera: the second eye's image
  matches the first's, its own camera reaches the renderer, the frame rate holds. An ERVR.ini that spelled out an
  earlier ERVR's keep sets is moved to the new ones at load (ERVR.log says so); a set of your own stays yours.
* A prompt's press is now decided in the first pass: a prompt asked in the second pass first leaves the press for
  its ask in the next first pass, when the game without ERVR takes it. A prompt that is only ever asked in the
  second pass still answers there. `[VR] Pass2PromptsInPass1=false` puts 0.7.4's rule back.
* Co-op crash after a death (2026-11-07, both players died to a boss): the second pass ran the character pipeline
  while the other player's character loaded back in after the death reload, and the first pass then read a
  character the second had left half-updated (the null read at `exe+0x4737ae` that August's warp crashes also
  hit). Characters no longer step in the second pass.
* Co-op crash when the host rests at a grace (and the one that ends a session on exit): resting reloads every
  player's world, and the second pass ran the in-game step that tears the world down, inside its own frame
  (`exe+0x514c4a`). The in-game step no longer runs in the second pass.
* Checked with a forced death and the second pass running straight through the death, the teardown and the
  respawn (the test harness's no-guards mode, where these crashes used to show): 0.7.4's sets crashed inside the
  second pass (`exe+0x32b05a`, the player's character half torn down); 0.7.5's ran on.
* Boss cinematics: skipping a boss's phase-2 cinematic left the view a mess - normal maps and scene buffers on the
  HUD layer - until you died. During the cinematic the HUD looked for its UI on another render target and took a
  10-bit scene buffer; the 2026-11-01 fix only ruled out float ones. The UI is never re-learned while a loading
  screen or a cinematic is up, and a 10-bit offscreen target is never taken for it.
* Launcher: unpacked into the game folder, it never said a new ERVR was out (it cannot update itself there - its
  files are the live install), so About showed "0.7.3 is out" with no way forward. It now checks anyway, says why
  it cannot update, and the Play tab links the download.

### For testers
* ERVR.log's 5 s `[p2watch]` line ends with "game systems stepping (pass 1/pass 2)": the talk scripts should read
  `N/0` (stepped once), the field-area updates `N/N` (the lighting, both passes). A grace press should log
  "accepted the interact press in pass 1".

## 0.7.4 (2026-11-06)

Graces, plants and items answer the button again; lifts come when you pull their lever.

### Fixes
* Lifts: pulling a lift's lever played the pull and the lift never came, and the lever would not pull again
  (reported on 0.7.1 and 0.7.2 at the Ruin-Strewn Precipice and Rampart Tower lifts). The game's event manager - which
  pulls levers, opens doors and chests - ran in both eyes' passes, and its second run cleared the lever's one-frame
  signal before the lift's script, which runs once per frame, saw it. The event manager and the action-button
  update now run once per frame, in the first eye's pass, as in the game without ERVR. Checked in the headset: the
  lever called the lift, and graces were asked the same in both passes. `[VR] Pass2EventsOnce=false` puts the
  both-pass order back, to compare.
* 0.7.3 broke graces (no prompt), plants to gather (no prompt), items on the ground and dropped runes (a prompt
  that flickered) and some doors (a prompt that ignored the button). The game keeps, for each kind of prompt, the
  nearest one, and lets a prompt in only if it is strictly nearer than the best so far; 0.7.3 reset that table once
  per frame (for the fog walls), so a prompt asked in both eyes' passes got in during the first and was refused at
  the same distance in the second. Answered in one pass only, it was forgotten by the other pass every frame. ERVR
  now sets each kind's nearest aside between the passes, so every pass's prompts get in as in the game without
  ERVR, and puts the frame's nearest back before the next reset, which the fog walls still need.
  `[VR] Pass2RegionBank=false` puts 0.7.3's behaviour back, to compare. The 5 s `[p2watch]` line counts the
  prompts set aside and carried; a grace's prompt calls should read about the same in pass 1 and pass 2.

## 0.7.3 (2026-11-06)

Boss fog walls let you through again; a two-handed weapon's second hand grips the shaft.

### Fixes
* Boss fog walls offer "Traverse the mist" again. The game asks a table which prompt of each kind is nearest and
  answers a prompt only if it was the nearest at the table's previous reset. The event scripts that run fog walls
  (and their other area prompts) run once per frame, in the first eye's pass, while the reset ran in both: the
  previous reset was always the second eye's, where the scripts never asked, so the fog wall was never the nearest
  and its prompt never appeared (0.6.0-0.6.5 and 0.7.1-0.7.2). The table now resets once per frame with the
  scripts; the second eye's pass still runs the rest of the update, which doors, levers and chests need.
  `[VR] Pass2RegionReset=true` puts the old reset back, to compare. ERVR.log names each kind of prompt the first
  time it is asked (`[p2watch] action button N asked in pass 1 from ...`) and counts the table's resets per pass.
* ...and the button takes you through. The game forgets, at each action-button update, any prompt not asked again
  since the previous one, and wipes a press that has no prompt left to go to; the update runs in both eyes' passes,
  the event scripts ask in the first only, so the fog wall's prompt was dropped and the press wiped every frame (it
  showed, and the button did nothing). ERVR now asks each event-script prompt again right before and after the
  second pass, with the press held back for that call so only the script's own ask can take it.
  `[VR] Pass2KeepScriptPrompts=false` turns it off. Checked unattended with `[Experimental] TestScriptPrompt=10000`
  (a "Traverse the mist" prompt asked like a script's): with it off the press never arrives, with it on the first
  press is taken.
* Two-handing: the hand you put on the weapon grips the shaft instead of hovering beside it. ERVR slid the
  weapon hand's WRIST down the hilt and hung the free hand from that, but a hand's palm (where the game puts a held
  weapon) is 9 cm from its wrist toward the fingers, so the free palm only met the shaft while both hands pointed the
  same way - since the free hand keeps its own grab-time angle it could sit up to 18 cm off. Its palm now goes on the
  shaft through the weapon hand's palm as the arm was actually solved. `[Combat] TwoHandPinSideOffset` moves the palm
  sideways off the shaft and is 0 by default; 0.03, the old shipped nudge, reads 0 (ERVR.log notes it). The log
  names where each palm sits (`[skel] R_Weapon (the palm) ...`).

## 0.7.2 (2026-11-05)

The launcher updates ERVR itself; strikes no longer drain stamina.

### The launcher updates ERVR
* When a newer ERVR is out, the Play tab says so. **Update now** downloads the release's zip from GitHub, checks it
  against the release's SHA-256 (GitHub's own digest of the asset) and puts its files in place of the launcher's,
  the running launcher included, then starts the new launcher; START VR copies the new ERVR into the game folder
  as it does for any newer ERVR, keeping your `ERVR.ini`. A failure puts every file back. *Later* asks again at the
  next start. After an update the banner says which version replaced which, with a link to what's new. Ported from
  Spidy's launcher.
* The check is one question to GitHub (`api.github.com`) when the launcher starts; About > Updates shows the last
  answer, has *Check now*, and its switch turns the start-up check off (`update_check=0` in
  `%APPDATA%\ERVR\launcher.ini`). Nothing is downloaded until you press Update now. `ERVR.dll` still never goes
  online; this is the launcher's only network code.
* A launcher unpacked into the game folder itself (its files are the live install, `ERVR.ini` and `ReShade.ini`
  your own) and one built from the source do not update themselves, and say why.
* Only an older ERVR's program files (`.dll .exe .md .txt`) that a new zip no longer ships are retired from its
  folders; settings, logs and anything else you put there stay. The replaced files wait in `.ervr-update\previous`
  until the next start.

### Combat
* Stamina "off" (the default) really takes the bar out of the loop now. The game charges a strike's stamina while
  ERVR runs the injected attack - inside ERVR's own call, so the frame-to-frame check that refunds it never saw the
  charge (a 0.7.1 log: 0 refunds, and stamina at -50 in every 5 s with a landed hit: "drains in a second in
  fights", "can't roll"). The stamina before and after that call is compared now and the difference given back,
  exactly, below 0 too. The 5 s `[combat]` line says how much was taken inside ERVR's attacks.

## 0.7.1 (2026-11-05)

Doors, levers and chests open again; a strike is one swing; ERVR comes back when the game's loading signal
sticks; a menu button on SteamVR.

### Fixes
* Doors, levers and chests open again. In 0.7.0 the prompt took the press (it vanished, and ERVR.log said
  `accepted the interact press in pass 1`) and then nothing happened, which stopped progress at the first
  locked door or lift lever. 0.7.0 had moved the game's action-button update (which prompt the interact button
  answers) to the first eye's pass only, while the event manager that opens doors, pulls levers and opens
  chests still runs in both: the two have to step together (0.6.0 had the mirror image and the same dead
  doors). The update runs in both passes again, as in 0.6.5. What 0.7.0 fixed stays fixed differently: a prompt
  that answers the same press twice within one stereo pair is told the second time that it was not pressed, so
  an item is still picked up once and its card still closes. `[VR] Pass2ActionButtons` from 0.7.0 is no longer
  read (players' `=true` workaround is now the default); `Pass2ActionButtonUpdate` and `Pass2RepeatPressGuard`
  are there to compare. Thanks to Paolo for finding the ini switch, and to EscalatorBoy, Wyrwislaw and
  MasterXploda for the reports and logs.
* A "the world is loading" signal stuck on no longer keeps ERVR switched off. Right after the Grafted Scion,
  in the Stranded Graveyard, three players got a flat, third-person picture with the Tarnished's head hidden,
  for good: ERVR's stand-down for loading screens and warps (the game's own camera, no second eye) with the
  world plainly running. When the loading-screen byte or the game's warp request stays set for 6 s while you
  walk the world (the stick pushed, the character moving in walking-size steps, the in-game clock running,
  none of which happens in a load or a cutscene), ERVR treats it as stuck, says so in the log
  (`[guard] loading-screen byte (0xNN) has stayed set ... while you walk the world`) and comes back: walk a
  few steps. The log now prints the byte's value with every change (a real load reads `0x80`).

### Combat
* A strike is one swing. A strike opened at 0.8 m/s of blade tip speed and then stayed live for as long as the
  tip moved at 0.25 m/s, with no limit: a blade carried while walking, resting on a wall or waved about was a
  live hitbox for seconds at a time (a 0.7.0 log: a strike live in 296-300 of every 300 frames, and up to 7 of
  them per hand every 5 s with no enemy near). That was the constant attacking: walls hit over and over, people
  hit by a sword that only touched them. Now a strike ends once the swing has slowed (under 40% of its peak for
  0.12 s) or after 1.5 s, the next one opens only when the blade speeds up again, and after a strike that
  landed the next waits 0.3 s (`[Combat] ContactDwellSeconds`, `ContactMaxSeconds`, `ContactCooldownSeconds`).
  A windup that slows at the top ends its strike, and the downswing opens a new one. Reported by Nickytheman12,
  Rahtz and MasterXploda.
* Rolls and jumps cost stamina again during a strike. With stamina "off", any drop of 4 or more while a strike
  was live counted as ERVR's own on-hit mark and was refunded, so with strikes live most of the time stamina
  felt unlimited, until it wasn't. Only a drop the size of the mark counts now, and a refund gives back at most
  the mark.
* A shield holds the guard only when you raise it: the left hand within 45 cm below your eyes and 10 cm in
  front of them (`[Combat] ShieldUpHeight`, `ShieldUpForward`, 15 cm of slack once up). "Up" was anywhere in
  front of you above the hips, so an arm at rest held the guard all the time, and stamina comes back slowly
  while guarding: a fight drained it for good. Holding the left stick click "fixed" it because ERVR's menu
  navigation mode lets go of the guard.

### Controls
* Both stick clicks pressed together are the menu button (tap: the menu, hold: the map), for SteamVR, which
  keeps a Quest's left menu button for its own dashboard: over Link, Air Link or Steam Link the game's menu
  could not be opened. Held together, neither click crouches or locks on (`[Buttons] MenuStickChord`).
* After a death, the left stick moves the cursor of the "Stake of Marika / last site of grace" choice. Neither
  of ERVR's menu readers sees that screen, so the stick stayed a stick and only the first answer could be
  taken.

### Testing
* `[VR] Pass2EventsOnce=true` (off by default) runs the event manager and the action-button update once per
  frame, in the first eye's pass, as the game does without ERVR. For a lift that stays put after its lever
  is pulled: if this moves it, and doors, levers, chests, graces and items still work, it becomes the default.
* `[Experimental] AutoPilotScript` takes sticks, held for their step: `LU LD LL LR` walk, `RL RR RU RD` turn
  the camera.

## 0.7.0 (2026-11-04)

ERVR settings in the game's own menu; body calibration; no more strikes from turning; the stuck item card fixed;
a sharper picture at the same GPU cost.

### Body calibration
* The first time you play, a panel in the headset asks for a T-pose: stand tall, look ahead, stretch both arms
  straight out to the sides and hold both triggers until the bar fills (1.5 s). A line on the panel says what to
  change (arms out, straighten them, look ahead, the headset's floor), and a ring at each controller turns gold
  while that arm is in place and fills with the hold. ERVR then has your height and arm length:
  * The Tarnished's arms take your arm's length. His hands always sat on your controllers, so what did not fit
    was the arm between: a long reach stretched his forearm and upper arm, a short arm kept his elbows bent.
    Now your straight arm is his straight arm, and a bent one bends his elbow as much. The stretch still adds to
    it past that reach.
  * The belt hangs at your waist: `[Belt] Drop` is for a 1.76 m player and follows your height (0.64 m below
    the eyes at 1.88 m).
* B skips it, and ERVR asks no more. From the panel's start until each is let go, the triggers, grips, A and B
  do nothing in the game (no swing, wheel, belt or grab either); the sticks, the menu button, X and Y work. A
  menu, a cutscene, a load, Torrent or the big screen holds the panel until play returns.
* The launcher's Options > Body > **Body calibration** shows your height and arm length; **Redo** asks for the
  T-pose again at the next moment of play, mid-game too (**Ask again** after a skip). In the game, the pause
  menu's System > ERVR Settings > **Body calibration** row does the same without leaving the headset. *Defaults* keeps your
  calibration. Kept in `ERVR.ini` `[Body]` (`EyeHeightMm`, `ArmLengthMm`; `FitArms` and `FitBelt` turn either
  use off). Numpad * (NumLock on) asks for it while testing.
* The log says what was measured (`[calib] body calibrated ...`) and what it changed (`[body] left arm fitted
  to your calibration ...`, `[belt] hangs ... fitted to your calibrated ...`).

### Fixes
* Turning with the stick no longer swings your weapons. The blade's speed was measured against ERVR's own
  play-space origin, which turns about your head with every stick turn and jumps to your head when you step
  35 cm away: a smooth turn at the default 120°/s added up to 0.7 m/s to every blade tip, a snap turn or a
  recentre a one-frame spike, against the 0.8 m/s that opens a strike. Speeds are now measured in the headset's tracking space, as the
  controllers report them, and a controller that jumps faster than a hand can move (a tracking loss coming
  back) opens nothing. Reported as constant attacking and a stamina bar that empties in fights.
* The launcher's sliders save what they show. A whole number lost its trailing zeros: a 90° weapon angle was
  saved as 9°, a 200°/s turn speed as 2°/s, 100% body haptics as 1%. Affected *Smooth turn speed*, *Weapon
  angle* and *Body haptics* in 0.6.0 and 0.6.5; if you moved one of them, set it again.
* The download steps say which file: **ERVR-v….zip** under *Assets*, not *Source code* or the green *Code*
  button, which have no launcher in them.
* The card shown after picking up a new item no longer stays on screen, ignoring every button and menu until
  you die or travel. Cause, measured: the game decides once per frame which prompt (door, lever, grace, item
  on the ground) the interact button answers, and the second eye's pass made that decision a second time.
  The same press was then answered twice and the item handled twice, leaving a card that nothing closed. The
  same double decision is why 0.6.0's doors ignored the button: which prompts survived to the next frame
  depended on which task groups the second pass re-ran. That decision now happens once per frame, as in the
  game without ERVR, and a prompt that answers the same press a second time is told it was not pressed
  (`[VR] Pass2ActionButtons=false`).
* The log records every interact press the game accepts, where each prompt is asked from, and what each pass
  changed in the prompts, the item-get popups and the menu's UI elements (`[p2watch]` lines,
  `[VR] Pass2Watch`), so a stuck card or a dead door can be traced from a log.

### Combat
* Options > Combat > **Swing threshold** (`[Combat] ContactOnSpeed`, 0.8 m/s as before): how fast the blade's
  tip must move to strike. Raise it if slow moves or a blade resting on a wall keep attacking.
* The 5 s combat line in `ERVR.log` now says where the stamina bar went: what the game took and gave back with
  a strike live and without one, what ERVR refunded, and the lowest it got. A report of stamina "gone in a
  blink" then shows whether strikes cost it or stopped its regeneration.

### Sharper at the same GPU cost
* The game now renders a frame shaped like the view the two eyes share (2736x2832 on a Quest 3 at
  Resolution 100%), not a 16:9 one. Each eye used to be cut from a 16:9 frame that was mostly field nobody
  sees, about 35% of the pixels the GPU drew; now it is about 67%. At the same pixel count, so the same GPU
  cost, a Quest 3 eye gets 2154x2327 px instead of 1588x1716: 1.36x sharper in both directions. Resolution
  86% (2352x2440) is now sharper than 100% was, for a quarter less GPU work.
  The shape comes from the headset's field of view and is cached in `ERVR.cache.ini`;
  `[VR] GameResShape=wide` brings the 16:9 frame back.
* Why the eye-shaped frame of August looked wrong (a smaller world that dragged with head motion): the game
  holds the horizontal field at its 16:9 width and lets the vertical follow the frame, so its camera's field
  of view spans the 16:9 height of the frame's width, not the frame's height. ERVR now crops against that,
  measured in-headset: the eye-shaped frame shows the world at exactly the scale the 16:9 one does
  (`[VR] GameRasterRefHeight=0` means width x 9/16 now; on a 16:9 frame nothing changes).
* Each eye is now a pixel-for-pixel copy of its part of the game's frame. The crop used to start at a
  fraction of a pixel and be stretched a pixel or two onto the eye image, so every eye pixel blended two of
  the game's, softening the finest detail (foliage, text, distant edges).
* A new Resolution takes effect at the next launch: the game reads its render size when it starts.

### Frame rate
* Below the headset's refresh, the log now says which side is short, at every log level: the engine passes'
  CPU work, the time presents spent blocked on the GPU, and the headset's own frame wait.
* `[VR] Pass2Characters=false` (opt-in, read at launch): the second eye's pass skips the character pipeline
  and shows the poses the first pass computed, the same moment either way. About 1 ms less per stereo pair
  on a 9800X3D (the second pass ~4.6 -> ~3.6 ms), which is headroom under a 90 Hz headset. Stereo depth stays;
  fast travel, deaths and fights have not been played with it yet, so it is off by default.
* `StereoMode=debugalt` (one eye per frame) renders stereo again: its eye reports are pushed by the same
  frame hook as full stereo, which it never installed, so it had shown the flat screen since August. Each
  frame costs one engine pass, but `xrEndFrame` holds ~3.4 ms of every frame under Virtual Desktop, so it does
  not yet clear 120 Hz.
* At `LogLevel=debug` the log reports how long `xrBeginFrame`, `xrEndFrame` and the swapchain-image waits
  hold the render thread, every 5 s.

### ERVR in the game's menu
* The game's System menu has an **ERVR Settings** tab, to the right of Graphics: Walk toward, Turning, Smooth
  turn speed, Physical melee, Damage, Weapon weight, Weapon angle, Body calibration, World scale, HUD follows,
  HUD distance and Rumble strength, as rows like the game's own. A change applies within a second and is written
  to `ERVR.ini`. *Body calibration* shows your height and arm length; choose *Redo when the menu closes* and the
  T-pose panel comes up once you are back in play (your old calibration stays in use until the new one is done,
  and B keeps it). Resolution, which needs a restart, stays in the launcher's Options. The game builds the tab
  and its rows with its own code; ERVR answers for the values. Exe 2.7.2.0 (1.17.2) only;
  `[SettingsTab] Enabled=false` leaves the game's Settings as they are, and `Icon` picks the tab's icon from the
  game's own (the game has no headset icon; 1 is a game pad).
* For unattended test runs, `[Experimental] AutoPilotScript` taps a list of buttons once the player is in the
  world (`START+3 UP+2 A+3`), and `SHOT+N` has ReShade save the frame; `tools\run_game.ps1 -ShotWindow`
  captures the game's window only.

## 0.6.5 (2026-11-02)

Doors, levers, lifts and new graces open again; Valve Index controls; the belt; Torrent rework.

### Fixes
* Doors, levers, lifts and undiscovered graces answer the interact button again. In 0.6.0 the prompt showed
  and Y did nothing, which kept new characters in the first room. 0.6.0 stopped the second eye's pass from
  re-running the event manager (task group 25) along with the event scripts (22-24); the event manager is
  back in that pass and the scripts stay out of it, so the pickup dialog that ignored every button stays
  fixed. Found by a player's bisect on 0.6.0 (16-21,25 opens the doors with no stuck popup; 16-25 opens
  them and brings the popup back).
* An installed `ERVR.ini` keeps its values across updates, and 0.6.0's spelled both keep sets out. ERVR now
  replaces 0.6.0's value, and the 16-25 workaround posted for it, when it loads the ini (ERVR.log says so);
  any other value there is yours and stays. The shipped ini leaves `Pass2FullFrameKeep` and `Pass2SafeKeep`
  commented out, so ERVR's own sets follow its updates.
* A D3D11 device made by an overlay or by the VR runtime (Meta Quest Link makes one as its session starts)
  is ignored. It used to clear ERVR's D3D12 flag for a frame and log that ERVR would stay idle, which it
  did not.

### Valve Index
* The left trackpad, pressed, is the menu button (tap: the menu, hold: the map). The Index's own menu button
  is SteamVR's system button, which never reaches a game, so 0.6.0 could not open the menu at all.
* The right trackpad, pressed at its top, bottom, left or right edge, is the D-pad. The direction is the one
  the press started in until you let go; the middle third is no direction. `[Buttons] LeftPad` / `RightPad`
  take `MENU`, `DPAD`, a button name or `none`; `PadForce` sets how hard a press is.
* The grips read how hard you squeeze (`squeeze/force`) instead of the touch sensor, which reads fully
  closed as soon as your fingers rest on the handle: resting fingers no longer open the weapon wheel or tap
  the grip's button (the skill on the left grip). `[Buttons] IndexGripForce` (0.5: a full grip at half
  force; 0: the touch sensor, as before), also Options > Body > Index grip squeeze in the launcher.
* ERVR.log names the Index sources the runtime bound (`trackpad L 1 R 1, grip force L 1 R 1`), and the
  debug `[xr] raw` line carries the trackpads. Built from the OpenXR Valve Index profile; not yet tried on
  an Index.

### Launcher
* Options > Body > Weapon angle (`[FirstPerson] HandRotOffsetX`, 45 by default): how the weapon sits in your
  hand, for anyone who has to bend the wrist to hold the blade upright. And Index grip squeeze.
* The Controls tab lists the Index trackpads.

### Tests
* `config`: ERVR.ini loading (retired keep sets replaced, anything else kept, the shipped ini's keys).
  `virtual_pad` checks the trackpads: the menu button's tap and hold, the D-pad direction latched at the
  press, the middle, the force threshold, other mappings.

### Belt
* The four pouch items the game gives a shortcut hang around the waist as objects, with the game's own
  icons and their counts: pouch slot 1 (top left) front left, 2 (top right) front right, 3 and 4 (the
  middle row) on the hips. Close a grip on one to take it, press X while holding it to use it, open the
  hand to put it back. X with nothing taken from the belt is the plain X it always was.
* Using an item is the game's own pouch shortcut: ERVR holds Event Action (Y) for 0.4 s, then presses
  the slot's D-pad direction (the game's controls list: up, right, left, down), so the game's rules,
  animation and counts apply. The weapon wheel's switch and the shortcut never press the D-pad at the
  same time.
* The belt hangs under the neck (the eyes pivot around it, so looking down does not move it) and turns
  with the body: the head may look `FollowDegrees` to either side, a big turn drags it along, a heading
  held for two seconds brings it under the head, and it holds still while a hand is at it.
* A grip that takes an item is the belt's until it opens: not a two-hand grab, not a weapon wheel, not
  the grip's `[Buttons]` mapping, and that hand does not swing.
* Every use is checked against what the game did next - the item's count, the item the game queued for
  its UseGoods event, and the selections a plain D-pad press would move - and said in ERVR.log. A use
  that took another slot's item swaps those two directions for the session and logs the `Directions`
  line to keep; a press the game read as a plain switch says to raise `ChordLead`.
* Layouts from fromsoftware-rs (read-only reference), each checked against the TGA cheat table: the
  pouch at PlayerGameData +0x678, the inventory at +0x5D0, EquipMagicData at +0x530; EquipParamGoods
  `iconId` / `maxNum` for the icons and counts.
* `[Belt]` in ERVR.ini (`Hands`, `UseButton`, `Drop`, `Radius`, `Angles`, `Size`, `GrabRadius`, ...).
  Regression test `belt` (directions, the chord's timing, the neck and belt geometry, the body yaw, the
  grab, the use verdicts); `virtual_pad` checks that X reaches the game unless the belt took it.

### Torrent
* On 1.17.2 ERVR never found the horse (the 2026-11-02 ride: no seated pose, no horse turns, no
  visibility fix, and nothing in the log said so): 1.17.2's Torrent is character 8002, and the resolver
  only accepted 8000. It now takes the first identity that holds - the rider's pair handle, Torrent's own
  pair link back to the rider, or the last mount being ridden - and a mounted flag left set while riding
  no longer hides it. A ride it still cannot resolve logs every value it read
  (`[ride] ... Torrent was not found`).
* Free look on Torrent (`RideFreeLookDeg`, 45): looking around within it neither steers the horse nor
  turns it. With `Forward=head` the move stick used to follow the head 1:1, so every glance steered.
  Past it, the excess becomes the new forward and turns Torrent and the rider with you.
* The camera no longer rides the gallop and jump animations' head, which crouched it forward into the
  rider's body and Torrent's neck. Seated, the rider is a seated pose taken from your own seat the first
  time it holds still in the saddle (kept for later rides), kept upright about the pelvis whatever
  Torrent's pitch, arms on the controllers. The camera sits on that pose's head, and Torrent's gallop
  bounce is smoothed out of the view in the horse's own frame (`RideCameraSmoothing`,
  `RideCameraMaxDeviation`); the rider moves with the smoothed view, so the horse moves under both and
  the body cannot rise into the camera.
* The second eye's pass re-places the rider on the horse (measured 15-35 cm off its camera while
  galloping and jumping), and both eyes are drawn from the first pass's camera. The second eye now draws
  the rider where the first eye placed it (keyed on the stereo pair: the game's frame counter advances
  between the rider's rebuild and the second pass).
* Turning your body past the free look turns the horse: with `BodyYaw=head`, Torrent and the rider turn
  with you like the right stick does, at `BodyYawFollowSeconds` (`RideTurnsWithHead`).
* The enemy watcher no longer counts Torrent as an enemy.
* Combat from the saddle: the two-hand grab and its learn presses send nothing while mounted (their
  attack button is a mounted attack there). `[Combat] RideAttacks=buttons` makes a swing press the game's
  own mounted attack instead of striking with the blade. The `[combat] 5 s` line says how close mounted
  strikes came to an enemy's body.
* A `[ride] 5 s` line reports whether the horse was found, which body rides, the view smoothing, the
  second eye's re-placement and the turns. `[body] rendered head vs camera eye` measures each eye against
  its own camera.
* Regression tests `mounted_rider` (the resolver, the seat, the upright frame, the camera smoothing, the
  physical turns, the second eye), `ride_state` (seated states) and `weapon_contact` (blade distance).

### Overlays
* The weapon wheel's drawing and its D3D12 upload moved into `overlay_canvas` / `overlay_layers`,
  shared with the belt: one command list copies every overlay that changed. Item icons are now read at
  256 px (the wheel still draws them at 128). The OpenXR backend logs the runtime's layer limit and
  never submits more layers than it allows.

## 0.6.0 (2026-11-02): first public release

ERVR's first download for everyone: the launcher, Seamless Co-op, game patch 1.17.2, the weapon wheel and
body haptics on top of everything 0.4.0 prepared (0.4.0 was never published). Offline or Seamless Co-op
only, never with Easy Anti-Cheat.

### Body haptics (bHaptics)
* bHaptics TactSuit, TactSleeves and TactVisor through the bHaptics Player (`[bHaptics]` in
  ERVR.ini): blows taken on the side the attacker stands (the game's own `last_hit_by`, the vest
  facing the body, not the head), blows landed, blocks, deflects, the two-hand grab and casts in the
  arm that made them, heals, death, the heart below 30% HP, the wind of a fall and the landing (the
  physics module's `is_falling`), and the game's rumble on the vest. `Level` sets the strength (0 =
  off), `Combat` / `Damage` / `Movement` / `Rumble` switch the groups; all hot-reload.
* Ported from Spidy's body haptics: bHaptics' own C/C++ SDK 2.7.1, its `bhaptics_library.dll`
  shipped unmodified beside ERVR.dll and loaded only while `Level` > 0, the patterns made in code
  and streamed to each device every 40 ms on a thread of their own. The Developer Portal app comes
  from the `ERVR_BHAPTICS_APP_ID` / `_API_KEY` secrets (or `bhaptics-app.local`).
* ERVR still has no network code of its own; bHaptics' library talks to the bHaptics Player on the
  PC. README, THIRD_PARTY.md and the Nexus texts say so.
* `ervr_haptics_test.exe`: every pattern and a motor-by-motor map without the game (`--dry` prints
  them as text). Regression test `body_haptics`. Launcher and `install.ps1` install and remove the
  library with ERVR.dll. Untested on a suit.

### Weapon wheel
* Hold a grip with that hand still and a wheel of the hand's three weapon slots opens around it, with
  the game's own item icons; move over a weapon and let go to switch to it (let go in the middle to
  switch nothing). A hand with a single weapon opens too.
* The switch is the game's: ERVR presses D-pad right / left until ChrAsm's active slot reads the slot
  picked, so the game's animation and rules apply and a refused switch buzzes. A weapon two presses
  away (`Switch=jump`) first gets the active slot moved to the slot before it, in the save's and the
  character's equipment alike after checking they match, so one switch takes it straight there
  instead of passing through the weapon in between; `Switch=cycle` presses once per slot.
* The grip keeps its other jobs: closed at the other weapon's hilt it is the two-hand grab, and while
  two-handing or in a menu the wheel stays shut; a moving hand never opens it. A squeeze shorter than
  `HoldSeconds` is passed on as one tap of the grip's `[Buttons]` mapping. A long squeeze that does
  not open the wheel says why in ERVR.log.
* Icons are read from the game's own archive in the background (`menu/hi/00_solo`, the inventory's
  item pictures: the RSA-keyed index decrypted with the executable's own keys, Oodle via the game's
  DLL), shrunk from 1024 to 128 px, about 10 ms each. Nothing is extracted to disk.
* `[WeaponWheel]` in ERVR.ini (`Hands`, `HoldSeconds`, `Switch`, `Size`, ...). Regression tests
  `weapon_wheel` (layout, picking, the game's slot cycle, the press-until-switched loop) and
  `icon_decode` (BC1/BC2/BC3/BC7 bit-exact against Pillow, the TPF walk, the shrink).

### Launcher
* `ERVR Launcher.exe` in the zip: unpack anywhere and press START VR. It finds the game in the Steam
  libraries and checks its patch against ERVR's verified address maps, lists the OpenXR runtimes
  (Automatic: Virtual Desktop while its streamer runs, SteamVR while it runs, else Windows' active
  one; the choice reaches ERVR through `XR_RUNTIME_JSON`), checks the headset, installs or updates
  ERVR in the game folder (`install.ps1`'s rules), parks flat render mods and R.E.A.L. leftovers
  (`game_mode.ps1`'s), starts Steam when needed, starts the game with EAC off (`steam_appid.txt`) or
  through Seamless Co-op, and shows `ERVR.log` as it is written.
* Options tab: the common `ERVR.ini` settings, written in place (every other byte kept) so ERVR
  hot-reloads them mid-game. Controls and About tabs; Copy report for bug reports; Play flat and
  Remove ERVR.
* Game graphics: START VR turns off the game's depth of field, motion blur and ray tracing in its own
  `GraphicsConfig.xml` before the game reads it (the value set to `DISABLE`, the preset to `CUSTOM`,
  auto-detect `OFF`, as the game's menu writes them; every other byte kept, the original saved once
  as `GraphicsConfig_ervr_backup.xml`). Each is a toggle in Options (on by default) and a row in This
  PC; nothing is written while the game runs, since it writes the file back as it closes.
* No network code, as ERVR. Dear ImGui (MIT) and the Cinzel and Barlow fonts (OFL), see THIRD_PARTY.md.
* Regression tests: `launcher_text` (ini edits, log lines, Steam files) and `launcher_folder`
  (install / park / flat / uninstall on a throwaway game folder).

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
* Dying in a session no longer crashes the respawn. ERVR resumed its pose and camera writes in the
  gap between the death load and Seamless Co-op's reload, through a skeleton cache and a follow-cam
  pointer left over from the world that had just been freed; an enemy built in that memory then
  crashed. Both are now checked against their live owner on every write (the player's
  hkbCharacter, the chr cam's follow cam).

### Combat
* Each swing now lands ONE hit scaled by the swing's quality instead of a slice of a hit on every frame
  of contact. ELDEN RING takes the enemy's flat defense off every hit, so slices of 6-10% of a hit dealt
  3-5 HP each where a full hit deals hundreds; swings "connected but did much less damage".
  `[Combat] KineticSlices=true` restores the slice model.
* Your own hits are marked by the game's on-hit stamina charge (with `KineticStamina=off` the strike's
  cost is 8 instead of 1, refunded). On enemies a co-op partner's game owns the game records no
  attacker, so a partner's hit used to read as yours; once the mark has been seen with your hits, it
  alone (or two strike frames in a row with HP drops) ends a swing.
* After a swing that hit, the next one arms only once the weapon speeds up again, so the
  follow-through cannot land a second hit.
* A swing whose damage budget was spent stayed inert until the weapon tip came to rest, which a long
  weapon rarely does mid-fight, so every later swing was swallowed ("attacks stopped registering").
  A spent swing now ends at the tip's next speed valley, or `[Combat] ContactSpentSeconds` (1 s)
  after it was spent, and the next swing starts with a fresh budget (`ContactValley`, 0.4 of the
  swing's peak speed).
* The log gets a 5 s combat summary while fighting: strikes, landed and budget-spent swings,
  damage, on-hit stamina marks, whose HP drops the enemy watcher saw, and how often enemy HP rose
  again (in co-op: the owner's game overriding a hit). In a co-op session the first drops of each
  kind are logged with the attacker handles.

### Frame rate and resolution
* The game's frame limiter now follows the headset (`[VR] LimiterFollowsHeadset`, on). With
  `FpsTarget=72` under a 90 Hz headset the game could never reach 90, so Virtual Desktop ran it at
  half rate (45 Hz) and synthesized the rest: ~52 stereo pairs/s, sluggish. With the limiter raised
  above the refresh, the same PC (RTX 5090, 9800X3D) runs ~88 pairs/s at 90 Hz. Virtual Desktop
  only offers its current streaming rate to games, so choose 72/80/90 Hz in VD itself; the log
  names the rate. `[VR] HeadsetRefreshHz` requests a rate on runtimes that allow it.
* `GameResScale` now gets exactly the size it asks for. The game picks the LAST resolution-table
  entry that fits the request, and ERVR's table entry arrived ~0.1 s after the game's boot walk, so
  most targets snapped down to a built-in size (3712x2088 came up 3200x1800). The table's last slot
  is now written at DLL attach. Default `GameResScale` 0.86 (3200x1800 for a Quest 3 at VD's
  2688x2880), the size the snap used to land on; 1.0 (3712x2088) was GPU-bound at 90 Hz.
* The size is exact 16:9, in steps of 128x72 (3584x2016 for a Quest Pro). The game renders a
  window even a few pixels off 16:9 as a 16:9 frame inside black bars, and the HUD layer could
  not find that frame's UI: at 3632x2040 the menus, map and HUD stayed in the 3D view (the
  menu outside both eyes, the map magnified). A non-16:9 back buffer is now a log warning.
* The "GAME SPEED n%" warning was wrong: the game runs on measured frame time and keeps real speed
  at any frame rate (in-game clock x0.98 at 52 pairs/s). It now reports pairs/s against the
  headset's refresh, at most every 30 s.
* Debug heartbeat timing line: each engine pass, ERVR's present work, the DXGI Present block,
  `xrWaitFrame` and the headset refresh (`hb: timing`).

### Fixes
* View suddenly near the ground, or stuck crouched, until you happened to stand still. The VR body is
  a snapshot of the game's own pose and the camera rides its head bone, and two snapshot paths could
  take a pose that was not a stance: after every respawn, warp or area load the snapshot was retaken
  ~3 s in with no checks at all (catching the respawn kneel, a roll, a stride), and the stand-still
  re-snapshot accepted any pose held still, a knockdown included. Now the standing snapshot is kept
  across reloads of the same character, every snapshot is judged by its head height against the
  skeleton's reference pose (a body on the ground is never taken, a first snapshot must be standing),
  and a low one (crouch) is left the moment the game's own animation stands back up, walking or not.
  The eye-height calibration comes from the standing body only. Logged per snapshot
  (`[skel] pose template captured (...): head X m above the feet = N% of the reference pose`), and the
  5 s `[body]` line now shows the camera's height above the feet.
* Black headset from boot through the title screen. Once the render size was exact 16:9 the HUD
  layer found the title screen's UI target, and at the title that UI is the whole picture (logo,
  ring, menu: 7 draws over a black clear), so the frame on the cinema screen came out pure black
  and the picture hung on the HUD panel alone; three boots in a row showed nothing. The UI now
  leaves the frame only while the eyes are stereo: flat frames (title, loading screens, the map,
  cutscenes) go onto the cinema screen whole, UI included, as on the monitor. The first minute of
  every VR session logs what reached the headset (`hb: xr`, `hb: hud`) whatever `LogLevel` says.
* The second-eye pass no longer re-runs the game's event scripts (groups 22-24 EmkSystem, 25
  EventMan). Running them twice per stereo pair left a centre dialog raised by an item pickup on
  screen, ignoring every button and menu until a respawn. Pass 2 renders the same moment as pass 1,
  so like characters, menus and the network, events must not advance between the eyes.
* A WorldChrMan character-list node holding a stale character (a character handle where the pointer
  belongs) crashed the ragdoll task in a long co-op session. The existing guard at that site only
  skipped one load, and the same function faulted again two instructions later; it now returns from
  that character's whole update, so the list walk goes on (only when the function's prologue is
  byte-identical to 2.7.2.0's).
* `[Cheats] SetLevel=N`: reach exactly level N once (points come off the highest stats), then the
  key sets itself back to 0 in ERVR.ini.
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
* `xrprobe` lists every extension the OpenXR runtime advertises.

### Help, docs and the page
* The launcher's help button and its About card open the Discord (`#ervr-help`) instead of GitHub
  Discussions, which the repository does not have; the Controls tab lists the weapon wheel.
* README rebuilt as the landing page, in Spidy's layout and ERVR's own look: download, Discord and
  Ko-fi up front, a gameplay reel, the controls and actions infographics, community and support.
  The players' guide (`docs/PLAYERS.md`, shipped in the zip as `ERVR\README.md`) has setup, every
  control, settings, co-op, body haptics and fixes; the modder internals moved to
  `docs/DEVELOPMENT.md`.
* Nexus description, release notes, issue forms and funding link point to the launcher, the
  Discord and Ko-fi.

## 0.4.0 (2026-09-10, never published; it ships in 0.6.0)

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
