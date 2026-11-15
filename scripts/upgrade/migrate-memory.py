#!/usr/bin/env python3
"""Carry 1.0.x memories into the installed memory implementation after an upgrade.

Releases through 1.0.2 kept memory in the instance's own PostgreSQL store: the
Server's personal memories in `user_memories` and a KB's shared memories in
`memories`. Later releases ship no built-in memory owner. The installed
implementation starts with its own empty store and never reads those tables.
The upgrade retains the rows, and this tool moves them in two explicit steps.

  export   Read the retained rows with psql (read-only, one snapshot) and write
           a private JSON Lines file. Only active, unexpired, unmerged rows that
           have no erasure intent are exported, so a retired or erased memory is
           never brought back.
  import   Store each exported record through the running instance's ordinary
           memory API. Every record carries an idempotency key derived from its
           old id and revision, so an interrupted import can simply be run again
           without creating duplicates.

Use `--placement server` for a Server (personal memory, local Unix socket) and
`--placement kb` for a KB (shared memory in global, project and workspace
scopes, authenticated loopback HTTP). The import uses only the public
`memory.store`/`memory.list` contract, so it works with any implementation that
serves those commands. Record ids, revision history, use counts and embeddings
are not carried over: the implementation assigns new ids and derives its own
indexes. Provenance is preserved as far as the store contract allows:
user-stated and human-reviewed records are stored with user authority,
everything else with model authority.

Import also protects retained source payloads and the snapshot with authenticated Vault references.
Rows, revisions, eligibility and authority remain retained; nothing is deleted.
"""
from __future__ import annotations

import argparse
import http.client
import json
import os
import shlex
import socket
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import memory_custody

EXPORT_SQL = {
    'server': """
BEGIN ISOLATION LEVEL REPEATABLE READ READ ONLY;
SELECT json_build_object(
  'placement', 'server', 'id', m.id, 'kind', m.kind, 'tier', m.tier, 'key', m.key,
  'content', m.content, 'confidence', m.confidence,
  'provenance_category', m.provenance_category, 'record_revision', m.record_revision,
  'scope_type', 'user', 'scope_value', '_user', 'use_cases', '', 'valid_until', m.valid_until)
FROM user_memories m
WHERE m.lifecycle_state = 'active'
  AND (m.valid_until IS NULL OR m.valid_until > now())
  AND NOT EXISTS (SELECT 1 FROM user_memory_erasure_intents e WHERE e.memory_id = m.id)
ORDER BY m.id;
COMMIT;
""",
    'kb': """
BEGIN ISOLATION LEVEL REPEATABLE READ READ ONLY;
SELECT json_build_object(
  'placement', 'kb', 'id', m.id, 'kind', m.kind, 'tier', m.tier, 'key', m.key,
  'content', m.content, 'confidence', m.confidence,
  'provenance_category', m.provenance_category, 'record_revision', m.record_revision,
  'scope_type', m.scope_type, 'scope_value', m.scope_value, 'use_cases', m.use_cases,
  'valid_from', m.valid_from, 'valid_until', m.valid_until)
FROM memories m
WHERE m.lifecycle_state = 'active'
  AND m.merged_into = 0
  AND (NULLIF(m.valid_until, '') IS NULL OR NULLIF(m.valid_until, '')::timestamptz > now())
  AND NOT EXISTS (SELECT 1 FROM memory_erasure_intents e WHERE e.memory_id = m.id)
ORDER BY m.id;
COMMIT;
""",
}
FIELDS = ('placement', 'id', 'kind', 'tier', 'key', 'content', 'confidence', 'provenance_category',
          'record_revision', 'scope_type', 'scope_value')
USER_AUTHORITY = {'user_stated', 'reviewed_model'}
ROUTES = {'server': '/v1/memory/', 'kb': '/v1/actions/memory.'}


def fail(message: str) -> 'NoReturn':
    raise SystemExit('migrate-memory: ' + message)


def export(args: argparse.Namespace) -> int:
    output = Path(args.file)
    if output.exists():
        fail(f'{output} already exists; refusing to overwrite a snapshot')
    command = [*shlex.split(args.psql), '-X', '-q', '-A', '-t', '-v', 'ON_ERROR_STOP=1']
    result = subprocess.run(command, input=EXPORT_SQL[args.placement], text=True, capture_output=True)
    if result.returncode:
        fail(f'psql refused the read-only export; check the connection and that this store belongs to a '
             f'{args.placement} instance')
    records = []
    for line in result.stdout.splitlines():
        line = line.strip()
        if not line.startswith('{'):
            continue
        record = json.loads(line)
        if any(name not in record for name in FIELDS) or not record['key'] or not record['content']:
            fail(f'unexpected row shape for memory id {record.get("id")}')
        records.append(record)
    descriptor = os.open(output, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(descriptor, 'w') as handle:
        for record in records:
            handle.write(json.dumps(record, sort_keys=True) + '\n')
    print(f'exported {len(records)} active {args.placement} memories to {output} (mode 0600; contains private memory)')
    return 0


class LocalConnection(http.client.HTTPConnection):
    def __init__(self, path: str):
        super().__init__('localhost', timeout=120)
        self.path = path

    def connect(self) -> None:
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.settimeout(120)
        self.sock.connect(self.path)


class Target:
    """The running instance's memory API: a Server's local socket or a KB's loopback HTTP."""

    def __init__(self, args: argparse.Namespace):
        self.api_command = getattr(args, 'api_command', None)
        self.placement = args.placement
        self.socket = args.socket
        self.address = args.address
        self.headers = {'Content-Type': 'application/json'}
        if self.placement == 'kb' and not self.api_command:
            token = Path(args.token_file).read_text().strip() if args.token_file else os.environ.get(
                'AIMEE_KB_API_BEARER_TOKEN', '')
            if not token:
                fail('a KB import needs its API bearer token: set AIMEE_KB_API_BEARER_TOKEN or pass --token-file')
            self.headers['Authorization'] = 'Bearer ' + token

    def call(self, verb: str, body: dict) -> tuple[int, dict]:
        if self.api_command:
            result = subprocess.run(shlex.split(self.api_command), input=json.dumps({'verb': verb, 'body': body}),
                                    text=True, capture_output=True, timeout=180)
            if result.returncode:
                fail('local API command failed; original data retained')
            response = json.loads(result.stdout)
            return response['status'], response['body']
        if self.placement == 'server':
            connection: http.client.HTTPConnection = LocalConnection(self.socket)
        else:
            host, _, port = self.address.rpartition(':')
            connection = http.client.HTTPConnection(host, int(port), timeout=120)
        try:
            route = '/v1/commands/memory.screen_content' if self.placement == 'server' and verb == 'screen_content' else ROUTES[self.placement] + verb
            connection.request('POST', route, json.dumps(body), self.headers)
            response = connection.getresponse()
            raw = response.read()
        finally:
            connection.close()
        try:
            return response.status, json.loads(raw)
        except ValueError:
            return response.status, {'status': 'error', 'message': raw.decode(errors='replace')[:200]}

    def require_ready(self) -> None:
        body = {'store': 'user', 'limit': 1} if self.placement == 'server' else {'limit': 1}
        status, reply = self.call('list', body)
        if status != 200 or not isinstance(reply.get('memories'), list):
            fail(f'the instance cannot serve memory (HTTP {status}); '
                 'install and select a memory implementation and wait for readiness before importing')


def load(path: Path, placement: str) -> list[dict]:
    records = []
    for number, line in enumerate(path.read_text().splitlines(), 1):
        if not line.strip():
            continue
        record = json.loads(line)
        if type(record.get('id')) is not int or record['id'] <= 0 or type(record.get('record_revision')) is not int or record['record_revision'] <= 0:
            fail(f'{path}:{number} has an invalid memory identity or revision')
        if any(name not in record for name in FIELDS):
            fail(f'{path}:{number} is not an exported memory record')
        if record['placement'] != placement:
            fail(f'{path}:{number} was exported from a {record["placement"]} store, not a {placement} store')
        records.append(record)
    return records


def store_body(record: dict) -> dict | None:
    """The public store request for one exported record, or None for a scope this tool cannot place."""
    body = {'key': record['key'], 'content': record['content'], 'kind': record['kind'], 'tier': record['tier'],
            'confidence': record['confidence'],
            'authority': 'user' if record['provenance_category'] in USER_AUTHORITY else 'model',
            'idempotency_key': f'upgrade-1.0-{record["placement"]}-{record["id"]}-{record["record_revision"]}'}
    for field in ('valid_from', 'valid_until'):
        if record.get(field):
            body[field] = record[field]
    if record['placement'] == 'server':
        body['store'] = 'user'
        return body
    if record.get('use_cases'):
        body['use_cases'] = record['use_cases']
    scope = record['scope_type']
    if scope == 'global':
        body['scope'] = 'all'
    elif scope in ('project', 'workspace'):
        body[scope] = record['scope_value']
    else:
        return None
    return body


def import_(args: argparse.Namespace) -> int:
    records = load(Path(args.file), args.placement)
    target = Target(args)
    target.require_ready()
    if not args.dry_run:
        if not args.psql:
            fail('import needs --psql for the retained source store; PII custody includes its history')
        try:
            records, protected_fields = memory_custody.protect_source(args.psql, args.placement, target, records, args.file, export_sql=EXPORT_SQL[args.placement])
        except (ValueError, OSError) as error:
            fail(str(error) if isinstance(error, ValueError) else 'retained source protection unavailable')
        print(f'retained source custody complete: {protected_fields} payload fields protected; snapshot protected')
    stored = replayed = failed = unplaced = 0
    for record in records:
        body = store_body(record)
        label = f'memory {record["id"]}'
        if body is None:
            unplaced += 1
            print(f'NOT IMPORTED {label}: scope has no '
                  'project, workspace or global placement', file=sys.stderr)
            continue
        if args.dry_run:
            stored += 1
            continue
        status, reply = target.call('store', body)
        if status == 200 and reply.get('status') == 'ok':
            if (reply.get('mutation_receipt') or {}).get('replayed') is True:
                replayed += 1
            else:
                stored += 1
        else:
            failed += 1
            print(f'FAILED {label}: HTTP {status}; memory API refused the protected record',
                  file=sys.stderr)
    verb = 'would store' if args.dry_run else 'stored'
    print(f'{verb} {stored}, already imported {replayed}, failed {failed}, not placeable {unplaced} '
          f'of {len(records)} exported records')
    return 1 if failed or unplaced else 0


def shared_scopes(records: list[dict]) -> list[str]:
    """AIMEE_MEMORY_SHARED_SCOPES entries a KB needs to accept every placeable record of a snapshot."""
    scopes = []
    for record in records:
        if record['scope_type'] in ('project', 'workspace') and record['scope_value']:
            entry = f'{record["scope_type"]}:{record["scope_value"]}'
            if entry not in scopes:
                scopes.append(entry)
    return ['global'] + sorted(scopes)


def scopes(args: argparse.Namespace) -> int:
    print(','.join(shared_scopes(load(Path(args.file), 'kb'))))
    return 0


def rpc_call(args):
    request = json.load(sys.stdin)
    if set(request) != {'verb', 'body'} or request['verb'] not in ('list', 'store', 'screen_content'):
        fail('unsupported local API operation')
    status, body = Target(args).call(request['verb'], request['body'])
    print(json.dumps({'status': status, 'body': body}))
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest='command', required=True)
    out = commands.add_parser('export', help='write retained 1.0.x memories to a private snapshot')
    out.add_argument('--placement', choices=('server', 'kb'), required=True)
    out.add_argument('--file', required=True, help='new JSON Lines snapshot to create (never overwritten)')
    out.add_argument('--psql', default='psql',
                     help='psql command for this instance\'s store, for example '
                          '"docker compose exec -T aimee-store-db psql -U postgres -d aimee_store". '
                          'The default uses the normal PG* environment.')
    out.set_defaults(run=export)
    need = commands.add_parser('scopes', help='print the AIMEE_MEMORY_SHARED_SCOPES value a KB snapshot needs')
    need.add_argument('--file', required=True, help='KB snapshot written by export')
    need.set_defaults(run=scopes)
    into = commands.add_parser('import', help='protect retained legacy payloads and import active records')
    into.add_argument('--placement', choices=('server', 'kb'), required=True)
    into.add_argument('--file', required=True, help='snapshot written by export')
    into.add_argument('--socket', default='/var/lib/aimee/aimee-http.sock', help='Server local HTTP socket')
    into.add_argument('--address', default='127.0.0.1:8741', help='KB loopback HTTP address')
    into.add_argument('--token-file', help='file holding the KB API bearer token '
                                           '(default: AIMEE_KB_API_BEARER_TOKEN)')
    into.add_argument('--psql', help='owner psql command for the same retained legacy store used by export')
    into.add_argument('--api-command', help='argv command for the local rpc subcommand inside the application container')
    into.add_argument('--dry-run', action='store_true', help='report what would be stored without writing')
    into.set_defaults(run=import_)
    rpc = commands.add_parser('rpc', help='local API transport for an operator-run import')
    rpc.add_argument('--placement', choices=('server', 'kb'), required=True)
    rpc.add_argument('--socket', default='/var/lib/aimee/aimee-http.sock')
    rpc.add_argument('--address', default='127.0.0.1:8741')
    rpc.add_argument('--token-file')
    rpc.set_defaults(run=rpc_call)
    args = parser.parse_args()
    return args.run(args)


if __name__ == '__main__':
    sys.exit(main())
