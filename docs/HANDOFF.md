# HANDOFF - Counter-Strike (Xbox) recomp, CounterStrikeXB (formerly CounterStrikeXB2)
Current intro/menu work and the earlier Airstrip observations are documented in
docs/STATUS.md. The earlier handoff was stopped 2026-11-11 12:02 PT.
2026-11-11: at David's request the old C:\Recomp\CounterStrikeXB was deleted, this project renamed from CounterStrikeXB2 to C:\Recomp\CounterStrikeXB, and published to github.com/dalves408/CounterStrikeXB (replacing the old repo). Shared C:\Recomp\tools\xboxrecomp untouched.

## Current priority: intros and menus
- Do not resume gameplay fixes yet. The goal is reliable intros, no menu flashes,
  quicker loading, and sustained real menu rendering at 60 FPS or better.
- Patch 0009 removes repeated combiner decoding and DXT decompression from hot
  pixel paths. It uses prepared shader programs with general fallback and a
  byte-validated, bounded texture cache; dynamic texture updates remain visible.
- Latest pin: `1e6abc3`, with a clean nine-patch replay and supported build.
  Focused rendering/format regressions pass and benchmark framebuffer hashes
  match. Generated code and the existing dispatch-gate setting are unchanged.
- `runs\menu_final_confirmed_20261112` completes both intros naturally, enters
  the main menu through START, and averages 36.8 FPS across 22 one-second samples
  (18-42 FPS), versus about 12 FPS before this work. The target is **not met**.
- `runs\menu_cached_20261112` also captures Single Player difficulty selection
  and Back. No gameplay or Training was entered. All owned live-test processes
  were closed normally.
- Next: investigate the remaining pixel costs and dispatch-gate/IRQ safe-point
  waits, then measure consecutive composed frames for movie cadence and flashes.
  Current five-second screenshots cannot establish either. Loading remains
  variable; fixed-time input schedules missed the title in two runs, so final
  confirmation waited for the title instead of assuming a launch delay.
- Runtime entry points for the remaining pixel work are `xf_rows`, `rc_tent`,
  and `prepare_textures` in `tools\xboxrecomp\src\kernel\nv2a_pb_exec.c`, plus
  the prepared evaluator in `nv2a_combiner.c`.
- Referenced run logs/screenshots and the profiling/benchmark helpers remain
  local or in session artifacts, not in GitHub. The versioned toolkit patches
  include the regression tests; follow README Setup to reproduce the build
  with a fresh checkout and a legally dumped copy of the game.

## Earlier gameplay work
- The observed unresolved x87 target and a verified sibling are pipeline seeds.
- Toolkit patch 0004 fixes transform-constant aliasing, fixed-function inline
  transforms, and large software raster bounds. Synthetic regressions pass.
- Toolkit patch 0005 removes shared per-pixel raster-worker statistics writes.
  A 12-worker synthetic benchmark improved from 8.05 to 1.93 ms per solid batch
  and from 12.74 to 7.13 ms per textured combiner batch, with matching pixels.
  Actual gameplay FPS after this change is unverified.
- Toolkit patch 0006 honors face culling and triangle-strip winding. A synthetic
  workload with every face duplicated in the opposite winding improved from
  3.01 to 2.00 ms per batch with matching pixels. Its Airstrip effect is unknown.
- Toolkit patch 0007 protects voice updates and streamed samples; the game
  launcher enables the one-CPU dispatch gate. A controlled submission race
  matches the missing-descriptor shape observed during an intro freeze, but
  the cause of the live freeze has not been proven.
- Toolkit patch 0008 forwards PVIDEO STOP events to the presenter and protects
  restarted banks from stale consumption. The focused overlay tests pass.
- In the post-fix Airstrip run David reported improved graphics, but substantial
  glitches and missing renders remained at about 1 FPS. Intro videos stuttered and
  map loading was slow. The window closed before an in-game frame trace or
  screenshot could be captured; its reason is unknown.
- Once frontend goals are met, resume independent gameplay verification through
  Single Player -> Beginner -> Airstrip, never Training, and capture a gameplay
  frame trace while the game remains open.
- Logged failed-open probes took 2.17 s for 3,750 in a standalone reproduction;
  that does not establish the cause of the roughly 130 s recorded without flips.
  Synthetic 640x480 PVIDEO conversion took 2.43 ms per frame; the cause of intro
  stutter is also unconfirmed.

## Previously working (live-verified before patch 0004)
- Clean project: own toolkit clone pinned (now v0.17.0 + local patches 0001-0009, applied by setup.ps1), pure generated code, clean disc extraction, project-local SaveData.
- Boot: intros (MGS/Valve XMV via PVIDEO), title screen, attract movie.
- Input: keyboard + pad injection drive menus (title START, profile auto-create, Main Menu, Training, Profile keyboard, A/B/Y). Fix = disasm false-split patch 0001 (XAPI device open). USB deadline patch 0003.
- Framing: full 640x480 screens (present-surface patch 0002).
- Map loads: game relaunches itself via XLaunchNewImage; src/cs_launch.c carries the launch page to a fresh exe. Training Hostage-Rescue and Single Player > Beginner > Airstrip both reach in-game.
- Launch: powershell -ExecutionPolicy Bypass -File scripts\run.ps1 (START/A skips intros). Airstrip path: Main Menu Down x1, then A on every screen.

## Earlier visual baseline / remaining investigation
The detailed visual observations below predate patch 0004; the remaining
post-patch defects need a new frame trace and screenshot.
1. In-game world geometry (BSP brushes) and sky not drawn; only props/foliage render. No first-person weapon.
2. HUD drawn ~45% down screen instead of edges; radar/crosshair misplaced; HUD ghosting; timer colon missing (likely 2D scale vs 640x960 supersample).
3. Performance: 0-4 FPS in match, game clock ~half speed; ~2 min map load.
4. Black window gap during the self-relaunch (looks like a crash) - keep old window until new one shows.
5. Intro movies not smooth.
6. Unverified: real physical XInput controller; first B on name keyboard ignored once.
7. Planned (David): DX11 renderer like the other C:\Recomp projects - study their D3D11 back ends first, likely the path to fix 1-3.

## Rules David set
Planning before work; short TLDR updates; verify changes before handoff; other projects read-only references, borrow only verified-compatible code; no menu bypasses or game-memory hacks; generated code stays pure; push/replace GitHub repo only when David approves.

## D3D11 back end (merged into main 2026-11-12)
- `src/cs_d3d11.c` registers a Direct3D 11 renderer with the executor (`RECOMP_CS_D3D11=0` = CPU rasteriser). Toolkit patches 0010 (combiner state and texture stages passed to 3D batches) and 0011 (material alpha default, format 0x3C twin) sit on top of patches 0001-0009.
- Menus and in-game (Airstrip) render on the GPU at about 8-13 FPS by log; the BSP world is still black in game (vertex diffuse alpha 0 from the vertex-program path; `RECOMP_CS_ALPHA_LIFT` is only a partial workaround). Details and evidence: docs/STATUS.md (D3D11 section) and docs/evidence/d3d11_*.
- Next candidates: find the instruction/default leaving oD0.a at 0 (toolkit side), measure D3D11 vs the CPU path on menus (36.8 FPS CPU-path menu average from patch 0009), relaunch black gap, mips/cube maps.
