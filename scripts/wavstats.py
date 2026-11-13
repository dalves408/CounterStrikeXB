"""Loudness profile of an audio capture (RECOMP_AUDIO_WAV), for comparing runs.

  python scripts/wavstats.py runs/a/audio.wav [runs/b/audio.wav ...]

Per file: duration, overall RMS (dBFS), share of near-silent seconds, clipped
samples, and the RMS of each 10 s block -- enough to see missing sound, a stuck
voice (a block that never goes quiet) or distortion, without listening.
"""
import math
import struct
import sys
import wave


def stats(path):
    w = wave.open(path, 'rb')
    ch, sw, rate, n = w.getnchannels(), w.getsampwidth(), w.getframerate(), w.getnframes()
    if sw != 2:
        return '%s: %d-bit not handled' % (path, sw * 8)
    blocks, silent, clipped, total_sq, count = [], 0, 0, 0.0, 0
    sec = rate
    blk_sq, blk_n = 0.0, 0
    s_idx = 0
    while True:
        data = w.readframes(sec)
        if not data:
            break
        v = struct.unpack('<%dh' % (len(data) // 2), data)
        sq = sum(x * x for x in v)
        clipped += sum(1 for x in v if x >= 32767 or x <= -32768)
        rms = math.sqrt(sq / max(1, len(v)))
        if rms < 30:
            silent += 1
        total_sq += sq
        count += len(v)
        blk_sq += sq
        blk_n += len(v)
        s_idx += 1
        if s_idx % 10 == 0:
            blocks.append(math.sqrt(blk_sq / max(1, blk_n)))
            blk_sq, blk_n = 0.0, 0

    def db(r):
        return 20 * math.log10(r / 32768.0) if r > 0 else -120.0

    return ('%s: %d ch %d Hz %.0f s, RMS %.1f dBFS, silent seconds %d (%.0f%%), clipped samples %d\n'
            '   10 s blocks (dBFS): %s' % (
                path, ch, rate, n / rate, db(math.sqrt(total_sq / max(1, count))), silent,
                100.0 * silent / max(1, s_idx), clipped, ' '.join('%.0f' % db(b) for b in blocks)))


for p in sys.argv[1:]:
    print(stats(p))
