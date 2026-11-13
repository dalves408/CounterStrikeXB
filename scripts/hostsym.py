"""Name host addresses of a build (as hostdis.py prints them, preferred base).

  python scripts/hostsym.py 0x146c94f68 [...]      (CS_EXE for another build)
"""
import ctypes
import ctypes.wintypes as wt
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from hostdis import EXE, SYMBOL_INFO  # noqa: E402

dbg = ctypes.WinDLL('dbghelp')
dbg.SymInitialize.argtypes = [wt.HANDLE, ctypes.c_char_p, wt.BOOL]
dbg.SymLoadModuleEx.restype = ctypes.c_uint64
dbg.SymLoadModuleEx.argtypes = [wt.HANDLE, wt.HANDLE, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_uint64,
                                wt.DWORD, ctypes.c_void_p, wt.DWORD]
dbg.SymFromAddr.argtypes = [wt.HANDLE, ctypes.c_uint64, ctypes.POINTER(ctypes.c_uint64), ctypes.POINTER(SYMBOL_INFO)]
proc = wt.HANDLE(0x1236)
dbg.SymSetOptions(0x2)
dbg.SymInitialize(proc, os.path.dirname(EXE).encode(), False)
dbg.SymLoadModuleEx(proc, None, EXE.encode(), None, 0x140000000, 0, None, 0)
for a in sys.argv[1:]:
    si = SYMBOL_INFO(); si.SizeOfStruct = ctypes.sizeof(SYMBOL_INFO) - 512; si.MaxNameLen = 511
    d = ctypes.c_uint64(0)
    ok = dbg.SymFromAddr(proc, int(a, 16), ctypes.byref(d), ctypes.byref(si))
    print(a, (si.Name.decode() + '+0x%X' % d.value) if ok else '?')
