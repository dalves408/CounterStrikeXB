"""Name host code addresses from a run's logs using the build's PDB.

  python scripts/symbolize.py runs/<label>            # every 0x7FF... address in stderr.log
  python scripts/symbolize.py runs/<label> 00007FF653E9588A ...

Needs the "[CS2] image base 00007FF6..." line the game prints at startup
(src/main.c) and build/RelWithDebInfo/cs2_xbox_recomp.pdb from the same build.
Uses dbghelp.dll through ctypes (Windows only).
"""
import ctypes
import ctypes.wintypes as wt
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.path.join(ROOT, 'build', 'RelWithDebInfo', 'cs2_xbox_recomp.exe')


class SYMBOL_INFO(ctypes.Structure):
    _fields_ = [('SizeOfStruct', wt.ULONG), ('TypeIndex', wt.ULONG), ('Reserved', ctypes.c_uint64 * 2),
                ('Index', wt.ULONG), ('Size', wt.ULONG), ('ModBase', ctypes.c_uint64), ('Flags', wt.ULONG),
                ('Value', ctypes.c_uint64), ('Address', ctypes.c_uint64), ('Register', wt.ULONG),
                ('Scope', wt.ULONG), ('Tag', wt.ULONG), ('NameLen', wt.ULONG), ('MaxNameLen', wt.ULONG),
                ('Name', ctypes.c_char * 512)]


class IMAGEHLP_LINE64(ctypes.Structure):
    _fields_ = [('SizeOfStruct', wt.DWORD), ('Key', ctypes.c_void_p), ('LineNumber', wt.DWORD),
                ('FileName', ctypes.c_char_p), ('Address', ctypes.c_uint64)]


def main():
    run = sys.argv[1]
    log = os.path.join(run, 'stderr.log')
    text = open(log, errors='replace').read() if os.path.exists(log) else ''
    m = re.search(r'\[CS2\] image base ([0-9A-Fa-f]+)', text)
    if not m:
        sys.exit('no "[CS2] image base" line in %s (build older than the base print?)' % log)
    run_base = int(m.group(1), 16)
    addrs = [int(a, 16) for a in sys.argv[2:]]
    if not addrs:
        addrs = sorted({int(a, 16) for a in re.findall(r'\b(0{0,4}7FF[0-9A-Fa-f]{9})\b', text)})

    dbg = ctypes.WinDLL('dbghelp')
    dbg.SymInitialize.argtypes = [wt.HANDLE, ctypes.c_char_p, wt.BOOL]
    dbg.SymLoadModuleEx.restype = ctypes.c_uint64
    dbg.SymLoadModuleEx.argtypes = [wt.HANDLE, wt.HANDLE, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_uint64,
                                    wt.DWORD, ctypes.c_void_p, wt.DWORD]
    dbg.SymFromAddr.argtypes = [wt.HANDLE, ctypes.c_uint64, ctypes.POINTER(ctypes.c_uint64), ctypes.POINTER(SYMBOL_INFO)]
    dbg.SymGetLineFromAddr64.argtypes = [wt.HANDLE, ctypes.c_uint64, ctypes.POINTER(wt.DWORD), ctypes.POINTER(IMAGEHLP_LINE64)]
    dbg.SymSetOptions(0x2 | 0x10)                      # UNDNAME | LOAD_LINES
    proc = wt.HANDLE(0x1234)                           # any unique pseudo handle
    if not dbg.SymInitialize(proc, os.path.dirname(EXE).encode(), False):
        sys.exit('SymInitialize failed')
    base = dbg.SymLoadModuleEx(proc, None, EXE.encode(), None, 0x140000000, 0, None, 0)
    if not base:
        sys.exit('SymLoadModuleEx failed for ' + EXE)
    for a in addrs:
        va = base + (a - run_base)
        si = SYMBOL_INFO(); si.SizeOfStruct = ctypes.sizeof(SYMBOL_INFO) - 512; si.MaxNameLen = 511
        disp = ctypes.c_uint64(0)
        name = '?'
        if dbg.SymFromAddr(proc, va, ctypes.byref(disp), ctypes.byref(si)):
            name = '%s+0x%X' % (si.Name.decode(errors='replace'), disp.value)
        ln = IMAGEHLP_LINE64(); ln.SizeOfStruct = ctypes.sizeof(IMAGEHLP_LINE64)
        d32 = wt.DWORD(0)
        where = ''
        if dbg.SymGetLineFromAddr64(proc, va, ctypes.byref(d32), ctypes.byref(ln)):
            where = '  %s:%d' % (os.path.basename(ln.FileName.decode(errors='replace')), ln.LineNumber)
        print('%016X  %s%s' % (a, name, where))


if __name__ == '__main__':
    main()
