# CounterStrikeXB (formerly CounterStrikeXB2) - status (verified facts only)

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