"""Disassemble a host function of the build by name (read-only aid).

  python scripts/hostdis.py sub_000B7E80 [max_instructions]

Looks the symbol up in build/RelWithDebInfo/cs2_xbox_recomp.pdb (dbghelp) and
prints its x64 code with capstone, plus a count of thread-local accesses
(gs:[0x58]) and memory operands -- a quick way to see what the C compiler made
of a translated guest function.
"""
import ctypes
import ctypes.wintypes as wt
import os
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), '.deps'))
import capstone  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.environ.get('CS_EXE') or os.path.join(ROOT, 'build', 'RelWithDebInfo', 'cs2_xbox_recomp.exe')


class SYMBOL_INFO(ctypes.Structure):
    _fields_ = [('SizeOfStruct', wt.ULONG), ('TypeIndex', wt.ULONG), ('Reserved', ctypes.c_uint64 * 2),
                ('Index', wt.ULONG), ('Size', wt.ULONG), ('ModBase', ctypes.c_uint64), ('Flags', wt.ULONG),
                ('Value', ctypes.c_uint64), ('Address', ctypes.c_uint64), ('Register', wt.ULONG),
                ('Scope', wt.ULONG), ('Tag', wt.ULONG), ('NameLen', wt.ULONG), ('MaxNameLen', wt.ULONG),
                ('Name', ctypes.c_char * 512)]


def main():
    name = sys.argv[1]
    limit = int(sys.argv[2]) if len(sys.argv) > 2 else 120
    dbg = ctypes.WinDLL('dbghelp')
    dbg.SymInitialize.argtypes = [wt.HANDLE, ctypes.c_char_p, wt.BOOL]
    dbg.SymLoadModuleEx.restype = ctypes.c_uint64
    dbg.SymLoadModuleEx.argtypes = [wt.HANDLE, wt.HANDLE, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_uint64,
                                    wt.DWORD, ctypes.c_void_p, wt.DWORD]
    dbg.SymFromName.argtypes = [wt.HANDLE, ctypes.c_char_p, ctypes.POINTER(SYMBOL_INFO)]
    proc = wt.HANDLE(0x1235)
    dbg.SymSetOptions(0x2)
    if not dbg.SymInitialize(proc, os.path.dirname(EXE).encode(), False):
        sys.exit('SymInitialize failed')
    base = dbg.SymLoadModuleEx(proc, None, EXE.encode(), None, 0x140000000, 0, None, 0)
    si = SYMBOL_INFO(); si.SizeOfStruct = ctypes.sizeof(SYMBOL_INFO) - 512; si.MaxNameLen = 511
    if not dbg.SymFromName(proc, name.encode(), ctypes.byref(si)):
        sys.exit('no symbol ' + name)
    rva = si.Address - base
    pe = open(EXE, 'rb').read()
    peo = struct.unpack_from('<I', pe, 0x3C)[0]
    nsec = struct.unpack_from('<H', pe, peo + 6)[0]
    opt = struct.unpack_from('<H', pe, peo + 20)[0]
    sh = peo + 24 + opt
    for i in range(nsec):
        vsz, va, rsz, raw = struct.unpack_from('<IIII', pe, sh + i * 40 + 8)
        if va <= rva < va + vsz:
            off = raw + rva - va
            break
    else:
        sys.exit('rva not in a section')
    size = si.Size or 4096
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    n = tls = mem = 0
    for ins in md.disasm(pe[off:off + size], 0x140000000 + rva):
        n += 1
        if 'gs:' in ins.op_str:
            tls += 1
        if '[' in ins.op_str:
            mem += 1
        if n <= limit:
            print('%X  %s %s' % (ins.address, ins.mnemonic, ins.op_str))
    print('%s: %d bytes, %d instructions, %d thread-local base loads, %d memory operands' % (name, size, n, tls, mem))


if __name__ == '__main__':
    main()
