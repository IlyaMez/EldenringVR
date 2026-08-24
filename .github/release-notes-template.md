## Install

1. **Had R.E.A.L. VR installed?** Delete its `dxgi.dll`, `RealVR64.dll`, `openvr_api.dll`,
   `cudart64_12.dll` and `RealVR.ini` first. ERVR does not use R.E.A.L., and both want to be `dxgi.dll`.
2. Unpack this zip into your `ELDEN RING\Game` folder, next to `eldenring.exe`. You get `dxgi.dll`
   (ReShade 6.8.0 with add-on support, bundled), `ReShade.ini`, `dinput8.dll` (loader) and the `ERVR`
   folder. Already using ReShade or a `dinput8.dll` mod loader? See the README's Download section.
3. Make sure your headset's OpenXR runtime is active (Meta Link / Air Link, Virtual Desktop VDXR, or
   SteamVR), launch the game offline with EAC disabled, and put the headset on.
4. `Game\ERVR\ERVR.log` should show `Active VR backend: openxr` and `[reshade] registered as add-on`.
   If the second line never appears, `dxgi.dll` is not the bundled one.

> ### ⚠️ Offline / EAC-disabled only
> Never run this with Easy Anti-Cheat enabled or while connected to online play. Anti-cheat and
> multiplayer are not supported and never will be.

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

If that passes, what you downloaded is byte-for-byte what this repository's workflow built from
public source. The bundled `dxgi.dll` is the unmodified `ReShade64.dll` from
`ReShade_Setup_6.8.0_Addon.exe` (hash in `THIRD_PARTY.md` — compare it against a fresh download from
reshade.me). Or skip all of it and build from source with `.\build.ps1` — it takes a minute.

## Notes

ERVR contains no telemetry, no analytics, no auto-updater, and no network code. It never phones
home. Bugs go to the issue tracker **with a `LogLevel=debug` log attached**; everything else goes to
Discussions.
