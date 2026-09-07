#!/usr/bin/env python3
"""Measure one/two-character projections after the original comparison finishes."""
import csv
import io
import json
import random
import time
from pathlib import Path
from measure import connect, fixture_rows, pg_array, query, CASES, fingerprint, summarize

ROOT = Path('/opt/aimee-encryption-bench')
conns = {mode: connect(str(ROOT/'sockets'/mode), 100000)
         for mode in ('ordinary', 'luks')}
setup = {}
for mode, conn in conns.items():
    with conn.cursor() as cur:
        cur.execute('CREATE TABLE short_projection(id bigint PRIMARY KEY, grams text[] NOT NULL)')
        started = time.perf_counter()
        buf = io.StringIO()
        writer = csv.writer(buf)
        for i, _, body, _ in fixture_rows(100000, 4096):
            grams = sorted({body[j:j+n] for n in (1, 2) for j in range(len(body)-n+1)})
            writer.writerow((i, pg_array(grams)))
            if i % 1000 == 0:
                buf.seek(0)
                cur.copy_expert('COPY short_projection FROM STDIN WITH CSV', buf)
                buf.seek(0)
                buf.truncate(0)
        load_seconds = time.perf_counter()-started
        started = time.perf_counter()
        cur.execute('CREATE INDEX short_grams ON short_projection USING gin(grams)')
        index_seconds = time.perf_counter()-started
        cur.execute('ANALYZE short_projection; CHECKPOINT')
        cur.execute("SELECT pg_total_relation_size('short_projection'),pg_indexes_size('short_projection')")
        total, indexes = cur.fetchone()
        setup[mode] = dict(load_seconds=load_seconds, index_seconds=index_seconds,
                           total_bytes=total, index_bytes=indexes)

rng = random.Random(253)
results = []
for term in ('qq', 'me', '#', 'm'):
    for count in (10000, 100000):
        case = ('short_projection', 'page', term, None, True)
        sql, args = query('encrypted', case, count)
        sql = sql.replace('JOIN session_keys k USING(id)',
                          'JOIN session_keys k USING(id) JOIN short_projection s USING(id)')
        sql = sql.replace('WHERE e.id <= %s', 'WHERE e.id <= %s AND s.grams @> %s::text[]')
        args.insert(1, [term])
        plain_sql, plain_args = query('plain', case, count)
        reference = None
        samples = {mode: [] for mode in conns}
        plans = {}
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
                    cur.execute(sql, args)
                    rows = cur.fetchall()
                    elapsed = (time.perf_counter()-started)*1000
                    assert fingerprint(rows) == reference
                    if iteration:
                        samples[mode].append(elapsed)
                    else:
                        cur.execute('EXPLAIN (ANALYZE,BUFFERS,FORMAT JSON) '+sql, args)
                        plans[mode] = cur.fetchone()[0]
        for mode in conns:
            results.append(dict(storage=mode, term=term, eligible=count,
                                rows_returned=len(rows), result_sha256=reference,
                                latency=summarize(samples[mode]), plan=plans[mode],
                                sql=sql, parameters=args))
        print(term, count, {m: round(summarize(t)['median_ms'], 3) for m, t in samples.items()}, flush=True)
(ROOT/'results'/'short-fragments.json').write_text(json.dumps(
    dict(setup=setup, measurements=results), indent=2)+'\n')
for conn in conns.values():
    conn.close()
