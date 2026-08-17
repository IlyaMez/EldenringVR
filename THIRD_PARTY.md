# Third-party components

ERVR itself is licensed under GPL-3.0-or-later (see [LICENSE](LICENSE)). It links against, or
derives knowledge from, the projects below. Nothing here is redistributed in this repository —
the build fetches sources at configure time.

## Linked at build time

| Component | Version | License | How it is used |
|---|---|---|---|
| [MinHook](https://github.com/TsudaKageyu/minhook) | v1.3.4 | BSD-2-Clause | Inline function hooking. Fetched by CMake `FetchContent`. |
| [HDE64](https://github.com/TsudaKageyu/minhook) (bundled inside MinHook) | v1.3.4 | BSD-2-Clause | Length-disassembler, reused for ERVR's mid-function hooks (`src/ervr/midhook.cpp`). |
| [OpenVR](https://github.com/ValveSoftware/openvr) headers | v1.16.8 | BSD-3-Clause | Headers only, to bind to the `openvr_api.dll` already loaded in the process. No Valve binaries are shipped or linked statically. |

Both licenses are permissive and compatible with GPL-3.0. Their copyright notices travel with
their own source trees, which CMake places under `build/_deps/`.

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

## Not included, and required separately

**R.E.A.L. VR** by Luke Ross is a prerequisite for using ERVR. It is a separate, closed-source,
independently distributed mod. This repository contains **no R.E.A.L. code, headers, binaries, or
decompiled output**, and none of ERVR's source refers to R.E.A.L. internals. ERVR interoperates
only through the public Oculus (`LibOVRRT64_1.dll`) and OpenVR (`openvr_api.dll`) runtime APIs,
which are Meta's and Valve's respectively.

Users must obtain R.E.A.L. themselves from its author. Do not open issues here asking where to get
it, and do not attach R.E.A.L. files to issues or pull requests.

## Game assets

No ELDEN RING code, assets, or binaries are included. ERVR reads and writes the memory of a
running, legitimately owned copy of the game. Addresses and byte patterns in this repository are
ERVR's own research output, not copied game code.
