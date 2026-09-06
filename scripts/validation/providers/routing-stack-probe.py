#!/usr/bin/env python3
"""Destructive routing exploration for an OWNED scratch local-stack-e2e instance.

Run only via AIMEE_E2E_PROBE_SCRIPT in a disposable installation: this replaces
its model roster. Native server, module bus, provider manager, enrollment and
PostgreSQL are real; only the external completion endpoint is deterministic.
"""
import importlib.util
import json
import os
from pathlib import Path
import threading
import subprocess
import signal
import ssl
import urllib.request
import urllib.error
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

ROOT = Path(os.environ['SCRATCH'])
WORKSPACE = ROOT / 'routing-workspace'
RECORDS = []
CALLS = []


class Provider(BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        CALLS.append({'model': body.get('model'), 'path': self.path})
        data = {'id': 'routing-fixture', 'object': 'chat.completion', 'model': body['model'],
                'choices': [{'index': 0, 'message': {'role': 'assistant', 'content': 'ok'}, 'finish_reason': 'stop'}],
                'usage': {'prompt_tokens': 4, 'completion_tokens': 1, 'total_tokens': 5}}
        raw = json.dumps(data).encode()
        self.send_response(200)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', str(len(raw)))
        self.end_headers()
        self.wfile.write(raw)


def model(name, score=80, price=None, tier=1):
    m = dict(name=name, model='fixture-' + name, provider='openai', auth_type='none',
             endpoint='http://127.0.0.1:18765/v1', roles=['all'], enabled=True,
             tools_enabled=False, context_window=32768, max_output=4096,
             max_parallel=4, cost_tier=tier,
             competence={'summarize': {'score': score, 'source': 'operator: routing fixture, not a model evaluation'}})
    if price is not None:
        m.update(price_in_per_mtok=price, price_out_per_mtok=price)
    return m


def seed(models, threshold=70):
    tmp = ROOT / 'models.routing.tmp'
    tmp.write_text(json.dumps({'models': models, 'role_contracts': {'summarize': {'min_competence': threshold}}}))
    tmp.chmod(0o600)
    tmp.replace(ROOT / 'models.json')


def api(path, body):
    context = ssl.create_default_context(cafile=str(ROOT / 'tls/server.crt'))
    context.check_hostname = False  # Scratch cert names the guest, listener is loopback.
    context.load_cert_chain(os.environ['CLIENT_CERT'], os.environ['CLIENT_KEY'])
    req = urllib.request.Request(os.environ['SERVER_URL'] + path, json.dumps(body).encode(),
                                 {'Content-Type': 'application/json', 'Authorization': 'Bearer ' + os.environ['BEARER']})
    try:
        with urllib.request.urlopen(req, context=context, timeout=90) as response:
            raw = response.read()
    except urllib.error.HTTPError as exc:
        raise RuntimeError(f'{path}: HTTP {exc.code}: {exc.read().decode()}') from None
    return gate.result_object(raw.decode())


def check(name, expected=None, error=None, **overrides):
    before = len(CALLS)
    request = dict(role='summarize', persona='reviewer', prompt=f'Summarize this sentence: The sky is blue. Validation case: {name}.', tools=False,
                   max_tokens=64, scope='bounded', timeout_ms=30000, handoff_json=False, cwd=str(WORKSPACE))
    request.update(overrides)
    result = gate.await_result(api, api('/v1/delegate/run', request))
    if expected:
        assert result.get('status') == 'ok' and result.get('agent') == expected, (name, result)
        assert len(CALLS) > before and CALLS[-1]['model'] == 'fixture-' + expected, (name, CALLS[before:])
    else:
        assert result.get('status') == 'error' and not result.get('agent'), (name, result)
        assert error in str(result.get('error', result.get('message', ''))), (name, result)
        assert len(CALLS) == before, (name, 'refused request reached vendor')
    RECORDS.append({'case': name, 'agent': result.get('agent'), 'passed': True})
    print('PASS', name, flush=True)


spec = importlib.util.spec_from_file_location('gate', Path(__file__).with_name('live-routing.py'))
gate = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gate)
server = ThreadingHTTPServer(('127.0.0.1', 18765), Provider)
threading.Thread(target=server.serve_forever, daemon=True).start()
try:
    WORKSPACE.mkdir()
    (WORKSPACE / 'README.md').write_text('The sky is blue.\n')
    for args in [['init', '-q'], ['add', '.'], ['-c', 'user.name=RoutingValidation', '-c', 'user.email=validation@localhost', 'commit', '-qm', 'fixture']]:
        subprocess.run(['git', '-C', str(WORKSPACE), *args], check=True)
    registered = api('/v1/workspaces', {'root_hint': str(WORKSPACE), 'provider': 'shared'})
    assert registered.get('status') == 'ok', registered
    seed([model('unknown', tier=0), model('paid', price=2), model('free', price=0, tier=5)])
    check('explicit free beats unknown and paid', 'free')
    seed([model('unknown', tier=0), model('paid', price=2)])
    check('known paid beats unknown price', 'paid')
    seed([model('cheap', price=0.2, tier=5), model('expensive', price=4, tier=1)])
    check('estimated spend beats legacy tier', 'cheap')
    seed([model('weak', score=69, price=0), model('qualified', score=70, price=2)])
    check('threshold excludes cheaper weak model', 'qualified')
    seed([model('weak', score=69, price=0)])
    check('all below threshold refuses before dispatch', error='competence contract')
    unknown = model('unassessed', price=0)
    del unknown['competence']
    seed([unknown])
    check('wildcard role cannot bypass missing assessment', error='competence contract')
    seed([model('qualified', price=0)])
    check('model override cannot inherit assessment', error='competence', model='unassessed-replacement')
    result = api('/v1/model/set', {'args': ['qualified', '--model', 'replacement']})
    assert result.get('status') == 'ok', result
    check('persisted model replacement invalidates assessment', error='competence contract')
    seed([model('zeta', price=0), model('alpha', price=0)])
    check('equal-cost selection is stable by name', 'alpha')
    seed([model('alpha', price=0), model('zeta', price=0)])
    check('roster order cannot change tie winner', 'alpha')
    # Suspend only this scratch instance's routing process; restore it even if
    # refusal assertions fail, so the surrounding harness can cleanly stop it.
    pids = []
    for entry in Path('/proc').glob('[0-9]*/cmdline'):
        try:
            args = entry.read_bytes().split(b'\0')
        except (FileNotFoundError, PermissionError):
            continue
        if args[0].endswith(b'/aimee-module-routing') and str(ROOT).encode() in b' '.join(args):
            pids.append(int(entry.parent.name))
    assert len(pids) == 1, pids
    os.kill(pids[0], signal.SIGSTOP)
    try:
        check('routing process timeout refuses without vendor call', error='routing module unavailable')
    finally:
        os.kill(pids[0], signal.SIGCONT)
    check('routing process resumes selection after recovery', 'alpha')
finally:
    server.shutdown()
    (Path(os.environ['RUN_ROOT']) / 'routing-results.json').write_text(json.dumps({'checks': RECORDS, 'vendor_calls': CALLS}, indent=2))
