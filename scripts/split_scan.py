"""Find detected function starts that fall in the middle of an instruction of
the preceding prologue function (linear decode). Read-only analysis aid.
   python scripts/split_scan.py [functions.json]
"""
import json, sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from xbedis import read, capstone
root = Path(__file__).resolve().parents[1]
fj = Path(sys.argv[1]) if len(sys.argv) > 1 else root / "tools/xboxrecomp/tools/disasm/output/functions.json"
fs = sorted(json.load(open(fj)), key=lambda f: int(f["start"], 16))
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
hits = []
owner = None
for f in fs:
    s = int(f["start"], 16)
    if f.get("has_prologue"):
        owner = f; continue
    if not owner: continue
    o = int(owner["start"], 16)
    if s - o > 0x4000: continue
    for ins in md.disasm(read(o, s - o + 16), o):
        if ins.address == s: break
        if ins.address < s < ins.address + ins.size:
            hits.append((s, f["detection_method"], owner["start"], f"{ins.address:08X} {ins.mnemonic} {ins.op_str}"))
            break
        if ins.address > s: break
for h in hits:
    print(f"0x{h[0]:08X} {h[1]:<18} in {h[2]} over {h[3]}")
print(len(hits), "mid-instruction starts")
