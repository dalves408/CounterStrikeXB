# CounterStrikeXB

A static recompilation of the original Xbox **Counter-Strike** (2003) into a native Windows x64 program, built on the
[xboxrecomp](https://github.com/sp00nznet/xboxrecomp) toolkit. The game's own code is translated to C and runs
natively, with the Xbox's kernel, GPU, audio, USB and network hardware modelled around it, and a Direct3D 11
renderer drawing it at up to 3x the original resolution.

**No game data is included.** You need your own legally dumped copy of the disc (*Counter-Strike (USA)*).

## Features

- **The whole game:** training, single player with bots, every map, at 60 FPS (72 on 144 Hz and FreeSync/G-Sync
  monitors).
- **Direct3D 11 renderer:** render scale 1x/2x/3x (640x480 to 1920x1440), MSAA up to 8x, anisotropic filtering up
  to 16x, VSync, a frame-rate cap of 30, 60 or 72, fullscreen, 4:3 or stretched. Picks the fast GPU on
  laptops with two.
- **Optional hi-res texture pack:** 5,362 recreated 4x textures (BC7), loaded in the background as they appear.
- **System link:** LAN play between PCs, and online with friends over a VPN, no Xbox needed: Tailscale is found by
  itself, any other VPN works with a peer list.
- **Xbox Live through [Insignia](https://insignia.live)** with your own console's identity: sign in, play online,
  and recover your Insignia account straight into the game.
- **Title update 257** (the version Xbox Live expects), made from your own disc: no download needed.
- **Office and Inferno,** the two downloadable maps, unlocked from the disc.
- **Settings that apply as you play:** F1 in the game, `config.exe`, or RecompLauncher.
- **Controller and keyboard:** XInput pads pass straight through; every key can be remapped.
- **Logs and problem reports** that RecompLauncher and `config.exe` can attach to a report.

## Install

- **Setup:** run `CounterStrike-Setup-<version>.exe` from the
  [latest release](https://github.com/dalves408/CounterStrikeXB/releases/latest) (Windows 10/11, 64-bit, no
  administrator rights needed). It asks for your disc image (`.iso`/`.xiso`) or a folder holding `default.xbe`,
  checks it is the USA release, and copies the 1.6 GB of game files into `<install folder>\game_files`. It can also
  install the hi-res texture pack, downloading it if the parts are not beside it. Running a newer Setup over an
  install only replaces the program files; the same version offers Repair. Silent install:
  `CounterStrike-Setup-<version>.exe /VERYSILENT /SOURCE=<image or folder> /DIR=<folder>`.
- **RecompLauncher:** add the release's `CounterStrike-v<version>-Windows-x64.zip`. The launcher asks for the disc,
  checks it against Redump, prepares it and starts the game. Its Big Picture mode always starts the game fullscreen.

Your settings, key map, saves, Xbox Live identity and logs are in `%LOCALAPPDATA%\CounterStrikeXB`. A new install
starts fullscreen; an upgrade or repair keeps your settings.

### Hi-res texture pack

Tick the texture task in Setup, or install it later with `CounterStrike-HiRes-Textures-<version>-Setup.exe` from the
[textures release](https://github.com/dalves408/CounterStrikeXB/releases/tag/textures-1.0.0). Either one uses the
`texture-NN.bin` parts placed beside it or downloads the missing ones (resumable, each checked by size and SHA-256),
and puts the pack in `texture_pack` in the game folder (6.26 GB). Run the add-on again with `/REPAIRTEXTURES=1` to
restore damaged files. F1 > Hi-res textures turns it off and on as you play.

## Playing

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

A connected XInput controller works as is. `config.exe` (in the game folder) remaps every control, with an
alternate key for each.

| Key | |
| --- | --- |
| F1, or hold BACK and press START on the pad | Settings menu |
| Esc | "Quit Counter-Strike?" |
| F11 or Alt+Enter | Fullscreen on/off |

Both menus work with the controller too: D-pad or left stick to move, A to confirm, B to close (the keys mapped to A
and B do the same). The settings menu also has Quit Counter-Strike, so the game can be closed from the pad. It is
drawn over the game and changes these as you play, saving them as you go:

| Setting | Choices |
| --- | --- |
| Render scale | 1x (640x480, default), 2x (1280x960), 3x (1920x1440) |
| Fullscreen | borderless, on the window's monitor (default on) |
| Stretch image | fill the window instead of keeping 4:3 |
| VSync | on / off |
| Frame rate cap | 30, 60 (default), 72 (on 144 Hz or FreeSync/G-Sync monitors) |
| Anti-aliasing | off, 2x, 4x, 8x MSAA |
| Anisotropic filtering | off, 2x to 16x |
| Hi-res textures | on / off (when the pack is installed) |

72 FPS is the game's own limit, as on the PC version of the time. It is offered only where it looks smooth: on a
144 Hz monitor (two refreshes per frame) or one with FreeSync/G-Sync Compatible; on other monitors 60 is the top,
which is smooth on 60, 120 and 240 Hz. A native G-Sync monitor that does not report adaptive sync gets 72 at 144 Hz.
Where the cap divides the monitor's refresh rate (30 on 60 Hz, 60 on 120 Hz, 72 on 144 Hz) the display paces it,
one frame every few refreshes, so every frame is shown for the same time.

**Xbox Live matches always run at 30 FPS,** as on the console. The game sends one movement command per frame, and
Insignia's hosts (consoles and xemu) correct the movement of a faster client, which makes the view stutter whenever
you move. Menus, single player and System Link keep your cap.

`config.exe` has the same options, and changes made there or in RecompLauncher while the game runs apply within a
second. Everything is stored in `%LOCALAPPDATA%\CounterStrikeXB\settings.ini` and `keymap.ini`.

### System link

System Link works between PCs on the same network: the games find each other by themselves.

### Online without an Xbox: System Link over a VPN

You can play online with friends with no original Xbox, no EEPROM and no Insignia account: System Link runs over a
VPN as if everyone were on one home network. There is no matchmaking or server list, so agree on a time and who hosts
on Discord (or however your group talks).

1. Everyone installs [Tailscale](https://tailscale.com) (free). One person creates the network and sends the others
   an invite; everyone joins it.
2. With Tailscale connected, start the game and choose **System Link**. One player creates the match; the others
   see it and join.

While Tailscale is connected the game asks it for the other PCs by itself, so there is nothing to type. Without
Tailscale it only searches the local network.

Another VPN works too. For partners the game does not find by itself, list their VPN addresses in `settings.ini`:

```ini
[network]
peers=100.64.0.2,100.64.0.3
```

### Xbox Live (Insignia)

[Insignia](https://insignia.live) revives the original Xbox Live. The game signs in as your own console, so you need:

1. A console registered with Insignia (with Insignia's Setup Assistant, see their
   [connection guide](https://insignia.live/guide/connect)).
2. That console's **EEPROM** (`eeprom.bin`, from your console or xemu).
3. Its **hard-drive key**: typed in, read from `hddinfo.txt`, or decrypted from your console's own BIOS and MCPX
   boot ROM dumps.
4. Your **Xbox Live account**: recovered from Insignia with your gamertag and the 16-digit number on your
   insignia.live Web Dashboard, or loaded from a dump of your console's drive (or your xemu drive).

**Insignia Setup** (`insignia_setup.exe`, in the game folder) takes all of these and checks each one. Recover from
Insignia briefly starts the game, which signs your console in, fetches the account and closes by itself. The recovery
number is never logged or stored. Creating a brand-new account still needs the Xbox dashboard on a console.

Then choose Xbox Live in the game and pick your gamertag. Matches run at 30 FPS, like everyone else's on Insignia.

### Problem reports and logs

Every run writes a log to `%LOCALAPPDATA%\CounterStrikeXB\logs` (the newest five are kept, your Windows user name
blanked out, the console's network address shortened to its maker prefix). It records the build, the PC, the video
settings and the frame-rate top picked for your monitor, an `[FPS]` line every minute of play (average FPS, 1% low,
longest frame, frames over 50 and 100 ms), a `[FREEZE]` report if the picture stops for 10 seconds and a `[CRASH]`
report if the game crashes.

- In `config.exe`, **Report a problem...** opens the logs folder with the newest log selected and a new GitHub issue
  filled in with the game version and PC; describe the problem and drag the log onto it.
- RecompLauncher's crash report attaches the newest log by itself.

## Building from source

Requirements: Windows 10/11 x64, Visual Studio 2022 with the C/C++ desktop workload, Git, Python 3, and your own
*Counter-Strike (USA)* XISO image (its expected SHA-256 values are in `dependencies.json`).

```powershell
# 1. Clone the pinned toolkit into tools\xboxrecomp (v0.17.0 + this project's patches)
powershell -ExecutionPolicy Bypass -File scripts\setup.ps1

# 2. Extract your disc image into game_files\
python scripts\extract_disc.py "Counter-Strike (USA) (En,Fr,Es,It,Ko).xiso.iso" game_files

# 3. Make game_files\default.xbe the title update's (the disc's is kept as default.disc.xbe)
python scripts\tu_patch.py apply --patch cs_tu.xbepatch

# 4. Translate default.xbe to C (src\recomp\gen)
python scripts\pipeline.py

# 5. Build
powershell -ExecutionPolicy Bypass -File scripts\build.ps1 -Configuration RelWithDebInfo

# Run (development settings, logs in runs\<label>\)
powershell -ExecutionPolicy Bypass -File scripts\run.ps1
```

`cs_tu.xbepatch` ships with every Setup; `scripts\tu_patch.py make` builds it from the update's `default.xbe`.

**Installer:** `python scripts\build_installer.py --version 1.0.0` (Inno Setup 6.4+ and Pillow) builds the player
configuration into `build_release\` and writes `installer\Output\CounterStrike-Setup-<version>.exe` and the
RecompLauncher ZIP, each with a `.sha256`, and keeps the release's symbols in `out\symbols\<version>` for
`python scripts\symbolize_log.py <log>`.

**Texture add-on:** `python scripts\build_texture_payload.py` makes the `texture-NN.bin` parts and
`texture-payload.json`; `python scripts\build_texture_addon.py <payload.json>` compiles its Setup with every part's
URL, size and hash pinned; `python scripts\test_texture_addon.py <Setup.exe>` installs it into a scratch folder and
checks every file. `python scripts\index_texture_pack.py <asset-root>` builds a pack's lookup index.

## How it is put together

- `src/` - Counter-Strike's host code: entry point and settings, the Direct3D 11 renderer, the texture pack,
  the F1/Esc menus, relaunch handling, the downloadable maps, Xbox Live account recovery, logs.
- `config/` - `config.exe`, the settings and controls program.
- `insignia/` - `insignia_setup.exe`.
- `installer/` - the Setup scripts, the disc importer and the title update patcher.
- `scripts/` - setup, disc extraction, the translation pipeline, build, run and release scripts.
- `seeds/` - function seeds for the pipeline.
- `patches/xboxrecomp/` - this project's changes to the toolkit (renderer, audio, USB, the network card and its
  router, console identity, the emulated hard disk, and more), applied by `scripts/setup.ps1` with a fixed identity
  so the pinned commit in `dependencies.json` is reproducible.

`src/recomp/gen` is pure pipeline output, generated from your disc, and is never edited by hand or committed.

## License

CounterStrikeXB is under the [PolyForm Strict License 1.0.0](LICENSE): you may read, study, build and play it for
personal, noncommercial use, but not change it or share copies, changed or not. Get the game from this project's
[releases](https://github.com/dalves408/CounterStrikeXB/releases). The toolkit patches in `patches/xboxrecomp/` stay
under the toolkit's MIT License.

**Why:** the game signs in to Xbox Live through Insignia, a community-run service shared with players on real
consoles. Keeping modified builds off it protects everyone there from cheating. Bug reports and suggestions are
welcome as [issues](https://github.com/dalves408/CounterStrikeXB/issues).

## Legal

This repository contains original source code, scripts and documentation, plus an icon (`resources/cs.ico`,
`cs.png`) made from the game's logo. It contains no Counter-Strike game code, assets or disc
images, and nothing from the Xbox dashboard. Release builds contain code translated from the game; the game files
come from the player's own disc. Xbox Live play goes through Insignia, an independent revival service.
Counter-Strike is a trademark of Valve Corporation; this project is not affiliated with or endorsed by Valve,
Microsoft or Insignia.
