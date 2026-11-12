#!/usr/bin/env python3
"""A KB with a synthesis model derives typed facts from its shared memory.

Brings up the shipped KB Compose stack with the managed aimee-llm sidecar
(profile `synthesis`), stores one shared fact on the KB, and requires the KB's
own extraction pipeline to publish its typed interpretation: `memory.facts`
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

MODEL = 'gemma-4-E2B-it'
FACT = 'Ada Lovelace works at the Analytical Engine Company in London.'


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
    status, stored = stack.kb_request('/v1/actions/memory.store', dict(key='typed-facts-e2e', content=FACT))
    check('KB stores the shared fact', status == 200 and stored.get('status') == 'ok' and stored.get('id'))
    deadline = time.monotonic() + extraction_seconds
    facts = ''
    started = time.monotonic()
    while time.monotonic() < deadline:
        status, result = stack.kb_request('/v1/actions/memory.facts', dict(query='Ada Lovelace'))
        facts = result.get('facts', '') if status == 200 and result.get('status') == 'ok' else ''
        if facts:
            break
        time.sleep(10)
    diagnostics['extraction_seconds'] = round(time.monotonic() - started, 1)
    check('KB worker publishes a typed fact for the shared memory',
          facts.startswith('Source-bound typed facts') and 'not verified world truth' in facts and '\n- ' in facts)
    check('typed fact names the stored subject', 'Ada Lovelace' in facts)
    status, graph = stack.kb_request('/v1/actions/memory.search_graph', dict(query='Ada Lovelace'))
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


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--keep', action='store_true', help='retain test containers for diagnosis')
    parser.add_argument('--extraction-seconds', type=int, default=1200,
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
                   SYNTHESIS_MODEL=MODEL, SYNTHESIS_ENDPOINT='https://aimee-llm:8761/v1')
        stack = matrix.Stack('kb', env, args.output)
        exercise(stack, check, diagnostics, args.extraction_seconds)
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
