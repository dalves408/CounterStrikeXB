"""Analyse a run's frame and movie timing logs.

  python scripts/analyze_frames.py runs/<label>

Reads (written by the game when the env vars are set, e.g. by probe.ps1 -ExtraEnv):
  frames.csv  RECOMP_CS_FRAME_CSV  one line per guest flip (src/cs_d3d11.c)
  pvideo.csv  RECOMP_CS_PVIDEO_CSV one line per NV_PVIDEO_BUFFER/STOP write (src/cs_gpu.c)
Reports: per-second guest FPS, stalls, single-frame flashes (a frame whose
brightness or black share differs sharply from both neighbours while those two
agree), intro-movie cadence, and boot/load milestones. Clock: ms since the back
end registered (a fraction of a second after process start).
"""
import csv
import os
import statistics as st
import sys


def load(path):
    if not os.path.exists(path):
        return []
    with open(path, newline='') as f:
        return list(csv.DictReader(f))


def fnum(v, d=None):
    try:
        return float(v)
    except (TypeError, ValueError):
        return d


def pct(xs, p):
    if not xs:
        return 0.0
    xs = sorted(xs)
    k = min(len(xs) - 1, max(0, int(round(p / 100.0 * (len(xs) - 1)))))
    return xs[k]


def segments(frames):
    """Group frames into phases by draws-per-frame (movie: ~1-2 draws, menu: many)."""
    out, cur = [], None
    for r in frames:
        kind = 'movie/blank' if r['draws'] <= 2 else 'scene'
        if not cur or cur['kind'] != kind or r['t'] - cur['end'] > 1500:
            cur = {'kind': kind, 'start': r['t'], 'end': r['t'], 'n': 0, 'draws': []}
            out.append(cur)
        cur['end'] = r['t']
        cur['n'] += 1
        cur['draws'].append(r['draws'])
    return [s for s in out if s['n'] >= 5]


def main():
    run = sys.argv[1] if len(sys.argv) > 1 else '.'
    raw = load(os.path.join(run, 'frames.csv'))
    frames = []
    for r in raw:
        frames.append({'t': fnum(r['t_ms'], 0), 'frame': int(r['frame']), 'dt': fnum(r['dt_ms'], 0),
                       'draws': int(r['draws']), 'tris': int(r['tris']), 'rb': fnum(r['rb_ms'], 0),
                       'luma': fnum(r['luma']), 'black': fnum(r['black_pct']), 'hash': r['hash']})
    pvrows = load(os.path.join(run, 'pvideo.csv'))
    pv = [(fnum(r['t_ms'], 0), r['reg']) for r in pvrows]
    # A STOP write only stops the overlay when it bumps the stop generation (bit 0 set);
    # D3D also writes STOP=0 before every frame. Older logs have no stop_gen column.
    real_stops, last_gen = [], None
    for r in pvrows:
        g = r.get('stop_gen')
        if r['reg'] == 'STOP' and g is not None:
            if last_gen is not None and g != last_gen:
                real_stops.append(fnum(r['t_ms'], 0))
            last_gen = g

    print(f'== {run}: {len(frames)} flips, {len(pv)} PVIDEO writes')
    if not frames:
        return

    # Milestones
    print('\n-- phases (by draws per frame) --')
    for s in segments(frames):
        dur = (s['end'] - s['start']) / 1000.0
        fps = (s['n'] - 1) / dur if dur > 0 else 0
        print(f"  {s['start']/1000:7.1f}s - {s['end']/1000:7.1f}s  {s['kind']:12s} {s['n']:5d} flips  "
              f"{fps:5.1f} fps  draws/frame median {st.median(s['draws']):.0f}")

    # Per-second FPS
    print('\n-- guest flips per second (only seconds with flips) --')
    buckets = {}
    for r in frames:
        buckets.setdefault(int(r['t'] // 1000), []).append(r)
    line = []
    for sec in sorted(buckets):
        b = buckets[sec]
        line.append(f"{sec}:{len(b)}")
    for i in range(0, len(line), 12):
        print('  ' + '  '.join(line[i:i + 12]))

    # Stalls
    dts = [r['dt'] for r in frames[1:]]
    print(f"\n-- frame interval: median {st.median(dts):.1f} ms, p95 {pct(dts, 95):.1f}, p99 {pct(dts, 99):.1f}, max {max(dts):.0f}")
    stalls = [r for r in frames[1:] if r['dt'] > 100]
    print(f'-- stalls > 100 ms: {len(stalls)}')
    for r in stalls[:40]:
        print(f"  at {r['t']/1000:7.2f}s  {r['dt']:8.0f} ms  (frame {r['frame']}, draws {r['draws']})")

    # Flashes
    print('\n-- flash candidates (single-frame luma/black outliers) --')
    nflash = 0
    sig = [r for r in frames if r['luma'] is not None]
    for i in range(1, len(sig) - 1):
        a, b, c = sig[i - 1], sig[i], sig[i + 1]
        dl = min(abs(b['luma'] - a['luma']), abs(b['luma'] - c['luma']))
        db = min(abs(b['black'] - a['black']), abs(b['black'] - c['black']))
        neigh_agree = abs(a['luma'] - c['luma']) < 8 and abs(a['black'] - c['black']) < 10
        if neigh_agree and (dl > 25 or db > 30):
            nflash += 1
            if nflash <= 30:
                print(f"  at {b['t']/1000:7.2f}s frame {b['frame']}: luma {a['luma']:.0f} -> {b['luma']:.0f} -> {c['luma']:.0f}, "
                      f"black {a['black']:.0f}% -> {b['black']:.0f}% -> {c['black']:.0f}%, draws {a['draws']}/{b['draws']}/{c['draws']}")
    print(f'  total: {nflash}')

    # Repeated-image share during scenes
    sc = [r for r in sig if r['draws'] > 2]
    if len(sc) > 1:
        rep = sum(1 for i in range(1, len(sc)) if sc[i]['hash'] == sc[i - 1]['hash'])
        print(f"\n-- scene frames identical to the previous one: {rep}/{len(sc) - 1} ({100.0 * rep / (len(sc) - 1):.0f}%)")

    # Readback cost
    rbs = [r['rb'] for r in frames]
    print(f"-- present_sync (GPU finish + readback): median {st.median(rbs):.2f} ms, p95 {pct(rbs, 95):.2f} ms")

    # Movie cadence
    buf = [t for t, reg in pv if reg == 'BUFFER']
    stops = real_stops if pvrows and 'stop_gen' in pvrows[0] else [t for t, reg in pv if reg == 'STOP']
    if buf:
        print(f'\n-- PVIDEO: {len(buf)} BUFFER writes, {len(stops)} STOP writes')
        # movies separated by STOP or gaps > 2 s
        movies, cur = [], [buf[0]]
        for t in buf[1:]:
            if t - cur[-1] > 2000 or any(cur[-1] < s < t for s in stops):
                movies.append(cur)
                cur = [t]
            else:
                cur.append(t)
        movies.append(cur)
        for m in movies:
            if len(m) < 3:
                continue
            iv = [m[i] - m[i - 1] for i in range(1, len(m))]
            med = st.median(iv)
            late = sum(1 for x in iv if x > 1.5 * med)
            dur = (m[-1] - m[0]) / 1000.0
            print(f"  movie {m[0]/1000:6.1f}s - {m[-1]/1000:6.1f}s: {len(m)} frames, {len(m)/dur if dur else 0:.1f} fps, "
                  f"interval median {med:.1f} ms p95 {pct(iv, 95):.1f} max {max(iv):.0f}, late (>1.5x) {late}")


if __name__ == '__main__':
    main()
