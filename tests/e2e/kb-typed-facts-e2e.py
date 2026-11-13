#!/usr/bin/env python3
"""A KB with a synthesis model derives typed facts from its shared memory.

Brings up the shipped KB Compose stack with the managed aimee-llm sidecar
(profile `synthesis`), stores one shared fact on the KB, and requires the KB's
own extraction pipeline to publish a typed interpretation of at least one of
several shared facts (a small CPU model gets some wrong and the pipeline
rejects those): `memory.facts`
renders it as a source-bound statement and the 1.0 relation views serve it as
a row marked as a Model interpretation. Inference runs on the sidecar over the
KB's private mTLS hop, so this also proves the loopback adapter reaches it with
the KB's client identity.

Run as root on a disposable Docker Linux VM. Candidate images are supplied in
AIMEE_APPLICATION_IMAGE, AIMEE_POSTGRES_IMAGE, AIMEE_EMBEDDER_IMAGE and
AIMEE_LLM_IMAGE. Evidence contains verdicts and sanitized states only.
"""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import time

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('matrix', ROOT / 'tests/e2e/deployment-matrix.py')
matrix = importlib.util.module_from_spec(spec)
spec.loader.exec_module(matrix)

MODELS = ('gemma-4-E4B-it', 'gemma-4-E2B-it')  # the supported synthesis models
# Simple undated statements. The memory module keeps only candidates grounded
# in the note and in the active ontology, and a small CPU model gets some of
# these wrong, so the gate requires at least one to come through, not every one.
FACTS = {
    'Margaret Hamilton': 'Margaret Hamilton works at MIT.',
    'Katherine Johnson': 'Katherine Johnson works at NASA Langley.',
    'Grace Hopper': 'Grace Hopper works at the Harvard Computation Laboratory.',
    'Alan Turing': 'Alan Turing works at Bletchley Park.',
}
# A relation the seed ontology lacks, in three distinct notes: the operator
# records each occurrence and promotes the relation at the third (aimee 1.0's
# threshold). A fourth note then publishes under the promoted relation.
NOVEL = 'mentors'
NOVEL_NOTES = ['Barbara Liskov mentors Andrew Myers.', 'Donald Knuth mentors Luis Trabb Pardo.',
               'Frances Allen mentors Vivek Sarkar.']
NOVEL_PROBE = ('Ada Lovelace', 'Ada Lovelace mentors Mary Somerville.')


def operator_lines(stack):
    """The processing operator's typed-fact lines from the KB's log (relation
    names and counts only; it never logs memory text)."""
    logs = subprocess.run(['docker', 'logs', '--tail', '5000', stack.application], capture_output=True,
                          text=True, timeout=60)
    return [line[line.index('typed facts:'):] for line in (logs.stdout + logs.stderr).splitlines()
            if 'typed facts:' in line]


def wait_publications(stack, count, seconds):
    # Receipt-bearing native reads revalidate the selected inventory before the
    # next provider send. Wait for this fixture's writes to finish rather than
    # racing extraction and treating a correct stale-source refusal as success.
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        if sum(line.startswith('typed facts: published ') for line in operator_lines(stack)) >= count:
            return True
        time.sleep(2)
    return False


def wait_facts(stack, name, seconds, needle=''):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        status, result = stack.kb_request('/v1/actions/memory.facts', dict(query=name))
        facts = result.get('facts', '') if status == 200 and result.get('status') == 'ok' else ''
        if facts and needle in facts:
            return facts
        time.sleep(15)
    return ''


SERVER_HTTP = '''import http.client,socket,json,sys
class C(http.client.HTTPConnection):
 def connect(self):
  self.sock=socket.socket(socket.AF_UNIX);self.sock.settimeout(90);self.sock.connect('/var/lib/aimee/aimee-http.sock')
a=json.load(sys.stdin);c=C('localhost',timeout=90)
c.request('POST',a['path'],json.dumps(a['body']),{'Content-Type':'application/json'})
r=c.getresponse();print(json.dumps([r.status,json.loads(r.read() or b'{}')]))
'''


def server_request(stack, path, body):
    """One request to the Server's own HTTP socket, as the account (uid 1000)."""
    result = subprocess.run(['docker', 'exec', '-i', '-u', '1000', stack.application, 'python3', '-c', SERVER_HTTP],
                            input=json.dumps(dict(path=path, body=body)), capture_output=True, text=True, timeout=120)
    try:
        return tuple(json.loads(result.stdout.strip().splitlines()[-1]))
    except (IndexError, ValueError):
        return 0, {}


def server_tool_text(stack, tool, arguments):
    helper = (ROOT / 'tests/e2e/typed-facts-native-query.py').read_text()
    result = subprocess.run(['docker', 'exec', '-i', '-u', '1000', '-e', 'AIMEE_TYPED_FACTS_NATIVE_FIXTURE=1',
                             stack.application, 'python3', '-c', helper],
                            input=json.dumps(dict(tool=tool, arguments=dict(arguments, store='user'))),
                            capture_output=True, text=True, timeout=150)
    try:
        body = json.loads(result.stdout.strip().splitlines()[-1])
    except (ValueError, IndexError):
        return ''
    if body.get('status') != 'completed' or body.get('provider_requests') != 2:
        print('Native graph query diagnostic: ' + json.dumps(body), flush=True)
        raise RuntimeError('native graph query did not complete its two provider requests')
    if 'source_custody_unavailable' in json.dumps(body.get('outputs', [])):
        raise RuntimeError('native graph tool refused source custody')
    return json.dumps(body.get('outputs', []))


def wait_tool(stack, tool, arguments, needles, seconds):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        text = server_tool_text(stack, tool, arguments)
        if all(needle in text for needle in needles):
            return text
        time.sleep(15)
    return ''


def exercise_server(stack, check, diagnostics, extraction_seconds):
    """A Server's typed facts over its account's personal memory, read through
    the Server's own MCP graph tools."""
    stack.start()
    check('Server with synthesis profile reaches healthy state', True)
    check('synthesis sidecar becomes healthy', wait_healthy(stack.project + '-aimee-llm-1', 900))
    for index, content in enumerate(FACTS.values()):
        status, stored = server_request(stack, '/v1/memory/store', dict(key=f'typed-facts-e2e-{index}', content=content))
        check(f'Server stores personal fact {index + 1}', status == 200 and stored.get('status') == 'ok')
    check('the processing worker finishes the four personal notes',
          wait_publications(stack, len(FACTS), extraction_seconds))
    started, subject_name, text = time.monotonic(), '', ''
    deadline = started + extraction_seconds
    while time.monotonic() < deadline and not text:
        for name in FACTS:
            text = server_tool_text(stack, 'search_graph', dict(query=name))
            if name in text and 'aims-parent-typed-graph/v1' in text:
                subject_name = name
                break
            text = ''
        else:
            time.sleep(15)
    diagnostics['extraction_seconds'] = round(time.monotonic() - started, 1)
    diagnostics['operator'] = operator_lines(stack)[-40:]
    check('the memory module publishes a typed fact for personal memory',
          any(line.startswith('typed facts: published ') and not line.startswith('typed facts: published 0 ')
              for line in operator_lines(stack)))
    check('search_graph serves the personal typed fact', bool(text) and subject_name in text)
    diagnostics['first_typed_subject'] = subject_name
    edges = server_tool_text(stack, 'get_entity_edges', dict(entity=subject_name))
    check('get_entity_edges serves the edges of that subject', subject_name in edges and 'aims-parent-typed-graph/v1' in edges)
    for index, content in enumerate(NOVEL_NOTES):
        status, stored = server_request(stack, '/v1/memory/store', dict(key=f'typed-facts-novel-{index}', content=content))
        check(f'Server stores novel-relation note {index + 1}', status == 200 and stored.get('status') == 'ok')
    deadline, promoted = time.monotonic() + extraction_seconds, False
    while time.monotonic() < deadline and not promoted:
        promoted = any(line.startswith('typed facts: promoted relation ' + NOVEL + ' ')
                       for line in operator_lines(stack))
        if not promoted:
            time.sleep(15)
    diagnostics['operator'] = operator_lines(stack)[-40:]
    check(f'the operator promotes {NOVEL} after three distinct notes', promoted)
    published = sum(line.startswith('typed facts: published ') for line in operator_lines(stack))
    name, content = NOVEL_PROBE
    status, stored = server_request(stack, '/v1/memory/store', dict(key='typed-facts-novel-probe', content=content))
    check('Server stores a note under the promoted relation', status == 200 and stored.get('status') == 'ok')
    check('the processing worker finishes the personal probe',
          wait_publications(stack, published + 1, extraction_seconds))
    text = wait_tool(stack, 'search_graph', dict(query=name), [name, NOVEL], extraction_seconds)
    diagnostics['operator'] = operator_lines(stack)[-40:]
    check('a personal fact under the promoted relation is published', bool(text))


def wait_healthy(container, seconds):
    deadline = time.monotonic() + seconds
    status = ''
    while time.monotonic() < deadline:
        status = matrix.command('docker', 'inspect', '--format', '{{.State.Health.Status}}', container)
        if status == 'healthy':
            return True
        time.sleep(5)
    return False


def exercise(stack, check, diagnostics, extraction_seconds):
    stack.start()
    check('KB with synthesis profile reaches healthy state', True)
    llm = stack.project + '-aimee-llm-1'
    check('synthesis sidecar becomes healthy', wait_healthy(llm, 900))
    for index, content in enumerate(FACTS.values()):
        status, stored = stack.kb_request('/v1/actions/memory.store', dict(key=f'typed-facts-e2e-{index}', content=content))
        check(f'KB stores shared fact {index + 1}', status == 200 and stored.get('status') == 'ok' and stored.get('id'))
    deadline = time.monotonic() + extraction_seconds
    facts, subject_name = '', ''
    started = time.monotonic()
    while time.monotonic() < deadline and not facts:
        for name in FACTS:
            status, result = stack.kb_request('/v1/actions/memory.facts', dict(query=name))
            facts = result.get('facts', '') if status == 200 and result.get('status') == 'ok' else ''
            if facts:
                subject_name = name
                break
        else:
            time.sleep(15)
    diagnostics['extraction_seconds'] = round(time.monotonic() - started, 1)
    if not facts:
        diagnostics['operator'] = operator_lines(stack)[-40:]
        # Shape only: row counts, kinds and which interpretation markers are
        # present. No memory text or model output is recorded.
        status, recalled = stack.kb_request('/v1/actions/memory.recall',
                                            dict(query=next(iter(FACTS)), scope='all', limit=20))
        rows = (recalled.get('recall') or {}).get('active_context') or recalled.get('results') or []
        diagnostics['recall'] = dict(
            status=status, result=recalled.get('status'), keys=sorted(recalled)[:12], rows=len(rows),
            kinds=sorted({str(row.get('kind')) for row in rows if isinstance(row, dict)}),
            with_accepted=sum(1 for row in rows if isinstance(row, dict)
                              and isinstance((row.get('attributes') or {}).get('accepted'), dict)),
            attribute_keys=sorted({key for row in rows if isinstance(row, dict)
                                   for key in (row.get('attributes') or {})})[:20])
    check('KB worker publishes a typed fact for the shared memory',
          facts.startswith('Source-bound typed facts') and 'not verified world truth' in facts and '\n- ' in facts)
    check('typed fact names the stored subject', subject_name in facts)
    diagnostics['first_typed_subject'] = subject_name
    status, graph = stack.kb_request('/v1/actions/memory.search_graph', dict(query=subject_name))
    relations = graph.get('relations') or []
    check('search_graph serves the typed fact as a relation row', status == 200 and graph.get('status') == 'ok' and
          graph.get('relation_extraction') == 'processing_pipeline' and relations)
    check('every relation row is marked as a Model interpretation',
          all(row.get('interpretation') == 'model' for row in relations))
    subject = relations[0].get('src_entity', '')
    status, edges = stack.kb_request('/v1/actions/memory.entity_edges', dict(entity=subject))
    rows = edges.get('edges') or []
    check('entity_edges serves the edges of that subject', status == 200 and rows and
          all(subject in (row.get('src_entity'), row.get('dst_entity')) for row in rows))

    for index, content in enumerate(NOVEL_NOTES):
        status, stored = stack.kb_request('/v1/actions/memory.store', dict(key=f'typed-facts-novel-{index}',
                                                                           content=content))
        check(f'KB stores novel-relation note {index + 1}', status == 200 and stored.get('status') == 'ok')
    deadline, promoted = time.monotonic() + extraction_seconds, False
    while time.monotonic() < deadline and not promoted:
        promoted = any(line.startswith('typed facts: promoted relation ' + NOVEL + ' ')
                       for line in operator_lines(stack))
        if not promoted:
            time.sleep(15)
    diagnostics['operator'] = operator_lines(stack)[-40:]
    check(f'the operator promotes {NOVEL} after three distinct notes', promoted)
    name, content = NOVEL_PROBE
    status, stored = stack.kb_request('/v1/actions/memory.store', dict(key='typed-facts-novel-probe',
                                                                       content=content))
    check('KB stores a note under the promoted relation', status == 200 and stored.get('status') == 'ok')
    facts = wait_facts(stack, name, extraction_seconds, NOVEL)
    diagnostics['operator'] = operator_lines(stack)[-40:]
    check('a fact under the promoted relation is published', NOVEL in facts and name in facts)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--keep', action='store_true', help='retain test containers for diagnosis')
    parser.add_argument('--role', choices=('kb', 'server'), default='kb',
                        help='kb: shared memory on a KB; server: an account\'s personal memory on a Server')
    parser.add_argument('--model', choices=MODELS, default=MODELS[0],
                        help='synthesis model the AIMEE_LLM_IMAGE sidecar serves')
    parser.add_argument('--extraction-seconds', type=int, default=2400,
                        help='bound on CPU extraction by the sidecar model')
    args = parser.parse_args()
    if os.geteuid() != 0:
        parser.error('run as root in a disposable Docker VM')
    for name in ('AIMEE_APPLICATION_IMAGE', 'AIMEE_POSTGRES_IMAGE', 'AIMEE_EMBEDDER_IMAGE', 'AIMEE_LLM_IMAGE'):
        if not os.environ.get(name):
            parser.error(name + ' must name the candidate image')
    args.output.mkdir(parents=True, exist_ok=True)
    checks, diagnostics = [], {}
    stack = None

    def check(name, passed):
        checks.append(dict(name=name, passed=bool(passed)))
        print(('PASS ' if passed else 'FAIL ') + name, flush=True)
        if not passed:
            raise RuntimeError(name)

    try:
        env = dict(os.environ, AIMEE_RUNTIME_WEB_ENABLED='0', AIMEE_POSTGRES_VOLUME_MIB='2048',
                   COMPOSE_PROFILES='synthesis', EMBEDDER_MODEL='bekko-a25m',
                   EMBEDDER_URL='https://aimee-embedder:8762', EMBEDDER_DIMS='384',
                   SYNTHESIS_MODEL=args.model, SYNTHESIS_ENDPOINT='https://aimee-llm:8761/v1')
        stack = matrix.Stack(args.role, env, args.output)
        if args.keep and args.role == 'kb':
            # A kept disposable stack is for diagnosis: keep its generated test
            # credential beside it, owner-only, so the stack can be driven.
            token = args.output / 'kb-bearer-token'
            token.write_text(stack.env['AIMEE_KB_API_BEARER_TOKEN'])
            token.chmod(0o600)
        (exercise if args.role == 'kb' else exercise_server)(stack, check, diagnostics, args.extraction_seconds)
    except (RuntimeError, OSError, ValueError, KeyError, IndexError, subprocess.SubprocessError):
        checks.append(dict(name='KB typed facts completed', passed=False))
        print('FAIL KB typed facts completed', flush=True)
        if stack:
            for name in (stack.application, stack.project + '-aimee-llm-1'):
                try:
                    state = json.loads(matrix.command('docker', 'inspect', '--format', '{{json .State}}', name))
                    diagnostics[name.rsplit('-', 2)[-2]] = matrix.safe_container_state(state)
                except (RuntimeError, OSError, ValueError, subprocess.SubprocessError):
                    pass
    finally:
        (args.output / 'kb-typed-facts.json').write_text(
            json.dumps(dict(checks=checks, diagnostics=diagnostics), indent=2) + '\n')
        if stack and not args.keep:
            stack.compose('down', '--volumes', '--remove-orphans')
    return 0 if checks and all(check['passed'] for check in checks) else 1


if __name__ == '__main__':
    raise SystemExit(main())
