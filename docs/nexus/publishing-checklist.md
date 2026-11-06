# Publishing on Nexus Mods

What goes where when creating or updating the ERVR page on Nexus Mods. The texts in this folder
are the source of truth; edit them here, then paste.

## Files in this folder

| File | Paste into |
|---|---|
| `summary.txt` | Mod page "Summary" (Nexus caps it at 255 characters; this one is 245). |
| `description.bbcode` | Mod page "Description" (the Nexus editor takes BBCode; switch the editor to BBCode/source mode before pasting). If list bullets land on the right, the editor flipped to right-to-left: paste in source mode, or Ctrl+A then Ctrl+Left Shift in the visual editor. |
| `changelog.txt` | "Changelogs" tab, one entry per version. |

## Cutting the release

1. Make sure `main` is what you want to ship and CI is green, and before the first tag:
   * the repository is **public**: the release workflow's provenance attestation is refused on a
     private repository on this plan, and every Releases link in the README and the launcher 404s
     while it is private;
   * the `ERVR_BHAPTICS_APP_ID` and `ERVR_BHAPTICS_API_KEY` repository secrets are set (see Body
     haptics below), or the release copy leaves body haptics out.
2. Tag and push. The release workflow builds, attests and publishes the GitHub release:
   ```powershell
   git tag v0.7.3
   git push origin v0.7.3
   ```
3. Download `ERVR-v0.7.3.zip` from the GitHub release, or build the identical file locally:
   ```powershell
   .\tools\package_release.ps1 -Build      # -> build\release\ERVR-v0.7.3.zip + .sha256
   ```
   Upload the **same zip** to Nexus that is on GitHub, so the SHA256 and the attestation in the
   release notes apply to both downloads.

## Page settings

* **Name**: `ERVR - native VR for ELDEN RING`
* **Category**: Gameplay (there is no VR category on the ELDEN RING Nexus; Utilities is the alternative).
* **Language**: English.
* **Tags**: Gameplay, Immersion, Utilities, Fair and balanced, Free.
* **Classification**: the mod contains DLLs that are loaded into the game process. Nexus asks you
  to state that; also state that it bundles ReShade (permission via its BSD-3 license, copy in
  `LICENSE-ReShade.txt` inside the zip).
* **Adult content**: no.
* **Requirements**
  * Off-site: "An OpenXR runtime (Meta Link / Air Link, Virtual Desktop VDXR, or SteamVR)".
  * Off-site: "ELDEN RING patch 1.16.2, 1.17.1 or 1.17.2 (exe 2.6.2.0 / 2.7.1.0 / 2.7.2.0), played offline
    with EAC off (the launcher starts it that way) or in Seamless Co-op".
  * No Nexus mod requirements. ReShade is bundled; R.E.A.L. VR is not used.
* **Mirrors**: link the GitHub Releases page.
* **Source**: `https://github.com/IlyaMez/EldenringVR`.

## Permissions (matches GPL-3.0-or-later)

* Other user's assets: **All the assets in this file belong to the author, or are from free-to-use
  modder's resources** (ReShade under BSD-3, MinHook BSD-2, OpenXR loader Apache-2.0; all credited).
* Upload permission: **You are allowed to upload this file to other sites but you must credit me
  as the creator of the file**, and keep the GPL license text with it.
* Modification permission: **You are allowed to modify my files and release bug fixes or improve on
  the features so long as you credit me as the original creator**. Add in the free-text box:
  "This mod is GPL-3.0-or-later. If you distribute a modified build you must publish its source
  under the same license."
* Conversion permission: allowed with credit (nothing to convert, but it costs nothing).
* Asset use permission: allowed with credit.
* Asset use permission in mods/files that are being sold: **not allowed** (GPL allows selling but
  the copyleft makes paywalled forks pointless; stating it here matches the README's stance).
* Asset use permission in mods/files that earn donation points: allowed.
* Donation Points: your call. The README already says every feature is and stays free; DP does
  not change that. Consistent with the GitHub tip jar, opting in is fine.
* Author notes: "Offline or Seamless Co-op only, never with EAC. No telemetry; ERVR.dll never goes online, the
  launcher only asks GitHub for a newer version (can be turned off) and downloads it on your click (body haptics
  use bHaptics' SDK library, which talks to the bHaptics Player). Help and bugs: #ervr-help
  on the Discord (discord.gg/x27ZXDpdNt) or GitHub issues, with ERVR.log."
* Body haptics: the release workflow needs the `ERVR_BHAPTICS_APP_ID` and `ERVR_BHAPTICS_API_KEY`
  repository secrets (the Developer Portal app's workspace ID and API key, the app deployed with at
  least one event); without them the zip's ERVR.dll logs `no app` and plays no body haptics.

## Files tab

* Main file: `ERVR-v0.7.3.zip`, version `0.7.3`, "Main Files" category.
* File description: paste the Install section from `description.bbcode` plus the two verification
  lines (SHA256 from the `.sha256` file, `gh attestation verify ERVR-v0.7.3.zip --repo IlyaMez/EldenringVR`).
* Tick "This file is a new version of an existing file" only for updates.
* Do not upload a separate ReShade or config file; the zip is complete.

## Images (needed before the page can go live)

Nexus requires at least one image. Ready in the repository: `docs/images/readme/hero.jpg` (the
README's banner), `controls-quest3.jpg` and `controls-actions.jpg`. More, captured in the headset:
1. A first-person in-game shot with a weapon in hand (hero image).
2. The stereo pair or the cinema screen, to show what "true stereo" means.
3. The HUD panel floating in front of a scene.
4. A two-hand grab or a swing mid-motion.

Every image must show the game offline. Avoid images with any online UI element.

## After posting

* Add the Nexus URL to the README's Install and play section and the players' guide, and to
  `.github/ISSUE_TEMPLATE/config.yml` if you want Nexus users routed to GitHub for bugs.
* Pin a "Known issues / read before posting" sticky in the Nexus Posts tab that repeats the EAC
  warning and the log requirement, and sends help questions to #ervr-help on the Discord.
* Consider locking the Bugs tab on Nexus and pointing it at GitHub, so reports arrive with logs.
