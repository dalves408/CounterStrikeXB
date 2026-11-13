"""What threads were doing during frame spikes, against normal frames.

  python scripts/spike_prof.py runs/<label> [threshold_ms] [from_s] [to_s]

Needs frames.csv and samples.csv (RECOMP_CS_PROF + RECOMP_CS_PROF_CSV) on the
same clock. For each thread with many samples, prints the functions whose
share of samples rises most inside spike frames (dt >= threshold) compared
with the other frames in the window, and any io.csv reads inside spikes.
"""
import bisect
import collections
import csv
import sys
from pathlib import Path

run = Path(sys.argv[1])
thr = float(sys.argv[2]) if len(sys.argv) > 2 else 30.0
t0 = float(sys.argv[3]) * 1000 if len(sys.argv) > 3 else 0
t1 = float(sys.argv[4]) * 1000 if len(sys.argv) > 4 else 1e12
fr = [r for r in csv.DictReader(open(run / 'frames.csv')) if r['dt_ms']]
ends = [float(r['t_ms']) for r in fr]
dts = [float(r['dt_ms']) for r in fr]
S = [r for r in csv.DictReader(open(run / 'samples.csv')) if t0 <= float(r['t_ms']) <= t1]


def in_spike(t):
    i = bisect.bisect_left(ends, t)
    if i >= len(ends):
        return None
    return dts[i] >= thr and ends[i] - dts[i] <= t


by = collections.defaultdict(lambda: [collections.Counter(), collections.Counter()])
for r in S:
    s = in_spike(float(r['t_ms']))
    if s is None:
        continue
    key = (r['guest'] or '-') + ' [' + r['leaf'] + ']'
    by[r['tid']][1 if s else 0][key] += 1
tids = sorted(by, key=lambda t: -sum(by[t][0].values()) - sum(by[t][1].values()))[:4]
for t in tids:
    norm, spk = by[t]
    nn, ns = sum(norm.values()) or 1, sum(spk.values()) or 1
    print('tid %s: %d samples in spikes, %d outside' % (t, ns, nn))
    rows = []
    for k in set(norm) | set(spk):
        rows.append((spk[k] / ns - norm[k] / nn, k))
    rows.sort(reverse=True)
    for d, k in rows[:8]:
        print('   %+6.1f%%  (spike %5.1f%%, normal %5.1f%%)  %s' % (100 * d, 100 * spk[k] / ns, 100 * norm[k] / nn, k))
io = run / 'io.csv'
if io.exists():
    rows = list(csv.DictReader(open(io)))
    hits = [r for r in rows if 't_ms' in r and in_spike(float(r['t_ms']))]
    print('io reads inside spikes: %d of %d' % (len(hits), len(rows)))
    for r in hits[:10]:
        print('   ', dict(r))
