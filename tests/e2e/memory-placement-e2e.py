#!/usr/bin/env python3
"""Memory release gate against an explicitly supplied disposable Docker stack.

Creates fixtures, restarts services, and stops each dependency temporarily.
Run after the Docker smoke bootstrap; never point this at an installation.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import time
import uuid

HTTP = '''import http.client,socket,json,sys
class C(http.client.HTTPConnection):
 def connect(self):
  self.sock=socket.socket(socket.AF_UNIX);self.sock.settimeout(40);self.sock.connect('/var/lib/aimee/aimee-http.sock')
a=json.load(sys.stdin);c=C('localhost',timeout=40)
c.request(a['method'],a['path'],json.dumps(a['body']),{'Content-Type':'application/json'})
r=c.getresponse();print(json.dumps([r.status,json.loads(r.read())]))
'''

class Gate:
    def __init__(self, args):
        self.args = args
        self.prefix = 'placement-e2e-' + uuid.uuid4().hex[:10]
        self.checks = []

    def docker(self, *args, input=None):
        return subprocess.check_output(['docker', *args], input=input, text=True, stderr=subprocess.PIPE, timeout=90).strip()

    def call(self, op, body=None, method='POST'):
        path = op if op.startswith('/') else '/v1/memory/' + op
        return json.loads(self.docker('exec', '-i', self.args.server, 'python3', '-c', HTTP,
            input=json.dumps(dict(method=method, path=path, body=body or {}))))

    def cli(self, *args):
        return json.loads(self.docker('exec', '-u', '1000', '-e',
            'AIMEE_API_ENDPOINT=unix:/var/lib/aimee/aimee-http.sock',
            self.args.server, 'aimee', '--json', 'memory', *map(str, args)))

    def sql(self, query):
        return self.docker('exec', self.args.kb, 'psql', '-h', '/var/lib/aimee/run',
                           '-d', 'aimee_shared', '-X', '-At', '-v', 'ON_ERROR_STOP=1', '-c', query)

    def digest(self):
        rows = self.sql("SELECT id,key,content FROM memories ORDER BY id")
        return hashlib.sha256(rows.encode()).hexdigest()

    def assert_no_personal_canary(self):
        tables = self.sql("SELECT tablename FROM pg_tables WHERE schemaname='public' ORDER BY tablename").splitlines()
        queries = []
        for table in tables:
            ident = '"' + table.replace('"', '""') + '"'
            literal = "'" + table.replace("'", "''") + "'"
            queries.append(f"SELECT {literal} WHERE EXISTS(SELECT 1 FROM public.{ident} t "
                "WHERE row_to_json(t)::text LIKE '%user@local.invalid%' "
                "OR row_to_json(t)::text LIKE '%person@local.invalid%')")
        found = self.sql(' UNION ALL '.join(queries))
        self.check('personal email canaries absent from all KB tables', not found,
                   dict(table_count=len(tables), matching_tables=found.splitlines()))

    def check(self, name, passed, detail=None):
        self.checks.append(dict(name=name, passed=bool(passed), detail=detail))
        print(('PASS ' if passed else 'FAIL ') + name, flush=True)

    def good(self, name, response):
        code, body = response
        passed = code == 200 and body.get('status') == 'ok'
        self.check(name, passed, None if passed else response)
        return body

    def wait(self, op, body=None):
        deadline = time.monotonic() + 150
        while time.monotonic() < deadline:
            try:
                response = self.call(op, body)
                if response[0] == 200 and response[1].get('status') == 'ok':
                    return response
            except (subprocess.SubprocessError, ValueError):
                pass
            time.sleep(1)
        raise RuntimeError('service did not recover: ' + op)

    def mcp(self, tool, arguments):
        code, body = self.call('/v1/mcp/call', dict(tool=tool, arguments=arguments))
        # The transport wraps the MCP content blocks in its result envelope.
        return code, json.dumps(body, ensure_ascii=False)

    def run(self):
        before = self.digest()
        content = 'Personal fixture user@local.invalid 🦊 ' + 'long note αβ ' * 500
        written = self.good('local store', self.call('store', dict(key=self.prefix, content=content)))
        mid = written['id']
        self.check('local store leaves KB unchanged', self.digest() == before)
        # Independent sequences deliberately overlap. Preserve any existing KB
        # row at this ID, or seed a benign shared record to create the collision.
        self.sql(f"INSERT INTO memories(id,key,content) VALUES({mid},'{self.prefix}-kb','shared collision fixture') ON CONFLICT(id) DO NOTHING")
        self.sql("SELECT setval(pg_get_serial_sequence('memories','id'),GREATEST(1,(SELECT max(id) FROM memories)),true)")
        shared = json.loads(self.sql(f"SELECT to_json(content) FROM memories WHERE id={mid}"))
        before = self.digest()
        got = self.good('local get', self.call('get', dict(id=mid)))
        self.check('get returns exact local content for colliding ID', got.get('memory', {}).get('content') == content)
        got = self.good('explicit KB get', self.call('get', dict(id=mid, store='kb', scope='all')))
        self.check('explicit KB get selects the other record', got.get('memory', {}).get('content') == shared)
        mcp_key = self.prefix + '-mcp'
        mcp_content = 'Personal MCP fixture person@local.invalid'
        code, reply = self.call('/v1/mcp/call', dict(tool='mutate', arguments=dict(verb='store', key=mcp_key, content=mcp_content)))
        result = json.loads(reply['content'][0]['text'])
        self.check('MCP store defaults to user', code == 200 and result.get('status') == 'ok' and result.get('store') == 'user', None if result.get('status') == 'ok' else reply)
        mcp_id = result['id']
        self.check('MCP personal content stays local', self.cli('get', mcp_id).get('memory', {}).get('content') == mcp_content and self.digest() == before)
        self.call('/v1/mcp/call', dict(tool='mutate', arguments=dict(verb='update', id=mcp_id, content='updated MCP fixture')))
        self.check('MCP update uses local ID', self.cli('get', mcp_id).get('memory', {}).get('content') == 'updated MCP fixture')
        self.call('/v1/mcp/call', dict(tool='mutate', arguments=dict(verb='forget', id=mcp_id)))
        code, reply = self.call('get', dict(id=mcp_id))
        self.check('MCP forget retires local ID', code >= 400 and reply.get('kind') == 'not_found' and self.digest() == before)
        self.check('CLI default get selects user', self.cli('get', mid).get('memory', {}).get('content') == content)
        self.check('CLI explicit get selects KB', self.cli('get', mid, '--store', 'kb', '--scope', 'all').get('memory', {}).get('content') == shared)
        cli_row = self.cli('store', self.prefix + '-cli', 'local CLI fixture')
        self.check('CLI store identifies local scope', cli_row.get('store') == 'user')
        self.cli('supersede', cli_row['id'], 'updated CLI fixture')
        self.check('CLI supersede updates local record', self.cli('get', cli_row['id']).get('memory', {}).get('content') == 'updated CLI fixture')
        self.cli('delete', cli_row['id'])
        self.check('CLI mutations leave KB unchanged', self.digest() == before)
        self.good('local stats', self.call('stats', method='GET'))
        for invalid in ('both', '', 1):
            code, body = self.call('store', dict(store=invalid, key=self.prefix+'-invalid', content='private invalid selector'))
            self.check('invalid store rejected '+repr(invalid), code >= 400 and body.get('status') == 'error')
        self.check('invalid selectors leave KB unchanged', self.digest() == before)
        for op, body, field in [('list', {}, 'memories'), ('review', {}, 'memories'),
                                ('search', {'keywords': [self.prefix]}, 'facts')]:
            result = self.good('local ' + op, self.call(op, body))
            self.check(op + ' returns local fixture', any(r.get('id') == mid and r.get('content') == content
                                                         for r in result.get(field, [])))
        for args, expected in [(dict(id=mid), content), (dict(handle='memory:' + str(mid), scope='all'), shared)]:
            code, result = self.mcp('memory_get', args)
            # Content is nested as a JSON string within the MCP text block.
            self.check('MCP get preserves selected store ' + str(args), code == 200 and json.dumps(expected, ensure_ascii=False)[1:-1] in result)
        self.good('local supersede', self.call('supersede', dict(old_id=mid, new_content='corrected local fixture')))
        self.check('local mutation leaves colliding KB record unchanged', self.digest() == before)
        got = self.good('get corrected local memory', self.call('get', dict(id=mid)))
        self.check('local replacement content', got.get('memory', {}).get('content') == 'corrected local fixture')
        self.docker('restart', self.args.server)
        got = self.good('local memory survives restart', self.wait('get', dict(id=mid)))
        self.check('restart preserves corrected value', got.get('memory', {}).get('content') == 'corrected local fixture')
        before = self.digest()
        self.docker('stop', self.args.store_db)
        try:
            for op, body in [('list', {}), ('get', {'id': mid}),
                             ('store', {'key': self.prefix + '-outage', 'content': 'private outage fixture'}),
                             ('supersede', {'old_id': mid, 'new_content': 'must not reach KB'}), ('delete', {'id': mid})]:
                code, body = self.call(op, body)
                self.check('local dependency outage fails ' + op, code >= 500 and body.get('status') == 'error', [code, body])
            self.check('local outage never writes KB', self.digest() == before)
        finally:
            self.docker('start', self.args.store_db)
        self.good('local dependency recovers', self.wait('get', dict(id=mid)))
        self.docker('stop', self.args.kb)
        try:
            self.good('local get works with KB offline', self.call('get', dict(id=mid)))
            self.good('local store works with KB offline', self.call('store', dict(key=self.prefix + '-offline', content='private offline fixture')))
            code, body = self.call('list', {'store': 'kb'})
            self.check('explicit KB outage is an HTTP failure', code >= 500 and body.get('status') == 'error', [code, body])
        finally:
            self.docker('start', self.args.kb)
        self.good('KB recovers', self.wait('get', dict(id=mid, store='kb', scope='all')))
        # Pause only the KB memory process: daemon health can stay green while
        # the data stage is unavailable. An empty successful list is a defect.
        pid = int(self.docker('exec', self.args.kb, 'python3', '-c',
            "from pathlib import Path; import os,signal; "
            "pids=[int(p.name) for p in Path('/proc').iterdir() if p.name.isdigit() and "
            "(p/'cmdline').exists() and (p/'cmdline').read_bytes().split(b'\\0')[0].endswith(b'/aimee-module-memory')]; "
            "assert len(pids)==1,pids; os.kill(pids[0],signal.SIGSTOP); print(pids[0])"))
        try:
            for op in ('list', 'get'):
                code, body = self.call(op, dict(store='kb', id=mid, scope='all'))
                self.check('KB memory process outage fails '+op, code >= 500 and body.get('status') == 'error', [code, body])
            self.good('user store works during KB memory process outage', self.call('get', dict(id=mid)))
        finally:
            # A paused process can lose its bus lease. Let the existing image
            # supervisor replace it so recovery follows the production path.
            self.docker('exec', self.args.kb, 'python3', '-c',
                'import os,signal,sys; p=int(sys.argv[1]); os.kill(p,signal.SIGCONT); os.kill(p,signal.SIGTERM)', str(pid))
        self.good('KB memory process recovers', self.wait('get', dict(id=mid, store='kb', scope='all')))
        self.good('local retirement', self.call('delete', dict(id=mid)))
        code, body = self.call('get', dict(id=mid))
        self.check('retired local ID never resolves to KB collision', code >= 400 and body.get('kind') == 'not_found', [code, body])
        self.check('local retirement leaves KB content unchanged', json.loads(self.sql(f'SELECT to_json(content) FROM memories WHERE id={mid}')) == shared)
        long_shared = 'shared release fixture ' + 'αβ🦊 ' * 1000
        row = self.good('explicit long KB store', self.call('store', dict(store='kb', key=self.prefix + '-long', content=long_shared)))
        got = self.good('explicit long KB get', self.call('get', dict(store='kb', id=row['id'])))
        self.check('KB get preserves full long Unicode content', got.get('memory', {}).get('content') == long_shared)
        # Current lookup must hide retired records; explicit historical lookup
        # must still return their complete retained content and validity.
        self.sql(f"UPDATE memories SET lifecycle_state='retired', valid_from='2026-01-01T00:00:00Z', valid_until='2026-06-01T00:00:00Z' WHERE id={row['id']}")
        code, body = self.call('get', dict(store='kb', id=row['id']))
        self.check('ordinary KB get hides retired record', code >= 400 and body.get('kind') == 'not_found', [code, body])
        for as_of, expected_valid in [('2026-03-01T00:00:00Z', True), ('2026-07-01T00:00:00Z', False)]:
            got = self.good('historical retired KB get ' + as_of,
                self.call('get', dict(store='kb', id=row['id'], as_of=as_of)))
            self.check('historical KB get preserves content and validity ' + as_of,
                got.get('memory', {}).get('content') == long_shared and
                got.get('as_of') == as_of and got.get('valid_at') is expected_valid)
        if self.args.upgrade_fixture:
            rows = json.loads(Path(self.args.upgrade_fixture).read_text())
            for row in rows:
                got = self.good('0.4.1 upgrade get ' + row['key'], self.call('get', dict(store='kb', id=row['id'], scope='all')))
                self.check('0.4.1 upgrade preserves exact content ' + row['key'], got.get('memory', {}).get('content') == row['content'])
            listing = self.good('0.4.1 upgrade list', self.call('list', dict(store='kb', scope='all', limit=64)))
            self.check('0.4.1 rows remain discoverable', all(any(r['id'] == old['id'] for r in listing.get('memories', [])) for old in rows))
        self.assert_no_personal_canary()
        return all(c['passed'] for c in self.checks)

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('server', 'kb', 'store-db', 'output'):
        parser.add_argument('--' + name, required=True)
    parser.add_argument('--upgrade-fixture')
    args = parser.parse_args()
    gate = Gate(args)
    try:
        passed = gate.run()
    finally:
        Path(args.output).write_text(json.dumps(gate.checks, indent=2, ensure_ascii=False) + '\n')
    raise SystemExit(0 if passed else 1)
