"""Receipt-time timeline for log lines matching patterns (stream-events.jsonl maps byte offsets to receipt_ns).

Usage: run_timeline.py <run_dir> <regex> [<regex> ...]
Receipt times are collector receipt times, not producer timestamps.
"""
import bisect, json, re, sys
from pathlib import Path

run = Path(sys.argv[1])
patterns = [re.compile(p) for p in sys.argv[2:]]
events = {'stdout': ([], []), 'stderr': ([], [])}
with (run / 'stream-events.jsonl').open('r', encoding='utf-8') as f:
    for line in f:
        o = json.loads(line)
        s = o.get('stream')
        if s in events:
            events[s][0].append(o['offset'])
            events[s][1].append(o['receipt_ns'])
rows = []
for stream in ('stdout', 'stderr'):
    offsets, times = events[stream]
    pos = 0
    with (run / f'{stream}.log').open('rb') as f:
        for raw in f:
            text = raw.decode('utf-8', 'replace').rstrip()
            for p in patterns:
                if p.search(text):
                    i = bisect.bisect_right(offsets, pos) - 1
                    t = times[i] / 1e9 if i >= 0 else float('nan')
                    rows.append((t, stream, text[:170]))
                    break
            pos += len(raw)
rows.sort()
for t, stream, text in rows:
    print(f'{t:8.1f}s {stream:6} {text}')
