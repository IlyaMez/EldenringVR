## Install

1. Download `{{ZIP}}` below and unpack it to a folder you can write to, such as `Documents\ERVR`.
2. Start **ERVR Launcher.exe**. If Windows says it protected your PC, choose *More info*, then *Run anyway*.
3. Fix anything the Play tab marks (the game and its patch, the VR runtime, the headset), connect your
   headset and press **START VR**. The launcher installs ERVR into the game folder, keeps your `ERVR.ini`,
   parks flat render mods and R.E.A.L. VR leftovers, and starts the game with EAC off, or through Seamless
   Co-op when it is installed.

Updating? Unpack over the old folder and press START VR. Prefer it by hand? Unpack the zip straight into
`ELDEN RING\Game`, next to `eldenring.exe`. Every control, setting and fix is in the
[players' guide](https://github.com/{{REPO}}/blob/main/docs/PLAYERS.md); what changed is in the
[changelog](https://github.com/{{REPO}}/blob/main/CHANGELOG.md).

> **Offline or Seamless Co-op only.** Never run ERVR with Easy Anti-Cheat on or in FromSoftware's online
> play: that risks a ban on your account.

Questions and bugs: `#ervr-help` on the [Discord](https://discord.gg/x27ZXDpdNt), with
`Game\ERVR\ERVR.log`. Release news is posted in `#ervr-updates` first.

## Verifying this download

**Your antivirus may flag this.** `ERVR.dll` injects into a running game process, and `dxgi.dll` is
ReShade's add-on build, which is unsigned by design. Both detections are generic, not a finding about
this code. You do not have to take my word for it:

```
SHA256  {{SHA256}}
```

This build was produced by a [public GitHub Actions workflow](https://github.com/{{REPO}}/actions/workflows/release.yml)
from the tagged commit, and carries a signed provenance attestation. Check it yourself:

```
gh attestation verify {{ZIP}} --repo {{REPO}}
```

If that passes, what you downloaded is byte for byte what this repository's workflow built from public
source. The bundled `dxgi.dll` is the unmodified `ReShade64.dll` from `ReShade_Setup_6.8.0_Addon.exe` (its
hash is in `THIRD_PARTY.md`: compare it against a fresh download from reshade.me). Or build it from source
with `.\build.ps1`.

## Notes

ERVR contains no telemetry, no analytics, no auto-updater, and no network code of its own. For body haptics
it loads bHaptics' own SDK library (`ERVR\bhaptics_library.dll`), which talks to the bHaptics Player on your
PC; `[bHaptics] Level=0` keeps it from loading.

ERVR is free. A tip on [Ko-fi](https://ko-fi.com/ilyamezerowsky) pays for updates and the next full VR
conversion. Free fan mod, not affiliated with or endorsed by FromSoftware or Bandai Namco.
