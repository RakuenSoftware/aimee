#!/usr/bin/env python3
"""Validate real asynchronous native memory execution in an owned test Server.

Uses a local synthetic provider, replaces/restores the disposable model roster,
and pauses/restarts only the Server's Go memory process. Results omit prompts,
credentials and provider headers. Never run against a user's installation.
"""
import argparse
import base64
import hashlib
import sqlite3
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
    checks, captures, runs, provider_errors, request_sizes = [], [], [], [], []
    lock = threading.Lock()
    receipt_captures = []
    expected_unresolved = set()
    prefix = 'native-memory-' + uuid.uuid4().hex[:10]
    content = 'Complete native Go memory fixture 界🦊; preserve LIMIT_7 and identifier ' + prefix
    owner_pid, memory_id = None, None
    refresh_id = None
    shared_change_id = None
    has_shared = False
    refreshed_content = 'New memory committed before turn-six context refresh 界🦊 ' + prefix
    provider_failure = 'native fixture "quoted"; line\nbreak; path \\ evidence 界'
    scenario, scenario_start = 'single', 0
    previous_recall = None
    exploration_session = None
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
            if response.getheader('Content-Type', '').startswith('application/x-ndjson'):
                return response.status, [json.loads(line) for line in raw.decode().splitlines() if line.strip()]
            return response.status, json.loads(raw)
        finally:
            conn.close()

    class Provider(BaseHTTPRequestHandler):
        def log_message(self, *_):
            pass

        def do_POST(self):
            nonlocal refresh_id, shared_change_id
            raw = self.rfile.read(int(self.headers.get('Content-Length', '0')))
            body = json.loads(raw)
            # Independent read-only connection sees committed intent before the
            # synthetic provider replies. No prompt or ledger detail is exported.
            admission = None
            try:
                uri = Path('/var/lib/aimee/audit/worm-live.db').as_uri() + '?mode=ro'
                with sqlite3.connect(uri, uri=True, timeout=5) as ledger:
                    for seq, detail in ledger.execute(
                            "SELECT seq,detail FROM audit_event WHERE action='memory.provider.prepared' ORDER BY seq DESC LIMIT 64"):
                        event = json.loads(detail)
                        binding = event.get('binding', {})
                        if binding.get('payload_sha256') != hashlib.sha256(raw).hexdigest():
                            continue
                        admitted = ledger.execute(
                            'SELECT seq,detail FROM audit_event WHERE event_id=?',
                            ('memory.provider.' + event['attempt_id'] + '.dispatch_admitted',)).fetchone()
                        if admitted and admitted[0] > seq and json.loads(admitted[1]).get('binding_sha256') == event['binding_sha256']:
                            admission = event
                            break
            except (OSError, sqlite3.Error, ValueError, KeyError):
                pass
            with lock:
                receipt_captures.append((admission, raw))
                captures.append(body)
                request_sizes.append(len(raw))
                ordinal = len(captures) - scenario_start
            response = dict(id=prefix, object='chat.completion', model=body['model'],
                choices=[dict(index=0, message=dict(role='assistant', content='NATIVE_MEMORY_OK'),
                              finish_reason='stop')],
                usage=dict(prompt_tokens=11, completion_tokens=2, total_tokens=13))
            if scenario in ('refresh-update', 'refresh-outage', 'protected-fold') and ordinal <= 5:
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
                            if admission is not None:
                                expected_unresolved.add(admission['attempt_id'])
                            os.kill(owner_pid, signal.SIGSTOP)
                    except Exception as exc:
                        provider_errors.append(type(exc).__name__ + ': ' + str(exc))
            if scenario == 'correction-before-retry' and ordinal == 1:
                try:
                    status, current = api('/v1/memory/get', dict(id=str(memory_id), include_version=True))
                    version = current.get('memory', {}).get('version')
                    if status != 200 or not isinstance(version, dict):
                        raise RuntimeError('retry correction could not read current version')
                    status, corrected = api('/v1/memory/supersede', dict(
                        old_id=str(memory_id), new_content=content + ' corrected before retry',
                        expected_version=version))
                    if status != 200 or corrected.get('status') != 'ok':
                        raise RuntimeError('retry correction was not committed')
                except Exception as exc:
                    provider_errors.append(type(exc).__name__ + ': ' + str(exc))
                response = dict(error=dict(message='retryable fixture response after source correction'))
            if scenario == 'shared-change-before-retry' and ordinal == 1:
                try:
                    status, stored = api('/v1/memory/store', dict(store='kb', scope='all', key=prefix+'-shared-change',
                        content='New shared constraint: deployment requires explicit review.', tier='L2', kind='fact'))
                    if status != 200 or stored.get('status') != 'ok':
                        raise RuntimeError('shared constraint was not committed')
                    shared_change_id = stored['id']
                except Exception as exc:
                    provider_errors.append(type(exc).__name__ + ': ' + str(exc))
                response = dict(error=dict(message='retryable fixture response after shared insertion'))
            if scenario == 'exploration-recovery' and ordinal <= 3:
                if ordinal == 1:
                    name, arguments = 'find_symbol', dict(identifier=prefix + '_missing_symbol')
                elif ordinal == 2:
                    try:
                        outputs = [m.get('content', '') for m in body.get('messages', []) if m.get('role') == 'tool']
                        gap = json.loads(outputs[-1].split('Exploration recovery: ', 1)[1])
                        check('native indexed miss exposes a host-owned recovery reference', gap.get('expansion_available') is True)
                        name, arguments = 'context_contract_expand', dict(reason='indexed lookup found no definition',
                            gap_ref=gap['gap_ref'], outcome_id=gap['outcome_id'])
                    except (KeyError, IndexError, ValueError) as exc:
                        provider_errors.append('missing host exploration gap: ' + str(exc))
                        name, arguments = 'context_contract_expand', dict(reason='fixture failure', gap_ref='invalid', outcome_id='invalid')
                else:
                    outputs = [m.get('content', '') for m in body.get('messages', []) if m.get('role') == 'tool']
                    check('native expansion tool accepts the observed gap', json.loads(outputs[-1]).get('status') == 'ok')
                    name, arguments = 'grep', dict(path='.', pattern=prefix, max_results=1)
                response['choices'] = [dict(index=0, finish_reason='tool_calls', message=dict(role='assistant', content=None,
                    tool_calls=[dict(id=f'{prefix}-recovery-{ordinal}', type='function', function=dict(name=name, arguments=json.dumps(arguments)))]))]
            if scenario == 'provider-error':
                response = dict(error=dict(message=provider_failure))
            data = json.dumps(response, ensure_ascii=False).encode()
            self.send_response(500 if scenario in ('correction-before-retry', 'shared-change-before-retry') else
                               400 if scenario == 'provider-error' else 200)
            self.send_header('Content-Type', 'application/json')
            self.send_header('Content-Length', str(len(data)))
            self.end_headers()
            self.wfile.write(data)

    def settle_shared_projection(name):
        if not has_shared:
            return
        # Completed runs publish shared feedback whose deterministic indexing
        # follows on the five-second worker tick. Isolate the next scenario
        # after observing an unchanged head for a complete worker cycle.
        deadline, previous, unchanged_since = time.monotonic()+90, None, None
        while time.monotonic() < deadline:
            status, reply = api('/v1/memory/recall', dict(store='kb', query=prefix, session_start=True, scope='all'))
            recall = reply.get('recall', {})
            memory = recall.get('collection_source')
            rules = recall.get('rule_collection_source')
            if status != 200 or not isinstance(memory, dict) or not isinstance(rules, dict):
                raise RuntimeError('shared projection settling probe unavailable')
            head = json.dumps([memory, rules], sort_keys=True)
            now = time.monotonic()
            if head != previous:
                previous, unchanged_since = head, now
            elif now-unchanged_since >= 6:
                check('shared projection settled before '+name, True)
                return
            time.sleep(0.25)
        check('shared projection settled before '+name, False)

    def run(name, limits=None, mode='single', input_text=None):
        nonlocal scenario, scenario_start
        if name != 'paused Go memory owner':
            settle_shared_projection(name)
        started, before = time.monotonic(), len(captures)
        scenario, scenario_start = mode, before
        status, created = api('/v1/runs', dict(model=prefix, input=input_text or 'Read the native memory fixture ' + prefix,
                                              max_output_tokens=32), limits)
        check(name + ' queues a real asynchronous worker', status == 200 and bool(created.get('id')))
        run_id = created['id']
        # A refresh with the owner deliberately stopped can traverse both
        # local and enrolled-KB paths before their bounded RPCs refuse. Keep
        # the normal-run bound, but allow this outage case to finish refusing.
        wait_seconds = 180 if mode == 'refresh-outage' else 90
        deadline = time.monotonic() + wait_seconds
        while time.monotonic() < deadline:
            status, result = api('/v1/runs/' + run_id)
            if status == 200 and result.get('status') in ('completed', 'failed', 'cancelled'):
                _, events = api('/v1/runs/' + run_id + '/events')
                runs.append(dict(name=name, run_id=run_id, status=result['status'],
                    elapsed_seconds=time.monotonic()-started, provider_requests=len(captures)-before,
                    provider_request_bytes=request_sizes[before:],
                    refusal_kinds=[kind for kind in ('request_budget_exceeded', 'request_budget_unavailable', 'unavailable', 'stale_context')
                                   if any(kind in text for text in strings(events))]))
                return result, events
            time.sleep(0.1)
        runs.append(dict(name=name, run_id=run_id, status='fixture_timeout',
            elapsed_seconds=time.monotonic()-started, wait_seconds=wait_seconds,
            provider_requests=len(captures)-before,
            provider_request_bytes=request_sizes[before:]))
        checks.append(dict(name=name + ' reaches a terminal state within fixture bound', passed=False))
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
        previous_owner = owner_pid
        if owner_pid is not None:
            os.kill(owner_pid, signal.SIGCONT)
            os.kill(owner_pid, signal.SIGTERM)
            owner_pid = None
        deadline = time.monotonic() + 90
        while time.monotonic() < deadline:
            # A resumed owner may serve a queued read before SIGTERM finishes.
            # That answer is not evidence that its supervised replacement is ready.
            if previous_owner is not None and Path('/proc', str(previous_owner)).exists():
                time.sleep(0.1)
                continue
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
        has_shared = config('get', 'kb_mode')['value'] == 'remote'
        previous_recall = config('get', 'memory_recall_enabled')['value']
        config('set', 'memory_recall_enabled', '1')
        check('native fixture explicitly enables automatic recall',
              config('get', 'memory_recall_enabled')['value'] == 1)
        # Role routing uses the default delegate; an explicit ingress model alone
        # does not select the worker's route. Isolate every route to this fixture.
        roster.write_text(json.dumps(dict(default_agent=prefix, default_delegate=prefix, fallback_chain=[],
            models=[dict(name=prefix, model='native-memory-fixture', provider='openai', auth_type='none',
                         endpoint=f'http://127.0.0.1:{provider.server_port}/v1', roles=['all'], enabled=True,
                         # Leave room for the real tool catalog and conversation
                         # under the fixture's unchanged 32 KiB operator ceiling.
                         # Go must select whole memory rows within this smaller
                         # model allocation, including both private identities.
                         tools_enabled=True, context_window=8192, max_tokens=4096,
                         max_output=4096, max_parallel=1, max_turns=8)])))
        status, stored = api('/v1/memory/store', dict(key='identity:' + prefix, content=content))
        check('native fixture stores complete private identity', status == 200 and stored.get('status') == 'ok')
        memory_id = stored['id']
        version_status, versioned = api('/v1/memory/get', dict(id=str(memory_id), include_version=True))
        observed_version = versioned.get('memory', {}).get('version')
        check('native fixture reads its private source version', version_status == 200 and isinstance(observed_version, dict))
        before = len(captures)
        result, _ = run('healthy native run')
        check('healthy native run completes', result.get('status') == 'completed' and 'NATIVE_MEMORY_OK' in list(strings(result)))
        check('healthy native run dispatches exactly once', len(captures) == before + 1)
        check('native provider receives complete Go memory', any(content in text for text in strings(captures[-1])))
        prepared, payload = receipt_captures[-1]
        check('provider arrival sees committed preparation and admission', prepared is not None)
        status, verification = api('/v1/commands/memory.verify_receipt', dict(
            prepared_receipt=prepared, payload_base64=base64.b64encode(payload).decode()))
        verification = verification.get('result', {})
        check('public receipt verification matches real provider bytes', status == 200 and
              verification.get('evidence', {}).get('binding_commitment') == 'matched' and
              verification.get('evidence', {}).get('payload_correspondence') == 'matched' and
              verification.get('evidence', {}).get('source_commitment') == 'matched')
        check('native receipt binds the retained private revision and fresh source check',
              verification.get('source_coverage') == 'retained_versioned_inputs' and
              len(prepared['binding'].get('source_check_id', '')) == 32 and
              any(ref.get('source_version', {}).get('record_kind') == 'user_memory_record' and
                  ref.get('source_version', {}).get('version') == observed_version
                  for ref in prepared['binding'].get('sources', [])))
        check('native receipt retains the private collection dependency', any(
            ref.get('source_version', {}).get('record_kind') == 'user_memory_collection'
            for ref in prepared['binding'].get('sources', [])))
        check('supplied receipt does not claim producer or chain authentication',
              verification.get('evidence', {}).get('authenticated_producer') == 'unavailable' and
              verification.get('evidence', {}).get('chain_included') == 'not_checked' and
              verification.get('evidence', {}).get('decision_replayed') == 'unavailable')
        status, mismatch = api('/v1/commands/memory.verify_receipt', dict(
            prepared_receipt=prepared, payload_base64=base64.b64encode(payload + b' ').decode()))
        mismatch = mismatch.get('result', {})
        check('public receipt verification detects changed provider bytes', status == 200 and
              mismatch.get('evidence', {}).get('payload_correspondence') == 'mismatch')
        status, stored_receipts = api('/v1/memory/receipt', dict(request_id=prepared['binding']['request_id']))
        own = [row for row in stored_receipts.get('receipts', []) if row.get('attempt_id') == prepared['attempt_id']]
        check('authenticated receipt lookup reads actual durable acknowledgement', status == 200 and len(own) == 1 and
              own[0].get('state') == 'acknowledged' and own[0].get('evidence', {}).get('chain_included') is True)
        check('real provider write has a distinct durable started observation', 'dispatch_started' in own[0]['stages'])
        check('commitment-only receipt does not invent replay or remote effects', own[0].get('replay') == 'unavailable_commitment_only' and
              own[0].get('evidence', {}).get('effect_confirmed') is False and own[0].get('evidence', {}).get('decision_replayed') is False)
        # A real primary session has authenticated durable ownership and a
        # concrete worktree. The stateless /v1/runs fixture above has neither.
        for command in [ ['git', 'init', '-b', 'main', fixture_files.name],
                         ['git', '-C', fixture_files.name, 'add', '.'],
                         ['git', '-C', fixture_files.name, '-c', 'user.name=Fixture', '-c', 'user.email=fixture@example.invalid',
                          'commit', '-m', 'Disposable exploration fixture'] ]:
            subprocess.run(command, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        status, session = api('/v1/sessions/create', dict(client_type='native-exploration-fixture'))
        exploration_session = session.get('session_id')
        check('exploration fixture creates an authenticated session', status == 200 and bool(exploration_session))
        status, pinned = api('/v1/sessions/' + exploration_session + '/primary', dict(agent=prefix))
        check('exploration fixture pins the local synthetic provider', status == 200 and pinned.get('agent') == prefix)
        scenario, scenario_start = 'exploration-recovery', len(captures)
        # A short, session-specific fixture persona leaves room for recovery
        # history beneath the unchanged 32 KiB operator ceiling. The complete
        # production tool catalog and memory assembly remain in use.
        persona = roster.parent / 'personas' / (prefix + '.md')
        persona.parent.mkdir(exist_ok=True)
        try:
            with persona.open('x') as target:
                target.write('## Persona\nInspect the disposable recovery fixture.\n'
                             '## Principles\nUse tools to verify results; report failures.\n')
            status, _ = api('/v1/sessions/' + exploration_session + '/persona', dict(name=prefix))
            check('exploration session selects its bounded fixture persona', status == 200)
            status, events = api('/v1/chat/stream', dict(message='Read the native memory fixture ' + prefix,
                aimee_session_id=exploration_session, cwd=fixture_files.name, model=prefix))
        finally:
            persona.unlink(missing_ok=True)
        check('primary session completes native indexed recovery', status == 200 and not provider_errors and
              len(captures) == scenario_start + 4 and any('NATIVE_MEMORY_OK' in text for text in strings(events)))
        worktrees = subprocess.check_output(['git', '-C', fixture_files.name, 'worktree', 'list', '--porcelain'], text=True)
        roots = [line[9:] for line in worktrees.splitlines() if line.startswith('worktree ') and line[9:] != fixture_files.name]
        check('primary fixture resolves its host-isolated worktree', len(roots) == 1)
        def external_tool(name, arguments):
            status, result = api('/v1/tools/execute', dict(tool=name, arguments=json.dumps(arguments),
                session_id=exploration_session, cwd=roots[0], timeout_ms=30000))
            check('external ' + name + ' passes authenticated dispatch', status == 200 and result.get('status') == 'ok')
            return result.get('result', '')
        lookup = external_tool('find_symbol', dict(identifier=prefix + '_external_miss'))
        gap = json.loads(lookup.split('Exploration recovery: ', 1)[1])
        expanded = external_tool('context_contract_expand', dict(reason='external indexed lookup was empty',
            gap_ref=gap['gap_ref'], outcome_id=gap['outcome_id']))
        check('external expansion accepts only host-observed evidence', json.loads(expanded).get('status') == 'ok')
        external_tool('grep', dict(path='.', pattern=prefix, max_results=1))
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
        # MR-03: force a real native history reduction and verify complete
        # user constraints after optional tool bodies exceed the excerpt size.
        fold_settings = dict(economizer_mode=2, fold_retained_msgs=4,
                             fold_min_fold_msgs=4, fold_excerpt_bytes=40)
        previous_fold = {key: config('get', key)['value'] for key in fold_settings}
        protected = ('Background context; ' * 80 +
                     'Do not delete. Limit 7. Deadline 2030-01-02. Keep CASE_83_界🦊.')
        try:
            for key, value in fold_settings.items():
                config('set', key, json.dumps(value))
            for turn in range(1, 6):
                (Path(fixture_files.name) / f'{turn}.txt').write_text(
                    f'Native refresh evidence {turn}: {prefix}\n' + 'optional source detail; ' * 60)
            before = len(captures)
            result, _ = run('protected native fold', mode='protected-fold', input_text=protected)
            check('protected native fold completes through five real tool calls',
                  result.get('status') == 'completed' and len(captures) == before + 6)
            check('native reduction preserves complete user constraint bytes', all(
                any(protected in text for text in strings(body)) for body in captures[before:]))
            check('native provider observes actual optional history reduction', any(
                'optional folded history' in text for text in strings(captures[-1])))
            check('native generated history is not promoted to user instructions', all(
                message.get('role') == 'assistant'
                for message in captures[-1].get('messages', [])
                if any('optional folded history' in text for text in strings(message))))
        finally:
            for key, value in previous_fold.items():
                config('set', key, json.dumps(value))
            for turn in range(1, 6):
                (Path(fixture_files.name) / f'{turn}.txt').write_text(f'Native refresh evidence {turn}: {prefix}\n')
        native_roster = roster.read_bytes()
        try:
            for backend in ('tmux-cli', 'provider-cli'):
                external = json.loads(native_roster)
                # Reach the execution fence, not the primary-only/client-only
                # routing guards. Claude provider-cli normalizes to tmux, so
                # use ACP for the distinct external adapter path.
                external['models'][0].update(backend=backend,
                    cli_kind='claude' if backend == 'tmux-cli' else 'acp',
                    cli_cmd='/bin/false', primary_only=False, is_server_hosted=True)
                roster.write_text(json.dumps(external))
                before = len(captures)
                result, events = run('unobservable ' + backend,
                                     dict(schema_version=1, max_request_bytes=0))
                check(backend + ' refuses unobservable final request accounting',
                      result.get('status') == 'failed' and any(
                          'request_budget_unavailable' in text for text in strings(events)))
                check(backend + ' accounting refusal sends no provider request', len(captures) == before)
        finally:
            roster.write_bytes(native_roster)
        before = len(captures)
        result, events = run('native provider error', mode='provider-error')
        check('provider failure terminates without a successful response', result.get('status') == 'failed')
        check('provider error events retain complete escaped diagnostics', any(
            json.dumps(dict(error=dict(message=provider_failure)), ensure_ascii=False) in text
            for text in strings(events)))
        check('permanent provider error is not retried', len(captures) == before + 1)
        if has_shared:
            before = len(captures)
            result, events = run('shared insertion before transport retry', mode='shared-change-before-retry')
            check('provider fixture commits a new shared constraint', not provider_errors and shared_change_id is not None)
            check('new shared constraint refuses stale native transport retry', result.get('status') == 'failed' and
                  any('stale_context' in text for text in strings(events)))
            check('new shared constraint prevents a second provider send', len(captures) == before+1)
        before = len(captures)
        result, events = run('private correction before transport retry', mode='correction-before-retry')
        check('provider fixture commits correction before retryable response', not provider_errors)
        check('private correction refuses stale native transport retry', result.get('status') == 'failed' and
              any('stale_context' in text for text in strings(events)))
        check('corrected private source prevents a second provider send', len(captures) == before + 1)
        attempts = [entry['attempt_id'] for entry, _ in receipt_captures if entry is not None]
        check('every actual native send has separate durable admission',
              len(attempts) == len(captures) and len(set(attempts)) == len(attempts))
        uri = Path('/var/lib/aimee/audit/worm-live.db').as_uri() + '?mode=ro'
        with sqlite3.connect(uri, uri=True, timeout=5) as ledger:
            observations = [ledger.execute('SELECT detail FROM audit_event WHERE event_id=?',
                ('memory.provider.' + attempt + '.acknowledged',)).fetchone() for attempt in attempts]
        check('completed native transports retain durable response observations',
              all(row and json.loads(row[0]).get('http_status') in (200, 400, 500)
                  for attempt, row in zip(attempts, observations) if attempt not in expected_unresolved))
        check('owner outage leaves admitted transport unresolved without invented acknowledgement',
              len(expected_unresolved) == 1 and all(row is None
                  for attempt, row in zip(attempts, observations) if attempt in expected_unresolved))
    finally:
        try:
            if owner_pid is not None:
                recover()
            if shared_change_id is not None:
                status, retired = api('/v1/memory/delete', dict(store='kb', scope='all', id=str(shared_change_id)))
                check('native fixture removes its shared constraint', status == 200 and retired.get('status') == 'ok')
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
            Path(output).write_text(json.dumps(dict(checks=checks, runs=runs,
                exploration_session=exploration_session,
                source_contracts=[entry['binding'].get('sources', []) for entry, _ in receipt_captures if entry is not None],
                receipt_attempts=[entry['attempt_id'] for entry, _ in receipt_captures if entry is not None],
                unresolved_attempts=sorted(expected_unresolved)), indent=2) + '\n')
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
        if run_result.returncode:
            return run_result.returncode
        evidence = json.loads(Path(args.output).read_text())
        # Read only this fixture's row from its owned Compose PostgreSQL.
        import re
        sid = evidence.get('exploration_session', '')
        if not re.fullmatch(r'[a-fA-F0-9-]{36}', sid):
            raise RuntimeError('missing fixture session identity')
        postgres = args.server.removesuffix('-aimee-server-1') + '-aimee-store-db-1'
        def session_state():
            return subprocess.check_output(['docker', 'exec', postgres, 'psql', '-U', 'postgres', '-d', 'aimee_store',
                '-X', '-qAt', '-v', 'ON_ERROR_STOP=1', '-c',
                "SELECT exploration_state FROM session_state WHERE session_id='" + sid + "'"], text=True).strip()
        persisted_session = session_state()
        state = json.loads(persisted_session)
        tasks = list(state.get('tasks', {}).values())
        integrated = (state.get('session') == sid and len(tasks) == 1 and
            len(tasks[0].get('revisions', [])) >= 3 and len(tasks[0].get('fallbacks', [])) == 2 and
            len(tasks[0].get('indexed_outcomes', {})) == 2 and state.get('usage', {}).get('raw_scans') == 2 and
            len(tasks[0].get('completed_turns', {})) == 3 and
            all(r.get('tier') == 'observe' for r in tasks[0]['revisions']) and
            all(a.get('started') for a in tasks[0].get('attempts', {}).values()))
        evidence['checks'].append(dict(name='native and external session persist observed recovery and exact dispatch charges', passed=integrated))
        Path(args.output).write_text(json.dumps(evidence, indent=2) + '\n')
        if not integrated:
            raise RuntimeError('native exploration session state did not match actual dispatches')
        # The fixture owns this entire container, confirmed by its Compose label
        # above. SIGKILL tests host-process loss, not a graceful SQLite close.
        def committed_rows():
            code = """import hashlib,json,sqlite3,sys
attempts=json.load(sys.stdin)
with sqlite3.connect('file:/var/lib/aimee/audit/worm-live.db?mode=ro',uri=True) as db:
 rows=[]
 for attempt in attempts:
  for seq,event,detail in db.execute('SELECT seq,event_id,detail FROM audit_event WHERE subject=? AND action LIKE ? ORDER BY seq',(attempt,'memory.provider.%')):
   rows.append(dict(sequence=str(seq),event=event,detail_sha256=hashlib.sha256(detail.encode()).hexdigest()))
print(json.dumps(rows))
"""
            return json.loads(subprocess.check_output(['docker', 'exec', '-i', '-u', '1000',
                args.server, 'python3', '-c', code], input=json.dumps(evidence['receipt_attempts']), text=True))
        before = committed_rows()
        subprocess.run(['docker', 'kill', '--signal', 'KILL', args.server], check=True, stdout=subprocess.DEVNULL)
        subprocess.run(['docker', 'start', args.server], check=True, stdout=subprocess.DEVNULL)
        deadline = time.monotonic() + 120
        recovered = False
        while time.monotonic() < deadline:
            health = subprocess.check_output(['docker', 'inspect', '--format', '{{.State.Health.Status}}', args.server], text=True).strip()
            if health == 'healthy':
                recovered = True
                break
            time.sleep(1)
        def record(name, passed):
            evidence['checks'].append(dict(name=name, passed=bool(passed)))
            Path(args.output).write_text(json.dumps(evidence, indent=2) + '\n')
            print(('PASS ' if passed else 'FAIL ') + name, flush=True)
            if not passed:
                raise RuntimeError(name)
        record('owned Server recovers after an ungraceful process kill', recovered)
        record('host crash preserves exploration revisions and shared accounting exactly',
               session_state() == persisted_session)
        after = committed_rows()
        evidence['post_crash_receipts'] = after
        record('host crash preserves every committed provider stage exactly', bool(before) and before == after)
        recorded = {row['event'] for row in after}
        record('host recovery does not invent acknowledgement for unresolved transport',
               bool(evidence['unresolved_attempts']) and all(
                   'memory.provider.' + attempt + '.dispatch_admitted' in recorded and
                   'memory.provider.' + attempt + '.acknowledged' not in recorded
                   for attempt in evidence['unresolved_attempts']))
        return 0
    finally:
        subprocess.run(['docker', 'exec', args.server, 'rm', '-f', remote, result], check=True)


if __name__ == '__main__':
    raise SystemExit(main())
