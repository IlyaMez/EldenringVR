## Install

1. Have **R.E.A.L. VR** installed and working first — ERVR does nothing on its own, and is not
   distributed with it. Get R.E.A.L. from its author.
2. Unpack this zip into your `ELDEN RING\Game` folder: `dinput8.dll` next to `eldenring.exe`, with
   the `ERVR` folder beside it.
3. Launch exactly as you launch R.E.A.L. Then read `Game\ERVR\ERVR.log` to confirm the VR backend
   attached and the XInput hook is live.

> ### ⚠️ Offline / EAC-disabled only
> Never run this with Easy Anti-Cheat enabled or while connected to online play. Anti-cheat and
> multiplayer are not supported and never will be.

## Verifying this download

**Your antivirus may flag this.** ERVR is a DLL that injects into a running game process, which is
behaviourally what a lot of malware does — the detection is generic, not a finding about this code.
You do not have to take my word for it:

```
SHA256  {{SHA256}}
```

This build was produced by a [public GitHub Actions workflow](https://github.com/{{REPO}}/actions/workflows/release.yml)
from the tagged commit, and carries a signed provenance attestation. Check it yourself:

```
gh attestation verify {{ZIP}} --repo {{REPO}}
```

If that passes, what you downloaded is byte-for-byte what this repository's workflow built from
public source. Or skip all of it and build from source with `.\build.ps1` — it takes a minute.

## Notes

ERVR contains no telemetry, no analytics, no auto-updater, and no network code. It never phones
home. Bugs go to the issue tracker **with a `LogLevel=debug` log attached**; everything else goes to
Discussions.
