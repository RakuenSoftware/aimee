#!/usr/bin/env python3
"""Exercise content authorization on an explicitly owned MR-07 test topology.

The caller provisions the real enrolled certificate, team memberships and code
project. This gate temporarily removes the service membership and changes the
registered fingerprint, restoring both in finally blocks. Never use production.
"""
import argparse
import json
from pathlib import Path
import re
import subprocess

HTTP = '''import http.client,json,socket,sys
class C(http.client.HTTPConnection):
 def connect(self):
  self.sock=socket.socket(socket.AF_UNIX);self.sock.settimeout(70)
  self.sock.connect('/var/lib/aimee/aimee-http.sock')
c=C('localhost');c.request('POST','/v1/index/investigate',sys.stdin.read(),{'Content-Type':'application/json'})
r=c.getresponse();print(json.dumps([r.status,json.loads(r.read())]))
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('server', 'kb-store-db', 'server-id', 'project', 'cwd', 'query', 'output'):
        parser.add_argument('--' + name, required=True)
    parser.add_argument('--team', type=int, required=True)
    args = parser.parse_args()
    if not re.fullmatch(r'aimee-e2e-server-[a-f0-9]+-aimee-server-1', args.server):
        parser.error('an owned deployment-matrix Server container is required')
    if not re.fullmatch(r'aimee-e2e-kb-[a-f0-9]+-aimee-store-db-1', args.kb_store_db):
        parser.error('an owned deployment-matrix KB database is required')
    if not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9._-]{0,126}', args.server_id) or args.team < 1:
        parser.error('invalid fixture binding')
    checks = []

    def check(name, ok):
        checks.append(dict(name=name, passed=bool(ok)))
        Path(args.output).write_text(json.dumps(checks, indent=2) + '\n')
        if not ok:
            raise RuntimeError(name)

    def sql(query):
        return subprocess.check_output(['docker', 'exec', args.kb_store_db, 'psql',
            '-U', 'postgres', '-d', 'aimee_store', '-X', '-qAt', '-v', 'ON_ERROR_STOP=1',
            '-c', query], text=True, stderr=subprocess.PIPE).strip()

    def request():
        body = dict(query=args.query, project=args.project, cwd=args.cwd,
                    include_code=True, fallback=False)
        result = subprocess.check_output(['docker', 'exec', '-i', '-u', '1000',
            args.server, 'python3', '-c', HTTP], input=json.dumps(body), text=True,
            stderr=subprocess.PIPE, timeout=90)
        return json.loads(result)

    selector = f"server_id='{args.server_id}' AND team_id={args.team}"
    fingerprint = sql('SELECT client_fingerprint FROM kb_server_registry WHERE ' + selector)
    check('fixture binds an actual enrolled fingerprint', bool(re.fullmatch('[a-f0-9]{64}', fingerprint)))
    membership = sql(f"SELECT is_default FROM kb_team_membership WHERE identity_key='aimee-server' AND team={args.team}")
    check('fixture has exactly one service membership', membership in ('0', '1'))
    check('verified service and caller reach the code index', request()[0] == 200)
    try:
        sql(f"DELETE FROM kb_team_membership WHERE identity_key='aimee-server' AND team={args.team}")
        check('certificate alone cannot replace service team membership', request()[0] == 403)
    finally:
        sql(f"INSERT INTO kb_team_membership(identity_key,team,is_default) VALUES('aimee-server',{args.team},{membership}) ON CONFLICT(identity_key,team) DO UPDATE SET is_default=EXCLUDED.is_default")
    check('restored membership permits the same authenticated request', request()[0] == 200)
    try:
        wrong = ('0' if fingerprint[0] != '0' else '1') + fingerprint[1:]
        sql(f"UPDATE kb_server_registry SET client_fingerprint='{wrong}' WHERE " + selector)
        check('service membership cannot replace the exact enrolled certificate', request()[0] == 403)
    finally:
        sql(f"UPDATE kb_server_registry SET client_fingerprint='{fingerprint}' WHERE " + selector)
    check('restored certificate binding permits the same request', request()[0] == 200)
    print(f'{len(checks)} content service-scope checks passed')


if __name__ == '__main__':
    main()
