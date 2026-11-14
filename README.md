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

# 3. Generate the recompiled C (src\recomp\gen) from game_files\default.xbe
python scripts\pipeline.py

# 4. Build
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
| Frame rate cap | 30, 60, 120, 144, 240, uncapped (default) |
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

This repository contains only original source code, scripts and documentation. It does not contain any Counter-Strike game code, assets or disc images. Counter-Strike is a trademark of Valve Corporation; this project is not affiliated with or endorsed by Valve or Microsoft.
