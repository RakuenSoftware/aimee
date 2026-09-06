#!/usr/bin/env python3
"""Check substring candidate completeness against PostgreSQL's actual operators."""
import json
import random
import psycopg2
from pathlib import Path
from measure import connect, OPTIONS

ROOT=Path('/opt/aimee-encryption-bench')
conn=connect(str(ROOT/'sockets'/'ordinary'))
cur=conn.cursor()
cur.execute('CREATE TEMP TABLE edge(id bigint PRIMARY KEY,body text,cipher bytea,grams text[])')
texts=['', 'a', 'ab', 'abc', 'crypt', 'crypto', 'cry rypt', 'foo.bar', 'foo_bar',
       'a%b', 'a_b', 'a\\b', 'a\nb', 'éclair', 'ÉCLAIR', 'İstanbul', 'straße',
       '東京の記憶', '👩\u200d💻 code', 'FOO::bar() -> value;', '100% coverage', 'abc xx xyz']
rng=random.Random(253)
texts += [''.join(rng.choices('abc xyz%_\\.-é',k=rng.randrange(0,45))) for _ in range(180)]
for i,body in enumerate(texts,1):
    cur.execute("INSERT INTO edge SELECT %s,b,pgp_sym_encrypt_bytea(int8send(%s)||"
                "convert_to(b,'UTF8'),fixture_key(%s),%s),"
                "ARRAY(SELECT DISTINCT substring(lower(b) FROM j FOR 3) "
                "FROM generate_series(1,length(lower(b))-2) j) FROM (SELECT %s::text b) s",
                (i,i,i,OPTIONS,body))


def required(pattern, short=False):
    runs=[]; run=''; i=0
    while i<len(pattern):
        char=pattern[i]; i+=1
        if char=='\\':
            if i==len(pattern): raise ValueError('trailing escape')
            run+=pattern[i]; i+=1
        elif char in '%_':
            runs.append(run); run=''
        else: run+=char
    runs.append(run)
    return sorted({run[j:j+width] for run in runs
                   for width in ([min(3, len(run))] if short and run else [3])
                   for j in range(len(run)-width+1)})


patterns=['','%','_','%a%','%ab%','%abc%','%crypt%','%foo.bar%','%foo_bar%',
          '%foo\\_bar%','%a\\%b%','%a\\_b%','%a\\\\b%','%a_b%','%abc%xyz%',
          '%écl%','%ÉCL%','%東京%','%code%','%100\\%%','abc','abc%','%abc','%qq%']
out=[]
for op in ['LIKE','ILIKE','NOT LIKE']:
    for pattern in patterns:
        grams=required(pattern) if op=='LIKE' else []
        cur.execute(f'SELECT id FROM edge WHERE lower(body) {op} %s ORDER BY id',(pattern,))
        baseline=cur.fetchall()
        cur.execute(f'SELECT id FROM edge WHERE grams @> %s::text[] AND '
                    f'lower(read_body(cipher,fixture_key(id),id)) {op} %s ORDER BY id',
                    (grams,pattern))
        actual=cur.fetchall()
        assert actual==baseline,(op,pattern,baseline,actual)
        out.append({'operator':op,'pattern':pattern,'matches':len(actual),'passed':True,
                    'candidate_filter':bool(grams)})
cur.execute("ALTER TABLE edge ADD COLUMN short_grams text[]")
cur.execute("UPDATE edge SET short_grams = ARRAY(SELECT DISTINCT "
            "substring(lower(body) FROM j FOR n) FROM generate_series(1,3) n "
            "CROSS JOIN LATERAL generate_series(1,length(lower(body))-n+1) j)")
short_cases=[]
for pattern in patterns:
    grams=required(pattern, short=True)
    cur.execute('SELECT id FROM edge WHERE lower(body) LIKE %s ORDER BY id',(pattern,))
    baseline=cur.fetchall()
    cur.execute('SELECT id FROM edge WHERE short_grams @> %s::text[] AND '
                'lower(read_body(cipher,fixture_key(id),id)) LIKE %s ORDER BY id',
                (grams,pattern))
    assert cur.fetchall()==baseline,('short fragments',pattern)
    short_cases.append({'pattern':pattern,'passed':True,'required':grams})
for sql, expected in [
        ("SELECT read_body(cipher,fixture_key(id),id+1) FROM edge WHERE id=1",
         'benchmark record context mismatch'),
        ("SELECT read_body(cipher,fixture_key(id+1),id) FROM edge WHERE id=1",
         'Wrong key or corrupt data')]:
    try:
        cur.execute(sql)
    except psycopg2.Error as error:
        assert expected in str(error), str(error)
        continue
    raise AssertionError('wrong key or record context accepted')
(ROOT/'results'/'edge-parity.json').write_text(json.dumps({
    'rows':len(texts),'cases':out,'short_fragment_cases':short_cases,'wrong_key_and_context_rejected':True},indent=2)+'\n')
print(f'{len(out)} PostgreSQL pattern cases and {len(short_cases)} extended-fragment cases passed; wrong key/context rejected')
conn.close()
