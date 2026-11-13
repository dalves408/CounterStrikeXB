"""Frame-time spikes in a window of a run's frames.csv.

  python scripts/spikes.py runs/<label> [from_s] [to_s] [threshold_ms]

Prints the frame-interval distribution (median, p95, p99, 1% low FPS) and
every frame over the threshold (default 25 ms) with its draws, readback time
and the gap to the previous spike, so periodic hitches stand out.
"""
import csv
import sys
from pathlib import Path

run = Path(sys.argv[1])
t0 = float(sys.argv[2]) * 1000 if len(sys.argv) > 2 else 100000
t1 = float(sys.argv[3]) * 1000 if len(sys.argv) > 3 else 148000
thr = float(sys.argv[4]) if len(sys.argv) > 4 else 25.0
rows = [r for r in csv.DictReader(open(run / 'frames.csv')) if t0 <= float(r['t_ms']) <= t1 and r['dt_ms']]
dts = sorted(float(r['dt_ms']) for r in rows)
if not dts:
    sys.exit('no frames in window')


def pct(p):
    return dts[min(len(dts) - 1, int(p * len(dts)))]


worst = dts[-max(1, len(dts) // 100):]
print('%d frames  median %.1f  p95 %.1f  p99 %.1f  max %.1f ms  1%%-low %.0f fps  avg %.0f fps' % (
    len(dts), pct(.5), pct(.95), pct(.99), dts[-1], 1000.0 / (sum(worst) / len(worst)),
    1000.0 * len(dts) / (sum(dts))))
last = None
n = 0
for r in rows:
    dt = float(r['dt_ms'])
    if dt >= thr:
        t = float(r['t_ms'])
        n += 1
        print('  t=%9.1f  dt=%6.1f  draws=%5s  rb=%5s  gap=%s' % (
            t, dt, r['draws'], r['rb_ms'], '%.0f ms' % (t - last) if last else '-'))
        last = t
print('%d frames over %.0f ms' % (n, thr))
