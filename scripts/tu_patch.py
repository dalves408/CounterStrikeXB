"""The title update as an install-time patch: the disc's default.xbe -> the update's.

    python scripts/tu_patch.py make  [--out installer/Output/cs_tu.xbepatch]
    python scripts/tu_patch.py apply [--patch FILE] [--dir game_files]

The game is translated from the title update's default.xbe (cert version 257),
but it also loads that file at run time (initial data, header), and players
supply only their disc. So Setup ships this patch, not the update: cs_import
--update turns the disc's default.xbe into the update's, checking both by
SHA-256. The patch carries the bytes needed to rebuild the update from the disc,
so like the installers it is not kept in git.

make reads game_files/default.disc.xbe (the disc's) and game_files/default.xbe
(the update's). apply is for a development tree: it keeps the disc's file as
default.disc.xbe and writes the update's.

Format (little-endian): "CSTU", u32 version 1, u32 block size, u32 source size,
32-byte source SHA-256, u32 target size, 32-byte target SHA-256, u32 packed
size, then the packed payload (Windows Compression API, LZMS): one i32 source
offset per target block (-1: none), then one byte per target byte, the
difference from the source byte at that offset (mod 256).
"""
from __future__ import annotations

import argparse
import ctypes
import hashlib
import struct
from ctypes import wintypes
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BLOCK = 64
MAGIC = b'CSTU'
LZMS = 5                        # COMPRESS_ALGORITHM_LZMS
RAW = 0x20000000                # COMPRESS_RAW: no block framing


def _cabinet():
    c = ctypes.WinDLL('cabinet')
    for name in ('CreateCompressor', 'CreateDecompressor'):
        f = getattr(c, name)
        f.argtypes = [wintypes.DWORD, ctypes.c_void_p, ctypes.POINTER(ctypes.c_void_p)]
        f.restype = wintypes.BOOL
    for name in ('Compress', 'Decompress'):
        f = getattr(c, name)
        f.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p, ctypes.c_size_t,
                      ctypes.POINTER(ctypes.c_size_t)]
        f.restype = wintypes.BOOL
    return c


def pack(data: bytes) -> bytes:
    c = _cabinet()
    h = ctypes.c_void_p()
    if not c.CreateCompressor(LZMS, None, ctypes.byref(h)):
        raise OSError('CreateCompressor failed')
    out = ctypes.create_string_buffer(len(data) + 65536)
    n = ctypes.c_size_t()
    if not c.Compress(h, data, len(data), out, len(out), ctypes.byref(n)):
        raise OSError('Compress failed')
    c.CloseCompressor(h)
    return out.raw[:n.value]


def unpack(data: bytes, size: int) -> bytes:
    c = _cabinet()
    h = ctypes.c_void_p()
    if not c.CreateDecompressor(LZMS, None, ctypes.byref(h)):
        raise OSError('CreateDecompressor failed')
    out = ctypes.create_string_buffer(size)
    n = ctypes.c_size_t()
    if not c.Decompress(h, data, len(data), out, size, ctypes.byref(n)) or n.value != size:
        raise OSError('Decompress failed')
    c.CloseDecompressor(h)
    return out.raw


def make(src: bytes, dst: bytes) -> bytes:
    """Per target block, the source offset whose bytes differ least (the
    previous block's offset carried on, or one found through an index of
    8-byte grams), then the byte differences. Most blocks are the same code
    moved, so the differences are mostly zero and pack small."""
    index: dict[bytes, list[int]] = {}
    for k in range(0, len(src) - 8, 4):
        index.setdefault(src[k:k + 8], []).append(k)
    nblk = (len(dst) + BLOCK - 1) // BLOCK
    offs = []
    diff = bytearray(len(dst))
    prev = 0

    def score(off, t, blk):
        if off < 0 or off + len(blk) > len(src):
            return -1
        s = src[off:off + len(blk)]
        return sum(1 for i in range(len(blk)) if s[i] == blk[i])

    for b in range(nblk):
        t = b * BLOCK
        blk = dst[t:t + BLOCK]
        cands = {prev + BLOCK, t}
        for g in range(0, len(blk) - 8, 8):
            for k in index.get(blk[g:g + 8], ())[:8]:
                cands.add(k - g)
        best, bs = -1, 0
        for off in cands:
            s = score(off, t, blk)
            if s > bs:
                best, bs = off, s
            if s == len(blk):
                break
        offs.append(best)
        if best >= 0:
            prev = best
            for i in range(len(blk)):
                diff[t + i] = (blk[i] - src[best + i]) & 0xFF
        else:
            prev += BLOCK
            diff[t:t + len(blk)] = blk
    payload = struct.pack('<%di' % nblk, *offs) + bytes(diff)
    packed = pack(payload)
    head = MAGIC + struct.pack('<III', 1, BLOCK, len(src)) + hashlib.sha256(src).digest() + \
        struct.pack('<I', len(dst)) + hashlib.sha256(dst).digest() + struct.pack('<I', len(packed))
    return head + packed


def apply(patch: bytes, src: bytes) -> bytes:
    if patch[:4] != MAGIC:
        raise ValueError('not a CSTU patch')
    ver, block, ssize = struct.unpack_from('<III', patch, 4)
    ssha = patch[16:48]
    dsize, = struct.unpack_from('<I', patch, 48)
    dsha = patch[52:84]
    plen, = struct.unpack_from('<I', patch, 84)
    if ver != 1 or len(src) != ssize or hashlib.sha256(src).digest() != ssha:
        raise ValueError('source is not the disc default.xbe this patch is for')
    nblk = (dsize + block - 1) // block
    payload = unpack(patch[88:88 + plen], nblk * 4 + dsize)
    offs = struct.unpack_from('<%di' % nblk, payload, 0)
    diff = payload[nblk * 4:]
    out = bytearray(dsize)
    for b, off in enumerate(offs):
        t = b * block
        n = min(block, dsize - t)
        for i in range(n):
            out[t + i] = (diff[t + i] + (src[off + i] if off >= 0 else 0)) & 0xFF
    if hashlib.sha256(out).digest() != dsha:
        raise ValueError('patched file does not match the title update')
    return bytes(out)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('cmd', choices=('make', 'apply'))
    ap.add_argument('--out', type=Path, default=ROOT / 'installer' / 'Output' / 'cs_tu.xbepatch')
    ap.add_argument('--patch', type=Path, default=ROOT / 'installer' / 'Output' / 'cs_tu.xbepatch')
    ap.add_argument('--dir', type=Path, default=ROOT / 'game_files')
    a = ap.parse_args()
    if a.cmd == 'make':
        src = (a.dir / 'default.disc.xbe').read_bytes()
        dst = (a.dir / 'default.xbe').read_bytes()
        p = make(src, dst)
        if apply(p, src) != dst:
            raise SystemExit('round trip failed')
        a.out.parent.mkdir(parents=True, exist_ok=True)
        a.out.write_bytes(p)
        print(f'{a.out}: {len(p)} bytes')
    else:
        cur = (a.dir / 'default.xbe').read_bytes()
        out = apply(a.patch.read_bytes(), cur)
        (a.dir / 'default.disc.xbe').write_bytes(cur)
        (a.dir / 'default.xbe').write_bytes(out)
        print('default.xbe is now the title update')


if __name__ == '__main__':
    main()
