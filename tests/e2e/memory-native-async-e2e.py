#!/usr/bin/env python3
"""Validate real asynchronous native memory execution in an owned test Server.

Uses a local synthetic provider, replaces/restores the disposable model roster,
and pauses/restarts only the Server's Go memory process. Results omit prompts,
credentials and provider headers. Never run against a user's installation.
"""
import argparse
import http.client
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import signal
import socket
import subprocess
import tempfile
import threading
import time
import uuid


def strings(value):
    if isinstance(value, str):
        yield value
    elif isinstance(value, dict):
        for child in value.values():
            yield from strings(child)
    elif isinstance(value, list):
        for child in value:
            yield from strings(child)


def inside(output):
    if os.environ.get('AIMEE_NATIVE_ASYNC_FIXTURE') != '1':
        raise RuntimeError('requires the disposable-container launcher')
    checks, captures, runs, provider_errors = [], [], [], []
    lock = threading.Lock()
    prefix = 'native-memory-' + uuid.uuid4().hex[:10]
    content = 'Complete native Go memory fixture 界🦊; preserve LIMIT_7 and identifier ' + prefix
    owner_pid, memory_id = None, None
    refresh_id = None
    refreshed_content = 'New memory committed before turn-six context refresh 界🦊 ' + prefix
    scenario, scenario_start = 'single', 0
    previous_recall = None
    fixture_files = tempfile.TemporaryDirectory(prefix=prefix + '-')
    for turn in range(1, 6):
        (Path(fixture_files.name) / f'{turn}.txt').write_text(f'Native refresh evidence {turn}: {prefix}\n')

    def check(name, passed):
        checks.append(dict(name=name, passed=bool(passed)))
        print(('PASS ' if passed else 'FAIL ') + name, flush=True)
        if not passed:
            raise RuntimeError(name)

    class Local(http.client.HTTPConnection):
        def connect(self):
            self.sock = socket.socket(socket.AF_UNIX)
            self.sock.settimeout(90)
            self.sock.connect('/var/lib/aimee/aimee-http.sock')

    def api(path, body=None, limits=None):
        conn = Local('localhost', timeout=90)
        try:
            conn.request('GET' if body is None else 'POST', path,
                         None if body is None else json.dumps(body),
                         {'Content-Type': 'application/json',
                          **({'X-Aimee-Context-Limits': json.dumps(limits)} if limits is not None else {})})
            response = conn.getresponse()
            raw = response.read()
            if response.getheader('Content-Type', '').startswith('text/event-stream'):
                return response.status, [json.loads(line[5:].strip()) for line in raw.decode().splitlines()
                    if line.startswith('data:') and line[5:].strip() != '[DONE]']
            return response.status, json.loads(raw)
        finally:
            conn.close()

    class Provider(BaseHTTPRequestHandler):
        def log_message(self, *_):
            pass

        def do_POST(self):
            nonlocal refresh_id
            raw = self.rfile.read(int(self.headers.get('Content-Length', '0')))
            body = json.loads(raw)
            with lock:
                captures.append(body)
                ordinal = len(captures) - scenario_start
            response = dict(id=prefix, object='chat.completion', model=body['model'],
                choices=[dict(index=0, message=dict(role='assistant', content='NATIVE_MEMORY_OK'),
                              finish_reason='stop')],
                usage=dict(prompt_tokens=11, completion_tokens=2, total_tokens=13))
            if scenario != 'single' and ordinal <= 5:
                # Distinct real reads avoid repeated-call detection. The fifth
                # reply is withheld until the memory state changes, so refresh
                # and provider dispatch cannot race the test's intervention.
                tool = dict(id=f'{prefix}-call-{ordinal}', type='function', function=dict(
                    name='read_file', arguments=json.dumps(dict(
                        path=str(Path(fixture_files.name) / f'{ordinal}.txt')))))
                response['choices'] = [dict(index=0, finish_reason='tool_calls',
                    message=dict(role='assistant', content=None, tool_calls=[tool]))]
                if ordinal == 5:
                    try:
                        if scenario == 'refresh-update':
                            status, stored = api('/v1/memory/store', dict(
                                key='identity:' + prefix + '-refresh', content=refreshed_content))
                            if status != 200 or stored.get('status') != 'ok':
                                raise RuntimeError('refresh identity was not committed')
                            refresh_id = stored['id']
                        elif scenario == 'refresh-outage':
                            os.kill(owner_pid, signal.SIGSTOP)
                    except Exception as exc:
                        provider_errors.append(type(exc).__name__ + ': ' + str(exc))
            data = json.dumps(response).encode()
            self.send_response(200)
            self.send_header('Content-Type', 'application/json')
            self.send_header('Content-Length', str(len(data)))
            self.end_headers()
            self.wfile.write(data)

    def run(name, limits=None, mode='single'):
        nonlocal scenario, scenario_start
        started, before = time.monotonic(), len(captures)
        scenario, scenario_start = mode, before
        status, created = api('/v1/runs', dict(model=prefix, input='Read the native memory fixture ' + prefix,
                                              max_output_tokens=32), limits)
        check(name + ' queues a real asynchronous worker', status == 200 and bool(created.get('id')))
        run_id = created['id']
        deadline = time.monotonic() + 90
        while time.monotonic() < deadline:
            status, result = api('/v1/runs/' + run_id)
            if status == 200 and result.get('status') in ('completed', 'failed', 'cancelled'):
                _, events = api('/v1/runs/' + run_id + '/events')
                runs.append(dict(name=name, run_id=run_id, status=result['status'],
                    elapsed_seconds=time.monotonic()-started, provider_requests=len(captures)-before,
                    refusal_kinds=[kind for kind in ('request_budget_exceeded', 'unavailable')
                                   if any(kind in text for text in strings(events))]))
                return result, events
            time.sleep(0.1)
        api('/v1/runs/' + run_id + '/stop', {})
        raise RuntimeError(name + ' did not reach a terminal state')

    def memory_owner():
        found = []
        for path in Path('/proc').iterdir():
            if not path.name.isdigit():
                continue
            try:
                if (path / 'cmdline').read_bytes().split(b'\0')[0].endswith(b'/aimee-module-memory'):
                    found.append(int(path.name))
            except OSError:
                pass
        check('native fixture identifies exactly one Go memory owner', len(found) == 1)
        return found[0]

    def recover():
        nonlocal owner_pid
        if owner_pid is not None:
            os.kill(owner_pid, signal.SIGCONT)
            os.kill(owner_pid, signal.SIGTERM)
            owner_pid = None
        deadline = time.monotonic() + 90
        while time.monotonic() < deadline:
            status, row = api('/v1/memory/get', dict(id=str(memory_id)))
            if status == 200 and row.get('memory', {}).get('content') == content:
                return
            time.sleep(0.5)
        raise RuntimeError('Go owner did not recover its committed memory')

    provider = ThreadingHTTPServer(('127.0.0.1', 0), Provider)
    threading.Thread(target=provider.serve_forever, daemon=True).start()
    roster = Path('/var/lib/aimee/models.json')
    previous = roster.read_bytes() if roster.exists() else None
    def config(*args):
        env = dict(os.environ, AIMEE_API_ENDPOINT='unix:/var/lib/aimee/aimee-http.sock')
        return json.loads(subprocess.check_output(['aimee', '--json', 'config', *args], env=env, text=True))

    try:
        # Exercise configured automatic recall without changing product defaults.
        previous_recall = config('get', 'memory_recall_enabled')['value']
        config('set', 'memory_recall_enabled', '1')
        check('native fixture explicitly enables automatic recall',
              config('get', 'memory_recall_enabled')['value'] == 1)
        # Role routing uses the default delegate; an explicit ingress model alone
        # does not select the worker's route. Isolate every route to this fixture.
        roster.write_text(json.dumps(dict(default_agent=prefix, default_delegate=prefix, fallback_chain=[],
            models=[dict(name=prefix, model='native-memory-fixture', provider='openai', auth_type='none',
                         endpoint=f'http://127.0.0.1:{provider.server_port}/v1', roles=['all'], enabled=True,
                         tools_enabled=True, context_window=32768, max_output=4096, max_parallel=1, max_turns=8)])))
        status, stored = api('/v1/memory/store', dict(key='identity:' + prefix, content=content))
        check('native fixture stores complete private identity', status == 200 and stored.get('status') == 'ok')
        memory_id = stored['id']
        before = len(captures)
        result, _ = run('healthy native run')
        check('healthy native run completes', result.get('status') == 'completed' and 'NATIVE_MEMORY_OK' in list(strings(result)))
        check('healthy native run dispatches exactly once', len(captures) == before + 1)
        check('native provider receives complete Go memory', any(content in text for text in strings(captures[-1])))
        before = len(captures)
        result, events = run('inherited zero byte cap', dict(schema_version=1, max_request_bytes=0))
        check('worker preserves inherited byte refusal', result.get('status') == 'failed' and
              any('request_budget_exceeded' in text for text in strings(events)))
        check('inherited byte refusal sends no provider request', len(captures) == before)
        owner_pid = memory_owner()
        os.kill(owner_pid, signal.SIGSTOP)
        try:
            before = len(captures)
            result, events = run('paused Go memory owner')
            check('native worker reports memory owner refusal', result.get('status') == 'failed' and
                  any('unavailable' in text for text in strings(events)))
            check('native owner refusal sends no provider request', len(captures) == before)
        finally:
            recover()
        check('supervised memory restart preserves committed identity', True)
        before = len(captures)
        result, _ = run('recovered native run')
        check('new native worker recovers and dispatches once', result.get('status') == 'completed' and
              len(captures) == before + 1)
        check('recovered provider retains complete Go memory', any(content in text for text in strings(captures[-1])))
        before = len(captures)
        result, events = run('native context refresh', mode='refresh-update')
        check('native refresh fixture commits its memory update', not provider_errors and refresh_id is not None)
        check('native multi-turn run completes after five tool calls', result.get('status') == 'completed' and
              len(captures) == before + 6)
        check('native refresh follows five real file reads', all(
            any(f'Native refresh evidence {turn}: {prefix}' in text for text in strings(captures[-1]))
            for turn in range(1, 6)))
        check('initial context predates the new identity',
              not any(refreshed_content in text for text in strings(captures[before])))
        check('refreshed provider context contains the new complete identity',
              any(refreshed_content in text for text in strings(captures[-1])))
        owner_pid = memory_owner()
        try:
            before = len(captures)
            result, events = run('native refresh owner outage', mode='refresh-outage')
            check('native refresh intervention succeeds', not provider_errors)
            check('native refresh reports memory owner refusal', result.get('status') == 'failed' and
                  any('unavailable' in text for text in strings(events)))
            check('native refresh refusal prevents a sixth provider request', len(captures) == before + 5)
        finally:
            recover()
        before = len(captures)
        result, _ = run('recovered native refresh')
        check('worker recovers after refresh refusal', result.get('status') == 'completed' and
              len(captures) == before + 1)
        check('refresh recovery retains both committed identities', all(
            any(value in text for text in strings(captures[-1])) for value in (content, refreshed_content)))
    finally:
        try:
            if owner_pid is not None:
                recover()
            if refresh_id is not None:
                status, retired = api('/v1/memory/delete', dict(id=str(refresh_id)))
                check('native fixture retires its refreshed identity', status == 200 and retired.get('status') == 'ok')
            if memory_id is not None:
                status, retired = api('/v1/memory/delete', dict(id=str(memory_id)))
                check('native fixture retires its private identity', status == 200 and retired.get('status') == 'ok')
        finally:
            if previous_recall is not None:
                config('set', 'memory_recall_enabled', json.dumps(previous_recall))
            if previous is None:
                roster.unlink(missing_ok=True)
            else:
                roster.write_bytes(previous)
            provider.shutdown()
            provider.server_close()
            fixture_files.cleanup()
            Path(output).write_text(json.dumps(dict(checks=checks, runs=runs), indent=2) + '\n')
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--server')
    parser.add_argument('--output', required=True)
    parser.add_argument('--inside', action='store_true', help=argparse.SUPPRESS)
    args = parser.parse_args()
    if args.inside:
        return inside(args.output)
    if not args.server or not args.server.startswith('aimee-e2e-server-') or not args.server.endswith('-aimee-server-1'):
        parser.error('requires a disposable deployment-matrix Server')
    inspected = json.loads(subprocess.check_output(['docker', 'inspect', args.server], text=True))[0]
    if inspected['Config']['Labels'].get('com.docker.compose.project') != args.server.removesuffix('-aimee-server-1'):
        parser.error('Server does not belong to the named disposable project')
    remote = '/tmp/' + uuid.uuid4().hex + '-native-async.py'
    result = remote + '.json'
    try:
        subprocess.run(['docker', 'cp', str(Path(__file__).resolve()), args.server + ':' + remote], check=True)
        run_result = subprocess.run(['docker', 'exec', '-u', '1000', '-e', 'AIMEE_NATIVE_ASYNC_FIXTURE=1',
            args.server, 'python3', remote, '--inside', '--output', result], timeout=600)
        subprocess.run(['docker', 'cp', args.server + ':' + result, args.output], check=True)
        return run_result.returncode
    finally:
        subprocess.run(['docker', 'exec', args.server, 'rm', '-f', remote, result], check=True)


if __name__ == '__main__':
    raise SystemExit(main())
