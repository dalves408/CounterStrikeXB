"""Regenerate src/recomp/gen from the title update's default.xbe with the pinned toolkit.

game_files/default.xbe is the update's (scripts/tu_patch.py apply makes it from the
disc's, which is kept as default.disc.xbe).

The generated tree is pure pipeline output: this script wipes it first and
nothing in it is ever edited by hand. Game-specific code lives in src/.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

root = Path(__file__).resolve().parents[1]
deps = json.loads((root / "dependencies.json").read_text())
toolkit = root / deps["xboxrecomp"]["path"]
xbe = root / "game_files" / "default.xbe"
gen = root / "src" / "recomp" / "gen"
logs = root / "analysis" / "pipeline"
analysis_json = root / "analysis" / "default_analysis.json"
seeds = root / "seeds" / "cs_seed_functions.json"

ap = argparse.ArgumentParser(description=__doc__)
ap.add_argument("--after-disasm", action="store_true", help="reuse the toolkit's disasm output")
args = ap.parse_args()

if not xbe.is_file():
    raise SystemExit(f"missing {xbe}; run scripts/extract_disc.py first")
digest = hashlib.sha256(xbe.read_bytes()).hexdigest()
if digest != deps["title_update"]["default_xbe_sha256"]:
    raise SystemExit(f"default.xbe sha256 {digest} != the title update's {deps['title_update']['default_xbe_sha256']}; run scripts/tu_patch.py apply")
head = subprocess.run(["git", "-C", str(toolkit), "rev-parse", "HEAD"], capture_output=True,
                      text=True, check=True).stdout.strip()
if head != deps["xboxrecomp"]["commit"]:
    raise SystemExit(f"toolkit at {head}, pin is {deps['xboxrecomp']['commit']}; run scripts/setup.ps1")

env = os.environ.copy()
env["PYTHONPATH"] = str(root / ".deps") + os.pathsep + env.get("PYTHONPATH", "")
env["PYTHONIOENCODING"] = "utf-8"
logs.mkdir(parents=True, exist_ok=True)


def run(tag: str, module: str, *options: object) -> None:
    log = logs / f"{tag}.log"
    print(f"[{tag}] python -m {module} ... (log {log})", flush=True)
    with log.open("w", encoding="utf-8") as out:
        subprocess.run([sys.executable, "-m", module, str(xbe), *map(str, options)],
                       cwd=toolkit, env=env, stdout=out, stderr=subprocess.STDOUT, check=True)


if not args.after_disasm:
    run("xbe_parser", "tools.xbe_parser", "--json", analysis_json)
    # D3D/DSOUND/XMV/... hold code but are not all flagged executable.
    run("disasm", "tools.disasm", "--text-only", "--analysis-json", analysis_json,
        "--extra-sections", "D3D,D3DX,DSOUND,XNET,XONLINE,XMV,WMADEC,XGRPH,XACTENG,XPP,DOLBY,XON_RD",
        "--seed-functions", seeds)
run("func_id", "tools.func_id", "-v")
run("abi_analysis", "tools.abi_analysis", "-v")
if gen.exists():
    shutil.rmtree(gen)
gen.mkdir(parents=True)
run("recomp", "tools.recomp", "--all", "--split", "250", "--game-name", "Counter-Strike",
    "--promote-regs",   # register-promotion hooks; inert unless the build defines RECOMP_REG_PROMOTE
    "--direct-mmio",    # absolute APU register accesses call the model, no host exception
    # hand-written replacements and entry hooks (sub_X_enter) in src/recomp_manual.c
    "--exclude-manual", root / "src" / "recomp_manual.c",
    "--gen-dir", gen)
print("pipeline complete", flush=True)
