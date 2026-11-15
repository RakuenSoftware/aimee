"""Representation-only protection of retained legacy memory. Never changes eligibility.

The operator supplies the same owner psql connection used for export. Writers
must remain stopped. All Vault seals precede one locked SQL transaction; user
triggers are restored to their original modes in that transaction. Primary
keys, revisions, authority, timestamps and original proposal digests survive.
"""
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile

# Payload-bearing relations owned by the removed memory implementation. Never
# discover arbitrary application/Vault tables or rewrite authority columns.
TABLES = {
    'server': {
        'user_memories': ('key', 'content', 'source_session'),
        'user_memory_versions': ('record',),
        'user_memory_correction_proposals': ('payload',),
    },
    'kb': {
        'memories': ('key', 'content', 'use_cases', 'source_session', 'archive_reason'),
        'memory_correction_proposals': ('payload',),
        'memory_rejection_tombstones': ('memory_key', 'memory_content', 'source', 'relation', 'target', 'reason'),
        'fact_graph_changes': ('diff_detail', 'before_state', 'after_state'),
        'knowledge_corrections': ('before_value', 'after_value'),
        'knowledge_review_decisions': ('requested_value', 'evidence_snapshot'),
        'memory_evidence_events': ('reason',),
    },
}
ROW_FILTERS = {
    'knowledge_corrections': "subject_kind='memory'",
    'knowledge_review_decisions': "source_queue IN ('memory_correction','conflict','contradiction','learning_proposal')",
    'memory_evidence_events': "object_kind IN ('memory','review')",
}
JSON_PAYLOADS = {'payload', 'diff_detail', 'before_state', 'after_state', 'before_value', 'after_value', 'evidence_snapshot'}
VAULT_REFS = re.compile(r'(?:\[\[vault:v1:[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}\]\])+')
STRING = re.compile(r'"(?:[^"\\]|\\.)*"', re.DOTALL)


def literal(value):
    return "'" + str(value).replace("'", "''") + "'"


def identifier(value):
    return '"' + value.replace('"', '""') + '"'


def query(command, sql):
    result = subprocess.run([*shlex.split(command), '-X', '-q', '-A', '-t', '-v', 'ON_ERROR_STOP=1'],
                            input='SET standard_conforming_strings=on;\n' + sql, text=True, capture_output=True)
    if result.returncode:
        # SQL diagnostics can contain payloads: never copy them into a report.
        raise ValueError('retained SQL custody transaction refused; no source changes committed')
    # json_agg(composite) can emit a multi-line value even with psql -At.
    rows, at, decoder = [], 0, json.JSONDecoder()
    while at < len(result.stdout):
        if result.stdout[at].isspace():
            at += 1
        elif result.stdout[at] == '{':
            try:
                row, at = decoder.raw_decode(result.stdout, at)
            except ValueError:
                raise ValueError('retained SQL returned invalid JSON') from None
            rows.append(row)
        else:
            end = result.stdout.find('\n', at)
            at = len(result.stdout) if end < 0 else end + 1
    return rows


def seal(target, text, binding=''):
    if not text or VAULT_REFS.fullmatch(text):
        return text
    status, reply = target.call('screen_content', {'content': text, 'key': binding})
    reply = reply.get('result', reply)
    if status != 200 or reply.get('status') != 'ok':
        raise ValueError('authenticated Vault screening unavailable; original data retained')
    if reply.get('verdict') == 'allow':
        return text
    safe = reply.get('redacted')
    if reply.get('verdict') != 'redact' or not isinstance(safe, str) or '[[vault:v1:' not in safe:
        raise ValueError('screening did not return an opaque Vault reference; original data retained')
    return safe


def protect_json(target, text):
    # Replace only string tokens. Parsing/reprinting JSON through binary floats
    # would corrupt historical integer/revision or numeric lexemes above 2^53.
    value = json.loads(text)
    binding = value.get('key', '') if isinstance(value, dict) else ''
    field = ''
    def replace(match):
        nonlocal field
        token = json.loads(match.group())
        if text[match.end():].lstrip().startswith(':'):
            field = token
            safe = seal(target, token)
            return match.group() if safe == token else json.dumps(safe, ensure_ascii=False)
        private_binding = binding if field in ('content', 'key', 'new_content') else field
        safe = seal(target, token, private_binding)
        return match.group() if safe == token else json.dumps(safe, ensure_ascii=False)
    safe = STRING.sub(replace, text)
    json.loads(safe)
    return safe


def protect_records(target, records):
    protected = []
    for record in records:
        safe = record.copy()
        for field in ('key', 'content', 'use_cases', 'source_session'):
            if isinstance(record.get(field), str):
                safe[field] = seal(target, record[field], record['key'] if field in ('key', 'content') else field)
        protected.append(safe)
    return protected


def inventory(command, placement):
    tables = TABLES[placement]
    names = ','.join(literal(t) for t in tables)
    metadata = query(command, f"""
SELECT json_build_object('table',c.relname,'columns',
 (SELECT json_agg(json_build_object('name',a.attname,'type',a.atttypid::regtype::text))
  FROM pg_attribute a WHERE a.attrelid=c.oid AND a.attnum>0 AND NOT a.attisdropped AND a.attgenerated=''),
 'pk',(SELECT json_agg(a.attname ORDER BY k.ordinality) FROM pg_index i,
  unnest(i.indkey) WITH ORDINALITY k(attnum,ordinality),pg_attribute a
  WHERE i.indrelid=c.oid AND i.indisprimary AND a.attrelid=c.oid AND a.attnum=k.attnum))
FROM pg_class c JOIN pg_namespace n ON n.oid=c.relnamespace
WHERE n.nspname=current_schema() AND c.relkind='r' AND c.relname IN ({names}) ORDER BY c.relname;
""")
    base = 'user_memories' if placement == 'server' else 'memories'
    if base not in {t['table'] for t in metadata}:
        raise ValueError('source is not the legacy store for this placement')
    result = []
    for table in metadata:
        table['fields'] = [c for c in table['columns'] if c['name'] in tables[table['table']]]
        if not table['pk'] or any(c['type'] not in ('text', 'jsonb', 'json', 'character varying') for c in table['fields']):
            raise ValueError('unsupported retained source schema')
        pk = ','.join(literal(k)+','+identifier(k)+'::text' for k in table['pk'])
        fields = ','.join(literal(c['name'])+','+identifier(c['name'])+'::text' for c in table['fields'])
        where = 'WHERE '+ROW_FILTERS[table['table']]+' ' if table['table'] in ROW_FILTERS else ''
        table['select'] = (f"SELECT json_build_object('pk',json_build_object({pk}),"
                           f"'payload',json_build_object({fields}), 'revision',to_jsonb(t)->>'record_revision',"
                           f"'row_hash',encode(sha256(convert_to(to_jsonb(t)::text,'UTF8')),'hex')) FROM {identifier(table['table'])} t "
                           +where+'ORDER BY '+','.join(identifier(k) for k in table['pk']))
        table['rows'] = query(command, 'BEGIN ISOLATION LEVEL REPEATABLE READ READ ONLY;\n'+table['select']+';\nCOMMIT;')
        result.append(table)
    return result


def rewrite_snapshot(path, records):
    path = Path(path)
    fd, temporary = tempfile.mkstemp(prefix='.'+path.name+'-', dir=path.parent)
    try:
        with os.fdopen(fd, 'w') as handle:
            os.fchmod(handle.fileno(), 0o600)
            for record in records:
                handle.write(json.dumps(record, sort_keys=True)+'\n')
            handle.flush()
            os.fsync(handle.fileno())
        os.replace(temporary, path)
        directory = os.open(path.parent, os.O_RDONLY | os.O_DIRECTORY)
        try:
            os.fsync(directory)
        finally:
            os.close(directory)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


def protect_source(command, placement, target, records, path, *, export_sql=None):
    tables = inventory(command, placement)
    # Bind the export to this source, before rewriting anything. On an
    # interrupted retry either plaintext or its deterministic Vault form matches.
    base = next(t for t in tables if t['table'] == ('user_memories' if placement == 'server' else 'memories'))
    source = {r['pk']['id']: r for r in base['rows']}
    protected = protect_records(target, records)
    if export_sql is not None:
        current = {str(row['id']): row for row in query(command, export_sql)}
        if len({str(row['id']) for row in records}) != len(records) or set(current) != {str(row['id']) for row in records}:
            raise ValueError('legacy eligibility changed since export; create a fresh snapshot with writers stopped')
        for old, safe in zip(records, protected):
            canonical = current[str(old['id'])]
            if any(key not in canonical or canonical[key] not in (value, safe.get(key)) for key,value in old.items()):
                raise ValueError('snapshot metadata differs from its source; create a fresh snapshot')
            # Older exports lacked temporal bounds. Take them from the verified
            # live source rather than silently making expiring data permanent.
            for key in ('valid_from', 'valid_until'):
                if key in canonical:
                    safe[key] = canonical[key]
    for old, safe in zip(records, protected):
        row = source.get(str(old['id']))
        if row is None or row['revision'] != str(old['record_revision']) or any(row['payload'].get(k) not in (old[k], safe[k]) for k in ('key', 'content')):
            raise ValueError('snapshot does not match the selected legacy SQL source')
    sql = ['BEGIN;', "SET LOCAL lock_timeout='10s';", r"""
CREATE TABLE IF NOT EXISTS aimee_legacy_memory_custody (
 placement text NOT NULL CHECK(placement IN ('server','kb')),
 source_table text NOT NULL, source_key jsonb NOT NULL, source_column text NOT NULL,
 original_sha256 text NOT NULL CHECK(original_sha256 ~ '^[0-9a-f]{64}$'),
 protected_sha256 text NOT NULL CHECK(protected_sha256 ~ '^[0-9a-f]{64}$'),
 original_vault_ref text NOT NULL CHECK(original_vault_ref ~ '^(\[\[vault:v1:[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}\]\])+$'),
 protected_at timestamptz NOT NULL DEFAULT CURRENT_TIMESTAMP,
 PRIMARY KEY(placement,source_table,source_key,source_column,original_sha256)
);
REVOKE ALL ON aimee_legacy_memory_custody FROM PUBLIC;
DO $custody_acl$ DECLARE recipient record; BEGIN
 FOR recipient IN SELECT DISTINCT a.grantee FROM pg_class c,LATERAL aclexplode(c.relacl) a
 WHERE c.oid='aimee_legacy_memory_custody'::regclass AND a.grantee<>c.relowner AND a.grantee<>0 LOOP
  EXECUTE format('REVOKE ALL ON aimee_legacy_memory_custody FROM %I',pg_get_userbyid(recipient.grantee));
 END LOOP;
END $custody_acl$;
CREATE OR REPLACE FUNCTION aimee_legacy_memory_custody_immutable() RETURNS trigger
LANGUAGE plpgsql SET search_path=pg_catalog AS $custody_guard$
BEGIN RAISE EXCEPTION 'retained original payload witnesses are immutable'; END $custody_guard$;
REVOKE ALL ON FUNCTION aimee_legacy_memory_custody_immutable() FROM PUBLIC;
DO $custody_trigger$ BEGIN
 IF NOT EXISTS(SELECT 1 FROM pg_trigger WHERE tgrelid='aimee_legacy_memory_custody'::regclass
  AND tgname='aimee_legacy_memory_custody_immutable') THEN
  CREATE TRIGGER aimee_legacy_memory_custody_immutable BEFORE UPDATE OR DELETE ON aimee_legacy_memory_custody
   FOR EACH ROW EXECUTE FUNCTION aimee_legacy_memory_custody_immutable();
 END IF;
END $custody_trigger$;
"""]
    changed = 0
    for table in tables:
        name = identifier(table['table'])
        sql.append(f'LOCK TABLE {name} IN ACCESS EXCLUSIVE MODE;')
        # Re-read under the lock and compare the complete inventory, including
        # unchanged rows. A newly inserted raw payload cannot escape this pass.
        expected = json.dumps([{'pk': r['pk'], 'row_hash': r['row_hash']} for r in table['rows']], sort_keys=True, separators=(',', ':'))
        # PostgreSQL compares JSONB exactly (including arbitrary precision nums).
        sql.append(f"DO $custody$ BEGIN IF (SELECT coalesce(jsonb_agg(x ORDER BY x->'pk'), '[]'::jsonb) "
                   f"FROM (SELECT jsonb_build_object('pk',v->'pk','row_hash',v->'row_hash') AS x FROM ({table['select']}) q(v)) locked) IS DISTINCT FROM "
                   f"(SELECT coalesce(jsonb_agg(x ORDER BY x->'pk'), '[]'::jsonb) FROM jsonb_array_elements({literal(expected)}::jsonb) x) "
                   "THEN RAISE EXCEPTION 'retained source changed; retry with writers stopped'; END IF; END $custody$;")
        updates, witnesses = [], []
        for row in table['rows']:
            binding = row['payload'].get('key', '')
            fields = []
            for column in table['fields']:
                field = column['name']
                old = row['payload'][field]
                if old is None:
                    continue
                structured = column['type'] in ('json', 'jsonb')
                if field in JSON_PAYLOADS and not structured:
                    try:
                        structured = isinstance(json.loads(old), (dict, list))
                    except ValueError:
                        pass
                safe = protect_json(target, old) if structured else seal(target, old, binding if field in ('key','content') else field)
                if safe != old:
                    # Retain exact bytes, including JSON escaping/whitespace.
                    # Token replacement alone cannot reproduce a signed JSON
                    # payload's original spelling after decoding its strings.
                    original = seal(target, old, 'retained_payload_secret')
                    if not VAULT_REFS.fullmatch(original):
                        raise ValueError('Vault did not seal the exact retained payload')
                    values = [literal(placement), literal(table['table']), literal(json.dumps(row['pk']))+'::jsonb',
                              literal(field), literal(hashlib.sha256(old.encode()).hexdigest()),
                              literal(hashlib.sha256(safe.encode()).hexdigest()), literal(original)]
                    witnesses.append('INSERT INTO aimee_legacy_memory_custody(placement,source_table,source_key,source_column,original_sha256,protected_sha256,original_vault_ref) VALUES ('+
                                     ','.join(values)+') ON CONFLICT DO NOTHING;')
                    fields.append(identifier(field)+'='+literal(safe)+'::'+column['type'])
                    changed += 1
            if fields:
                where = ' AND '.join(identifier(k)+'::text='+literal(v) for k,v in row['pk'].items())
                updates.append(f'UPDATE {name} SET '+','.join(fields)+' WHERE '+where+';')
        if updates:
            # DDL and its rollback are transactional. Internal FK triggers stay
            # enabled. No other session can observe a disabled trigger window.
            sql.append(f"CREATE TEMP TABLE custody_triggers_{table['table']} ON COMMIT DROP AS "
                       f"SELECT tgname,tgenabled FROM pg_trigger WHERE tgrelid={literal(table['table'])}::regclass AND NOT tgisinternal;")
            sql.append(f'ALTER TABLE {name} DISABLE TRIGGER USER;')
            sql.extend(witnesses)
            sql.extend(updates)
            sql.append(f"DO $custody$ DECLARE t record; BEGIN FOR t IN SELECT * FROM custody_triggers_{table['table']} LOOP "
                       f"EXECUTE format('ALTER TABLE {name} %s TRIGGER %I',CASE t.tgenabled "
                       "WHEN 'D' THEN 'DISABLE' WHEN 'A' THEN 'ENABLE ALWAYS' WHEN 'R' THEN 'ENABLE REPLICA' ELSE 'ENABLE' END,t.tgname); "
                       "END LOOP; END $custody$;")
    sql.append('COMMIT;')
    query(command, '\n'.join(sql))
    rewrite_snapshot(path, protected)
    return protected, changed
