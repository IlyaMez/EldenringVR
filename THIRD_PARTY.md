# Third-party components

ERVR itself is licensed under GPL-3.0-or-later (see [LICENSE](LICENSE)). It redistributes one
third-party binary, links against a few libraries fetched at configure time, and derives knowledge
from published research. All of the licenses below are permissive and GPL-3.0-compatible.

## Redistributed in the release zip

| Component | Version | License | How it is used |
|---|---|---|---|
| [ReShade](https://reshade.me) *with add-on support* | 6.8.0.2155 | BSD-3-Clause | Shipped **unmodified** as `dxgi.dll`. ERVR.dll registers itself as a ReShade add-on and does all of its rendering work (D3D12 frame capture, stereo, HUD quad, OpenXR submission) inside ReShade's `present` event. |

The binary is byte-for-byte the `ReShade64.dll` installed by `ReShade_Setup_6.8.0_Addon.exe`,
SHA256 `0cee63f9c9f13f3ac909c5b4903f4dbb4b719a7ab3b4f13b0deaf83c814b94f7`. It lives in
[`third_party/reshade/`](third_party/reshade/) together with its license, which the release zip
carries as `LICENSE-ReShade.txt`. The add-on build is unsigned by design (it is the variant
anti-cheat vendors do not whitelist); ERVR is offline/EAC-off only, so that costs nothing.

## Linked at build time

| Component | Version | License | How it is used |
|---|---|---|---|
| [MinHook](https://github.com/TsudaKageyu/minhook) | v1.3.4 | BSD-2-Clause | Inline function hooking. Fetched by CMake `FetchContent`. |
| [HDE64](https://github.com/TsudaKageyu/minhook) (bundled inside MinHook) | v1.3.4 | BSD-2-Clause | Length-disassembler, reused for ERVR's mid-function hooks (`src/ervr/midhook.cpp`). |
| [ReShade headers](https://github.com/crosire/reshade) (`include/reshade*.hpp`) | v6.8.0 | BSD-3-Clause | Header-only add-on API. `reshade.hpp` finds the loaded ReShade module at runtime; nothing is linked. |
| [OpenXR SDK loader](https://github.com/KhronosGroup/OpenXR-SDK) | release-1.1.62 | Apache-2.0 | Statically linked into `ERVR.dll` so the distribution stays a single DLL. ERVR creates and owns its own `XrSession` bound to the game's D3D12 device. |
| [JsonCpp](https://github.com/open-source-parsers/jsoncpp) (vendored by the OpenXR loader) | as shipped with the loader | MIT / Public Domain | Runtime-manifest parsing inside the loader. |
| [Dear ImGui](https://github.com/ocornut/imgui) | v1.92.9b | MIT | The window of `ERVR Launcher.exe` (Win32 + DirectX 11 backends). Fetched by CMake `FetchContent`; not in ERVR.dll. |
| [Barlow](https://github.com/jpt/barlow) (Medium, SemiBold, Condensed Bold and SemiBold) | as in the repository | SIL OFL 1.1 | The launcher's text, compiled into `ERVR Launcher.exe`. Files and license in [`src/launcher/fonts/`](src/launcher/fonts/). |
| [Cinzel](https://github.com/NDISCOVER/Cinzel) (Bold) | as in the repository | SIL OFL 1.1 | The launcher's name, headlines and tabs, compiled into `ERVR Launcher.exe`. Files and license in [`src/launcher/fonts/`](src/launcher/fonts/). |

Their copyright notices travel with their own source trees, which CMake places under `build/_deps/`.
The OpenXR loader is also linked into the launcher, for its headset check.

## Knowledge and reference, not code

No source from the following is copied into this repository. They are credited because ERVR's
memory layouts and event templates were derived from their published research:

* [DSAnimStudio](https://github.com/Meowmaritus/DSAnimStudio) (Meowmaritus) — TAE event templates,
  specifically the `AttackBehavior` argument layout in `TAE.Template.ER.xml`.
* [fromsoftware-rs](https://github.com/vswarte/fromsoftware-rs) and erfps2 (Dasaav) —
  `CSChrTaeAnimEvent` and `ChrIns` module layouts, camera step and head-hiding chains.
* Souls Modding wiki (Muffin and contributors) — `BehaviorParam` row scheme.
* [Halo-MCC-VR](https://github.com/pancreations/Halo-MCC-VR),
  [cyberpunk-vr-port](https://github.com/dariulone/cyberpunk-vr-port) — structural inspiration.

## Provided by the user's system

An **OpenXR runtime** — Meta's (Quest Link / Air Link), Virtual Desktop's VDXR, or SteamVR — is
required and not shipped. ERVR talks to whichever is the active system OpenXR runtime.

## Not used: R.E.A.L. VR

Early ERVR versions were a companion to Luke Ross's R.E.A.L. VR and attached to the session it
opened. Since the OpenXR rewrite ERVR renders on its own and **R.E.A.L. is neither required nor
compatible** — both install as `dxgi.dll`. Remove R.E.A.L.'s files (`dxgi.dll`, `RealVR64.dll`,
`openvr_api.dll`, `cudart64_12.dll`, `RealVR.ini`) before installing ERVR. This repository has never
contained R.E.A.L. code, headers, binaries, or decompiled output.

## Game assets

No ELDEN RING code, assets, or binaries are included. ERVR reads and writes the memory of a
running, legitimately owned copy of the game. Addresses and byte patterns in this repository are
ERVR's own research output, not copied game code.
