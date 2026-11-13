"""Summarise a RECOMP_CS_PROF_CSV sample file: the busiest threads and where they spend time.

  python scripts/prof_summary.py runs/<label>/samples.csv [top_threads]

"Busy" excludes samples whose host leaf is a wait/sleep. For each busy thread it
lists the top (innermost guest function, host leaf) pairs as a share of all of
that thread's samples.
"""
import collections
import csv
import sys

IDLE = ('Wait', 'Delay', 'Alert', 'Remove', 'SleepEx')
S = list(csv.DictReader(open(sys.argv[1])))
top = int(sys.argv[2]) if len(sys.argv) > 2 else 6
by = collections.Counter(r['tid'] for r in S)
busy = collections.Counter(r['tid'] for r in S if not any(w in r['leaf'] for w in IDLE))
for tid, n in busy.most_common(top):
    c = collections.Counter((r['guest'] or '-', r['leaf']) for r in S
                            if r['tid'] == tid and not any(w in r['leaf'] for w in IDLE))
    print('tid %s busy %.0f%% of %d samples' % (tid, 100.0 * n / by[tid], by[tid]))
    for k, v in c.most_common(10):
        print('   %5.1f%%  %s  [%s]' % (100.0 * v / by[tid], k[0], k[1]))
