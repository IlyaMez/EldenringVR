# Contributing to ERVR

Thanks for looking. A few things up front so nobody wastes their time.

## What this project is

A hobby project. It is maintained when I have time and interest, and there is no SLA on issues or
pull requests. If something is broken and you need it fixed on a schedule, the fix is to send a
patch, not to wait on me.

## Before you open an issue

**Use the bug report template and attach `Game\ERVR\ERVR.log` with `LogLevel=debug` set.** Reports
without a log get closed. The log answers, in about five lines, questions that otherwise take a
dozen back-and-forth messages: which VR backend attached, whether the XInput hook is on the game's
call path, and what your actual swing speeds are.

Check the pinned Known issues in `#ervr-help` on the [Discord](https://discord.gg/x27ZXDpdNt) first: a good
fraction of reports are already there, and setup questions get answered there fastest.

These are out of scope and will be closed:

* Anything involving EAC or FromSoftware's online play. Offline or Seamless Co-op only, permanently. See the
  disclaimer in the README.
* Anything about R.E.A.L. VR. ERVR does not use it; remove its files before installing.
* Requests for game files, save editing, or anything that isn't VR input.
* Game versions other than the one in the README, unless you include the AOB scan results from
  `tools/verify_signatures.py`.

## Pull requests

Welcome, especially:

* Controller bindings and quirks for hardware I don't own (Index, WMR, Pico, Vive wands).
* Offsets and AOBs for game versions past 2.7.2.0, with the `verify_signatures.py` output showing
  the pattern is unique.
* Judge ids for stances I haven't covered (two-handing, power stance, specific weapon classes).

Practical notes:

* Match the surrounding style. C++20, MSVC, no exceptions in hook paths, no allocation on the
  game's threads.
* Anything running inside a detour must be reentrancy-safe and must not block. A hang in
  `XInputGetState` or the skeleton hook freezes the game.
* New memory reads go through `game_memory.h` so they fail soft on a layout change rather than
  crashing someone's run.
* Say in the PR which game version and which headset you actually tested on. "Compiles" is not
  tested.

Camera anchoring, pose-coordinate, and action-queue regression checks can run without the game or a headset:

```powershell
cmake -S . -B build -A x64 -DERVR_BUILD_REGRESSION_TESTS=ON
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

## What I will not merge

Stated plainly so we can skip the debate later:

* **Telemetry or analytics of any kind.** The mod does not phone home. It never will.
* **Auto-updaters**, or anything that fetches and runs code at runtime.
* **Advertising, sponsor messages, or bundled software** in the DLL, the installer, or the log.
* **Paywalls or license checks** on any feature.
* Obfuscation, anti-tamper, or anything that makes the binary harder to audit. People are injecting
  this into a process on their own machine; they are entitled to read every line of it.

## Licensing

ERVR is GPL-3.0-or-later. By submitting a pull request you agree your contribution is licensed the
same way. There is no CLA and I am not asking for copyright assignment: you keep your copyright.

If you fork this and distribute your fork, GPL-3.0 requires you to publish your source too. That
requirement is the reason for the license choice: it keeps derivative builds of this work
inspectable and free, rather than sitting behind someone's subscription.
