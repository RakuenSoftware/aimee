#!/usr/bin/env python3
"""Ordinary Server/KB PII writes must establish Vault custody and safe references.

Only owned disposable deployment-matrix stacks. Evidence never includes raw
values, private keys, plaintext request frames or bearer capabilities.
"""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import secrets
import subprocess
import sys
import uuid

ROOT = Path(__file__).resolve().parents[2]


def load(name, file):
    spec = importlib.util.spec_from_file_location(name, ROOT / 'tests/e2e' / file)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


VAULT = '''import base64,json,sys
from pathlib import Path
from cryptography.hazmat.primitives import hashes
from cryptography.hazmat.primitives.kdf.hkdf import HKDF
from cryptography.hazmat.primitives.keywrap import aes_key_unwrap
from cryptography.hazmat.primitives.ciphers.aead import AESGCM
a=json.load(sys.stdin);root=Path(a['vault_root'])
kek=HKDF(algorithm=hashes.SHA256(),length=32,salt=b'aimee-svrkek-v1\\0',info=b'aimee-vault-kek-v1').derive((root/'.server-master.key').read_bytes())
def dec(s):return base64.urlsafe_b64decode(s+'='*((-len(s))%4))
hits=set();failed=0;good=0
for p in root.glob('*.json'):
 v=json.loads(p.read_text());principal=v['principal']
 for e in v.get('creds',[]):
  if 'wrapped_dek_server' not in e:continue
  try:
   dek=aes_key_unwrap(kek,dec(e['wrapped_dek_server']));aad=(principal+'|'+e['agent']+'|'+e['cred']).encode()
   if e.get('immutable_generation_v1'):aad=b'vault-immutable-v1|'+e['immutable_generation_v1'].encode()+b'|'+aad
   clear=AESGCM(dek).decrypt(dec(e['nonce']),dec(e['ciphertext'])+dec(e['tag']),aad);good+=1
   for row in a['canaries']:
    if row['value'].encode() in clear:hits.add(row['id'])
  except Exception:failed+=1
print(json.dumps(dict(decrypted=good,failed=failed,canary_ids=sorted(hits),plaintext_exported=False)))
'''
SCAN = '''import json,sys
from pathlib import Path
a=json.load(sys.stdin);hits=set();errors=0;files=0
forms=[(r['id'],[r['value'].encode(),json.dumps(r['value'],ensure_ascii=True)[1:-1].encode()]) for r in a['canaries']]
for p in Path('/var/lib/aimee').rglob('*'):
 try:
  if p.is_symlink() or not p.is_file():continue
  files+=1;tail=b''
  with p.open('rb') as f:
   while chunk:=f.read(1024*1024):
    block=tail+chunk
    for label,values in forms:
     if any(v and v in block for v in values):hits.add(label)
    tail=block[-4096:]
 except OSError:errors+=1
print(json.dumps(dict(files=files,errors=errors,raw_canary_ids=sorted(hits))))
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--role', choices=('server', 'kb', 'both'), default='both')
    parser.add_argument('--keep', action='store_true')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    matrix = load('matrix', 'deployment-matrix.py')
    typed = load('typed', 'kb-typed-facts-e2e.py')
    tag = uuid.uuid4().hex
    values = [
        ('email', 'contact-' + tag + '@example.invalid'),
        ('email_plus', 'contact+' + tag + '@example.invalid'),
        ('phone_international', '+31 20 555 01 47'),
        ('phone_parentheses', '+1 (202) 555-0147'),
        ('ipv4_public', '203.0.113.147'), ('ipv4_private', '10.83.47.219'),
        ('ipv4_private_172', '172.19.47.219'), ('ipv6', '2001:db8:' + tag[:4] + ':' + tag[4:8] + '::147'),
        ('ssn', '847-39-2614'), ('dob', 'date_of_birth=1988-03-17'),
        ('password', 'password=' + secrets.token_hex(20)),
        ('github_token', 'ghp_' + secrets.token_hex(20)),
        ('aws_key', 'AKIA' + ''.join(secrets.choice('ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789') for _ in range(16))),
        ('private_key', '-----BEGIN RSA PRIVATE KEY-----\n' + secrets.token_hex(24) + '\n-----END RSA PRIVATE KEY-----'),
        ('payment_card', '4111 1111 1111 1111'),
        ('postal_address', '147 Canary Orchard Lane, Testville'),
        ('person_name', 'Morgan ' + tag[:8] + ' Canary'),
        ('passport_label', 'passport_number=QA' + tag[:9].upper()),
    ]
    canaries = [dict(id=name, value=value) for name, value in values]
    checks, observations, stacks = [], {}, []

    def check(name, passed, **details):
        checks.append(dict(name=name, passed=bool(passed), **details))
        print(('PASS ' if passed else 'FAIL ') + name, flush=True)
        if not passed:
            raise RuntimeError(name)

    try:
        roles = ('server', 'kb') if args.role == 'both' else (args.role,)
        for role in roles:
            canaries = [dict(id=name, value=value) for name, value in values]
            env = dict(os.environ, AIMEE_RUNTIME_WEB_ENABLED='0', COMPOSE_PROFILES='',
                       EMBEDDER_MODEL='bekko-a25m', EMBEDDER_DIMS='384',
                       EMBEDDER_URL='https://aimee-embedder:8762')
            stack = matrix.Stack(role, env, args.output)
            stacks.append(stack)
            stack.start()

            def request(verb, body):
                return typed.server_request(stack, '/v1/memory/' + verb, body) if role == 'server' else stack.kb_request('/v1/actions/memory.' + verb, body)

            status, control = request('store', dict(key='privacy-control-' + tag,
                content='Ordinary retained control 界 🦊', idempotency_key=str(uuid.uuid4())))
            check(role + ' ordinary control remains admitted', status == 200 and control.get('status') == 'ok')
            control_id = control['id']
            before = request('get', dict(id=str(control_id)))[1]['memory']
            for label, value in values:
                status, result = request('store', dict(key='privacy-' + tag + '-' + label,
                    content='Privacy fixture ' + tag + ' ' + label + ' ' + value,
                    idempotency_key=str(uuid.uuid4())))
                check(role + ' ' + label + ' accepted with a memory reference',
                      status == 200 and result.get('status') == 'ok', http_status=status)
                status, result = request('get', dict(id=str(result['id'])))
                memory = result.get('memory', {})
                check(role + ' ' + label + ' memory holds an opaque locator without raw PII',
                      status == 200 and '[[vault:v1:' in memory.get('content', '') and
                      value not in json.dumps(memory, ensure_ascii=False))
            if role == 'server':
                status, screened = typed.server_request(stack, '/v1/commands/memory.screen_content',
                                                       dict(content=values[0][1]))
                screened = screened.get('result', screened)
            else:
                status, screened = request('screen_content', dict(content=values[0][1]))
            check(role + ' screening returns the protected text for callers to retain',
                  status == 200 and screened.get('status') == 'ok' and
                  screened.get('verdict') == 'redact' and
                  '[[vault:v1:' in screened.get('redacted', '') and
                  values[0][1] not in json.dumps(screened))
            if role == 'server':
                for field in ('key', 'session_id', 'source_session', 'use_cases'):
                    value = 'metadata-' + field + '-' + tag + '@example.invalid'
                    canaries.append(dict(id='metadata_' + field, value=value))
                    body = dict(key='metadata-' + field + '-' + tag,
                                content='Ordinary metadata control ' + tag, idempotency_key=str(uuid.uuid4()))
                    body[field] = value
                    status, result = request('store', body)
                    check(role + ' ' + field + ' metadata is admitted through Vault',
                          status == 200 and result.get('status') == 'ok', http_status=status)
            mounts = json.loads(matrix.command('docker', 'inspect', stack.application))[0]['Mounts']
            home = next(row['Source'] for row in mounts if row['Destination'] == '/var/lib/aimee')
            if role == 'server':
                probe = (ROOT / 'tests/e2e/privacy-provider-probe.py').read_text()
                observed = json.loads(matrix.command('docker', 'exec', '-i', '-u', '1000',
                    '-e', 'AIMEE_PRIVACY_PROVIDER_FIXTURE=1', stack.application, 'python3', '-c', probe,
                    data=json.dumps(dict(canaries=canaries,
                        input='Privacy prompt ' + values[0][1],
                        instructions='Privacy instruction ' + values[10][1] + ' ' + values[3][1]))))
                check('server actual provider receives a completed protected prompt',
                      observed.get('run_status') == 'completed' and bool(observed['provider_requests']))
                check('server actual provider request contains no submitted raw PII',
                      all(not row['canary_ids'] for row in observed['provider_requests']))
                observations['provider'] = observed
            custody = json.loads(matrix.command(sys.executable, '-c', VAULT,
                       data=json.dumps(dict(canaries=canaries, vault_root=str(Path(home) / '.vault')))))
            check(role + ' actual encrypted Vault contains every submitted PII canary',
                  custody['failed'] == 0 and set(custody['canary_ids']) == {r['id'] for r in canaries})
            check(role + ' control content and exact version remain unchanged',
                  request('get', dict(id=str(control_id)))[1].get('memory') == before)
            scan = json.loads(matrix.command('docker', 'exec', '-i', stack.application,
                   'python3', '-c', SCAN, data=json.dumps(dict(canaries=canaries))))
            check(role + ' persisted files and capture contain no raw submitted canary',
                  scan['errors'] == 0 and not scan['raw_canary_ids'], files_scanned=scan['files'])
            sql = matrix.command('docker', 'exec', stack.postgres,
                                 'pg_dump', '-U', 'postgres', '-d', 'aimee_store', '--no-owner', '--no-privileges')
            check(role + ' SQL persistence contains no raw submitted canary',
                  not any(row['value'] in sql for row in canaries))
            observations[role] = dict(vault=custody, raw_scan=scan)
            if args.keep:
                # Private replay state stays on the disposable controller. It
                # is deliberately separate from exportable verdict evidence.
                private = args.output / ('.private-' + role + '.json')
                fd = os.open(private, os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o600)
                with os.fdopen(fd, 'w') as stream:
                    json.dump(dict(canaries=canaries, control_id=control_id, control=before,
                        stack={k: str(v) if isinstance(v, Path) else v
                               for k, v in stack.__dict__.items()}), stream)
    except (RuntimeError, ValueError, KeyError, TypeError, OSError, subprocess.SubprocessError) as error:
        checks.append(dict(name='privacy qualification completed', passed=False, error=type(error).__name__))
        print('FAIL privacy qualification: ' + str(error), flush=True)
    finally:
        (args.output / 'privacy-custody.json').write_text(json.dumps(dict(checks=checks, observations=observations), indent=2) + '\n')
        if not args.keep:
            for stack in reversed(stacks):
                stack.compose('down', '--volumes', '--remove-orphans')
    return 0 if checks and all(row['passed'] for row in checks) else 1


if __name__ == '__main__':
    raise SystemExit(main())
