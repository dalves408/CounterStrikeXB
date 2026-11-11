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
- Kernel init OK, AvSetDisplayMode 640x480 fmt 0x12.
- NOTE: run.ps1 defaults alone (runs/b1_long) hang earlier in an IRQL spin; the
  working set is `-ExtraEnv 'RECOMP_PB_EXEC=1;RECOMP_CONTIG_AVOID_IMAGE=1;RECOMP_AC97_READY=1;RECOMP_VBLANK=paced'`
  (to be baked into run.ps1 once confirmed).

## Current blocker: intro movies use the NV2A video overlay (PVIDEO) - not modelled
Evidence (runs/b2_full, b3_wd, b5_ftrace, b6_peek; 2026-11-11 05:00-05:25 PT):
- Boot flow sub at 0x000AD8A0: 0x000ADB46 plays d:\Media\Videos\MGS_Logo.xmv, then
  Valve_Logo.xmv (0x000ADB53) via movie player 0x000C6F70, then waits for cache
  flag [0x552D68] (loop 0x000ADB78), optional ESRB.xmv, then `open SuperLauncher_Root`
  (0x000ADBE5) = the real menus.
- Movie loop 0x000C7183..0x000C72D2: XMVDecoder_GetNextFrame (0x0029030E) via 0x000C6D40;
  status 2 sets end flag [0x552308]=1 (a pad button >0x1E at 0x384F7A table skips).
- Frames are shown with D3D UpdateOverlay (YUY2 overlay plane), not drawn: the PB
  frame trace shows only a full-screen copy of the black back buffer.
- b6_peek: [0x552308] becomes 1 (~15 s) -> movie DID finish. Then cleanup 0x000C6BC0
  calls D3D EnableOverlay(FALSE) at 0x0030A8D0: writes NV_PVIDEO_STOP
  (regs+0x8704 = 1) and spins until NV_PVIDEO_BUFFER (regs+0x8700) == 0. Nothing models
  PVIDEO, so BUFFER never clears -> permanent hang, black screen (b2_full: triangle count
  frozen for >100 s; watchdog stacks in v17_wd/b3_wd agree).
- Fix plan: hand-written CS PVIDEO model (STOP clears BUFFER, like xemu's pvideo_write),
  then composite the YUY2 overlay into the presented frame so intros are visible.
## Borrowings from other projects
- NV2A fence-mirror pattern: concept from old CS project / toolkit (Wreckless);
  addresses re-derived for this XBE.

## Rules in force
Small local commits, never push. Bounded test runs, kill only own PIDs.

