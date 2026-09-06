#!/usr/bin/env python3
"""Measure real PostgreSQL queries in disposable Docker databases.

Never point this harness at a production database. init refuses existing tables.
Session key fixtures exclude Vault service/network costs from SQL measurements.
"""
import argparse
import concurrent.futures
import csv
import hashlib
import io
import json
import math
import random
import statistics
import time
from pathlib import Path

import psycopg2
from psycopg2.extras import execute_values

OPTIONS = 'cipher-algo=aes256,disable-mdc=0,compress-algo=0,s2k-mode=3,s2k-count=65536'
WORDS = ('memory context document project workspace search database query source '
         'function result content record index vector token lexical reference '
         'scope version update transaction request response permission module '
         'test cache runtime process storage service configuration array value '
         'return integer string parser compiler branch checkout commit file '
         'read write open close match filter order count code user server client').split()
CASES = [
    ('lexical_rare', 'lexical', 'needleuniqueterm', None, True),
    ('substring_rare', 'page', 'needleuniqueterm', None, True),
    ('substring_broad', 'page', 'context', None, True),
    ('short_common', 'page', 'me', None, True),
    ('short_absent', 'page', 'qq', None, True),
    ('short_absent_project', 'page', 'qq', 7, True),
    ('rank_broad', 'rank', 'context', None, True),
    ('rare_without_grams', 'page', 'needleuniqueterm', None, False),
]


def key(i):
    return hashlib.sha256(f'aimee-encryption-benchmark:{i}'.encode()).hexdigest()


def fragments(s):
    return sorted({s[i:i+3] for i in range(len(s)-2)})


def pg_array(values):
    return '{' + ','.join('"' + s.replace('\\', '\\\\').replace('"', '\\"') + '"'
                          for s in values) + '}'


def fixture_rows(count, width):
    rng = random.Random(400253)
    templates = [' '.join(rng.choices(WORDS, k=900)) for _ in range(128)]
    for i in range(1, count + 1):
        marker = 'needleuniqueterm' if i % 5000 == 0 else 'ordinaryentry'
        # Include guaranteed gram false positives without containing the rare term.
        if i % 5000 == 1:
            marker = 'nee eed edl dle leu eun uni niq iqu que uet ete ter erm'
        prefix = f'memory {i:07d} context {marker}. path::file_name = value; '
        body = (prefix + templates[i % len(templates)])[:width]
        yield i, (i - 1) % 100, body, fragments(body.lower())


def connect(socket_dir, keys=0):
    conn = psycopg2.connect(host=socket_dir, dbname='bench', user='postgres',
                            application_name='aimee-encryption-benchmark')
    conn.autocommit = True
    with conn.cursor() as cur:
        cur.execute("SET jit=off; SET max_parallel_workers_per_gather=0; "
                    "SET work_mem='64MB'; SET temp_buffers='64MB'; SET statement_timeout='180s'")
        if keys:
            cur.execute('CREATE TEMP TABLE session_keys(id bigint PRIMARY KEY, secret text)')
            buf = io.StringIO(''.join(f'{i}\t{key(i)}\n' for i in range(1, keys+1)))
            cur.copy_from(buf, 'session_keys')
            cur.execute('ANALYZE session_keys')
    return conn


def initialize(conn, count, width):
    timings = {}
    cur = conn.cursor()
    cur.execute(Path(__file__).with_name('schema.sql').read_text())
    started = time.perf_counter()
    digest = hashlib.sha256()
    buf = io.StringIO()
    writer = csv.writer(buf)
    for row in fixture_rows(count, width):
        digest.update(f'{row[0]}:{row[1]}:'.encode() + row[2].encode() + b'\n')
        writer.writerow((*row[:3], pg_array(row[3])))
        if row[0] % 1000 == 0 or row[0] == count:
            buf.seek(0)
            cur.copy_expert('COPY fixture FROM STDIN WITH CSV', buf)
            buf.seek(0); buf.truncate(0)
    timings['fixture_seconds'] = time.perf_counter()-started
    operations = [
        ('plain_load', "INSERT INTO plain SELECT id,project,body,to_tsvector('simple',body) FROM fixture"),
        ('encrypted_load', "INSERT INTO encrypted SELECT id,project,"
         "pgp_sym_encrypt_bytea(int8send(id)||convert_to(body,'UTF8'),fixture_key(id),%s),"
         "grams,to_tsvector('simple',body) FROM fixture"),
        ('plain_indexes', "CREATE INDEX plain_project ON plain(project,id); "
         "CREATE INDEX plain_trgm ON plain USING gin(lower(body) gin_trgm_ops); "
         "CREATE INDEX plain_lexical ON plain USING gin(lexical)"),
        ('encrypted_indexes', "CREATE INDEX encrypted_project ON encrypted(project,id); "
         "CREATE INDEX encrypted_grams ON encrypted USING gin(grams); "
         "CREATE INDEX encrypted_lexical ON encrypted USING gin(lexical)"),
    ]
    for label, sql in operations:
        started = time.perf_counter()
        cur.execute(sql, (OPTIONS,) if label == 'encrypted_load' else None)
        timings[label+'_seconds'] = time.perf_counter()-started
        print(label, round(timings[label+'_seconds'], 3), flush=True)
    cur.execute('ANALYZE plain; ANALYZE encrypted; CHECKPOINT')
    cur.execute("SELECT count(*) FROM plain p JOIN encrypted e USING(id) "
                "WHERE p.body IS DISTINCT FROM read_body(e.ciphertext,fixture_key(e.id),e.id)")
    assert cur.fetchone()[0] == 0, 'Payload roundtrip mismatch'
    cur.execute('DROP TABLE fixture; CHECKPOINT')
    return {'rows': count, 'body_bytes': width, 'fixture_sha256': digest.hexdigest(),
            'pgp_options': OPTIONS, 'timings': timings, 'roundtrip': 'passed'}


def query(table, case, count):
    _, kind, term, project, indexed = case
    encrypted = table == 'encrypted'
    source = f'{table} e' + (' JOIN session_keys k USING(id)' if encrypted else '')
    body = 'read_body(e.ciphertext,k.secret,e.id)' if encrypted else 'e.body'
    where = ['e.id <= %s']; args = [count]
    if project is not None:
        where.append('e.project=%s'); args.append(project)
    if kind == 'lexical':
        sql = (f'SELECT e.id,{body} FROM {source} WHERE ' + ' AND '.join(where) +
               " AND e.lexical @@ plainto_tsquery('simple',%s) "
               "ORDER BY ts_rank_cd(e.lexical,plainto_tsquery('simple',%s)) DESC,e.id LIMIT 20")
        return sql, args+[term, term]
    needed = fragments(term.lower()) if indexed else []
    if encrypted and needed:
        where.append('e.grams @> %s::text[]'); args.append(needed)
    pattern = '%' + term.replace('\\', '\\\\').replace('%', '\\%').replace('_', '\\_') + '%'
    if kind == 'rank':
        # Materialize once because both exact matching and ranking consume the body.
        sql = (f'WITH d AS MATERIALIZED (SELECT e.id,{body} AS body FROM {source} WHERE ' +
               ' AND '.join(where) + ') SELECT id,body FROM d WHERE lower(body) LIKE %s '
               'ORDER BY similarity(body,%s) DESC,id LIMIT 20')
        return sql, args+[pattern, term]
    if not encrypted:
        return (f'SELECT e.id,e.body FROM {source} WHERE ' + ' AND '.join(where) +
                ' AND lower(e.body) LIKE %s ORDER BY e.id LIMIT 20'), args+[pattern]
    # Preserve metadata ordering and permit PostgreSQL to stop after 20 exact hits.
    # OFFSET 0 prevents flattening the lateral body expression into two decrypt calls.
    sql = (f'SELECT e.id,d.body FROM {source} CROSS JOIN LATERAL '
           f'(SELECT {body} AS body OFFSET 0) d WHERE ' + ' AND '.join(where) +
           ' AND lower(d.body) LIKE %s ORDER BY e.id LIMIT 20')
    return sql, args+[pattern]


def summarize(samples):
    ordered = sorted(samples)
    return {'samples_ms': samples, 'median_ms': statistics.median(samples),
            'p95_ms': ordered[max(0, math.ceil(.95*len(ordered))-1)],
            'min_ms': ordered[0], 'max_ms': ordered[-1]}


def fingerprint(rows):
    return hashlib.sha256(json.dumps(rows, ensure_ascii=False).encode()).hexdigest()


def metadata(conn):
    with conn.cursor() as cur:
        cur.execute('SELECT version()'); version = cur.fetchone()[0]
        cur.execute("SELECT name,setting,unit FROM pg_settings WHERE name IN "
                    "('shared_buffers','work_mem','max_parallel_workers_per_gather','jit',"
                    "'fsync','synchronous_commit','full_page_writes','data_checksums',"
                    "'default_toast_compression','server_encoding','lc_collate','lc_ctype')")
        settings = cur.fetchall()
        cur.execute("SELECT relname,pg_relation_size(oid),pg_indexes_size(oid),"
                    "pg_total_relation_size(oid) FROM pg_class WHERE relname IN ('plain','encrypted')")
        sizes = cur.fetchall()
    return {'postgres': version, 'settings': settings, 'relation_sizes': sizes}


def measure(connections, counts, repeats, out):
    result = {'metadata': {name: metadata(conn) for name, conn in connections.items()},
              'warm': [], 'parity': []}
    rng = random.Random(253)
    for count in counts:
        for case in CASES:
            arms = [(storage, table) for storage in connections for table in ('plain','encrypted')]
            sqls = {table: query(table, case, count) for table in ('plain','encrypted')}
            reference = None
            samples = {arm: [] for arm in arms}
            plans = {}
            for iteration in range(repeats+1):
                rng.shuffle(arms)
                for storage, table in arms:
                    cur = connections[storage].cursor()
                    sql, args = sqls[table]
                    started = time.perf_counter()
                    cur.execute(sql,args); rows = cur.fetchall()
                    elapsed = (time.perf_counter()-started)*1000
                    found = fingerprint(rows)
                    if reference is None: reference = found
                    assert found == reference, (count,case[0],storage,table,'result parity')
                    if iteration:
                        samples[(storage,table)].append(elapsed)
                    else:
                        cur.execute('EXPLAIN (ANALYZE,BUFFERS,FORMAT JSON) '+sql,args)
                        plans[(storage,table)] = cur.fetchone()[0]
                    cur.close()
                print(json.dumps({'case':case[0], 'eligible':count,'iteration':iteration,
                                  'last_ms':round(elapsed,2)}),flush=True)
            for storage,table in arms:
                result['warm'].append({'storage':storage,'payload':table,'eligible':count,
                    'case':case[0], 'rows_returned':len(rows), 'result_sha256':reference,
                    'latency':summarize(samples[(storage,table)]),
                    'sql':sqls[table][0], 'parameters':sqls[table][1],
                    'plan':plans[(storage,table)]})
            result['parity'].append({'eligible':count,'case':case[0],'passed':True})
            result['last_host_loadavg'] = Path('/proc/loadavg').read_text().strip()
            out.write_text(json.dumps(result,indent=2)+'\n')
    return result


def concurrent_measure(sockets, count, workers, repeats):
    result=[]
    for case in [CASES[1],CASES[4],CASES[5]]:
        for storage,socket_dir in sockets.items():
            for table in ('plain','encrypted'):
                # Preload synthetic keys before timing; each worker has its own connection.
                conns=[connect(socket_dir,count) for _ in range(workers)]
                sql,args=query(table,case,count)
                def worker(conn):
                    times=[]; last=None
                    with conn.cursor() as cur:
                        for _ in range(repeats):
                            start=time.perf_counter(); cur.execute(sql,args); rows=cur.fetchall()
                            times.append((time.perf_counter()-start)*1000); last=fingerprint(rows)
                    return times,last
                start=time.perf_counter()
                with concurrent.futures.ThreadPoolExecutor(max_workers=workers) as pool:
                    data=list(pool.map(worker,conns))
                seconds=time.perf_counter()-start
                assert len({r[1] for r in data})==1
                result.append({'case':case[0],'storage':storage,'payload':table,'eligible':count,
                    'workers':workers,'operations':workers*repeats,'elapsed_seconds':seconds,
                    'qps':workers*repeats/seconds,'latency':summarize([t for ts,_ in data for t in ts])})
                for conn in conns: conn.close()
                print(json.dumps(result[-1]),flush=True)
    return result


def write_measure(connections, count, width, repeats):
    result=[]
    batch_size=100
    batches=list(fixture_rows(batch_size*(repeats+1),width))
    rng=random.Random(253)
    arms=[(storage,table) for storage in connections for table in ('plain','encrypted')]
    times={arm:[] for arm in arms}
    for iteration in range(repeats+1):
        rows=[(count+i,p,b,g) for i,p,b,g in
              batches[iteration*batch_size:(iteration+1)*batch_size]]
        rng.shuffle(arms)
        for storage,table in arms:
            if table=='plain':
                sql=("INSERT INTO plain SELECT id,project,body,to_tsvector('simple',body) "
                     "FROM (VALUES %s) x(id,project,body,grams)")
            else:
                sql=("INSERT INTO encrypted SELECT id,project,pgp_sym_encrypt_bytea("
                     "int8send(id)||convert_to(body,'UTF8'),fixture_key(id),'"+OPTIONS+"'),"
                     "grams,to_tsvector('simple',body) "
                     "FROM (VALUES %s) x(id,project,body,grams)")
            with connections[storage].cursor() as cur:
                start=time.perf_counter()
                execute_values(cur,sql,rows,template='(%s::bigint,%s::integer,%s::text,%s::text[])',
                               page_size=batch_size)
                elapsed=(time.perf_counter()-start)*1000
            if iteration: times[(storage,table)].append(elapsed)
    for (storage,table),samples in times.items():
        result.append({'storage':storage,'payload':table,'batch_rows':batch_size,
                       'latency':summarize(samples),
                       'rows_per_second':batch_size/(statistics.median(samples)/1000)})
    return result


def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('action',choices=['init','measure','concurrent','write'])
    parser.add_argument('--root',default='/opt/aimee-encryption-bench')
    parser.add_argument('--rows',type=int,default=100000)
    parser.add_argument('--width',type=int,default=4096)
    parser.add_argument('--repeats',type=int,default=5)
    args=parser.parse_args()
    root=Path(args.root); (root/'results').mkdir(exist_ok=True)
    sockets={name:str(root/'sockets'/name) for name in ('ordinary','luks')}
    if args.action=='init':
        results={}
        for name,sock in sockets.items():
            conn=connect(sock)
            results[name]=initialize(conn,args.rows,args.width); conn.close()
            (root/'results'/'load.json').write_text(json.dumps(results,indent=2)+'\n')
    elif args.action=='measure':
        conns={name:connect(sock,args.rows) for name,sock in sockets.items()}
        measure(conns,[min(10000,args.rows),args.rows],args.repeats,root/'results'/'search.json')
        for conn in conns.values(): conn.close()
    elif args.action=='concurrent':
        result=concurrent_measure(sockets,args.rows,4,3)
        (root/'results'/'concurrent.json').write_text(json.dumps(result,indent=2)+'\n')
    else:
        conns={name:connect(sock) for name,sock in sockets.items()}
        result=write_measure(conns,args.rows,args.width,args.repeats)
        (root/'results'/'write.json').write_text(json.dumps(result,indent=2)+'\n')
        for conn in conns.values(): conn.close()


if __name__=='__main__':
    main()
