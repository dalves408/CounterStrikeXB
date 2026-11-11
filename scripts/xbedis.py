"""Disassemble guest code straight from game_files/default.xbe (read-only aid).
   python scripts/xbedis.py 0x000C6BC7 [count] [--back N]
"""
import struct, sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / ".deps"))
import capstone
xbe = (Path(__file__).resolve().parents[1] / "game_files" / "default.xbe").read_bytes()
base = struct.unpack_from("<I", xbe, 0x104)[0]
nsec, sec_hdr = struct.unpack_from("<II", xbe, 0x11C)
secs = []
for i in range(nsec):
    o = sec_hdr - base + i * 56
    flags, va, vsz, raw, rsz = struct.unpack_from("<IIIII", xbe, o)
    secs.append((va, vsz, raw, rsz))
def read(va, n):
    for s_va, vsz, raw, rsz in secs:
        if s_va <= va < s_va + rsz:
            off = va - s_va
            return xbe[raw + off: raw + min(rsz, off + n)]
    return b""
def u32(va): return struct.unpack("<I", read(va, 4))[0]
if __name__ == "__main__":
    va = int(sys.argv[1], 16); cnt = int(sys.argv[2]) if len(sys.argv) > 2 and not sys.argv[2].startswith("--") else 40
    back = int(sys.argv[sys.argv.index("--back") + 1]) if "--back" in sys.argv else 0
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    start = va - back
    for ins in md.disasm(read(start, cnt * 16), start):
        mark = "=>" if ins.address == va else "  "
        print(f"{mark}{ins.address:08X}  {ins.mnemonic} {ins.op_str}")
        cnt -= 1
        if cnt <= 0: break
