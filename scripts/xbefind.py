"""Find values in default.xbe and the code that references them (read-only aid).

  python scripts/xbefind.py float 3.0517578e-05      # a 32-bit float constant
  python scripts/xbefind.py double 32768              # a 64-bit double constant
  python scripts/xbefind.py bytes 00000038            # raw hex bytes
  python scripts/xbefind.py refs 0x0031A2B0           # instructions whose operand names this address
  python scripts/xbefind.py imm 1ea4 2F0000 320000    # instructions with that hex text in an operand

Prints the virtual addresses of every match; with `refs`, scans all executable
sections with capstone and prints each instruction that mentions the address.
"""
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / ".deps"))
import capstone  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
xbe = (ROOT / "game_files" / "default.xbe").read_bytes()
base = struct.unpack_from("<I", xbe, 0x104)[0]
nsec, sec_hdr = struct.unpack_from("<II", xbe, 0x11C)
secs = []
for i in range(nsec):
    o = sec_hdr - base + i * 56
    flags, va, vsz, raw, rsz = struct.unpack_from("<IIIII", xbe, o)
    secs.append((flags, va, vsz, raw, rsz))


def find(pattern):
    for flags, va, vsz, raw, rsz in secs:
        data = xbe[raw:raw + rsz]
        i = data.find(pattern)
        while i >= 0:
            yield va + i
            i = data.find(pattern, i + 1)


def refs(addr):
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    md.skipdata = True                      # keep going over data in code sections
    needle = "0x%x" % addr
    for flags, va, vsz, raw, rsz in secs:
        if not (flags & 4):                     # executable sections
            continue
        data = xbe[raw:raw + rsz]
        off = 0
        while off < len(data):
            n = 0
            for ins in md.disasm(data[off:off + 0x10000], va + off):
                n = ins.address + ins.size - (va + off)
                if needle in ins.op_str:
                    print("%08X  %s %s" % (ins.address, ins.mnemonic, ins.op_str))
            off += max(n, 1) if n else 1


kind = sys.argv[1]
if kind == "float":
    pat = struct.pack("<f", float(sys.argv[2]))
elif kind == "double":
    pat = struct.pack("<d", float(sys.argv[2]))
elif kind == "bytes":
    pat = bytes.fromhex(sys.argv[2])
elif kind == "refs":
    refs(int(sys.argv[2], 16))
    sys.exit(0)
elif kind == "imm":                       # imm <hex substring> [lo hi]: operands containing it
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    md.skipdata = True                      # keep going over data in code sections
    sub = sys.argv[2].lower()
    lo = int(sys.argv[3], 16) if len(sys.argv) > 3 else 0
    hi = int(sys.argv[4], 16) if len(sys.argv) > 4 else 0xFFFFFFFF
    for flags, va, vsz, raw, rsz in secs:
        if not (flags & 4):
            continue
        a, b = max(va, lo), min(va + rsz, hi)
        if a >= b:
            continue
        for ins in md.disasm(xbe[raw + (a - va):raw + (b - va)], a):
            if sub in ins.op_str:
                print("%08X  %s %s" % (ins.address, ins.mnemonic, ins.op_str))
    sys.exit(0)
else:
    sys.exit(__doc__)
for v in find(pat):
    print("%08X" % v)
