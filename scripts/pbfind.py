"""Find NV2A vertex-program constant uploads in a RAM image (xemu physical.ram dump).

  python scripts/pbfind.py <physical.ram> [const_index ...]

Looks for the pushbuffer sequence SET_TRANSFORM_CONSTANT_LOAD(i) followed by
SET_TRANSFORM_CONSTANT[0..n) (method 0x0B80) and prints the uploaded floats, so
what real hardware received can be compared with what the recomp computes.
Hardware index = D3D constant + 96.
"""
import struct
import sys
from collections import Counter

ram = open(sys.argv[1], 'rb').read()
want = set(int(a, 0) for a in sys.argv[2:]) or None
n = len(ram) // 4
dw = struct.unpack('<%dI' % n, ram[:n * 4])
seen = Counter()
for i in range(n - 2):
    h = dw[i]
    if (h & 0xE0001FFF & ~0xE000) != 0x1EA4 or (h >> 29) or ((h >> 18) & 0x7FF) != 1:
        continue
    idx = dw[i + 1]
    if idx > 191 or (want and idx not in want):
        continue
    h2 = dw[i + 2]
    if (h2 & 0x1FFC) != 0x0B80 or (h2 >> 29):
        continue
    cnt = (h2 >> 18) & 0x7FF
    if cnt == 0 or cnt > 32 or i + 3 + cnt > n:
        continue
    vals = struct.unpack('<%df' % cnt, struct.pack('<%dI' % cnt, *dw[i + 3:i + 3 + cnt]))
    key = (idx, tuple('%.6g' % v for v in vals))
    seen[key] += 1
    if seen[key] == 1:
        print('%08X  c[%d] x%d  %s' % (i * 4, idx, cnt // 4, ' '.join('%.6g' % v for v in vals)))
