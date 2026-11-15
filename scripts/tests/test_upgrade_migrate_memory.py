#!/usr/bin/env python3
"""Tests for scripts/upgrade/migrate-memory.py with a fake psql and fake Server/KB endpoints."""
from __future__ import annotations

import contextlib
import http.server
import importlib.util
import io
import json
import os
import socketserver
import stat
import sys
import tempfile
import threading
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location('migrate_memory', ROOT / 'scripts/upgrade/migrate-memory.py')
tool = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(tool)


def row(identifier: int, placement: str = 'server', **changes) -> dict:
    base = dict(placement=placement, id=identifier, kind='fact', tier='L2', key=f'key-{identifier}',
                content=f'content {identifier}', confidence=0.9, provenance_category='user_stated',
                record_revision=2, scope_type='user' if placement == 'server' else 'global',
                scope_value='_user' if placement == 'server' else '_global', use_cases='')
    base.update(changes)
    return base


class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        state = self.server.state
        state['requests'].append((self.path, body, self.headers.get('Authorization')))
        status, reply = 200, {'status': 'ok'}
        if state['unavailable']:
            status, reply = 503, {'status': 'error', 'kind': 'unavailable', 'message': 'memory module unavailable'}
        elif self.path.endswith('list'):
            reply = {'status': 'ok', 'memories': []}
        elif body['key'] in state['sensitive']:
            # The memory client screen answers a raw sensitive value with HTTP 400.
            status, reply = 400, {'status': 'error', 'kind': 'invalid_argument',
                                  'message': 'memory does not store sensitive values; keep the value in the vault'}
        elif body['key'] in state['refuse']:
            # The KB action bridge returns HTTP 200 for a domain refusal envelope.
            status, reply = 200, {'status': 'error', 'kind': 'invalid_argument', 'message': 'refused'}
        else:
            replayed = body['idempotency_key'] in state['seen']
            state['seen'].add(body['idempotency_key'])
            reply = {'status': 'ok', 'id': len(state['seen']), 'mutation_receipt': {'replayed': replayed}}
        data = json.dumps(reply).encode()
        self.send_response(status)
        self.send_header('Content-Length', str(len(data)))
        self.end_headers()
        self.wfile.write(data)


class UnixServer(socketserver.UnixStreamServer):
    def get_request(self):
        request, _ = super().get_request()
        return request, ('local', 0)


class MigrateMemoryTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.root = Path(self.directory.name)
        self.socket = str(self.root / 'aimee-http.sock')
        state = dict(requests=[], seen=set(), refuse=set(), sensitive=set(), unavailable=False)
        self.state = state
        self.servers = [UnixServer(self.socket, Handler), http.server.HTTPServer(('127.0.0.1', 0), Handler)]
        for server in self.servers:
            server.state = state
            threading.Thread(target=server.serve_forever, daemon=True).start()
        self.address = '127.0.0.1:%d' % self.servers[1].server_port
        self.previous_token = os.environ.pop('AIMEE_KB_API_BEARER_TOKEN', None)

    def tearDown(self):
        for server in self.servers:
            server.shutdown()
            server.server_close()
        if self.previous_token is not None:
            os.environ['AIMEE_KB_API_BEARER_TOKEN'] = self.previous_token
        self.directory.cleanup()

    def fake_psql(self, rows, code=0) -> str:
        script = self.root / 'psql'
        script.write_text('#!/bin/sh\ncat > "%s"\ncat "%s"\nexit %d\n' % (self.root / 'sql', self.root / 'rows', code))
        script.chmod(0o755)
        (self.root / 'rows').write_text('BEGIN\n' + ''.join(json.dumps(r) + '\n' for r in rows) + 'COMMIT\n')
        return str(script)

    def run_tool(self, *argv) -> int:
        previous = sys.argv
        sys.argv = ['migrate-memory.py', *argv]
        try:
            return tool.main()
        finally:
            sys.argv = previous

    def snapshot(self, rows, placement='server', name='snapshot.jsonl') -> Path:
        path = self.root / name
        self.assertEqual(self.run_tool('export', '--placement', placement, '--psql', self.fake_psql(rows),
                                       '--file', str(path)), 0)
        return path

    def stores(self):
        return [(route, body, auth) for route, body, auth in self.state['requests'] if route.endswith('store')]

    def test_export_is_read_only_filtered_private_and_never_overwrites(self):
        for placement, table, erasure in (('server', 'FROM user_memories m', 'user_memory_erasure_intents'),
                                          ('kb', 'FROM memories m', 'FROM memory_erasure_intents')):
            path = self.snapshot([row(1, placement), row(2, placement)], placement, placement + '.jsonl')
            self.assertEqual(stat.S_IMODE(path.stat().st_mode), 0o600)
            self.assertEqual([json.loads(line)['id'] for line in path.read_text().splitlines()], [1, 2])
            sql = (self.root / 'sql').read_text()
            for required in ('READ ONLY', "lifecycle_state = 'active'", 'valid_until', table, erasure):
                self.assertIn(required, sql)
            for forbidden in ('INSERT', 'UPDATE ', 'DELETE', 'TRUNCATE', 'DROP', 'ALTER'):
                self.assertNotIn(forbidden, sql.upper())
            with self.assertRaises(SystemExit):
                self.run_tool('export', '--placement', placement, '--psql', self.fake_psql([row(1, placement)]),
                              '--file', str(path))
        self.assertIn('merged_into = 0', tool.EXPORT_SQL['kb'])

    def test_export_refuses_a_failed_query_and_malformed_rows(self):
        with self.assertRaises(SystemExit):
            self.run_tool('export', '--placement', 'server', '--psql', self.fake_psql([row(1)], code=3),
                          '--file', str(self.root / 'a.jsonl'))
        with self.assertRaises(SystemExit):
            self.run_tool('export', '--placement', 'server', '--psql', self.fake_psql([row(1, content='')]),
                          '--file', str(self.root / 'b.jsonl'))
        self.assertFalse((self.root / 'a.jsonl').exists() or (self.root / 'b.jsonl').exists())

    def test_server_import_maps_fields_and_authority_and_is_idempotent(self):
        path = self.snapshot([row(1), row(2, provenance_category='agent_message'),
                              row(3, provenance_category='reviewed_model'), row(4, provenance_category='unknown')])
        self.assertEqual(self.run_tool('import', '--placement', 'server', '--file', str(path),
                                       '--socket', self.socket, '--dry-run'), 0)
        self.assertEqual(self.stores(), [])
        self.assertEqual(self.run_tool('import', '--placement', 'server', '--file', str(path),
                                       '--socket', self.socket), 0)
        stores = self.stores()
        self.assertEqual([route for route, _, _ in stores], ['/v1/memory/store'] * 4)
        self.assertEqual([body['authority'] for _, body, _ in stores], ['user', 'model', 'user', 'model'])
        self.assertEqual(stores[0][1], dict(store='user', key='key-1', content='content 1', kind='fact', tier='L2',
                                            confidence=0.9, authority='user',
                                            idempotency_key='upgrade-1.0-server-1-2'))
        self.assertEqual(self.run_tool('import', '--placement', 'server', '--file', str(path),
                                       '--socket', self.socket), 0)
        self.assertEqual(len(self.state['seen']), 4)

    def test_kb_import_places_each_scope_and_authenticates(self):
        path = self.snapshot([row(1, 'kb'), row(2, 'kb', scope_type='project', scope_value='apollo', use_cases='deploys'),
                              row(3, 'kb', scope_type='workspace', scope_value='/srv/ws'),
                              row(4, 'kb', scope_type='session', scope_value='s-1')], 'kb')
        with self.assertRaises(SystemExit):
            self.run_tool('import', '--placement', 'kb', '--file', str(path), '--address', self.address)
        token = self.root / 'token'
        token.write_text('secret-token\n')
        self.assertEqual(self.run_tool('import', '--placement', 'kb', '--file', str(path), '--address', self.address,
                                       '--token-file', str(token)), 1)
        stores = self.stores()
        self.assertEqual([route for route, _, _ in stores], ['/v1/actions/memory.store'] * 3)
        self.assertTrue(all(auth == 'Bearer secret-token' for _, _, auth in stores))
        self.assertEqual(stores[0][1]['scope'], 'all')
        self.assertEqual((stores[1][1]['project'], stores[1][1]['use_cases']), ('apollo', 'deploys'))
        self.assertEqual(stores[2][1]['workspace'], '/srv/ws')
        self.assertTrue(all('store' not in body for _, body, _ in stores))
        self.assertEqual(stores[1][1]['idempotency_key'], 'upgrade-1.0-kb-2-2')

    def test_scopes_names_what_a_kb_must_serve_before_import(self):
        path = self.snapshot([row(1, 'kb'), row(2, 'kb', scope_type='project', scope_value='zephyr'),
                              row(3, 'kb', scope_type='project', scope_value='apollo'),
                              row(4, 'kb', scope_type='project', scope_value='apollo'),
                              row(5, 'kb', scope_type='workspace', scope_value='/srv/ws'),
                              row(6, 'kb', scope_type='session', scope_value='s-1')], 'kb')
        with contextlib.redirect_stdout(io.StringIO()) as out:
            self.assertEqual(self.run_tool('scopes', '--file', str(path)), 0)
        self.assertEqual(out.getvalue().strip(), 'global,project:apollo,project:zephyr,workspace:/srv/ws')
        server = self.snapshot([row(7)], name='personal.jsonl')
        with self.assertRaises(SystemExit):
            self.run_tool('scopes', '--file', str(server))

    def test_a_refused_sensitive_record_is_reported_and_the_rest_imported(self):
        path = self.snapshot([row(1), row(2, content='call me at person@example.invalid'), row(3)])
        self.state['sensitive'] = {'key-2'}
        out = io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(out):
            self.assertEqual(self.run_tool('import', '--placement', 'server', '--file', str(path),
                                           '--socket', self.socket), 1)
        self.assertEqual(len(self.state['seen']), 2)
        self.assertIn('FAILED', out.getvalue())
        self.assertIn('vault', out.getvalue())

    def test_import_refuses_the_wrong_placement_an_unready_instance_and_reports_failures(self):
        path = self.snapshot([row(1), row(2)])
        with self.assertRaises(SystemExit):
            self.run_tool('import', '--placement', 'kb', '--file', str(path), '--address', self.address)
        self.state['unavailable'] = True
        with self.assertRaises(SystemExit):
            self.run_tool('import', '--placement', 'server', '--file', str(path), '--socket', self.socket)
        self.assertEqual(self.stores(), [])
        self.state['unavailable'] = False
        self.state['refuse'] = {'key-2'}
        self.assertEqual(self.run_tool('import', '--placement', 'server', '--file', str(path),
                                       '--socket', self.socket), 1)
        self.assertEqual(len(self.state['seen']), 1)


if __name__ == '__main__':
    unittest.main()
