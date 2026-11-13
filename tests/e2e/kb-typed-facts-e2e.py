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


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--keep', action='store_true', help='retain test containers for diagnosis')
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
        stack = matrix.Stack('kb', env, args.output)
        if args.keep:
            # A kept disposable stack is for diagnosis: keep its generated test
            # credential beside it, owner-only, so the stack can be driven.
            token = args.output / 'kb-bearer-token'
            token.write_text(stack.env['AIMEE_KB_API_BEARER_TOKEN'])
            token.chmod(0o600)
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
