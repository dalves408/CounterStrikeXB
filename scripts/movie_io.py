"""Line up intro-movie hitches with the movie's file reads.

  python scripts/movie_io.py runs/<label>

Needs pvideo.csv (RECOMP_CS_PVIDEO_CSV) and io.csv (RECOMP_CS_IO_CSV, src/cs_kernel.c).
For every gap > 120 ms between movie frames, lists the reads in the 400 ms before
and during it: (ms relative to the gap start, length, duration).
"""
import csv
import os
import sys

run = sys.argv[1]
pv = [float(r['t_ms']) for r in csv.DictReader(open(os.path.join(run, 'pvideo.csv'))) if r['reg'] == 'BUFFER']
io = list(csv.DictReader(open(os.path.join(run, 'io.csv'))))
gaps = [(pv[i - 1], pv[i]) for i in range(1, len(pv)) if pv[i] - pv[i - 1] > 120]
print(f'{len(pv)} movie frames, {len(io)} reads, {len(gaps)} gaps > 120 ms')
by_handle = {}
for r in io:
    by_handle.setdefault(r['handle'], []).append(float(r['t_ms']))
for h, ts in by_handle.items():
    if len(ts) > 20:
        iv = sorted(ts[i] - ts[i - 1] for i in range(1, len(ts)))
        print(f'  handle {h}: {len(ts)} reads, {ts[0]/1000:.1f}-{ts[-1]/1000:.1f} s, median interval {iv[len(iv)//2]:.0f} ms')
for a, b in gaps:
    rs = [r for r in io if a - 400 < float(r['t_ms']) < b]
    print(f'gap {a/1000:6.2f}s +{b-a:4.0f} ms: ' +
          ', '.join(f"{float(r['t_ms'])-a:+.0f}ms {int(r['length'])//1024}K {float(r['dur_ms']):.1f}ms" for r in rs))
