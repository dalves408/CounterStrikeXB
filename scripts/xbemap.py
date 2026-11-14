"""Find where code at a disc default.xbe address lives in the title update's (read-only aid).

    python scripts/xbemap.py 0x000C6310 [0x...]

Matches the bytes at each address (64 by default) against the update's .text
and library sections, treating any 4-byte value that looks like an address in
the image (0x00010000-0x00900000) as a wildcard, since everything after the
first change moved. Prints the candidates with how many bytes matched.
"""
import struct, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OLD = (ROOT / 'game_files' / 'default.disc.xbe').read_bytes()
NEW = (ROOT / 'game_files' / 'default.xbe').read_bytes()


def sections(d):
    base, = struct.unpack_from('<I', d, 0x104)
    n, sva = struct.unpack_from('<II', d, 0x11C)
    out = []
    for i in range(n):
        fl, va, vs, ra, rs, nm = struct.unpack_from('<IIIIII', d, sva - base + i * 56)
        out.append((va, rs, ra))
    return out


def raw_at(d, va):
    for sva, rs, ra in sections(d):
        if sva <= va < sva + rs:
            return ra + va - sva
    return None


def pattern(va, n=64):
    r = raw_at(OLD, va)
    b = OLD[r:r + n]
    mask = [1] * len(b)
    for i in range(len(b) - 3):
        v, = struct.unpack_from('<I', b, i)
        if 0x10000 <= v < 0x900000:
            for k in range(4):
                mask[i + k] = 0
    return b, mask


def find(va, n=64):
    b, mask = pattern(va, n)
    need = sum(mask)
    anchor = next(i for i in range(len(b) - 8) if all(mask[i:i + 8]))
    key = b[anchor:anchor + 8]
    hits = []
    start = 0
    while True:
        p = NEW.find(key, start)
        if p < 0:
            break
        s = p - anchor
        if s >= 0:
            m = sum(1 for i in range(len(b)) if mask[i] and NEW[s + i] == b[i])
            hits.append((m, s))
        start = p + 1
    hits.sort(reverse=True)
    res = []
    for m, s in hits[:3]:
        for sva, rs, ra in sections(NEW):
            if ra <= s < ra + rs:
                res.append((sva + s - ra, m, need))
    return res


if __name__ == '__main__':
    for a in sys.argv[1:]:
        va = int(a, 16)
        print('%08X ->' % va, ', '.join('%08X (%d/%d)' % h for h in find(va)) or 'not found')
