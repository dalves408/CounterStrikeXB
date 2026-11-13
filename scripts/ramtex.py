"""Decode a texture from a RAM image (xemu physical.ram, or a recomp RAM dump) to PNG.

  python scripts/ramtex.py <ram> <offset> <fmt> <w> <h> <out.png> [more: off fmt w h out ...]

fmt is the NV2A colour format byte: 0C DXT1, 0E DXT3, 0F DXT5,
06/07 A8R8G8B8/X8R8G8B8 (swizzled), 12 A8R8G8B8 linear. Offsets are physical
(the 0x80000000 bit is ignored).
"""
import struct
import sys
import zlib


def png(path, w, h, px):
    raw = b''.join(b'\0' + bytes(px[y * w * 4:(y + 1) * w * 4]) for y in range(h))
    def chunk(t, d):
        return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xFFFFFFFF)
    open(path, 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0))
                           + chunk(b'IDAT', zlib.compress(raw)) + chunk(b'IEND', b''))


def c565(c):
    return [((c >> 11) & 31) * 255 // 31, ((c >> 5) & 63) * 255 // 63, (c & 31) * 255 // 31]


def dxt(ram, off, w, h, kind):
    px = bytearray(w * h * 4)
    bs = 8 if kind == 1 else 16
    p = off
    for by in range(0, h, 4):
        for bx in range(0, w, 4):
            blk = ram[p:p + bs]; p += bs
            cb = blk[bs - 8:]
            c0, c1, bits = struct.unpack('<HHI', cb)
            a, b = c565(c0), c565(c1)
            if kind != 1 or c0 > c1:
                pal = [a + [255], b + [255], [(2 * a[i] + b[i]) // 3 for i in range(3)] + [255],
                       [(a[i] + 2 * b[i]) // 3 for i in range(3)] + [255]]
            else:
                pal = [a + [255], b + [255], [(a[i] + b[i]) // 2 for i in range(3)] + [255], [0, 0, 0, 0]]
            alpha = None
            if kind == 3:
                av = struct.unpack('<Q', blk[:8])[0]
                alpha = [((av >> (4 * i)) & 15) * 17 for i in range(16)]
            elif kind == 5:
                a0, a1 = blk[0], blk[1]
                av = int.from_bytes(blk[2:8], 'little')
                if a0 > a1:
                    ap = [a0, a1] + [((6 - i) * a0 + (1 + i) * a1) // 7 for i in range(6)]
                else:
                    ap = [a0, a1] + [((4 - i) * a0 + (1 + i) * a1) // 5 for i in range(4)] + [0, 255]
                alpha = [ap[(av >> (3 * i)) & 7] for i in range(16)]
            for i in range(16):
                x, y = bx + (i & 3), by + (i >> 2)
                if x < w and y < h:
                    c = list(pal[(bits >> (2 * i)) & 3])
                    if alpha:
                        c[3] = alpha[i]
                    q = (y * w + x) * 4
                    px[q:q + 4] = bytes(c)
    return px


def unswz(ram, off, w, h, bpp):
    px = bytearray(w * h * 4)
    for y in range(h):
        for x in range(w):
            # Morton order over the shorter side, linear beyond it
            i = 0; bit = 1; sx, sy = x, y; m = min(w, h)
            j = 0
            while (1 << j) < m:
                i |= ((sx >> j) & 1) << (2 * j) | ((sy >> j) & 1) << (2 * j + 1)
                j += 1
            rest = (x >> j) if w > h else (y >> j)
            i |= rest << (2 * j)
            b, g, r, a = ram[off + i * 4:off + i * 4 + 4]
            q = (y * w + x) * 4
            px[q:q + 4] = bytes([r, g, b, a])
    return px


ram = open(sys.argv[1], 'rb').read()
args = sys.argv[2:]
while args:
    off, fmt, w, h, out = int(args[0], 16) & 0x7FFFFFFF, int(args[1], 16), int(args[2]), int(args[3]), args[4]
    args = args[5:]
    if fmt == 0x0C:
        px = dxt(ram, off, w, h, 1)
    elif fmt == 0x0E:
        px = dxt(ram, off, w, h, 3)
    elif fmt == 0x0F:
        px = dxt(ram, off, w, h, 5)
    elif fmt in (0x06, 0x07):
        px = unswz(ram, off, w, h, 4)
    else:
        px = bytearray(w * h * 4)
        for i in range(w * h):
            b, g, r, a = ram[off + i * 4:off + i * 4 + 4]
            px[i * 4:i * 4 + 4] = bytes([r, g, b, a])
    png(out, w, h, px)
    print(out)
