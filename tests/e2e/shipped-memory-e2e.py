#!/usr/bin/env python3
"""Memory release gate for the shipped implementation, through public surfaces only.

Runs against a disposable Docker stack started by deployment-matrix.py. Every
check goes through the Server's HTTP socket or the KB's authenticated action
API, never through a database: the shipped implementation keeps its records in
its own store, and what an operator can rely on is the public contract.

Personal (Server): store with a durable receipt, idempotent replay, get, search,
recall, refusal of a raw sensitive value, delete. Shared (KB, when given):
global, project and workspace placement, scope isolation between projects,
statistics and hygiene over the stored rows.
"""
import argparse
import json
import os
from pathlib import Path
import subprocess
import uuid

SERVER_HTTP = '''import http.client,socket,json,sys
class C(http.client.HTTPConnection):
 def connect(self):
  self.sock=socket.socket(socket.AF_UNIX);self.sock.settimeout(70);self.sock.connect('/var/lib/aimee/aimee-http.sock')
a=json.load(sys.stdin);c=C('localhost',timeout=70)
c.request(a['method'],a['path'],json.dumps(a['body']),{'Content-Type':'application/json'})
r=c.getresponse();print(json.dumps([r.status,json.loads(r.read() or b'{}')]))
'''
KB_HTTP = '''import http.client,json,sys
token,path,body=sys.stdin.read().split('\\n',2)
c=http.client.HTTPConnection('127.0.0.1',8741,timeout=70)
c.request('POST',path,body,{'Content-Type':'application/json','Authorization':'Bearer '+token})
r=c.getresponse();print(json.dumps([r.status,json.loads(r.read() or b'{}')]))
'''


class Gate:
    def __init__(self, args):
        self.args = args
        self.tag = uuid.uuid4().hex[:10]
        self.checks = []

    def check(self, name, passed, observed=None):
        row = dict(name=name, passed=bool(passed))
        if not passed and observed is not None:
            # Fixed diagnostic fields only: status, kind and error class.
            reply = observed[1] if isinstance(observed, list) and len(observed) == 2 else {}
            row['observed'] = dict(http_status=observed[0] if isinstance(observed, list) else None,
                                   status=reply.get('status') if isinstance(reply, dict) else None,
                                   kind=reply.get('kind') if isinstance(reply, dict) else None)
        self.checks.append(row)
        print(('PASS ' if passed else 'FAIL ') + name, flush=True)

    def docker_json(self, container, script, data):
        result = subprocess.run(['docker', 'exec', '-i', '-u', '1000', container, 'python3', '-c', script],
                                input=data, capture_output=True, text=True, timeout=90)
        try:
            return json.loads(result.stdout.strip().splitlines()[-1])
        except (IndexError, ValueError):
            return [0, {}]

    def server(self, path, body=None, method='POST'):
        return self.docker_json(self.args.server, SERVER_HTTP,
                                json.dumps(dict(method=method, path='/v1/memory/' + path, body=body or {})))

    def kb(self, action, body):
        return self.docker_json(self.args.kb, KB_HTTP,
                                self.args.kb_token + '\n/v1/actions/' + action + '\n' + json.dumps(body))

    @staticmethod
    def ok(reply):
        return reply[0] == 200 and isinstance(reply[1], dict) and reply[1].get('status') == 'ok'

    def personal(self):
        key, word = 'shipped-' + self.tag, 'KESTREL' + self.tag.upper()
        content = 'Personal shipped-memory fixture ' + word + ' 🦊'
        request = dict(key=key, content=content, idempotency_key='shipped-e2e-' + self.tag)
        stored = self.server('store', request)
        receipt = stored[1].get('mutation_receipt') or {}
        self.check('personal store returns a durable receipt', self.ok(stored) and receipt.get('outcome') == 'stored'
                   and receipt.get('replayed') is False, stored)
        replay = self.server('store', request)
        self.check('personal store replay names the original commit', self.ok(replay)
                   and (replay[1].get('mutation_receipt') or {}).get('replayed') is True
                   and (replay[1].get('mutation_receipt') or {}).get('commit_id') == receipt.get('commit_id'), replay)
        record = stored[1].get('id') or (stored[1].get('memory') or {}).get('id')
        got = self.server('get', dict(id=record))
        self.check('personal get returns the stored content', self.ok(got)
                   and (got[1].get('memory') or {}).get('content') == content, got)
        found = self.server('search', dict(keywords=[word], limit=5))
        self.check('personal search finds the record', self.ok(found) and word in json.dumps(found[1]), found)
        recalled = self.server('recall', dict(query=word))
        self.check('personal recall includes the record', self.ok(recalled) and word in json.dumps(recalled[1]), recalled)
        refused = self.server('store', dict(key=key + '-contact', content='My address is person@example.invalid'))
        message = refused[1].get('message', '') if isinstance(refused[1], dict) else ''
        self.check('a raw sensitive value is refused and pointed to the vault',
                   refused[0] == 400 and refused[1].get('kind') == 'invalid_argument' and 'vault' in message, refused)
        searched = self.server('search', dict(keywords=['person@example.invalid'], limit=5))
        self.check('a refused sensitive value is not stored', 'person@example.invalid' not in json.dumps(searched[1]),
                   searched)
        deleted = self.server('delete', dict(id=record))
        self.check('personal delete succeeds', self.ok(deleted), deleted)
        after = self.server('search', dict(keywords=[word], limit=5))
        self.check('a deleted record is no longer found', word not in json.dumps(after[1]), after)

    def shared(self):
        word = 'OSPREY' + self.tag.upper()
        global_reply = self.kb('memory.store', dict(key='g-' + self.tag, content='global fact ' + word + 'G'))
        self.check('KB stores a global fact', self.ok(global_reply), global_reply)
        apollo = 'apollo-' + self.tag
        project = self.kb('memory.store', dict(key='p-' + self.tag, content='project fact ' + word + 'P',
                                               project=apollo, scope_context=True))
        self.check('KB stores a project fact', self.ok(project), project)
        workspace = self.kb('memory.store', dict(key='w-' + self.tag, content='workspace fact ' + word + 'W',
                                                 workspace='/srv/' + self.tag, scope_context=True))
        self.check('KB stores a workspace fact', self.ok(workspace), workspace)
        inside = self.kb('memory.search', dict(keywords=[word + 'P'], limit=5, project=apollo, scope_context=True))
        self.check('the project sees its own fact', word + 'P' in json.dumps(inside[1]), inside)
        outside = self.kb('memory.search', dict(keywords=[word + 'P'], limit=5, project='other-' + self.tag,
                                                scope_context=True))
        self.check('another project does not see it', self.ok(outside) and word + 'P' not in json.dumps(outside[1]),
                   outside)
        shared = self.kb('memory.search', dict(keywords=[word + 'G'], limit=5, project='other-' + self.tag,
                                               scope_context=True))
        self.check('every project sees the global fact', word + 'G' in json.dumps(shared[1]), shared)
        stats = self.kb('memory.stats', dict(view='console'))
        self.check('KB statistics count the stored facts', self.ok(stats)
                   and stats[1].get('stats', {}).get('total', 0) >= 1, stats)
        hygiene = self.kb('memory.hygiene', dict(dry_run=True, scope=dict(type='global', value='_global'), max_rows=16))
        self.check('KB hygiene compares the stored global rows', self.ok(hygiene)
                   and hygiene[1].get('rows_compared', 0) >= 1 and hygiene[1].get('canonical_writes') == 0, hygiene)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--server', required=True)
    parser.add_argument('--kb')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    # The bearer arrives in the environment, never on a command line.
    args.kb_token = os.environ.get('AIMEE_E2E_KB_BEARER', '')
    if args.kb and not args.kb_token:
        parser.error('--kb requires AIMEE_E2E_KB_BEARER')
    gate = Gate(args)
    try:
        gate.personal()
        if args.kb:
            gate.shared()
    finally:
        args.output.write_text(json.dumps(gate.checks, indent=2) + '\n')
    return 0 if gate.checks and all(row['passed'] for row in gate.checks) else 1


if __name__ == '__main__':
    raise SystemExit(main())
