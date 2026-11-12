# smalltask — Task 3 + Task 4 complete (SP re-verified)

## Status
Done. Coordinator: leave idle until next assignment.

## Task 3 (black BSP/sky)
Root cause is **not** primarily in `src/cs_d3d11.c` draw logic. World batches arrive with `diffuse.a == 0` from the NV2A toolkit VP path; combiners use `T0.a * V0.a` and GREATER/127, so brushes fail alpha test / blend to void.

## Task 4 (why diffuse.a=0 + workaround + patches)
See `runs/d9_tk/REPORT.md` (UTF-8 no BOM).

- **Toolkit:** FFP material alpha BSS=0 is a real bug but CS world is VP-dominated; VP `oD0.a` often stays 0 when RGB is lit. Draft patches in `runs/d9_tk/patches/` (0005 material+oD0 alpha, 0006 linear_twin 0x3C->0x41). Scratch only — did not touch `tools/xboxrecomp` or pin.
- **Workaround:** `RECOMP_CS_ALPHA_LIFT` in `src/cs_d3d11.c` (default on; `=0` disables). Honest: does **not** fully restore textured BSP/sky.
- **Probe fix:** `scripts/ingame_probe.ps1` uses Down+Z for Single Player (not Return on main menu).

### SP A/B evidence
| Run | Env | Menu proof | In-game |
|-----|-----|------------|---------|
| `d9_lift_on` | lift default on | `docs/evidence/d3d11_world_d9_lift_on_sp_menu.png` + `_loading.png` | grey void + tree sliver; bots in kill feed (`_360`/`_380`) |
| `d9_lift_off` | `RECOMP_CS_ALPHA_LIFT=0` | `docs/evidence/d3d11_world_d9_lift_off_sp_menu.png` + `_loading.png` | black sky/ground, trees/fence, garbled models (`_360`/`_380`) |

## RESULTS_SUMMARY
Task 3/4 complete. Black world is toolkit VP diffuse.a=0, not a D3D11 draw bug. ALPHA_LIFT is a guarded partial workaround. Upstream draft patches 0005/0006 in `runs/d9_tk/patches/`. Both lift on/off probes re-verified in **Single Player Airstrip** after menu-path fix.