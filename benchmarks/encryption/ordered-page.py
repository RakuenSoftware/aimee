#!/usr/bin/env python3
"""Compare metadata-ordered first pages with matching evaluation barriers."""
import json
import random
import time
from pathlib import Path
from measure import connect, fingerprint, query, summarize, CASES

ROOT = Path('/opt/aimee-encryption-bench')
conns = {mode: connect(str(ROOT/'sockets'/mode), 100000)
         for mode in ('ordinary', 'luks')}
rng = random.Random(253)
results = []
for count in (10000, 100000):
    arms = [(mode, table) for mode in conns for table in ('plain', 'encrypted')]
    times = {arm: [] for arm in arms}
    plans = {}
    reference = None
    for iteration in range(6):
        rng.shuffle(arms)
        for mode, table in arms:
            sql, args = query(table, CASES[2], count)
            if table == 'plain':
                sql = ('SELECT e.id,d.body FROM plain e CROSS JOIN LATERAL '
                       '(SELECT e.body OFFSET 0) d WHERE e.id <= %s '
                       'AND lower(d.body) LIKE %s ORDER BY e.id LIMIT 20')
            with conns[mode].cursor() as cur:
                start = time.perf_counter()
                cur.execute(sql, args)
                rows = cur.fetchall()
                elapsed = (time.perf_counter()-start)*1000
                found = fingerprint(rows)
                if reference is None:
                    reference = found
                assert found == reference
                if iteration:
                    times[(mode, table)].append(elapsed)
                else:
                    cur.execute('EXPLAIN (ANALYZE,BUFFERS,FORMAT JSON) '+sql, args)
                    plans[(mode, table)] = cur.fetchone()[0]
    for (mode, table), samples in times.items():
        results.append({'storage': mode, 'payload': table, 'eligible': count,
                        'latency': summarize(samples), 'rows_returned': len(rows),
                        'result_sha256': reference, 'plan': plans[(mode, table)]})
(ROOT/'results'/'ordered-page.json').write_text(json.dumps(results, indent=2)+'\n')
for conn in conns.values():
    conn.close()
print('Metadata-ordered page comparisons passed')
