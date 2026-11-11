# HANDOFF - Counter-Strike (Xbox) recomp, CounterStrikeXB (formerly CounterStrikeXB2)
Stopped 2026-11-11 12:02 PT at David's request. Full detail and evidence: docs/STATUS.md.
2026-11-11: at David's request the old C:\Recomp\CounterStrikeXB was deleted, this project renamed from CounterStrikeXB2 to C:\Recomp\CounterStrikeXB, and published to github.com/dalves408/CounterStrikeXB (replacing the old repo). Shared C:\Recomp\tools\xboxrecomp untouched.

## Working (verified)
- Clean project: own toolkit clone pinned (v0.17.0 + local patches 0001-0003, applied by setup.ps1), pure generated code, clean disc extraction, project-local SaveData.
- Boot: intros (MGS/Valve XMV via PVIDEO), title screen, attract movie.
- Input: keyboard + pad injection drive menus (title START, profile auto-create, Main Menu, Training, Profile keyboard, A/B/Y). Fix = disasm false-split patch 0001 (XAPI device open). USB deadline patch 0003.
- Framing: full 640x480 screens (present-surface patch 0002).
- Map loads: game relaunches itself via XLaunchNewImage; src/cs_launch.c carries the launch page to a fresh exe. Training Hostage-Rescue and Single Player > Beginner > Airstrip both reach in-game.
- Launch: powershell -ExecutionPolicy Bypass -File scripts\run.ps1 (START/A skips intros). Airstrip path: Main Menu Down x1, then A on every screen.

## Broken / next (priority order)
1. In-game world geometry (BSP brushes) and sky not drawn; only props/foliage render. No first-person weapon.
2. HUD drawn ~45% down screen instead of edges; radar/crosshair misplaced; HUD ghosting; timer colon missing (likely 2D scale vs 640x960 supersample).
3. Performance: 0-4 FPS in match, game clock ~half speed; ~2 min map load.
4. Black window gap during the self-relaunch (looks like a crash) - keep old window until new one shows.
5. Intro movies not smooth.
6. Unverified: real physical XInput controller; first B on name keyboard ignored once.
7. Planned (David): DX11 renderer like the other C:\Recomp projects - study their D3D11 back ends first, likely the path to fix 1-3.

## Rules David set
Planning before work; short TLDR updates; David drives game tests unless he asks; other projects read-only references, borrow only verified-compatible code; no menu bypasses or game-memory hacks; generated code stays pure; push/replace GitHub repo only when David approves.
