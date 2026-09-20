#!/usr/bin/env python3
"""Capture real provider-bound memory requests in a disposable deployment matrix.

The Server, Go memory owner, storage, ingress and provider serializers are real.
Only the external completion endpoint is a loopback fixture. This records exact
wire bytes/digests and declared byte-cap refusals; it does not claim exact token
limits or durable receipts.
No credentials, prompt bodies or provider headers are written to the result.
"""
import argparse
import hashlib
import http.client
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import socket
import subprocess
import threading
import time
import uuid


def strings(value):
    if isinstance(value, str):
        yield value
    elif isinstance(value, list):
        for child in value:
            yield from strings(child)
    elif isinstance(value, dict):
        for child in value.values():
            yield from strings(child)


def admission_idle_cpu():
    """Measure the owned economizer process while this fixture sends no work."""
    matches = []
    for process in Path('/proc').iterdir():
        if not process.name.isdigit():
            continue
        try:
            if (process / 'exe').resolve(strict=True).name == 'aimee-module-economizer':
                matches.append(process)
        except OSError:
            continue
    if len(matches) != 1:
        raise RuntimeError('idle admission measurement needs exactly one economizer process')
    process = matches[0]

    def sample():
        # Fields after comm begin with state (field 3); CPU is fields 14/15.
        fields = (process / 'stat').read_text().rpartition(')')[2].split()
        return int(fields[19]), int(fields[11]) + int(fields[12])

    identity, before = sample()
    start = time.perf_counter()
    time.sleep(10)
    elapsed = time.perf_counter() - start
    after_identity, after = sample()
    if identity != after_identity or after < before:
        raise RuntimeError('economizer restarted during idle CPU measurement')
    ticks = os.sysconf('SC_CLK_TCK')
    cpu_seconds = (after - before) / ticks
    return dict(module='economizer', source='Linux proc stat process user+system ticks',
                elapsed_seconds=elapsed, cpu_seconds=cpu_seconds,
                one_core_percent=100 * cpu_seconds / elapsed, clock_ticks_per_second=ticks)


def inside(output, budget_benchmark=False):
    if os.environ.get('AIMEE_MEMORY_PROVIDER_BOUNDARY_FIXTURE') != '1':
        raise RuntimeError('requires the disposable-container launcher')
    captures, checks, accounting = [], [], []
    timings = []
    idle_cpu = None
    responses_ids = {}
    lock = threading.Lock()

    def check(name, passed):
        checks.append(dict(name=name, passed=bool(passed)))
        if not passed:
            raise RuntimeError(name)

    class Provider(BaseHTTPRequestHandler):
        def log_message(self, *_):
            pass

        def do_POST(self):
            raw = self.rfile.read(int(self.headers.get('Content-Length', '0')))
            body = json.loads(raw)
            with lock:
                captures.append((self.path, raw, body))
            if self.path.endswith('/messages'):
                response = dict(id='memory-boundary-message', type='message', role='assistant',
                    model=body['model'], content=[dict(type='text', text='MEMORY_BOUNDARY_OK')],
                    stop_reason='end_turn', stop_sequence=None,
                    usage=dict(input_tokens=11, output_tokens=2))
            else:
                response = dict(id='memory-boundary-chat', object='chat.completion', model=body['model'],
                    choices=[dict(index=0, message=dict(role='assistant', content='MEMORY_BOUNDARY_OK'),
                                  finish_reason='stop')],
                    usage=dict(prompt_tokens=11, completion_tokens=2, total_tokens=13))
            if any('RETURN_MEMORY_BOUNDARY_TOOL' in text for text in strings(body)):
                if self.path.endswith('/messages'):
                    response['content'] = [dict(type='tool_use', id='memory-boundary-call',
                        name='inspect_memory_boundary', input=dict(record_id='synthetic'))]
                    response['stop_reason'] = 'tool_use'
                else:
                    response['choices'] = [dict(index=0, message=dict(role='assistant', content=None,
                        tool_calls=[dict(id='memory-boundary-call', type='function', function=dict(
                            name='inspect_memory_boundary', arguments='{"record_id":"synthetic"}'))]),
                        finish_reason='tool_calls')]
            data = json.dumps(response).encode()
            self.send_response(200)
            self.send_header('Content-Type', 'application/json')
            self.send_header('Content-Length', str(len(data)))
            self.end_headers()
            self.wfile.write(data)

    class Local(http.client.HTTPConnection):
        def connect(self):
            self.sock = socket.socket(socket.AF_UNIX)
            self.sock.settimeout(90)
            self.sock.connect('/var/lib/aimee/aimee-http.sock')

    def api(path, body=None, limits=None):
        conn = Local('localhost', timeout=90)
        try:
            conn.request('GET' if body is None else 'POST', path,
                None if body is None else json.dumps(body), {'Content-Type': 'application/json',
                 **({'X-Aimee-Context-Limits': limits} if limits is not None else {})})
            response = conn.getresponse()
            raw = response.read()
            if response.getheader('Content-Type', '').startswith('text/event-stream'):
                events = [json.loads(line[5:].strip()) for line in raw.decode().splitlines()
                          if line.startswith('data:') and line[5:].strip() != '[DONE]']
                return response.status, dict(events=events)
            return response.status, json.loads(raw)
        finally:
            conn.close()

    provider = ThreadingHTTPServer(('127.0.0.1', 0), Provider)
    threading.Thread(target=provider.serve_forever, daemon=True).start()
    roster = Path('/var/lib/aimee/models.json')
    previous = roster.read_bytes() if roster.exists() else None
    prefix = 'memory-wire-' + uuid.uuid4().hex[:10]
    memory_id = None
    try:
        # Keep existing registrations; every test explicitly names its local fixture.
        models = json.loads(previous) if previous else dict(models=[])
        for protocol in ('openai', 'anthropic'):
            models.setdefault('models', []).append(dict(name=prefix + '-' + protocol,
                model='memory-boundary-fixture', provider=protocol, auth_type='none',
                endpoint=f'http://127.0.0.1:{provider.server_port}/v1', roles=['all'], enabled=True,
                tools_enabled=False, context_window=32768, max_output=4096, max_parallel=4))
        roster.write_text(json.dumps(models))
        content = 'Memory wire fixture αβ🦊; limit=7; quoted="value"; path=C:\\fixture\\memory\nsecond line'
        status, stored = api('/v1/memory/store', dict(key=prefix, content=content))
        check('Go private memory stores the Unicode and escaping fixture', status == 200 and stored.get('status') == 'ok')
        memory_id = stored['id']
        status, current = api('/v1/memory/get', dict(id=str(memory_id), include_version=True))
        check('Go memory returns the exact content and current version', status == 200 and
              current.get('memory', {}).get('content') == content and bool(current.get('memory', {}).get('version')))
        status, recall = api('/v1/memory/recall', dict(query=prefix, store='user'))
        check('Go recall selects the stored fixture', status == 200 and
              any(content in text for text in strings(recall.get('recall'))))
        projection = json.dumps(recall['recall'], ensure_ascii=False, separators=(',', ':'))
        policy = 'Fixture user constraint: do not exceed LIMIT_7. Preserve identifier αβ🦊.'
        context = '<memory_data trust="untrusted">' + projection + '</memory_data>'
        tool = dict(name='inspect_memory_boundary', description='Fixture schema with Unicode αβ🦊 and "quotes".',
                    parameters=dict(type='object', properties=dict(record_id=dict(type='string')), required=['record_id']))
        for protocol in ('openai', 'anthropic'):
            model = prefix + '-' + protocol
            cases = [
                ('chat', '/v1/chat/completions', dict(model=model, stream=False, max_tokens=32,
                    messages=[dict(role='system', content=policy), dict(role='user', content=context)],
                    tools=[dict(type='function', function=tool)])),
                ('responses', '/v1/responses', dict(model=model, stream=False, max_output_tokens=32,
                    instructions=policy, input=[dict(role='user', content=context)],
                    tools=[dict(type='function', **tool)])),
                ('responses_stream', '/v1/responses', dict(model=model, stream=True, max_output_tokens=32,
                    instructions=policy, input=[dict(role='user', content=context)],
                    tools=[dict(type='function', **tool)])),
                ('messages', '/v1/messages', dict(model=model, stream=False, max_tokens=32,
                    system=policy, messages=[dict(role='user', content=context)],
                    tools=[dict(name=tool['name'], description=tool['description'], input_schema=tool['parameters'])])),
            ]
            for frontend, path, body in cases:
                name = frontend + ' to ' + protocol
                with lock:
                    before = len(captures)
                status, response = api(path, body)
                check(name + ' completes through the real provider adapter', status == 200 and
                      any('MEMORY_BOUNDARY_OK' in text for text in strings(response)))
                with lock:
                    selected = captures[before:]
                check(name + ' dispatches exactly one request', len(selected) == 1)
                if frontend == 'responses':
                    responses_ids[protocol] = response['id']
                endpoint, raw, sent = selected[0]
                if protocol == 'anthropic':
                    check(name + ' uses Anthropic system and tool fields', endpoint.endswith('/messages') and
                          bool(sent.get('system')) and all(m.get('role') in ('user', 'assistant')
                          for m in sent.get('messages', [])) and
                          all('input_schema' in t and 'function' not in t for t in sent.get('tools', [])))
                else:
                    check(name + ' uses OpenAI chat fields', endpoint.endswith('/chat/completions') and
                          isinstance(sent.get('messages'), list) and
                          all(t.get('type') == 'function' and 'function' in t for t in sent.get('tools', [])))
                values = list(strings(sent))
                check(name + ' preserves the complete Go memory projection', any(context in text for text in values))
                check(name + ' preserves the user constraint', any(policy in text for text in values))
                check(name + ' includes the tool schema in the final request', tool['name'] in values and
                      tool['description'] in values and 'record_id' in json.dumps(sent.get('tools')))
                check(name + ' captures provider serialization overhead', len(raw) > len(context.encode()) + len(policy.encode()))
                if frontend in ('chat', 'responses', 'responses_stream'):
                    check(name + ' includes standing memory guidance exactly once',
                          sum(text.count('explore-with: aimee answers CODE questions') for text in values) == 1)
                # Count the actual serialized provider body, including wrappers, tools,
                # memory and escaped Unicode. Headers never become provider JSON.
                exact_limit = json.dumps(dict(schema_version=1, max_request_bytes=len(raw)))
                before_budget = len(captures)
                capped_status, capped_response = api(path, body, exact_limit)
                check(name + ' admits an exact final-byte fit', capped_status == 200 and
                      any('MEMORY_BOUNDARY_OK' in text for text in strings(capped_response)))
                check(name + ' exact-fit dispatch preserves all provider bytes',
                      len(captures) == before_budget + 1 and captures[-1][1] == raw)
                for label, limits, error, expected_status in (
                    ('one byte under', dict(schema_version=1, max_request_bytes=len(raw)-1),
                     'request_budget_exceeded', 413),
                    ('literal zero', dict(schema_version=1, max_request_bytes=0),
                     'request_budget_exceeded', 413),
                    ('unavailable tokens', dict(schema_version=1, max_request_bytes=len(raw), max_request_tokens=100000),
                     'token_count_unavailable', 400),
                    ('null cap', dict(schema_version=1, max_request_bytes=None), 'request_budget_invalid', 400),
                    ('unknown field', dict(schema_version=1, max_request_bytes=len(raw), typo=1),
                     'request_budget_invalid', 400),
                    ('duplicate cap', '{"schema_version":1,"max_request_bytes":0,"max_request_bytes":999999}',
                     'request_budget_invalid', 400),
                ):
                    before_budget = len(captures)
                    limits = limits if isinstance(limits, str) else json.dumps(limits)
                    refused_status, refused = api(path, body, limits)
                    check(name + ' ' + label + ' reports explicit refusal',
                          refused_status == (200 if body.get('stream') else expected_status) and
                          error in list(strings(refused)) and
                          not any('MEMORY_BOUNDARY_OK' in text for text in strings(refused)))
                    check(name + ' ' + label + ' sends no provider request', len(captures) == before_budget)
                if frontend in ('chat', 'messages'):
                    before_budget = len(captures)
                    refused_status, refused = api(path, dict(body, stream=True),
                        '{"schema_version":1,"max_request_bytes":0}')
                    check(name + ' streaming zero cap emits a failure', refused_status == 200 and
                          'request_budget_exceeded' in list(strings(refused)) and
                          not any(e.get('type') in ('message_stop', 'response.completed')
                                  for e in refused.get('events', [])))
                    check(name + ' streaming zero cap sends no provider request', len(captures) == before_budget)
                if budget_benchmark and frontend == 'chat':
                    plain_ms, capped_ms = [], []
                    # Alternate order to balance warming and temporal effects.
                    for pair in range(32):
                        for limited in ([False, True] if pair % 2 == 0 else [True, False]):
                            before_measurement = len(captures)
                            begin = time.perf_counter_ns()
                            measured_status, measured_response = api(path, body, exact_limit if limited else None)
                            elapsed_ms = (time.perf_counter_ns() - begin) / 1e6
                            if (measured_status != 200 or len(captures) != before_measurement + 1 or
                                    captures[-1][1] != raw or
                                    not any('MEMORY_BOUNDARY_OK' in text for text in strings(measured_response))):
                                raise RuntimeError('paired byte-admission benchmark changed the provider result')
                            (capped_ms if limited else plain_ms).append(elapsed_ms)
                    timings.append(dict(frontend=frontend, provider=protocol, pairs=32,
                        fixture='loopback provider; real host, Go owner and bus; no external model latency',
                        no_limit_ms=plain_ms, declared_limit_ms=capped_ms))
                accounting.append(dict(frontend=frontend, provider=protocol, endpoint=endpoint,
                    boundary='provider_http_body', count_state='exact', unit='utf8_bytes',
                    request_bytes=len(raw), memory_projection_bytes=len(context.encode()),
                    digest='sha256:' + hashlib.sha256(raw).hexdigest(),
                    token_count_state='unavailable', retention='commitment_only'))
        for protocol in ('openai', 'anthropic'):
            name = 'buffered Responses to ' + protocol
            new_policy = 'Followup user constraint: preserve LIMIT_7 and αβ🦊.'
            before = len(captures)
            status, response = api('/v1/responses', dict(model=prefix + '-' + protocol,
                input='FOLLOWUP_MEMORY_BOUNDARY', instructions=new_policy,
                previous_response_id=responses_ids[protocol], max_output_tokens=32))
            check(name + ' continuation completes', status == 200 and
                  any('MEMORY_BOUNDARY_OK' in text for text in strings(response)))
            selected = captures[before:]
            check(name + ' continuation dispatches once', len(selected) == 1)
            values = list(strings(selected[0][2]))
            check(name + ' continuation preserves prior memory and current input',
                  any(context in text for text in values) and
                  any('FOLLOWUP_MEMORY_BOUNDARY' in text for text in values))
            check(name + ' continuation uses current instructions',
                  any(new_policy in text for text in values) and not any(policy in text for text in values))
            before = len(captures)
            status, response = api('/v1/responses', dict(model=prefix + '-' + protocol,
                input=context + '\nRETURN_MEMORY_BOUNDARY_TOOL', instructions=policy,
                tools=[dict(type='function', **tool)], max_output_tokens=32))
            check(name + ' tool result is relayed without local execution', status == 200 and
                  len(captures) == before + 1)
            calls = [item for item in response.get('output', []) if item.get('type') == 'function_call']
            check(name + ' preserves returned call identity', len(calls) == 1 and
                  calls[0].get('name') == tool['name'] and calls[0].get('call_id') == 'memory-boundary-call')
            check(name + ' preserves returned call arguments',
                  json.loads(calls[0].get('arguments', 'null')) == dict(record_id='synthetic'))
        if budget_benchmark:
            idle_cpu = admission_idle_cpu()
    finally:
        try:
            if memory_id is not None:
                status, retired = api('/v1/memory/delete', dict(id=str(memory_id)))
                check('fixture memory is retired after capture', status == 200 and retired.get('status') == 'ok')
        finally:
            if previous is None:
                roster.unlink(missing_ok=True)
            else:
                roster.write_bytes(previous)
            provider.shutdown()
            provider.server_close()
            Path(output).write_text(json.dumps(dict(checks=checks, accounting=accounting,
                **(dict(timings=timings, idle_cpu=idle_cpu) if budget_benchmark else {})), indent=2) + '\n')
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--server')
    parser.add_argument('--budget-benchmark', action='store_true',
                        help='measure 32 alternating capped/uncapped pairs per provider')
    parser.add_argument('--output', required=True)
    parser.add_argument('--inside', action='store_true', help=argparse.SUPPRESS)
    args = parser.parse_args()
    if args.inside:
        return inside(args.output, args.budget_benchmark)
    if not args.server or not args.server.startswith('aimee-e2e-server-') or not args.server.endswith('-aimee-server-1'):
        parser.error('requires a disposable deployment-matrix Server')
    inspected = json.loads(subprocess.check_output(['docker', 'inspect', args.server], text=True))[0]
    project = args.server.removesuffix('-aimee-server-1')
    if inspected['Config']['Labels'].get('com.docker.compose.project') != project:
        parser.error('Server does not belong to the named disposable project')
    remote = '/tmp/' + uuid.uuid4().hex + '-memory-provider-boundary.py'
    result = remote + '.json'
    try:
        subprocess.run(['docker', 'cp', str(Path(__file__).resolve()), args.server + ':' + remote], check=True)
        run = subprocess.run(['docker', 'exec', '-u', '1000', '-e', 'AIMEE_MEMORY_PROVIDER_BOUNDARY_FIXTURE=1',
            args.server, 'python3', remote, '--inside', '--output', result,
            *(['--budget-benchmark'] if args.budget_benchmark else [])], timeout=600)
        subprocess.run(['docker', 'cp', args.server + ':' + result, args.output], check=True)
        return run.returncode
    finally:
        subprocess.run(['docker', 'exec', args.server, 'rm', '-f', remote, result], check=True)


if __name__ == '__main__':
    raise SystemExit(main())
