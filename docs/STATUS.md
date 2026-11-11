# CounterStrikeXB2 - status (verified facts only)

Fresh rebuild of the Counter-Strike (Xbox, 0x4D530036) -> Windows static recomp.
Old project `C:\Recomp\CounterStrikeXB` and the shared `C:\Recomp\tools\xboxrecomp`
are read-only references. Generated code (`src/recomp/gen/`, gitignored) is pure
pipeline output; every fix lives in hand-written files under `src/`.

## Goal (David)
1. Clean boot through intros to the real menus, rendering correctly (no menu bypass).
2. Proper controller (XInput -> USB XID via toolkit OHCI/XAPI) and keyboard input in
   hand-written code. No guest-memory pokes.
Map loading / gameplay: out of scope for now.

## How to rebuild from scratch
- `scripts\setup.ps1` : private toolkit clone at `tools\xboxrecomp`, pinned in
  `dependencies.json` (xboxrecomp v0.17.0 `343b6d8`), capstone 5.0.9 in `.deps\`.
- `scripts\extract_disc.py` : `game_files\` from the ISO (sha256 in dependencies.json).
- `scripts\pipeline.py` : regenerate `src\recomp\gen\` (seed 0x000C6310; 14343 functions).
- `scripts\build.ps1` : `build\RelWithDebInfo\cs2_xbox_recomp.exe`.
- `scripts\run.ps1 [-Seconds N]` : launch; logs in `runs\<label>\`. Scrubs inherited
  RECOMP_*/CS_* env. SaveData is project-local (`SaveData\`).
- `scripts\probe.ps1 -Label x -Seconds N -Keys '60:Return'` : bounded run with FB
  snapshots (`snap_<sec>.png`) and a keyboard schedule via `send_key.ps1`.

## Hand-written CS code (all fixes live here)
- `src/main.c` : host entry from toolkit template, entry 0x00202C3C, CS_SAVE_ROOT.
- `src/cs_gpu.c` : registers toolkit `xbox_Nv2aMirrorFence` for the D3D8LTCG
  completion fence (device ptr 0x316FE8, +0x2C put, +0x30 -> semaphore). Without it
  device creation spins in BlockOnTime (sub_0030A000). Re-derived from disassembly.
- `src/cs_kernel.c` : NtReadFile wrapper keeps IO_STATUS_BLOCK on synchronous NT
  error (NT semantics). Fixed first-boot "problem with the disc" (Cache error
  0x005522E8) caused by SXWad read-ahead past EOF.
- `src/recomp_manual.c` : manual lookup hook (kernel override dispatch).

## Commit log (verified with git log)
- 3f12ec1 skeleton; 2d0dd32 clean extraction + pure pipeline; 752ed93 host entry,
  SaveData, OHCI USB init; 61dba5d launcher + key sender; 20e5c8a env scrub + probe
  harness; 712dc9a D3D completion fence; 65f5e07 toolkit v0.17.0 + regen;
  76ed79f NtReadFile IOSB fix (fresh cache built in runs/v17_fresh2: 37 files,
  56,682,396 B).

## Where boot gets to (runs/v17_fresh2, 2026-11-11 ~04:53 PT)
- Kernel init OK, AvSetDisplayMode 640x480 fmt 0x12 fb 0xD44000.
- `d:\Media\Videos\MGS_Logo.xmv` opens (status 0) but nothing visible: FB stays
  black (`[GPU] brightest pixel written 0x00000000`, `[PB] 0 pushbuffer calls followed`,
  427 batches drawn with all vertex x/y = 0).
- Game proceeds to load valve/cstrike data, `launcher/` files etc.
- OPEN: why no visible output (intros/menus). Next investigation target.

## Borrowings from other projects
- NV2A fence-mirror pattern: concept from old CS project / toolkit (Wreckless);
  addresses re-derived for this XBE.

## Rules in force
Small local commits, never push. Bounded test runs, kill only own PIDs.
