# Original-game reference capture (Windows + nGlide)

Automation for the golden reference set: screenshots (and later feel
recordings) from the retail 333B game rendered through Glide/nGlide and driven
with synthesized keystrokes. Captures are written under
`exports/reference/shots/<Map>/` in your private reference directory and are
never committed; each map manifest records its camera poses.

## Scripts (PowerShell 5.1, repo root, Windows only)

- `extract_playerstarts.ps1` parses `exports/t3d/<Map>.t3d` and emits
  `exports/reference/shots/poses.json`.
- `capture_original_shots.ps1 -Maps Arena_01[,...]` launches one fresh game per
  map, sends `ToggleUI`, `ghost`, and `fly`, applies each pose, and captures F9
  screenshots. It stages `Shot####.bmp` plus `manifest.json` under
  `%TEMP%\wot-capture-stage` by default.
- Pass `-SystemDir <private-copy\System>` for diagnostic work. The driver stores
  the exact process it starts and focuses, exits, or force-stops only that PID;
  it never searches for or terminates another `WoT.exe` by image name.
- `convert_reference_shots.ps1` converts staged BMPs to named PNGs in the
  reference tree and flags suspicious runs of near-identical frames.
- `wot_sendkeys.ps1` is the ad-hoc interactive key/console-command helper.

## Camera-pose contract

- Ordinary PlayerStart poses use collision-aware `JumpTo x y z`, integer UE1
  rotation units (65536 per revolution), and normally `pitch=0, roll=0`.
- A diagnostic pose may provide fractional coordinates, integer `pitch` and
  `roll`, and `direct_location=true`. That option writes the exact
  `WOTPlayer Location` property when `JumpTo` would reject a point inside a
  volume or brush. The manifest records which location command was used.
- In the `ghost`/`fly` capture state, the pawn is in its jump/fall animation and
  EyeHeight is `0.7 * BaseEyeHeight(42) = 29.4`, measured by a
  parallax fit. FOV is 90. Priority maps also receive a reverse view (+32768
  yaw).
- Golden captures use 1600x1200, 16-bit Glide, brightness 0.6, and default
  nGlide configuration. F9 reads the Glide LFB, so the image is the real 4:3
  framebuffer regardless of desktop stretching.

## Runtime facts

- `SendInput` requires the full 40-byte x64 `INPUT` union, including
  `MOUSEINPUT`; the keyboard-only layout silently fails.
- `Running.ini` is the crash marker. A forced exit can leave it behind and
  trigger Recovery Mode, so the driver removes it before its private launch and
  prefers a graceful console `exit`.
- `WoT.log` is buffered. The driver audits its flushed copy after exit for every
  expected `Console Command:` echo; a missing command discards and retries the
  map.
- `FirstRun=333` suppresses the first-run dialog. The private copy must set
  `[Engine.Engine] GameRenderDevice=GlideDrv.GlideRenderDevice` explicitly.
- Synthesized input is accepted only while the owned game window is foreground;
  the driver re-focuses that exact window before every send.
- `JumpTo`, `ghost`, `fly`, and `set` are valid in standalone play.
  `ToggleUI` hides the HUD/crosshair; the driver waits for console text to fade.

## nGlide notes

With no `HKCU\SOFTWARE\Zeus Software\Glide` key, nGlide uses its defaults. The
desktop presentation may stretch, but LFB captures do not.
