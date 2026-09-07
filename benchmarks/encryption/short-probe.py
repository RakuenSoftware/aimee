#!/usr/bin/env python3
"""Choose a complete small candidate list or the full ordered path in one snapshot."""
import json
import random
import time
from pathlib import Path
from measure import connect, query, fingerprint, summarize

ROOT = Path('/opt/aimee-encryption-bench')
conns = {mode: connect(str(ROOT/'sockets'/mode), 100000)
         for mode in ('ordinary', 'luks')}
rng = random.Random(253)
results = []
threshold = 512
probe = ('SELECT id FROM short_projection WHERE id <= %s '
         'AND grams @> %s::text[] LIMIT %s')
for term in ('qq', 'me', '#', 'm', '99'):
    for count in (10000, 100000):
        case = ('short_probe', 'page', term, None, True)
        sql, args = query('encrypted', case, count)
        full_sql = sql.replace('JOIN session_keys k USING(id)',
                               'JOIN session_keys k USING(id) JOIN short_projection s USING(id)')
        full_sql = full_sql.replace('WHERE e.id <= %s',
                                    'WHERE e.id <= %s AND s.grams @> %s::text[]')
        full_args = [args[0], [term], *args[1:]]
        small_sql = sql.replace('WHERE e.id <= %s',
                               'WHERE e.id <= %s AND e.id = ANY(%s::bigint[])')
        plain_sql, plain_args = query('plain', case, count)
        samples = {mode: [] for mode in conns}
        plans, paths = {}, {}
        reference = None
        for iteration in range(6):
            modes = list(conns)
            rng.shuffle(modes)
            for mode in modes:
                with conns[mode].cursor() as cur:
                    if iteration == 0:
                        cur.execute(plain_sql, plain_args)
                        expected = fingerprint(cur.fetchall())
                        if reference is None:
                            reference = expected
                        assert expected == reference
                    started = time.perf_counter()
                    cur.execute('BEGIN ISOLATION LEVEL REPEATABLE READ READ ONLY')
                    cur.execute(probe, (count, [term], threshold+1))
                    ids = [row[0] for row in cur.fetchall()]
                    if not ids:
                        rows = []
                        path = 'empty'
                    elif len(ids) <= threshold:
                        cur.execute(small_sql, [args[0], ids, *args[1:]])
                        rows = cur.fetchall()
                        path = 'complete_candidates'
                    else:
                        # A truncated probe only selects the strategy. Search all
                        # candidates on the ordered path, preserving result limits.
                        cur.execute(full_sql, full_args)
                        rows = cur.fetchall()
                        path = 'full_ordered'
                    cur.execute('COMMIT')
                    elapsed = (time.perf_counter()-started)*1000
                    assert fingerprint(rows) == reference
                    if iteration:
                        samples[mode].append(elapsed)
                    else:
                        paths[mode] = path
                        cur.execute('EXPLAIN (ANALYZE,BUFFERS,FORMAT JSON) '+probe,
                                    (count, [term], threshold+1))
                        plans[mode] = cur.fetchone()[0]
        for mode in conns:
            results.append(dict(storage=mode, term=term, eligible=count,
                                rows_returned=len(rows), result_sha256=reference,
                                latency=summarize(samples[mode]), path=paths[mode],
                                probe_plan=plans[mode], probe_sql=probe,
                                probe_parameters=[count, [term], threshold+1]))
        print(term, count, {m: round(summarize(t)['median_ms'], 3) for m, t in samples.items()}, flush=True)
(ROOT/'results'/'short-probe.json').write_text(json.dumps(results, indent=2)+'\n')
for conn in conns.values():
    conn.close()
