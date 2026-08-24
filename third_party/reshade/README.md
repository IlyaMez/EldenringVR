# Bundled ReShade

`ReShade64.dll` is the **unmodified** 64-bit ReShade binary *with add-on support*, as installed by
`ReShade_Setup_6.8.0_Addon.exe` from <https://reshade.me>. The build copies it into the release as
`dxgi.dll`; ERVR.dll registers itself with it as an add-on at runtime.

| | |
|---|---|
| Version | 6.8.0.2155 |
| SHA256 | `0cee63f9c9f13f3ac909c5b4903f4dbb4b719a7ab3b4f13b0deaf83c814b94f7` |
| License | BSD-3-Clause, [LICENSE.md](LICENSE.md) (Copyright 2014 Patrick Mours) |
| Source | <https://github.com/crosire/reshade> (tag v6.8.0) |

The add-on build is deliberately **unsigned** — it is the variant anti-cheat vendors do not
whitelist, which is fine because ERVR is offline/EAC-off only. Antivirus heuristics may flag it;
compare the hash above against a fresh download of the installer if in doubt.

`ReShade.ini` is a minimal default config (tutorial dismissed, overlay on Home, screenshot on
PrintScreen, no effects). Installers keep any `ReShade.ini` the user already has.

Updating: install the newer add-on setup into any game, copy the resulting `dxgi.dll`
(or `ReShade64.dll`) here, update the version/hash in this file and in `THIRD_PARTY.md`, and bump
the `reshade` `GIT_TAG` in `CMakeLists.txt` so the headers ERVR compiles against match.
