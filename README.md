# CounterStrikeXB

Static recompilation of the original Xbox **Counter-Strike** (2003) to a native Windows x64 executable, built on the [xboxrecomp](https://github.com/sp00nznet/xboxrecomp) toolkit.

The game's `default.xbe` is translated to C by the toolkit's pipeline and linked with the xboxrecomp runtime (kernel, GPU and USB models) plus a small amount of Counter-Strike-specific host code in `src/`.

**No game data is included.** You need your own legally dumped copy of the disc.

## Status (work in progress)

Previously verified:
- Boots and plays the intro movies (MGS and Valve logos), title screen and attract movie.
- Menus are fully navigable with the keyboard or an XInput pad (title START, profile creation, Main Menu, Training, Single Player, Xbox Live, System Link screens), full 640x480 framing.
- Loading a map: the game relaunches itself (`XLaunchNewImage`), which is modelled as a host relaunch (`src/cs_launch.c`). Training > Hostage Rescue and Single Player > Beginner > Airstrip both load in-game.

Current focus: reliable intros, flash-free menus, quicker loading, and real menu
rendering at 60 FPS or better, before returning to gameplay. The latest pinned
build completes both intros and reaches the main menu. Its measured main-menu
average is 36.8 FPS (18-42 FPS samples), improved from the roughly 12 FPS baseline
but still below target. Submenu entry and Back also worked in a bounded run.
Smooth movie cadence, absence of individual-frame flashes, and normal loading
times remain unverified. Earlier gameplay still had major glitches at about 1 FPS.

Known issues from the earlier gameplay baseline:
1. In-game world geometry (BSP brushes) and sky are not drawn; only props/foliage render. No first-person weapon (viewmodel).
2. HUD is drawn about 45% down the screen instead of at the screen edges; radar and crosshair misplaced; HUD digits ghosted; timer colon missing.
3. Performance: 0-4 FPS in a match, game clock runs at about half speed; map load takes about 2 minutes.
4. The window goes black for 1-2 minutes during the self-relaunch when a map loads (looks like a crash, but isn't).
5. Intro movies don't play smoothly.
6. Not yet verified with a physical XInput controller.

The current renderer is CPU-based; the optional D3D11 renderer does not yet
cover all the state needed by this game.

## Install (players)

Two ways, the same as the Marvel vs. Capcom 2 recompilation's:

- **Setup:** run `CounterStrike-Setup-<version>.exe` (Windows 10/11, 64-bit, no administrator rights needed). It asks
  for your own disc image (`.iso`/`.xiso`) or a folder holding `default.xbe`, checks it is the USA release, and copies
  the 1.6 GB of game files into `<install folder>\game_files`. Running a newer Setup over an install only replaces
  the program files (no disc needed); the same version offers Repair. Silent install:
  `CounterStrike-Setup-<version>.exe /VERYSILENT /SOURCE=<image or folder> /DIR=<folder>`.
- **RecompLauncher:** add the GitHub release (`CounterStrike-v<version>-Windows-x64.zip`). The launcher asks for the
  disc, checks it against Redump, runs the same import (`cs_import.exe --prepare`), and starts the game.

The game runs as its **title update** (version 257, Nov 2003, the version Xbox Live/Insignia expects). You still
supply only the original disc: Setup and the launcher turn its `default.xbe` into the update's with a small
patch (`cs_tu.xbepatch`, installed beside the game), so no download or update file is needed.

Your settings, key map, saves and logs are in `%LOCALAPPDATA%\CounterStrikeXB`.

### Optional DDS texture pack

Install it with `CounterStrike-HiRes-Textures-<version>-Setup.exe` from the
[textures release](https://github.com/dalves408/CounterStrikeXB/releases/tag/textures-1.0.0):
it finds the installed game, uses the `texture-NN.bin` parts beside it or downloads
the missing ones (resumable, each checked by size and SHA-256), and puts the pack in
`texture_pack` in the game folder. Run it again with `/REPAIRTEXTURES=1` to restore
damaged files. F1 > Hi-res textures (or `config.exe`) turns the pack off and on.

The D3D11 renderer looks for `texture_pack\index.tsv` next to the game executable.
Set `RECOMP_CS_TEXTURE_PACK` to an absolute pack folder to use another location,
or set it to `0` for the original textures. Restart the game after changing packs.
The pack contains 4× BC7_UNORM textures with full mip chains. Matches use the
original guest texture's content and dimensions, so maps and localized screens
can use different images with the same name. Missing or invalid files fall back
to the original texture. Render targets and indexed lookup data stay original.

With the extracted PNGs and manifests from the texture project, generate the
lookup index with `python scripts/index_texture_pack.py <asset-root>` (requires
Pillow). Use the resulting `texture_pack_dds` folder as your pack directory.

Building the add-on: `python scripts/build_texture_payload.py` makes the verified
`texture-NN.bin` parts and `texture-payload.json` in `installer\Output\texture-addon-<version>`;
`python scripts/build_texture_addon.py <that>\texture-payload.json --url-base <release download URL>/`
compiles the Setup with every part's URL, size and hash pinned (needs Inno Setup 6.4+ and the
`cs_texture_download` target); `python scripts/test_texture_addon.py <Setup.exe> [--download]`
installs it into a scratch folder and checks every file.

### Problem reports and logs

The game writes a log for every run and every map change to `%LOCALAPPDATA%\CounterStrikeXB\logs`
(`cs-<date>-<time>.log`, the newest five kept, your Windows user name blanked out). It records the build, the PC
(Windows, CPU, GPU, memory), the video settings, a `[FREEZE]` report if the picture stops for 10 seconds and a
`[CRASH]` report with code offsets if the game crashes.

- In `config.exe`, **Report a problem...** opens the logs folder with the newest log selected and a new GitHub issue
  filled in with the game version and PC; describe the problem and drag the log onto it.
- RecompLauncher's own crash report sends the newest log by itself (the manifest's `folders.logs`).

Release builds ship without symbols; `scripts/build_installer.py` keeps each release's `.pdb` in `out\symbols\<version>`
and `python scripts/symbolize_log.py <log>` names the offsets in a player's log.

### Building the installer

```powershell
python scripts/build_installer.py --version 1.0.0
```

Needs Inno Setup 6.4+ and Pillow. It builds the player configuration (`-DCS_RELEASE=ON`: static C runtime, data in
`%LOCALAPPDATA%`) into `build_release\`, then writes `installer\Output\CounterStrike-Setup-<version>.exe` and the
RecompLauncher ZIP, each with a `.sha256`. Both contain code translated from the game, so they are not in git.

## Requirements

- Windows 10/11 x64
- Visual Studio 2022 with the C/C++ desktop workload (CMake is used from PATH or from Visual Studio)
- Git, Python 3
- Your own dump of *Counter-Strike (USA)* as an XISO image. Expected SHA-256 values are in `dependencies.json` (`disc.image_sha256`, `disc.default_xbe_sha256`).

## Setup

```powershell
# 1. Clone the pinned toolkit into tools\xboxrecomp (v0.17.0 + local patches) and install capstone into .deps
powershell -ExecutionPolicy Bypass -File scripts\setup.ps1

# 2. Extract your disc image into game_files\
python scripts\extract_disc.py "Counter-Strike (USA) (En,Fr,Es,It,Ko).xiso.iso" game_files

# 3. Make game_files\default.xbe the title update's (the disc's is kept as default.disc.xbe). The patch is
#    built by `scripts\tu_patch.py make` from the update's default.xbe (TDATA\4d530036\$u on a console's drive),
#    and ships with every Setup as cs_tu.xbepatch.
python scripts\tu_patch.py apply --patch cs_tu.xbepatch

# 4. Generate the recompiled C (src\recomp\gen) from game_files\default.xbe (the title update's)
python scripts\pipeline.py

# 5. Build
powershell -ExecutionPolicy Bypass -File scripts\build.ps1 -Configuration RelWithDebInfo
```

Output: `build\RelWithDebInfo\cs2_xbox_recomp.exe`.

## Run

```powershell
powershell -ExecutionPolicy Bypass -File scripts\run.ps1
```

On the earlier verified build the title screen appeared after roughly 30-40
seconds; press START or A to skip the intros. Close the window to quit. Logs
are written to `runs\<label>\`, and saves to the project-local `SaveData\`.

Keyboard controls:

| Key | Xbox pad |
| --- | --- |
| Arrow keys | D-pad |
| Enter | START |
| Backspace | BACK |
| Z / X / C / V | A / B / X / Y |
| W A S D | Left stick |
| I J K L | Right stick |
| Q / E | White / Black |
| 1 / 3 | Left / Right trigger |

A connected XInput controller is also passed through.

These are the default keys; `config.exe` (next to the game, in
`build\RelWithDebInfo\`) changes them.

### In-game menus and settings

| Key | |
| --- | --- |
| Esc | "Quit Counter-Strike?" prompt (Esc again or No to go back) |
| F1 | Settings menu |
| F11 or Alt+Enter | Fullscreen on/off |

Both menus are drawn over the game, which keeps running underneath. While a
menu is open its keys (and controller 1, which also drives it) do not reach the
game. The F1 menu changes these as you play, and saves them as you go:

| Setting | Choices |
| --- | --- |
| Render scale | Low 1x (640x480, the Xbox's own; default), Medium 2x (1280x960), High 3x (1920x1440) |
| Fullscreen | borderless, on the window's monitor |
| Stretch image | fill the window instead of keeping 4:3 |
| VSync | on / off |
| Frame rate cap | 30, 60 (default), 120, 144, 240, uncapped |
| Anti-aliasing | off, 2x, 4x, 8x MSAA |
| Anisotropic filtering | off, 2x to 16x |

`config.exe` has the same options on its Video tab, for before the game starts,
and a Controls tab that sets a key and an alternate key for every Xbox control
(Esc, F1 and F11 are kept for the menus). Settings are stored in
`%LOCALAPPDATA%\CounterStrikeXB\settings.ini` and `keymap.ini`.

## Toolkit pin and local patches

`dependencies.json` pins xboxrecomp to tag v0.17.0 (`343b6d8`) plus nine local
patches in `patches/xboxrecomp/`, applied by `scripts/setup.ps1` with a fixed
identity so the resulting commit (`1e6abc3`) is reproducible:

- `0001` disasm: drop phase-shifted call targets (fixes the game's controller-open routine being split mid-function).
- `0002` pb_exec: present the display-sized 640x480 surface instead of the oversized back buffer.
- `0003` usb-ohci: keep waiting for the driver to start the controller when the game starts USB late.
- `0004` nv2a: unify fixed-function transform state and handle large raster bounds.
- `0005` nv2a: accumulate raster statistics per worker rather than per pixel.
- `0006` nv2a: honor face culling and triangle-strip winding.
- `0007` apu: synchronize voice updates and correct streamed-sample boundaries.
- `0008` nv2a: clear stopped video overlays and protect restarted banks.
- `0009` nv2a: prepare common combiner programs per draw, cache byte-validated
  decoded DXT images, and reduce per-pixel conversion/filtering work.

The generated code in `src/recomp/gen` is pure pipeline output and is never edited by hand; game-specific code lives in `src/`.

## Repository layout

- `src/` - host code: entry point, kernel/GPU hooks, relaunch handling, manual function overrides
- `scripts/` - setup, disc extraction, recompilation pipeline, build, run and probe scripts
- `seeds/` - function seeds for the pipeline
- `patches/xboxrecomp/` - local toolkit patches
- `docs/STATUS.md` - detailed verified status and evidence screenshots (`docs/evidence/`)
- `docs/HANDOFF.md` - short handoff: what works, what's broken, next steps

## Legal

This repository contains original source code, scripts and documentation, plus an icon (`resources/cs.ico`, `cs.png`) made from the game's logo. It does not contain any Counter-Strike game code, other assets or disc images; the release builds contain code translated from the game, and the game files come from the player's own disc. Counter-Strike is a trademark of Valve Corporation; this project is not affiliated with or endorsed by Valve or Microsoft.
