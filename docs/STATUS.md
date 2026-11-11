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

## Milestone: intros play, boot reaches the title screen (2026-11-11 ~05:20 PT)
- Commit 81dbd71: `src/cs_gpu.c` calls the toolkit's `nv2a_hook_init` and the VEH routes
  the 4 KB NV2A PVIDEO page (0xFD008000) to `nv2a_hook_handle_mmio`. This reaches the
  toolkit's existing PVIDEO model (STOP clears pending banks, like xemu) and overlay
  scanout (toolkit #220). No game state written; generated code untouched.
- Root cause it fixes: intros are XMV movies shown through D3D UpdateOverlay (PVIDEO);
  at movie end D3D EnableOverlay(FALSE) (0x0030A8D0) writes NV_PVIDEO_STOP and spins
  until NV_PVIDEO_BUFFER == 0; plain aperture memory never cleared it -> black hang.
  (Boot flow 0x000AD8A0: MGS_Logo.xmv, Valve_Logo.xmv, wait cache [0x552D68], optional
  ESRB, then `open SuperLauncher_Root` at 0x000ADBE5. Movie loop 0x000C7183, end flag
  [0x552308].)
- Verified runs/p1_pvideo (120 s, no input): MGS logo movie visible (snap_015), Valve logo,
  sound banks, `SuperLauncher_Root.xpr` opened, title screen rendered at ~36-39 s
  (snap_039), idle -> AttractMovie.xmv plays (snap_087..120). Evidence copies:
  docs/evidence/intro_mgs_logo_p1.png, attract_movie_p1.png.
- Required env (not yet defaults in run.ps1): RECOMP_PB_EXEC=1, RECOMP_CONTIG_AVOID_IMAGE=1,
  RECOMP_AC97_READY=1, RECOMP_VBLANK=paced. Without them (runs/b1_long) boot hangs early.

## Open issues / next
1. Title screen framing: CS logo cut at the bottom edge (snap_039); back buffer is
   640x960 (2x vertical AA) downsampled by D3D into the 640x480 front buffer - check
   whether the layout/scale is right vs hardware.
2. Input: send_key.ps1 rewritten (C# EnumWindows; the PowerShell-callback version never
   found the window, so earlier key schedules never reached the game). Next: START on
   title screen -> profile flow (Initial_CreateProfile / SelectProfile) -> main menu.
3. Bake required env into run.ps1 once confirmed stable.
4. INPUT BLOCKER (runs/n2_start, n3_usb, 05:40 PT): keys reach the window ([KEY] down
   vk=0x0D...) but the title never reacts. RECOMP_USB_STATS: USB enumeration stalls after
   5 control setups / 3 OUT0, interrupt ED 00A3A090 never gets a TD, 0 reports served
   (usb_gamepad_report never called). OHCI log: "irq held off by guest IRQL (depth 1)";
   stats show irql-depth=1 permanently -> a guest raise without matching lower, so
   OHCI interrupts/DPCs are never delivered. Old project papered over this with
   RECOMP_IRQL_UNSTICK (not carried over). Next: RECOMP_IRQL_TRACE to find the raise.

## Input root cause found: lifter mis-split of XAPI device open (2026-11-11 ~06:10 PT)
- Not a missing KfLowerIrql in the guest and not cli/sti. CS_IRQL_DIAG (src/cs_kernel.c,
  env-gated logging of KeRaiseIrqlToDpcLevel/KfLowerIrql with guest stack) showed the
  unmatched raise is in XPP sub_0031B21C (XAPI internal device open): raise at 0x0031B239,
  then the generated body simply ended at 0x0031B3AA ("Original: 0x0031B21C - 0x0031B3AA").
- Cause: toolkit disasm accepted a bogus function start 0x0031B3A7 (detection call_target),
  which is the MIDDLE of `mov byte [ebp-0x26],4` at 0x0031B3A6. The "call" is data decoded
  as code at 0x00318220 (no function there). The bogus start clamped the real function, so
  the open never sent its class request, never lowered IRQL, never armed the pad interrupt
  URB -> IRQL depth 1 forever, OHCI irq held off, interrupt ED never gets a TD, 0 reports.
- scripts/split_scan.py scans all starts for mid-instruction landings: 31 hits, almost all
  linear-decode drift through padding/data; the two real ones are 0x0031B3A7 (XPP) and
  0x0026962A (XNET, inside 0x002695D1 at `mov edi,[ecx+8]`).
- Toolkit `--coalesce-functions` cannot fix it (it refuses call_target/tail_jump_alias
  interior starts as "independent evidence").
- An earlier WIP hand-wrote the missing 0x0031B3AA..0x0031B3EE instructions (src/cs_xpp.c);
  discarded in favour of fixing the detector (backup: %TEMP%\cs2_wip_xpp on Champ).
- FIX = LOCAL TOOLKIT PATCH (for later upstreaming):
  patches/xboxrecomp/0001-disasm-drop-phase-shifted-call-targets-CounterStrike.patch, new
  Pass 5b in tools/disasm/functions.py: drop a call_target start that lies strictly inside
  an instruction of a prologue function AND whose every direct caller is outside all
  detected functions. On CS it drops exactly 0x0031B3A7 and 0x0026962A (disasm.log);
  0x0031B21C now spans 0x0031B21C-0x0031B46C (592 B), 0x002695D1 spans to 0x00269B64.
  dependencies.json: base_commit 343b6d8 (v0.17.0) + local_patches -> commit 6551070;
  scripts/setup.ps1 applies them with git am and a fixed identity (sha verified reproducible
  in a scratch clone). Generated code is still pure pipeline output (14319 functions).
- run.ps1 now sets RECOMP_PB_EXEC=1, RECOMP_CONTIG_AVOID_IMAGE=1, RECOMP_AC97_READY=1,
  RECOMP_VBLANK=paced by default.

## Borrowings from other projects
- PVIDEO: none from other projects; uses the pinned toolkit own model (#220, written for Bink/Midtown Madness 3).
- NV2A fence-mirror pattern: concept from old CS project / toolkit (Wreckless);
  addresses re-derived for this XBE.

## Rules in force
Small local commits, never push. Bounded test runs, kill only own PIDs.


- Runtime verification of the patched build: PENDING (see next entry).
