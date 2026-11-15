# CounterStrikeXB (formerly CounterStrikeXB2) - status (verified facts only)

Fresh rebuild of the Counter-Strike (Xbox, 0x4D530036) -> Windows static recomp.
Old project `C:\Recomp\CounterStrikeXB` and the shared `C:\Recomp\tools\xboxrecomp`
are read-only references. Generated code (`src/recomp/gen/`, gitignored) is pure
pipeline output; fixes live in hand-written project code, seeds, or toolkit patches.

## Goal (David)
1. Reliable intros, flash-free real menus, quicker loading, and at least 60 FPS
   from actual menu rendering (not repeated presentation of an old frame).
2. Proper controller (XInput -> USB XID via toolkit OHCI/XAPI) and keyboard input in
   hand-written code. No guest-memory pokes.
3. Correct in-game rendering and stable gameplay, checked through Single Player ->
   Beginner -> Airstrip (not Training).

## Current intro/menu work (2026-11-12)
- Gameplay work is deferred until the intro/menu goals are met. Keep the one-CPU
  dispatch gate enabled; no guest timing changes, menu bypasses, generated-code
  edits, or forced audio completions were used for the performance work.
- `runs\menu_baseline_20261112_110948` recorded roughly 12 FPS in the main menu.
  Its frame trace identifies a 640x960 DXT background, font/icon draws, and a
  full-screen linear-texture resolve into the 640x480 display surface. All of
  this shading runs on the CPU; D3D11 currently presents the resulting image.
- Patch 0009 prepares supported single-product combiner programs once per draw
  and keeps the general interpreter for unsupported programs. It preserves
  input mappings, intermediate saturation, final constants, and alpha.
  DXT palettes are decoded once per block; a bounded image cache compares
  source bytes at each draw, so rewritten textures do not remain stale.
  SSE2 color conversion/filtering and row-invariant edge calculations reduce
  scalar pixel work without fast-math or changed interpolation arithmetic.
- Focused checks cover 76,800 general prepared-program comparisons, the recorded
  copy/modulate patterns, unsupported-program fallback, DXT color/alpha modes,
  texture rewrites, cache eviction, format changes, filtering, transforms, and
  raster ordering. All 22 focused CTest cases and the format smoke test pass.
- On the same 12-worker RelWithDebInfo synthetic workload, resolve time fell
  from 16.44 to 6.64 ms per batch and UI modulation from 16.12 to 6.69 ms;
  framebuffer hashes remain `e4d1b967` and `9f1efad0`. A separate full-size
  background-plus-resolve fixture takes about 16.7 ms, but contains only two
  draws and is not a measurement of a complete game frame.
- The compiler-inlining hypothesis was investigated separately. Release and
  RelWithDebInfo differ in `/Ob2` versus `/Ob1`; no project compiler flags were
  changed. Final measurements above use the game's RelWithDebInfo settings.
- Final pinned-build run: `runs\menu_final_confirmed_20261112`. Both movies
  completed without being skipped; START was sent after the title appeared
  around 69 seconds. The menu workload appeared around 72 seconds. From
  75 seconds onward, 22 one-second main-menu samples averaged **36.8 FPS**,
  ranging **18-42 FPS**. These count completed guest flips, not window refreshes.
  The process was closed normally after the bounded run.
- `runs\menu_cached_20261112` additionally verified Main Menu -> Single Player
  difficulty selection -> Back, with composed screenshots and real keyboard
  input. No Training or gameplay was entered.
- Two intervening final-build runs exposed unreliable fixed-time input
  scheduling: one pressed START before the title; another observed the title
  briefly before the attract movie started. They are not menu verification
  passes. The final confirmation instead reacts to the title's draw workload.
- **Still open:** sustained 60 FPS, smooth decoded movie cadence, proof that
  no individual frame flashes, and normal loading times. Five-second screenshots
  do not prove flash-free presentation. Startup timing varied materially between
  runs. IRQ logs still show delayed delivery while a dispatch-gate holder has no
  safe point (roughly 0.1 s in the final confirmation, up to about 0.74 s in the
  attract-movie run). These waits warrant investigation, not disabling the gate.
- Toolkit pin `1e6abc3` is clean, all nine patches replay to the same commit, and
  the supported `scripts\build.ps1` build succeeds. This is a verified
  performance improvement, **not completion of the frontend milestone**.

## Earlier gameplay and audio work (2026-11-12)
- The supplied Airstrip log reported eight unresolved calls to x87 dispatch target
  `0x002353A4`. It and the verified sibling target `0x00235446` are now pipeline seeds.
- Toolkit patch 0004 shares fixed-function matrix/texgen state with the vertex-program
  constant file, transforms fixed-function inline and immediate vertices, and clips
  large raster bounds before integer conversion. Both constant-upload paths and the
  software pixels have focused regression coverage.
- The old log counted roughly two million batches rejected as non-screen-space; it
  did not identify their contents. The post-fix Airstrip run reported none.
- Toolkit patch 0005 removes shared per-pixel statistics writes from raster workers,
  publishes the counters after each batch, and fixes the Windows small-batch worker
  diagnostic. It also publishes visibility statistics on the allocation-failure
  fallback. Raster, fixed-function, and shader regressions cover these paths.
- On the same synthetic batches with 12 workers, solid raster time fell from
  8.05 to 1.93 ms per batch and textured combiner time from 12.74 to 7.13 ms.
  Framebuffer hashes matched before and after; these are not gameplay FPS results.
- Toolkit patch 0006 honors the cull and front-face registers in both software
  paths and uses alternating triangle-strip winding when culling is enabled.
  The reported Airstrip run frequently enabled back-face culling, which the
  software renderer previously ignored. Focused tests cover both orientations,
  front/back/both modes, strips, near-plane clipping, and the optional backend.
  The y-down winding convention matches xemu's
  [front-face setup](https://github.com/xemu-project/xemu/blob/5759e9c0ee3fe0e699986f2aa939c11a69a497b1/hw/xbox/nv2a/pgraph/gl/draw.c)
  and [screen-to-clip transform](https://github.com/xemu-project/xemu/blob/5759e9c0ee3fe0e699986f2aa939c11a69a497b1/hw/xbox/nv2a/pgraph/glsl/vsh-prog.c).
- In a synthetic workload that duplicated every face with the opposite winding,
  culling reduced median solid-batch time from 3.01 to 2.00 ms with the same
  framebuffer hash. The scene and its speedup are not representative measurements
  of Airstrip.
- In the post-fix run (`runs\airstrip_fix_20261112_091216`), David reported that
  graphics improved but still had substantial glitches and missing renders, and
  gameplay ran at about 1 FPS. Intro videos stuttered and Airstrip loaded slowly.
  Stability and the remaining graphics defects are not yet established.
- The game window closed before an in-game frame trace or screenshot was captured;
  the log records a window-close exit but does not establish why. A future
  user-driven check should follow Single Player -> Beginner -> Airstrip and capture
  the frame trace while gameplay remains open.
- A standalone reproduction of 3,750 representative failed opens, including the
  toolkit path translation, file open, logging, and missing-file reporting, took
  2.17 seconds total (median 0.49 ms; p90 0.81 ms). This is not a loaded game
  measurement and does not identify the cause of the roughly 130-second period
  with no flips during the recorded load.
- Intro movies use the guest XMV decoder and the PVIDEO overlay. A synthetic
  640x480 PVIDEO conversion measured 2.43 ms per frame; this alone does not explain
  the much larger observed vblank gaps.
- In the subsequent run (`runs\airstrip_final_20261112_100759`), the first intro
  froze: two composed captures over a minute apart were identical, even though
  the window and GPU flips continued. Two guest audio packets remained pending;
  one voice described a two-entry scatter list whose second entry was zero.
  The malformed entry previously underflowed the stream end offset and could
  never complete. This establishes the stuck state, not its original cause.
- Toolkit patch 0007 waits for guest-locked voices, publishes notification bytes
  together, includes the final stream sample, and reports malformed descriptors
  without falsely completing them. A controlled test reproduces the observed
  missing-descriptor shape when concurrent guest submissions run without the
  one-CPU dispatch gate; the game launch now enables that gate. The test does
  not prove that race caused the live freeze.
- Toolkit patch 0008 delivers self-clearing PVIDEO STOP events to the presenter
  and prevents an old snapshot from consuming a bank restarted after STOP.
  Presenter and PVIDEO tests cover stopping, restart, and bank reuse.
- At this earlier checkpoint the audio/overlay changes still needed live
  verification. The frontend checks above supersede that status; gameplay
  graphics, speed, loading, and stability remain deferred.

## How to rebuild from scratch
- `scripts\setup.ps1` : private toolkit clone at `tools\xboxrecomp`, pinned in
  `dependencies.json` (xboxrecomp v0.17.0 `343b6d8`), capstone 5.0.9 in `.deps\`.
- `scripts\extract_disc.py` : `game_files\` from the ISO (sha256 in dependencies.json).
- `scripts\pipeline.py` : regenerate `src\recomp\gen\` using `seeds\cs_seed_functions.json`.
- `scripts\build.ps1` : `build\RelWithDebInfo\cs2_xbox_recomp.exe`.
- `scripts\run.ps1 [-Seconds N]` : launch; logs in `runs\<label>\`. Scrubs inherited
  RECOMP_*/CS_* env. SaveData is project-local (`SaveData\`).
- `scripts\probe.ps1 -Label x -Seconds N -Keys '60:Return'` : bounded run with FB
  snapshots (`snap_<sec>.png`) and a keyboard schedule via `send_key.ps1`.

## Hand-written CS code
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


- VERIFIED runs/i1_split (06:15 PT, 80 s, keyboard Enter at 48/56/64 s via send_key.ps1):
  irql-depth=0, OHCI 8 setups / 6 OUT0, interrupt-IN reports flowing (14149 by 80 s).
  Title reacted to START: CurrentProfile.dat missing -> "You need a profile" prompt
  (snap_050), then a default profile NewPlayer000 was created and the real Main Menu
  rendered: Training / Single Player / Xbox Live / System Link ... (snap_065).
  Evidence: docs/evidence/profile_prompt_i1.png, docs/evidence/main_menu_i1.png.
- Framing analysis (06:30 PT): full UI is drawn into the 640x960 back buffer 0x00AEC000
  (pitch 2560; fb.bmp00013 in runs/i1_split shows the complete Create-a-Profile screen
  incl. A SELECT / B BACK at the bottom). D3D then draws a resolve pass into the 640x480
  front buffer 0x00D44000 (draw surface alternates in [GPU] stats), and the front buffer
  ends up holding the TOP HALF 1:1 -> the cut-off. Suspects: AA/supersample scale state
  (pb_exec surface_apply_clip maps AA=1 to 2x horizontal, sy only for AA=2) or resolve
  texture coordinates.
- Frame trace runs/f1_trace (RECOMP_FRAME_TRACE=<flag file> + RECOMP_FRAME_TRACE_METHODS=1,
  flag created at 44 s on the title): back buffer SURFACE_FORMAT 0x128 (AA bits 0, so the
  2x is D3D supersampling, not NV2A AA), clip 640x960; front clip 640x480. Resolve = ONE
  triangle on surf 00D44000 640x480, texture 0 = 80AEC000 640x960 fmt 0x12 (linear
  A8R8G8B8 rect, 0x1B04=00011229, rect 0x1B1C=028003C0, filter 0x1B14=02022000 linear),
  positions v0 (0,0) v1 (2560,0) v2 (0,1920) (oversized, should cover all 640x480) but
  the rasteriser reports px 171601 (< 307200). Next: dump that triangle's texcoords in
  pb_exec and check rect-texture uv (texel units) and the px coverage.
- VERIFIED runs/n1_nav (06:42 PT): saved profile reused (SaveData/TDATA/4d530036/
  CurrentProfile.dat); START x3 -> Main Menu; keyboard Down, Down moves highlight Training
  -> Xbox Live (snap_074), Up -> Single Player (snap_082). Evidence:
  docs/evidence/main_menu_nav_down2_n1.png, main_menu_nav_up_n1.png. Keyboard reaches the
  title through the same USB XID report path a pad uses (RECOMP_KEYBOARD -> pad 0).
  NOT yet tested: a real XInput controller, A/B on menu items.
- Still open: framing (bottom of screen cut: profile prompt text and lower menu items cut
  off; fb dumps alternate 640x480 and 640x960), D-pad/A/B navigation, real XInput pad test.

## FRAMING FIXED: presenter picked the 640x960 back buffer (2026-11-11 ~06:35 PT)
- Not a rasteriser/sampling bug. runs/f2_tc (FTRACE now prints texcoord 0): resolve triangle
  texcoords 0,0 / 2560,0 / 0,3840 against positions 0,0 / 2560,0 / 0,1920 = correct 1:2
  vertical downsample into the front buffer 0x00D44000. (The FTRACE "px" figure is not a
  coverage count for queued program triangles; ignore it.)
- Real cause: pb_exec note_drawn() presents "the biggest surface drawn since the last flip"
  (toolkit fcea264, for Burnout 3 shadow maps). The 640x960 back buffer beats the 640x480
  front, so the window got the back buffer read as 640x480 = top half 1:1. PCRTC_START is
  never written by the title (runs/f3_crtc, RECOMP_NV2A_TRACE), so no scanout address exists.
- FIX = local toolkit patch patches/xboxrecomp/0002-pb_exec-present-the-display-sized-surface-
  over-a-tal.patch: prefer a display-sized surface (640x480 logical at its AA scale) over a
  non-display-sized one; same-kind ties keep biggest-wins (Burnout/X-Men unchanged).
  dependencies.json commit -> 616b31f (343b6d8 + 0001 + 0002), reproduced bit-exact in a
  scratch clone. setup.ps1 now clones with core.autocrlf=false (git am failed on a
  CRLF-converted checkout: "does not match index").
- VERIFIED runs/fx1_present (85 s, Return 48/56/64, Down 72): full screens. Title with
  "Please press START" + copyright lines (snap_050), Main Menu all 6 items Training..Change
  Profile + A SELECT hint (snap_058), Return on Training -> Training submenu Hostage-Rescue /
  Demolition with description and A SELECT / B BACK (snap_066), Down -> Demolition (snap_074).
  So A (confirm) via keyboard Return is verified. Evidence: docs/evidence/title_full_fx1.png,
  main_menu_full_fx1.png, training_menu_full_fx1.png (copies in C:\Users\dynam\cs2_snaps\fx1_*).
- Next: B (back) key, profile keyboard rows (Change Profile -> create), pad injection (-PadLive).

## USB never started on slow boots: toolkit OHCI 30 s deadline (2026-11-11 ~06:50 PT)
- runs/f2_tc and i3_menu reached the title but never got input: no "[OHCI0] operational"
  line at all. ohci.c's controller thread gave up after 1500 ticks ("30 s and it never
  started"); CS's XAPI starts the HC only after the intro movies, logged 22-29 s on normal
  boots (n5/n6/v17_vbl ~29 s), so a slower boot crossed it and the session had no pad or
  keyboard at all.
- FIX = local toolkit patch 0003-usb-ohci-keep-waiting-for-the-driver-to-start-the-co.patch
  (keep polling, log once at 1500). Pin -> a31cc9e (343b6d8 + 0001..0003), reproduced
  bit-exact in a scratch clone.
## MENUS VERIFIED: keyboard + pad injection (runs/i4_menu, 2026-11-11 ~06:55-07:05 PT)
Interactive bounded run (run.ps1 -PadLive -FbDumpEvery 30, own PID stopped at the end),
build at pin a31cc9e. OHCI operational at 22.5 s; title at ~36 s.
- Pad injection (RECOMP_PAD_LIVE -> same USB XID report path as an XInput pad):
  start on title -> Main Menu (pad_start_main_menu_i4.png); a on Training -> Training submenu
  (pad_a_training_i4.png); b -> back to Main Menu (pad_b_back_i4.png); y on Profile Selection
  -> Create a Profile keyboard; right/down/down moves the highlight Done -> 1 -> a -> k
  (profile_keyboard_pad_nav_i4.png); b (150 ms) -> Profile Selection, b:400 -> Main Menu
  (pad_b_cancel_main_i4.png).
- Keyboard (send_key.ps1 -> window -> toolkit key map): Z (A) -> Training (kbd_z_training_i4),
  X (B) -> back (kbd_x_back_i4), Down x5 -> Change Profile with its description
  (kbd_down5_change_profile_i4), Z -> Profile Selection: profile list, settings column,
  RENAME / Y NEW / X REMOVE / A SELECT / B CANCEL hints (profile_selection_i4), V (Y) -> Create
  a Profile: all keyboard rows render (Done/1-0, Shift/a-j, Caps Lock/k-t, Symbols/u-z/
  Backspace, Accents/Space/<-/->) + A SELECT / B BACK (profile_keyboard_i4).
- Visuals: no garbage; text/layout correct on every screen. One dump (snap_f_kbd_x) caught the
  description line mid-redraw after a screen change (next frames clean) - a transition frame.
- Minor/unexplained: the first pad b on the keyboard screen (highlight on "k") did nothing;
  later b presses there worked. Possibly game behaviour on a letter key; left for David.
- Keyboard map (toolkit): arrows = D-pad, Enter = START, Backspace = BACK, Z/X/C/V = A/B/X/Y,
  WASD = left stick, IJKL = right stick, Q/E = White/Black, 1/3 = L/R trigger.
- Not tested here: a physical XInput controller (left for David).
- How David launches: from C:\Recomp\CounterStrikeXB,
  `powershell -ExecutionPolicy Bypass -File scripts\run.ps1` (normal play; he closes it).

## "CRASH AFTER THE MENUS" = XLaunchNewImage into itself (2026-11-11 ~11:20 PT)
- David's runs runs/run_20261111_110459 and _110721 (Main Menu -> Training -> Hostage-Rescue):
  no exception, no dump. Log tail: `[PATH] \Device\CdRom0\-xbox_cs-f.xbe`, launch data page
  type=0 titleid=0x4D530036 path='\Device\CdRom0;default.xbe', launch data 00000002, then
  `HalReturnToFirmware: routine=2` -> toolkit bridge ExitProcess. The window simply closed.
- Cause: CS's front end does not load a map in-process. It probes d:\-xbox_cs-f.xbe (dev name),
  then XLaunchNewImage("d:\default.xbe") with launch data (strings at 0x00329A88) = quick
  reboot into ITSELF. The toolkit treats every HalReturnToFirmware as quit.
- FIX (hand-written, src/cs_launch.c; no toolkit change, generated code untouched): override of
  the HalReturnToFirmware thunk (slot 0x003211BC, ordinal 49). Routine 2 + launch page naming
  \Device\CdRom0;default.xbe + title 0x4D530036 -> save the 4 KB page to %TEMP%, CreateProcess
  the same exe (same cmdline/cwd/std handles, env CS_LAUNCH_PAGE/CS_LAUNCH_PARENT), then the
  toolkit bridge exits as before. The child waits for the parent, main.c calls
  cs_launch_restore() after xbox_kernel_bridge_init(): page copied into a guest heap page and
  KDATA LaunchDataPage pointed at it (what the kernel export reads). Other targets still exit.
  run.ps1 sets CS_RUN_DIR; relaunched PIDs are appended to runs\<label>\pid.txt; bounded
  run.ps1/probe.ps1 follow and stop the whole chain (own PIDs only).
- VERIFIED runs/l1_training (keys Return 48/56, Z 64): Training submenu (snap_065) -> relaunch
  PID 18088 -> 19116 at ~66 s, child skips intros (launch data honoured), reads
  maps\cs_training.txt/.res, shows "Counter-Terrorist Briefing / Hostage-Rescue Training /
  Loading..." (docs/evidence/training_briefing_relaunch_l1.png).
- NEXT BLOCKER (map load, was out of scope): loading bar stays empty to 200 s; the engine
  loops FileSystem lookups for "*1".."*76" across t:\ z:\ d:\media d:\shared d:\cstrike
  d:\valve (inline brush-model names being opened as files).
## Open issues logged from David (2026-11-11 ~11:30 PT)
- Intro movies (MGS/Valve XMV) do not play smoothly on David's runs. Not investigated yet
  (after the Training-launch work). Tip from David: START or A skips the intros in test runs.
## VERIFIED: Training map loads and plays after the relaunch (runs/l4_training, 11:32-11:37 PT)
- Keys: Return every 5 s from 25 to 100 s (title -> Main Menu -> Training -> Hostage-Rescue).
  Relaunch PID 20312 -> 25260; CS_LAUNCH_SAVE kept the hand-over page at
  runs\launch_page_training_hostage.bin (type 0, title 0x4D530036, launch data byte 0 = 0x02).
- By snap_225 the training level is running: in-game HUD ($16000) behind the tutorial prompt
  "Buying Weapons and Equipment ... Press A to continue" (docs/evidence/training_ingame_prompt_l4.png).
  No exception, no bugcheck, no exit up to the 320 s stop. So in Training there is NO map-load
  crash; runs/l1_training only looked stuck because it was stopped at 200 s mid-load (the "*1".."*76"
  loose-file probes are inline brush-model names checked across the search path; slow but finite).
- Load time after relaunch is long (~2 min incl. briefing); file-probe logging is a suspect for later.
- Direct-to-game test: `run.ps1 -ExtraEnv 'CS_LAUNCH_PAGE=<copy of a saved page>'` (the file is only
  deleted when handed over by a parent instance).
- Next (David 11:37): Training has too many prompts -> test Single Player / System Link match start.
- 11:38 PT: test launches paused at David's request (he is picking a mode himself). runs/s1_single was my own aborted start (PID 28432, stopped after ~5 s; no data).

## David's Single Player -> Beginner -> Airstrip run (runs/run_20261111_113815, 11:38 PT)
- Path: Main Menu, Down (Single Player), A on every screen (Beginner, Airstrip defaults).
- PID 7216 handed over at ~11:39:02 (launch data byte 0 = 0x03, page type 0, title 0x4D530036)
  to PID 7596. No exception, no bugcheck. The child loaded Airstrip and at 11:42 was in game at
  the team-select menu (Auto-Select / Counter-Terrorist / Terrorist, $16000 HUD behind;
  desktop capture docs/evidence/airstrip_team_select_david_run.png). Window title FPS 0-2 during
  load; draws climbing.
- So the "crash" David saw is the menu window closing on the self-relaunch: the new window is
  black for ~1-2 min while the map loads (no intro, no loading picture yet), which reads as a crash.
- Next: UX of the hand-over (keep the old window up until the new one exists / show it in the
  same place), load time (~2 min), in-game FPS (window title showed 0-2 FPS during load; measure in game).
## Airstrip in-game visuals, David's PID 7596 (11:56-11:57 PT, read-only window captures)
- By the time we looked, the team was already chosen; the Select Weapon (buy) wheel was open, so no
  key was sent. Captures: C:\Users\dynam\cs2_snaps\ingame_pre_select.png, ingame_glitch_01..08.png
  (copy: docs/evidence/airstrip_ingame_buymenu_david.png).
- FPS (window title): 0-3 FPS throughout (draws/frame ~720-760). Round timer went 4:14 -> 4:00 ->
  3:52 -> 3:47 across ~30 s real time, so the game clock runs roughly half speed.
- World: almost the whole scene is flat grey. Only alpha-tested foliage (palm, pine, ferns), one
  striped pole and a concrete post are visible. No ground, walls, hangars or sky texture can be made
  out, so BSP world surfaces/sky are missing or drawn as flat grey (fog/clear colour). Needs a frame
  without the buy menu to confirm.
- No first-person weapon viewmodel visible.
- HUD: numerals (health 100, armor 0, timer, ammo 20/40, $9250) look smeared/ghosted with a dark
  offset copy, and the timer colon is missing. A light-blue translucent ellipse sits top-left
  (probably the radar background with no radar contents). Small stray glyphs ("q" marks) below the
  wheel. Chat/event text ("Quade picked up the bomb", player names Mike/Zane) renders.
- Buy wheel itself renders legibly (dark segments, labels).
- Suspects for later (unverified): world brushes use a lightmap/multitexture or a combiner path
  the toolkit pb_exec does not handle; HUD ghosting from AA/supersample scaling of 2D quads.
## Airstrip in-game, buy menu closed (David's PID 7596, 12:00 PT, read-only captures)
- C:\Users\dynam\cs2_snaps\ingame_clean_01..04.png (copy docs/evidence/airstrip_ingame_clean_david.png).
  FPS (title) 2-4, draws/frame 675-766.
- World geometry: MISSING. Left ~84% of the view is a flat mottled grey; the right ~16% is flat white
  with a cyan patch top-right. No ground, runway, walls, hangars or skybox. Only studio models and
  alpha-tested foliage render (palms, pine, ferns, striped pole, concrete post), correctly textured
  and depth-sorted among themselves, standing on nothing. So BSP world brushes + sky are not drawn
  (or drawn flat); entities/models are fine.
- Viewmodel: no first-person weapon visible (ammo HUD shows 20/40, so a weapon is equipped).
- HUD POSITION (David: "drawn in the middle of the screen, should be at the top/edges"): the whole
  status row (health cross 100, armor 0, round timer 4:42/4:33 with no colon, ammo 20 | 40, $12500)
  sits at ~45% of screen height instead of the screen edge; the radar oval (light-blue, empty) and
  buy-zone cart icon are inset from the top-left; crosshair-like glyphs at ~88% height, x centre,
  instead of screen centre; chat ("Quade / Fire in the hole!") lower-left. Positions are not a
  uniform shift, consistent with HUD 2D quads being placed in one coordinate space (640x480) while
  the frame is the 640x960 supersampled back buffer, or with a viewport/scissor scale on the 2D pass.
  Suspect: same family as the earlier 640x960 vs 640x480 issue (toolkit pb_exec AA/supersample
  scale for screen-space/transformed vertices). Unverified.
- HUD text: earlier ghosting looks milder without the buy overlay; digits readable, colon missing.
- Light-grey bar across the top-left edge (x 0-370, first ~20 px) of unknown origin.
## D3D11 back end (2026-11-11 evening, UNCOMMITTED)
- New `src/cs_d3d11.c`/`.h`: an Nv2aBackend (registered in `cs_gpu_init`) that draws through Direct3D 11 on the GPU instead of the toolkit CPU rasteriser. `RECOMP_CS_D3D11=0` restores the CPU path.
- Toolkit local patch 0010 (`patches/xboxrecomp/0010-nv2a_backend-pass-register-combiners-texture-stages.patch`; originally 0004 before main renumbered the chain): 3D (program/fixed-function) batches now carry Nv2aCombiner, the four texture stages, fog colour and per-vertex d1/tex/fog. Screen-space 2D batches unchanged.
- Why it was needed: D3D's back-buffer resolve (640x960 -> 640x480) is a texture-only draw with black vertex colour; only the register combiners make it show the texture. Without them every post-intro frame was black.
- Combiner HLSL is a port of `nv2a_combiner.c`, checked line by line (also matches the interpreter used by the other xboxrecomp D3D11 back ends on this PC, used as references only).
- Verified: intro movies (no more PVIDEO garbage), title, Main Menu, menus, loading screen with the 3D map behind it, and in-game (Airstrip) with models/foliage. In-game about 8-13 FPS by log (was 0-4).
- Open: BSP world (ground/walls/sky) is still black in game while models/foliage draw (`docs/evidence/d3d11_ingame_airstrip_*`); no first-person weapon seen; HUD layout unchanged. Task 3 (worker) investigates the world: cull / depth / combiner modes. Not modelled yet: mip levels, cube maps, bump stages.
- Debug switches: RECOMP_CS_D3D11_LOG, RECOMP_CS_TRACE=N, RECOMP_CS_DUMP=<dir> (+dump.flag), RECOMP_CS_CULL=0, RECOMP_CS_PIXOFS.
- Gotcha: `scripts/probe.ps1 -Keys` needs one semicolon-separated string. In game Enter = START (pause); use Z (A) to dismiss prompts.

## Frontend milestone: interrupt stalls, flashes, menu FPS (2026-11-12)
Measured with the new per-flip/PVIDEO logs and `scripts/menu_probe.ps1` (reactive: waits for the real title, then walks the menus).
- **Cause of slow/stuttering intros and the ~84 s boot: unbuffered stderr under the dispatch gate.** A guest DPC holding the one-CPU gate wrote log lines (or waited for the CRT stderr lock), so vblank/APU interrupts waited up to 1.9 s (runs/b0_base, b2_gate `[GATE]` stacks). stderr is now fully buffered with a 250 ms flusher (src/main.c; `RECOMP_CS_STDERR_BUF=0` restores the old behaviour). Result: first frame at ~4 s (was 24 s), both intros complete in ~19 s at ~24 fps (were ~55 s at ~4 fps in bursts), title at ~26 s (was ~84 s), interrupt waits < 1 ms.
- **Cause of the black menu flashes: dropped pushbuffer commands on ring wraps.** The default segment scan guessed the wrap from the highest/lowest PUT seen and skipped commands written past them; now and then those were the combiner words of D3D's back-buffer resolve, which then ran with the menu's modulate combiner and black vertex colour. Toolkit patch 0012 follows the actual JUMP (pin 24337f9). Verified: 0 flashes in 7 runs / ~210 menu key presses (were 6 in 5 runs).
- **Menus:** 71 guest FPS sustained, every frame fully drawn (15 draws), D3D11 back end.
- **Still open:** intro movies at ~24 fps against a 30 fps cadence (the second, Valve, movie has 160-570 ms hitches while every thread is waiting: pacing, not CPU); an intermittent intro freeze (1 run in ~10: APU stops raising interrupts); map loading; gameplay.
- **Diagnostic tools added:** `RECOMP_CS_FRAME_CSV`, `RECOMP_CS_PVIDEO_CSV`, `RECOMP_CS_FLASH=<dir>` (draw lists + targets of a black frame), `RECOMP_CS_GATE_WATCH=1` (stack of a long gate holder), `RECOMP_CS_PROF=a-b` (sampling profiler), `scripts/analyze_frames.py`, `scripts/menu_probe.ps1`, `scripts/symbolize.py`.

## Gameplay milestone: the BSP world draws (2026-11-12)
Compared against David's xemu captures of Airstrip (C:\Recomp\tools\XemuRamDump\dumps).
- **Cause of the grey ground, white streaked "walls" and missing surfaces: the whole precompiled world pushbuffer was skipped.** The ring JUMPs to it (0x81BD5000, ~135 KB) and it JUMPs back to the word after; the segment scan ended every segment at a top-level JUMP. Toolkit patch 0015 follows a JUMP whose target chain comes straight back (pin 7abd611). Before, the ground was never drawn (the 0x4D4D4D clear showed through). Now: textured ground, hangar, crates and fence match the xemu reference (runs/g42_detour vs dump_001/screenshot.png).
- Ruled out on the way: the detail-texture constant c105 is huge on real hardware too (xemu pushbuffer has the same values); texture memory matches xemu.
- Gameplay ~21 FPS now that the full world is drawn (was ~35 with most of it missing). Menus unchanged (71.6 FPS, 0 flashes, runs/v4_menu).
- `RECOMP_STRIP_ALT_WINDING=1` (toolkit) flips odd strip triangles for culling; no visible difference on the world, left off.
- **Tools:** `RECOMP_CS_DUMP` now also writes `ram.bin` (64 MB physical/contiguous view) and `va_game_00010000.bin`, laid out like xemu's dumps; `scripts/pbfind.py` (vertex-constant uploads in a RAM image), `scripts/ramtex.py` (decode a texture from a RAM image); `RECOMP_PB_DETOUR_LOG=1` lists detours.
- **Next:** GPU vertex programs (CPU VSH is the cost now), haze/smoke check, map load time.

## Gameplay milestone: ~60 FPS in Airstrip (2026-11-13)
21 FPS -> 60-64 FPS (runs/g60_kicks, g61_kicks; median frame 15.8 ms). Menus unchanged (71.8 FPS, 0 flashes, runs/v5_menu). Steps, each measured with `RECOMP_CS_PROF`:
- **GPU vertex programs** (toolkit patch 0016 + `src/cs_vsh_hlsl.c`): the executor hands program batches over untransformed; each NV2A program is translated to an HLSL vertex shader (cached by content) mirroring the CPU interpreter; GPU culling/clipping. All 6 CS programs translate. 21 -> 35 FPS. `RECOMP_CS_GPU_VSH=0` returns to the CPU path.
- **Specialised combiner pixel shaders** (`src/cs_rc_hlsl.c`): the generic combiner interpreter (dynamic `R[16]` indexing in a loop) cost the GPU ~23 ms a frame; per-setup shaders take it to ~14 ms. `RECOMP_CS_RC_SPECIAL=0` keeps the generic one (also used by `RECOMP_CS_RC_DEBUG`).
- **Kickoffs answered mid-walk** (patch 0016): the game's KickOff spun on the PFB flush bit for the whole segment walk, so game and executor never overlapped. Bounded (8 kicks/walk, 192 KB ahead, not while a flip is held) because XDK D3D otherwise takes its "GPU far behind" path, which waits for a software-method interrupt we do not deliver (froze loading / at ~55 s before the bounds). The 0x400B10 kick latch is now kept for the segment scan.
- Per-draw overhead: texture lookup index, verified-this-frame check before hashing, unchanged constant buffers not re-uploaded.
- **Tools:** `RECOMP_CS_D3D11_LOG` lines now show GPU ms/frame (timestamp queries) and GPU-VSH/fallback draw counts; `RECOMP_CS_GPU_VSH_DUMP=<file>` writes the HLSL of a program that fails to compile.
- **Still open:** the far-behind interrupt itself (would remove the bounds), 1% lows (p99 ~35 ms), haze/smoke check, map load time.

## Gameplay milestone: round-start dips gone (register promotion, 2026-11-13)
The remaining dips were the game's own code (bot collision traces, `SV_RecursiveHullCheck` at 0xB7E80) running slowly as translated: guest registers live in thread-local globals, so every register access was memory and MSVC reloaded the thread-local block after each guest store (245 TLS loads, 2414 memory operands in that one function).
- **Toolkit patch 0017 (`--promote-regs`, pin 1ab5820):** translated functions keep the registers in locals, written back right before every call/tail call/longjmp/return and read back after each call. Inert unless the build defines `RECOMP_REG_PROMOTE` (CMake option `CS_REG_PROMOTE`, default ON, generated sources only), so the same generated tree builds either way. `scripts/pipeline.py` passes the flag; regeneration verified to differ only by the hooks (14,334 functions). The hot function: 8 TLS loads, 1455 memory operands.
- **Results:** round start (66-95 s) worst frame 35 ms, p99 20 ms (were bursts of 30-42 ms frames); steady play 14 ms median (~72 FPS), p99 18-21 ms. Menus/intros unchanged (71.8 FPS, 0 flashes). 10-minute soak: ~43,700 frames across many rounds, no crash; it ended during the match-end map change (the title relaunches itself; still slow to load).
- **Also fixed:** `cs_kernel_override` called `getenv("CS_IRQL_DIAG")` on every IRQL raise/lower (~15% of the game thread in `strchr`); the frame CSV now appends across the title's relaunches (probes clear it at start).
- **Clang (clang-cl, `build-clang`, `cmake -T ClangCL`):** before promotion it was slower than MSVC (19 ms vs 13.9 ms median): every thread-local access carried the MS-ABI on-demand init guard (`__dyn_tls_on_demand_init`). clang 19.1.5 has no `-fno-ms-tls-guards`; `/clang:-fms-compatibility-version=19.20` on the generated sources drops the guard (hot function 54 KB -> 14.8 KB, 1006 -> 12 TLS loads). With promotion, A/B against MSVC is within run-to-run noise (round start p99 19.1 vs 20.3 ms, steady 23.5 vs 19.1 ms). MSVC stays the default.
- `/Ob2 /GS-` on the generated code (`CS_GEN_INLINE`, off): identical code for the hot function, no measurable change.
- **Toolkit patch 0018 (`--direct-mmio`, pin 38d46cf):** absolute-address APU register accesses (99 in CS, DirectSound's per-voice writes to `[0xFE82xxxx]`) call the APU model directly instead of faulting. Audio capture (`RECOMP_AUDIO_WAV`, `scripts/wavstats.py`) matches the trapping build. Small gain only: game-thread exception share 3.7% -> 3.4%, most APU traffic is register-relative and still traps.
- **Remaining tail:** steady/round-start p95 17-18 ms, p99 19-21 ms, worst ~21-27 ms; median ~13.9 ms (~72 FPS). The game thread profile is flat; remaining levers are codegen-wide (x87 stack still thread-local and index-addressed) and the APU MMIO traps (~7% of the game thread, host exceptions per DirectSound register write).
- **Tools:** `scripts/spikes.py` (frame-time distribution and spike list for a window), `scripts/spike_prof.py` (what each thread did inside spike frames vs normal frames), `scripts/hostdis.py` (host disassembly of a translated function from the PDB; `CS_EXE` for another build), `run.ps1 -Exe` / `CS_EXE` to launch another build.

## First-release checklist (agreed 2026-11-13)
Must-have, in order:
1. Known bugs: ~~intermittent intro freeze~~ (fixed 2026-11-13, below); ~~Valve-intro hitches~~ (not a bug: the movie's own frame times hold the logo, below); ~~slow map load~~ (fixed 2026-11-13, 28 s -> 8 s, below); relaunch on map change (likely the same wait, not yet re-measured); rare boot stall (a thread at raised IRQL spins up to ~7 s with an APU interrupt pending; seen in about 1 boot in 50).
2. Every map loads and plays (Airstrip and Dust verified; 18 to go); watch for unimplemented paths (cube maps, water).
3. Visual correctness: smoke/haze vs xemu, mipmaps (distant shimmer), runway decal.
4. Multi-hour soak including several map rotations and menu <-> game relaunches.
5. Input and settings: controller layout, keyboard/mouse, in-game options saved.
6. Other machines: another GPU vendor, a weaker CPU; windowed/fullscreen; frame pacing.
7. Packaging: ~~users build from their own disc~~ changed by David 2026-11-13: an installer and RecompLauncher ZIP like MvC2 1.0.2 (the exe ships, the game files come from the player's own disc), below.
Later: GPU vertex fetch, x87 stack in the recompiler, system link.

## Intro freeze fixed: title-built KEVENTs (2026-11-13)
The first intro movie (MGS logo) froze for 32.7 s in about 1 boot in 10 (1 in 4 under profiler load), and the title screen never came within the probe's time.
- Not audio: the movie's audio stream starving was a consequence. The XMV decoder had stopped first: it lost count of the frames left in a packet (`[decoder+0x3C]`), read past the packet end, and took data as a frame time of 0x7FF9 ms.
- Why: the frame decoder `sub_00292F5D` returned with its caller's `esi` (the decoder object) changed by 0x80000 (found with the new `CS_ABI_CHECK` build option), so the decrement went to the wrong object.
- Who: a hardware write watchpoint on that stack slot (both memory views, every thread) caught the USB completion DPC `sub_0031D685` doing `or byte [urb+22h], 8` on a request that lived on the main thread's stack, long after that frame was gone.
- Root cause: the XAPI input code (`sub_0031B21C`, opening a pad) builds a KEVENT on its stack and waits on it (50 ms, then cancel and wait again). Without `RECOMP_TITLE_KEVENTS` the bridge did not know that event, so both waits failed at once with STATUS_UNSUCCESSFUL; the code returned with the USB request still pending and its completion later wrote into reused stack.
- Fix: `RECOMP_TITLE_KEVENTS=1` in scripts/run.ps1. Verified: 24 stress boots with the watchpoint armed, 0 freezes and 0 stray writes (was about 1 in 4); on the committed build 0 of 24 normal boots and 0 of 24 under profiler load. Gameplay unchanged (Airstrip ~72 FPS; the 20 s map load is the known one).
- Kept as tools: `scripts/boot_loop.ps1` (repeated boots, freeze count), `CS_ABI_CHECK` (CMake; callee-saved checks in a separate build), toolkit patch 0019 (`RECOMP_APU_VTRACE=<voice>`, `RECOMP_APU_VSTAT`), io.csv event/APC/bytes columns, `[CLOCK] t0_qpc_ms` (lines up QPC-clock traces with frames.csv).

## Map load 28 s -> 8 s: no Ethernet link (2026-11-13)
Every map load (game_probe: relaunch into Airstrip) sat 20 s on one blank frame with all threads idle.
- The main thread looped in `sub_0006D150`: `XNetGetTitleXnAddr` (`sub_0026304B`) then Sleep(10) until it stopped answering "pending", then "Could not get TCP/IP address, TCP/IP disabled". Counter-Strike starts its network stack for every game, single player included (the local server binds through XNet).
- Our `PhyGetLinkState` always reported link-up with no network adapter behind it, so XNet ran DHCP against nothing and waited out its timeout (polling the link ~200,000 times a second).
- Fix: toolkit patch 0020, `RECOMP_PHY_LINK=0` (cable unplugged, as on a console with none), set in scripts/run.ps1. Scene now starts at 8.1 s instead of 28.7 s; 12/12 boots to the menu; Airstrip 71.3 FPS over 2 min.
- Also: the profiler CSV (`RECOMP_CS_PROF_CSV`) has a `chain` column, every translated function on the sampled stack.

## Valve intro "hitches" are in the movie (2026-11-13)
The overlay video pauses for 100-570 ms several times between ~15 and ~20.6 s of a boot, the same in every run.
- The XMV frame times in Valve_Logo.xmv have those gaps: after the first second the deltas include 166, 200, 300, 367, 468, 767 and 968 ms where the logo holds still (read live from the decoder's next-frame time, `[decoder+0xF8]`).
- Playback follows them to within a vblank, the main thread is asleep in the movie loop 89% of the time (`sub_000C6D40`, 10 ms sleeps while no frame is due), and the game loop keeps flipping at 60 Hz throughout. Nothing to fix.
- Real stalls left around the intros: ~0.3-0.4 s as the MGS movie starts (zlib inflate on the main thread, `sub_002422F1`; a console spends at least as long) and ~0.15 s at the title transition.

## Dust loads and plays (2026-11-13)
- Menu path (probe keys from boot): 25 s Return (title), Down (Single Player), A, A (Beginner), then the Map Selection list is alphabetical -- Airstrip, Aztec, Chateau, Cobble, Corruption, Dust, ... -- so Down x5 for Dust, then A on each screen. Terrorist Briefing "Dust" loading screen, in the match ~15 s after picking the map (relaunch included).
- Saved launch page: `CS_LAUNCH_SAVE=runs\launch_page_dust.bin`; `scripts/game_probe.ps1 -Page runs\launch_page_dust.bin` goes straight in.
- From the page: scene at 7.4 s; 64.3 FPS over 142 s (median 14.5 ms, p95 20.7, p99 31.7; worst in-match frame 257 ms at round start). About 1,700 draws per frame against Airstrip's ~1,000.
- Visuals: geometry, textures, sky, HUD, radar, team-join message and tutorial hint all render; rounds play out (money $6,000 -> $9,250). Frames: docs/evidence/dust_ingame_2026-11-13.png.
- Probe note: A (Z) also opens the buy wheel in the match, so repeated A presses leave it open.

## ESC / F1 menus, settings and config.exe (2026-11-13)
Modelled on NHLHitzPro's (C:\Recomp\NHLHitzPro, reference only), adapted for Counter-Strike: no texture-pack options, the game never pauses (multiplayer later), and the renderer itself presents the frame.
- Presentation: the D3D11 back end now presents the title's front buffer into a swap chain on the window at each flip (toolkit patch 0021, `xbox_FramebufferSetPresenter`), with the intro-movie overlay composited by the toolkit and drawn with its alpha. The per-flip copy back into guest memory is skipped unless a dump is running (`RECOMP_FB_DUMP`, `RECOMP_CS_DUMP`). `RECOMP_CS_PRESENT=0` keeps the old window path.
- Settings (src/cs_settings.c, `%LOCALAPPDATA%\CounterStrikeXB\settings.ini`): render scale Low 1x / Medium 2x / High 3x (every target and depth buffer made N times larger, viewport scaled), MSAA 2/4/8 (resolved before sampling or showing; capped at what the card supports), anisotropic filtering, VSync (tearing-capable flip model when off), frame-rate cap, stretch, borderless fullscreen. Scale/MSAA/anisotropy apply between frames; the rest at each present.
- Menus (src/cs_overlay.c): a layered window over the game, drawn with GDI; Esc = quit prompt, F1 = settings, controller 1 drives them too. A host-side layer only: it never touches the game's clocks, sound, frames or memory; while one is open its keys, and controller 1 (`xbox_InputSetHold`), do not reach the game.
- config.exe (config/config.c, CMake target `config`): Video tab (the same options) and Controls tab (key + alternate per Xbox control) writing `keymap.ini`, which the toolkit reads at start (`RECOMP_KEYMAP`, set by cs_settings_startup).
- Checked (scratch LOCALAPPDATA; scripts/settings_probe.ps1 + scripts/winshot.py): both menus over a running match; 3x + 4x MSAA + 16x AF on Dust visibly sharper at 69 FPS (66 at 1x: CPU-bound); live F1 changes 1x -> 2x -> 3x and 2x MSAA applied mid-round and saved; fullscreen + stretch at 1920x1080; 60 FPS cap and VSync both 16.6 ms median; intro movies and title through the new presenter; config.exe set A -> Space and 2x, the game read the key map (24 controls) and Space opened Training.
- Tools: scripts/settings_probe.ps1, scripts/winshot.py (captures the window as shown; the dumps no longer hold the final picture), scripts/config_check.py.

## Installer, logger and RecompLauncher level 3 (2026-11-13)
Modelled on Marvel vs. Capcom 2 1.0.2 (C:\Recomp\mvc2_pc, reference only). At David's request the release now ships the program (checklist item 7); the game files still come from the player's own disc.
- Player build (`-DCS_RELEASE=ON`, build_release/): static C runtime (no redistributable), cwd = the exe's folder, RECOMP_* cleared and the run settings built into main.c, data in `%LOCALAPPDATA%\CounterStrikeXB` (settings, keymap, SaveData, logs). The .pdb is kept in out/symbols/<version>, not shipped.
- Logger (src/cs_diag.c): `logs\cs-<date>-<time>.log` per run and per map relaunch, newest five kept, 20 MB cap, user name blanked; `[BUILD]` (version, pdb id, Windows, CPU, GPU, RAM), `[VIDEO]` (at start and on every settings change), `[FREEZE]` (no flip for 10 s: every thread's stack as module offsets), `[CRASH]` (unhandled exception). scripts/symbolize_log.py names the offsets.
- config.exe: "Report a problem..." opens the logs folder with the newest log selected and a GitHub issue pre-filled with the version and PC.
- Setup (installer/cs.iss, Inno Setup 6): the same pages and update/repair logic as MvC2's; installer/cs_import.exe (not installed) checks the image against installer/manifest.h (82 files, SHA-256 each, default.xbe = dependencies.json's) and copies them to `game_files`.
- RecompLauncher (scripts/recomplauncher_manifest.py, `CounterStrike-v<ver>-Windows-x64.zip`): level 3 -- Redump dump (disc 35835, CRC-32 275fb9f8), `prepare` = `cs_import --prepare`, the seven video settings, saves/data/logs folders (`folders.logs` is what the launcher attaches to crash reports). The launcher starts the game exe itself (cs_play.exe, which held the relaunch chain, was replaced by the frame below). The manifest validates against the published schema (0 errors).
- Checked with test build 0.9.0: no CRT DLLs imported by any of the four exes; silent Setup from the .iso in 12 s, 82 files / 1,675,487,281 bytes, exit 0; installed game (scratch LOCALAPPDATA) boots to the title, writes its log with [BUILD] and [VIDEO]; through cs_play a relaunch made a second log and cs_play ended with the game; silent uninstall left no files, no registry entry and no shortcuts. Not yet checked: the Setup wizard pages by hand, the Report button, RecompLauncher itself, another PC.

## One window across map changes (2026-11-13)
David: entering a match closed the window and opened another. The match is a quick reboot of the Xbox into the game (XLaunchNewImage, src/cs_launch.c), modelled as a fresh process -- kept, since a clean process is the honest reboot.
- src/cs_frame.c: the process the player starts is now only the frame: the window (title, icon, fullscreen, taskbar button), "Loading..." between processes, a job holding every game process (close = ask the game window to close, then end the job; the last process's exit code is returned). It starts the game as a copy of itself with CS_FRAME_HWND; every relaunch inherits it.
- Toolkit patch 0022 (pin 6c29fb4, reproduced from base + patches): RECOMP_FB_PARENT makes the framebuffer window a child filling the frame; fullscreen requests go to the parent (WM_APP + 0x46) and the state is read from its style.
- main.c sets RECOMP_FB_PARENT from CS_FRAME_HWND. CS_NO_FRAME=1 (scripts/run.ps1) keeps one top-level window per process for the probes.
- Checked (release exe, scratch LOCALAPPDATA): title -> profile -> Main Menu -> Training -> Hostage-Rescue, the relaunch (PID 3024 -> 17952) drew into the same window, which never closed; keyboard focus is on the game window when the frame is in front; F11 to fullscreen and back through the frame; the Esc menu over the game; closing the frame ended both processes in 144 ms through the game's own exit.
- Seen, not changed: the toolkit prints a `[GPU] draw #` line every 200 draws (2.4 MB of log in 90 s), so a player log reaches its 20 MB cap in about 12 minutes and the last lines can be behind the flush.

## Freeze in frame mode; Intel flicker narrowed (2026-11-13)
- **Freeze:** with the frame window (0.9.1+) a match froze within a minute in 3 of ~8 runs (champ and hpvictus): the game's D3D waits in KeWaitForSingleObject (guest sub_0030A000) for an event the GPU-interrupt path never sets. Inside the frame the game process owns no visible top-level window, so Windows 11 power-throttles it (ignores its 1 ms timer resolution; EcoQoS). main.c now opts out (SetProcessInformation ProcessPowerThrottling, execution speed + timer resolution). After: 0 freezes in 6 idle champ runs and 7 hpvictus captures. The race itself remains: WARP (much slower) still froze once.
- **Intel flicker (hpvictus, UHD Graphics, i5-12450H):** reproduced with a 12 s spin capture, 1-4% of frames. Bad frames lack the gun, fog/haze and rain, and show a dark slab where the chain-link fence and the scenery behind it are; they cluster at view angles facing the fence. Not the frame window (also with CS_NO_FRAME=1), not failed uploads (every write Map checked: 0 failures), not the wrong surface (only 00D44000 is ever presented), not a draw-count drop (frame CSV). Not seen on champ (RTX 3070).
- Diagnostics added: CS_KEEP_ENV=1 (player build keeps RECOMP_* for checks on a player PC), driver capability line, checked Maps, RECOMP_CS_WARP=1 (software rasteriser), presented-surface log, gv/fog columns in RECOMP_CS_FRAME_CSV.

## Freeze root cause: D3D's BlockOnTime waits on an interrupt never raised (2026-11-13)
- Reproduced at will on WARP (software rendering, slow): every run froze within 30 s. The main thread waits (no timeout) on title event 0x0031690C (device 0x00314B40 + 0x1DCC) from D3D at 0x0030A15B: BlockOnTime clears the event, rewrites a fence slot to NO_OPERATION(5) and sleeps; D3D's PGRAPH handler (sub_0030E4D0 -> software-method switch sub_0030E710, method 5 = KeSetEvent) would set it. The executor treated software methods as no-ops, and at the freeze DMA PUT = GET: the executor had already passed the slot.
- Toolkit patch 0023 (pin 95c1e5e, reproduced): software method 5 raised as the console's PGRAPH error interrupt (trapped method/data, delivered on the timer thread under the gate, handler completion seen on PGRAPH_FIFO); and a title-event wait stuck 100 ms with the GPU idle raises it (the GPU would have, passing the slot). Methods 8/9/13 stay no-ops by default: delivered, they took frames from 9.6 to 80 ms.
- Results: WARP 3/3 runs without a freeze (6/6 before), ~15 rescues per 90 s each answered by D3D's handler; RTX 3070 frame times unchanged (median 9.2 ms, p99 11.3 ms, no frame over 50 ms).
- The DMA walk (RECOMP_PB_WALK=dma) with these interrupts also stops the freeze, but stutters (82 frames over 50 ms a minute); left off.

## Intel flicker: our bug, reproduced on WARP (2026-11-14)
- Not the Intel driver: WARP (Windows' software rasteriser, RECOMP_CS_WARP=1) shows the same bad frames on champ. It appears whenever the emulated GPU runs behind the game (a slow GPU, WARP); champ's RTX keeps up and never shows it.
- Bad frames: the executor itself runs fewer draws (RECOMP_CS_DRAWLOG flip records: ~1,100 vs 1,960 in a static buy-menu scene; the CPU-path draws -- HUD, menus, sprites, the weapon -- go missing, vertex-program world draws stay). The renderer drops nothing (no missing targets, no all-culled batches).
- One cause found and fixed (toolkit patch 0024, pin 6b40db0, reproduced): a recorded pushbuffer whose JUMP back the title had already rewritten for its next run was taken for the ring's wrap and the rest of the segment dropped.
- Main cause still open: in bad frames XDK D3D wraps its command ring early (mid-ring, right after the world) and only ~65 KB of commands (~55 draws) follow before the flip, against ~245 KB (~900 draws) in good frames (RECOMP_PB_SEGLOG). D3D's view of free ring space depends on GET, which the default segment scan publishes only after a whole segment; the DMA walk (GET per command) changes the pattern but does not fix it. Next: model GET/ring consumption as the hardware does under lag.
- RTX 3070 frame times unchanged with 0023+0024 (median 9.7 ms, no frame over 50 ms); with both off the match froze again in the same A/B.

## Flicker fixed: frames lost their tail on a mid-frame ring wrap (2026-11-14)
- Cause: the wrap handler (patch 0012) took the first JUMP after the old PUT for the end of the lap. Counter-Strike runs recorded pushbuffers (RunPushBuffer: JUMP out and back) for its HUD, sprites and models, so that first JUMP was usually a detour; the scan resumed at the recorded buffer's address (above the new PUT) and the rest of the lap -- the frame's tail -- was never walked. The ring wraps mid-frame whenever the title runs ahead of the executor: a slow GPU (the Intel laptop), WARP, or the 30/60 FPS cap (it sleeps on the executor thread).
- Toolkit patch 0025 (pin 5ee1b3c, reproduced): on a wrap, walk on from the old PUT as any segment (detours run) to the first JUMP not taken for a detour, then on from its target.
- Results: 30 FPS cap on the RTX 3070, every frame complete (was 672 of 753 draws every 6th frame); WARP, 2699/2699 frames complete and all 144 captured frames clean (10-20% bad before); Intel UHD laptop, 2933 in-match frames with no dip; RTX frame times unchanged (median 9.5 ms).
- Earlier "game skips entities when busy" and "fence lag" theories were wrong: D3D's GPU lag was the same in good and bad frames.
- Test build 0.9.4.

## Buy menu, team select, wall decals, Xbox Live New Account (2026-11-14)
David's report on 0.9.5: buy menu dark or missing, team select not right, the A/B site marks on walls flickering while walking, and Xbox Live > New Account crashing.
- Buy menu and team select: their wedges and panels use a game pixel shader with dependent texture reads (stage program 0x41E1: stage 1 DEPENDENT_AR, stage 2 DEPENDENT_GB, looking colours up in a 64x64 swizzled G8B8 texture at stage 0's colour). The specialised combiner shader (src/cs_rc_hlsl.c) gave those stages black, and the texture decoder had no G8B8 case. Now: dependent reads sample their stage's texture at stage 0's (alpha, red) / (green, blue); G8B8 decodes as (R, G, B, A) = (lo, hi, lo, hi), xemu's mapping. Buy menu now shows the teal translucent wedges, labels, ammo arcs and weapon cards; team select its background and character card. The input-stage register (SET_SHADER_OTHER_STAGE_INPUT) is not tracked; stage 0 is assumed.
- Wall decals: polygon offset was never applied. Toolkit patch 0026 (pin d2bef21, reproduced) passes SET_POLY_OFFSET_FILL_ENABLE and SET_POLYGON_OFFSET_SCALE_FACTOR/_BIAS (0x338/0x384/0x388) in Nv2aRenderState; the D3D11 back end turns them into DepthBias/SlopeScaledDepthBias (decals: factor -0.25, units -1). David confirmed the B mark no longer flickers. RECOMP_CS_ZBIAS=0 turns it off.
- Xbox Live > New Account: the game asks to launch the Dashboard (LDT_LAUNCH_DASHBOARD, reason 7, sign-up) and there is none. src/cs_launch.c now relaunches the game as if returning from the Dashboard (LDT_FROM_DASHBOARD with the context); it lands on the title screen in the same window. The Xbox Live menu stays for future online support.
- Diagnostics: RECOMP_CS_RC_SRC=<dir> keeps each specialised combiner shader with its words; RECOMP_CS_DEPLOG logs dependent-read stages.
- Test build 0.9.6.

## System link: an Ethernet controller model (2026-11-14)
- Counter-Strike links XNet and drives the MCPX NIC (0xFEF00000) itself: register writes, TX/RX descriptor rings in contiguous memory, packet buffers in the pool, an ISR (connected on vector 0 here; HalGetInterruptVector's thunk route answers 0).
- Toolkit patch 0027 (pin 43c7a5c, reproduced): nvnet.c models the controller (trapped MMIO as OHCI; xemu's nvnet register map and descriptor format), nvnet_link.c carries each frame in a UDP datagram (port 30740) to every interface's directed broadcast and to `RECOMP_NET_PEERS`. A pool address from MmGetPhysicalAddress is resolved as RAM (xbox_PhysRamNamedAt), not the contiguous window -- the first frames went out with garbage headers until that.
- XNet's DHCP is answered by the NIC itself (169.254.x.y/16 from the MAC, no gateway), so map loads do not wait out the 20 s DHCP timeout that patch 0020's `RECOMP_PHY_LINK=0` avoided; the link is now up and `RECOMP_NET=1` is a game default. Dust still reaches team select in ~25 s.
- XC_FACTORY_ETHERNET_ADDR answers the NIC's MAC: 00:50:F2 plus a hash of the computer name (`RECOMP_XBOX_MAC` overrides).
- settings.ini `[network] peers=host,...` for partners on another subnet or over a VPN (Tailscale).
- First two-PC test (0.9.7): Join kept searching. Two causes, both fixed in toolkit patch 0028 (pin 0e6cd76, reproduced):
  - The relaunch (cs_launch.c, CreateProcess with inheritance for the log pipe) handed the NIC's UDP socket to the new process; the port stayed bound to the dead process's socket and the host never read a probe. The socket is now non-inheritable.
  - Joining then failed with "This game session is no longer available": XNet's key exchange calls XcModExp (Diffie-Hellman) and DES/3DES (XcKeyTable, XcBlockCrypt, XcBlockCryptCBC), all stubs. Now real: modexp checked against Python pow() at 768 bits, DES against the standard vector.
- Verified on champ with two instances (ports 30750/30751, peers on 127.0.0.1): the client lists the host's Airstrip match, joins, picks a team, and both are in the same round (clock 4:56 on both). Two-PC test (build 0.9.8, champ hosting, hpvictus joining over Tailscale): David played a full system link match and reports it plays well; champ's log shows ~3,000 frames each way with hpvictus. Logs show the first frames each way and a traffic line every 5 s.

## The game is now the title update (2026-11-14)
- David's console, online with Insignia, has Counter-Strike's title update (TDATA/4d530036/$u): a complete rebuilt default.xbe (built 2003-11-25, certificate version 257; the disc's is 2003-10-22, version 1), plus new update.xbe/downloader.xbe. Same size, but the game's own code was recompiled (its layout changed throughout) and every library and data section moved by +0x40.
- The game is now translated from the update (scripts/pipeline.py checks dependencies.json title_update.default_xbe_sha256). Our own code had seven game addresses: entry point 0x00202C7C, D3D device pointer 0x00317028, kernel thunk slots +0x40 (table at 0x00321100), and three seeds remapped by code bytes (scripts/xbemap.py; the thread start is 0x000C5FD0). The toolkit patches use no game addresses.
- Players still supply only the disc. The run time loads default.xbe too (data, header), so Setup ships cs_tu.xbepatch (162 KB, scripts/tu_patch.py: per-block source offsets plus byte differences, LZMS through the Windows Compression API), applied by installer/tu_apply.c: by cs_import after a copy (exit 7 if it cannot), and by the game at start-up when an install made before still has the disc's file. The game refuses a default.xbe whose certificate version is not 257, with a message, instead of crashing. The patch is derived from the game, so it is built with the installers and not kept in git; game_files/default.disc.xbe keeps the disc's for it.
- Verified: boot to the main menu; two instances host and join a system link Airstrip match (team select, buy menu, same round); fresh import from the ISO gives the update's default.xbe (8 s); an upgraded 0.9.8 install on champ updated itself on first launch and reached the title screen. Test build 0.9.9 on champ and hpvictus.
- Next: the two downloaded maps (Office, Inferno: on the disc, unlocked by signed content packages), then Insignia.

## Office and Inferno, the downloadable maps (2026-11-14)
- Both maps are on the disc (cs_office.SXWad, de_inferno.SXWad); Xbox Live delivered only an unlock, a content package in E:\TDATA\4d530036\$c\<offer> (4d53003600000001 Office, ...02 Inferno; David's console had both). The map list shows a map when its package exists. XAPI's enumeration (sub_00202380) reads only ContentMeta.xbx: the XCSF header, offer ID, flags and display name. Copying David's packages in put both maps in the list.
- src/cs_content.c writes the two ContentMeta.xbx files at start-up when missing (an existing package, such as one copied from a console, is kept). They are signed as XAPI's check (sub_00207288 -> sub_002071A3) verifies: key = HMAC-SHA1(XboxHDKey, title ID), digest = HMAC-SHA1(key, header 0x14..0x88). The runtime's XboxHDKey is zeros. From an empty $c the game wrote both and listed both maps.
- Office crashed on load: the guest heap (47 MB, 0x01090000-0x04000000) ran out at 49.4 MB, a NULL allocation was written through, the main thread's TIB at 0x4000 was overwritten and XAPI's SetLastError faulted on fs:[4] = 0. main.c now calls xbox_SetMapSize(128 MB): the heap runs to 0x08000000 (111 MB) while the game still sees a 64 MB console. Office and Inferno both load and play.
- The game also opens t:\$u\contentmeta.xbx (the title update's own package; absent here). Look at it when Insignia comes up.
- Test build 0.9.10 on champ and hpvictus.

## Insignia groundwork (2026-11-14)
- David's console is registered with Insignia. Each player supplies their own console's EEPROM (never in git: /eeprom/ and *.eeprom are ignored). David's: factory and user checksums valid.
- Toolkit patch 0029 (pin 8ef7989, reproduced):
  - console_id.c: eeprom.bin (RECOMP_XBOX_EEPROM, used only when both checksums are right) answers the factory queries (serial, MAC, online key, AV/game region) and the block reads XNet makes (XC_MAX_FACTORY, XC_ENCRYPTED_SECTION, XC_MAX_ALL with this runtime's user settings and a fresh checksum); the NIC's MAC comes from it. XboxHDKey from RECOMP_XBOX_HDKEY, or decrypted from the EEPROM with the player's BIOS EEPROM key (RECOMP_XBOX_EEPROM_KEY); Microsoft's key is not in the project. Nothing secret is logged.
  - nvnet_nat.c: a router at 10.0.0.1. DHCP now hands out 10.x.y.z/8 from the MAC with Insignia's DNS (46.101.64.175, then 1.1.1.1; RECOMP_NET_DNS). UDP relayed through host sockets (same port when free), IP fragments both ways, ARP/ping for the router; TCP refused for now. Frames to other consoles still take the system link (re-tested: two instances host and join).
  - RtlNtStatusToDosError: facility-7 statuses map to their low word. XboxKrnlVersion 1.0.5838.1 (retail; RECOMP_XBOX_KERNEL_BUILD).
- Game: eeprom.bin in %LOCALAPPDATA%\CounterStrikeXB and settings.ini [insignia] hdd_key= are passed to the toolkit.
- Verified: David's identity loads; Xbox Live > Sign-In shows only "New Account" (no account on the drive yet), and no Xbox Live traffic happens before an account exists.
- Next: the HDD key (from the player), and an Xbox Live account on the emulated drive (partition 0 sectors 8+). Creating or recovering one is the dashboard's job; the CS disc's dashupdate.xbe carries the online dashboard.
