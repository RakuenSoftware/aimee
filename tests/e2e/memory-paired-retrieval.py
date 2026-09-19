#!/usr/bin/env python3
"""Compare two immutable application images on fresh KB stores and a real embedder.

Uses identical public requests, corpus bytes, model image and hardware. Reports
per-case rankings and request latency, including Docker exec overhead on both
sides. Never substitutes lexical-only results for missing embedding readiness.
"""
import argparse
from datetime import datetime, timezone
import hashlib
import importlib.util
import json
import math
import os
from pathlib import Path
import statistics
import time

spec = importlib.util.spec_from_file_location('matrix', Path(__file__).with_name('deployment-matrix.py'))
matrix = importlib.util.module_from_spec(spec)
spec.loader.exec_module(matrix)


def metrics(expected, retrieved):
    relevant = set(expected)
    ranks = [i + 1 for i, fid in enumerate(retrieved) if fid in relevant]
    result = {'mrr': 1 / ranks[0] if ranks else 0.0}
    for k in (5, 10):
        result['recall_' + str(k)] = len(relevant.intersection(retrieved[:k])) / len(relevant)
        ideal = sum(1 / math.log2(i + 2) for i in range(min(k, len(relevant))))
        result['ndcg_' + str(k)] = sum(1 / math.log2(r + 1) for r in ranks if r <= k) / ideal
    return result


def percentile(values, q):
    ordered = sorted(values)
    return ordered[max(0, math.ceil(len(ordered) * q) - 1)]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--baseline', required=True)
    parser.add_argument('--candidate', required=True)
    parser.add_argument('--baseline-revision', required=True)
    parser.add_argument('--candidate-revision', required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--measurement-ready-file', type=Path, help='wait for a coordinator to finish other workloads before timing')
    parser.add_argument('--keep', action='store_true', help='retain owned stacks for failure investigation')
    parser.add_argument('--corpus', type=Path, default=matrix.ROOT / 'tests/eval/memory_retrieval_corpus.json')
    parser.add_argument('--repetitions', type=int, default=3)
    args = parser.parse_args()
    if args.repetitions < 2:
        parser.error('paired measurements require at least two repetitions')
    args.output.mkdir(parents=True, exist_ok=True)
    raw = args.corpus.read_bytes()
    corpus = json.loads(raw)
    env = dict(os.environ, AIMEE_RUNTIME_WEB_ENABLED='0', COMPOSE_PROFILES='',
               EMBEDDER_MODEL='bekko-a25m', EMBEDDER_URL='https://aimee-embedder:8762', EMBEDDER_DIMS='384')
    for name in ('AIMEE_POSTGRES_IMAGE', 'AIMEE_EMBEDDER_IMAGE'):
        if not env.get(name):
            parser.error(name + ' must name a pinned image')
    report = {'harness_sha256': hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
              'started_utc': datetime.now(timezone.utc).isoformat(),
              'embedding_model': 'bekko-a25m', 'embedding_dimensions': 384,
              'provider_images': {key: dict(image=env[key], image_id=matrix.command('docker', 'image', 'inspect', '--format', '{{.Id}}', env[key]))
                                  for key in ('AIMEE_POSTGRES_IMAGE', 'AIMEE_EMBEDDER_IMAGE')},
              'query_policy': 'full corpus query, limit=20, scope=all; default rank settings',
              'corpus_sha256': hashlib.sha256(raw).hexdigest(), 'repetitions': args.repetitions,
              'fixtures': len(corpus['fixtures']), 'cases': len(corpus['cases']),
              'latency_boundary': 'public KB HTTP via docker exec; identical harness for both images',
              'arms': {}, 'complete': False}
    stacks = []
    try:
        for label, image in [('baseline', args.baseline), ('candidate', args.candidate)]:
            stack = matrix.Stack('kb', dict(env, AIMEE_APPLICATION_IMAGE=image), args.output)
            stacks.append(stack)
            arm = report['arms'][label] = {'image': image, 'image_id': matrix.command(
                'docker', 'image', 'inspect', '--format', '{{.Id}}', image), 'project': stack.project,
                'revision': args.baseline_revision if label == 'baseline' else args.candidate_revision,
                'runs': []}
            stack.start()
            ids = {}
            for row in corpus['fixtures']:
                code, reply = stack.kb_request('/v1/actions/memory.store',
                    {key: row[key] for key in ('tier', 'kind', 'key', 'content')})
                if code != 200 or reply.get('status') != 'ok' or not reply.get('id'):
                    raise RuntimeError(label + ' corpus store refused: ' + row['fid'])
                ids[str(reply['id'])] = row['fid']
            stack.fixture_ids = ids
            if label == 'candidate':
                # Background indexing shares the re-embedding maintenance lock.
                # Wait for its durable queue before requesting exclusive cutover.
                deadline = time.monotonic() + 1200
                next_progress = 0
                while True:
                    pending = matrix.command('docker', 'exec', stack.postgres, 'psql', '-U', 'postgres',
                        '-d', 'aimee_store', '-At', '-c',
                        "SELECT (SELECT count(*) FROM kb_async_jobs WHERE kind='memory_index' AND status<>'done') + "
                        "(SELECT count(*) FROM vector_index_ops WHERE status IN ('pending','failed'))")
                    if time.monotonic() >= next_progress:
                        print('candidate: pending index jobs ' + pending, flush=True)
                        next_progress = time.monotonic() + 30
                    if pending == '0':
                        break
                    if time.monotonic() > deadline:
                        raise RuntimeError('candidate background indexing failed to drain: ' + pending)
                    time.sleep(2)
            # The legacy image exposes direct indexing; the candidate requires
            # a verified, activated version. Both must persist every real vector.
            steps = [('embed', dict(all=True, version='paired-bekko-a25m',
                                    embedding_command=env['EMBEDDER_URL']))] if label == 'baseline' else [
                ('reembed_start', dict(version='paired-bekko-a25m',
                    embedding_command=env['EMBEDDER_URL'], scope='all')),
                ('reembed_cutover', dict(scope='all'))]
            for verb, body in steps:
                attempts = []
                deadline = time.monotonic() + 1200
                while True:
                    code, reply = stack.kb_request('/v1/actions/memory.' + verb, body)
                    attempts.append(dict(http_status=code, response=reply))
                    arm[verb] = attempts
                    if code != 200 or reply.get('status') != 'ok' or reply.get('failed', 0):
                        raise RuntimeError(label + ' ' + verb + ' failed: ' + json.dumps(reply))
                    if verb != 'reembed_start' or reply.get('ready') is True:
                        break
                    if time.monotonic() > deadline:
                        raise RuntimeError(label + ' re-embedding failed to complete')
            deadline = time.monotonic() + 1200
            while True:
                count = int(matrix.command('docker', 'exec', stack.postgres, 'psql', '-U', 'postgres',
                    '-d', 'aimee_store', '-At', '-c', 'SELECT count(*) FROM memory_embeddings e JOIN memories m ON e.point_id=m.id'))
                if count >= len(ids):
                    break
                if time.monotonic() > deadline:
                    raise RuntimeError(label + ' real embedding readiness failed: ' + str(count))
                time.sleep(2)
            arm['embedded_records'] = count
            schema = matrix.command('docker', 'exec', stack.postgres,
                'pg_dump', '-U', 'postgres', '-d', 'aimee_store', '--schema-only', '--no-owner',
                '--no-privileges')
            # PostgreSQL 17 dump guards contain a fresh random nonce per dump.
            schema = '\n'.join(line for line in schema.splitlines()
                               if not line.startswith(('\\restrict ', '\\unrestrict ')))
            arm['schema_sha256'] = hashlib.sha256(schema.encode()).hexdigest()
            arm['fixture_time_range'] = matrix.command('docker', 'exec', stack.postgres,
                'psql', '-U', 'postgres', '-d', 'aimee_store', '-At', '-c',
                'SELECT min(created_at),max(created_at) FROM memories')
            arm['embedding_dimensions'] = matrix.command('docker', 'exec', stack.postgres,
                'psql', '-U', 'postgres', '-d', 'aimee_store', '-At', '-c',
                'SELECT DISTINCT vector_dims(embedding) FROM memory_embeddings ORDER BY 1')
            if arm['embedding_dimensions'] != '384':
                raise RuntimeError(label + ' embedding dimension differs from the pinned model')

            print(label + ': seeded and embedded ' + str(len(ids)), flush=True)
        if args.measurement_ready_file:
            deadline = time.monotonic() + 1800
            while not args.measurement_ready_file.is_file():
                if time.monotonic() > deadline:
                    raise RuntimeError('measurement coordinator did not become ready')
                time.sleep(2)
        report['measurement_started_utc'] = datetime.now(timezone.utc).isoformat()
        # Alternate arms by repetition to reduce systematic warmup/order bias.
        # Warmup is retained separately and excluded from the paired summary.
        for repetition in range(-1, args.repetitions):
            order = (0, 1) if repetition % 2 == 0 else (1, 0)
            for index in order:
                label = ('baseline', 'candidate')[index]
                stack = stacks[index]
                receipts = []
                for case in corpus['cases']:
                    start = time.perf_counter()
                    code, reply = stack.kb_request('/v1/actions/memory.find_facts',
                        dict(query=case['query'], limit=20, scope='all'))
                    elapsed = (time.perf_counter() - start) * 1000
                    if code != 200 or reply.get('status') == 'error' or not isinstance(reply.get('facts'), list):
                        raise RuntimeError(label + ' retrieval failed: ' + case['id'])
                    retrieved = [stack.fixture_ids[str(row['id'])] for row in reply['facts']]
                    if len(set(retrieved)) != len(retrieved):
                        raise RuntimeError(label + ' duplicate retrieval IDs')
                    receipts.append(dict(id=case['id'], expected=case['expected'], retrieved=retrieved,
                        latency_ms=elapsed, metrics=metrics(case['expected'], retrieved)))
                report['arms'][label]['runs'].append(dict(repetition=repetition, cases=receipts))
                print(label + ': repetition ' + str(repetition) + ' complete', flush=True)
        for arm in report['arms'].values():
            rows = [row for run in arm['runs'] if run['repetition'] >= 0 for row in run['cases']]
            latency = [r['latency_ms'] for r in rows]
            arm['summary'] = {k: statistics.mean(r['metrics'][k] for r in rows) for k in rows[0]['metrics']}
            arm['summary'].update(latency_p50_ms=percentile(latency, .5), latency_p95_ms=percentile(latency, .95), samples=len(rows))
        report['delta'] = {key: report['arms']['candidate']['summary'][key] - value
                           for key, value in report['arms']['baseline']['summary'].items()}
        report['complete'] = True
        report['finished_utc'] = datetime.now(timezone.utc).isoformat()
    finally:
        (args.output / 'paired-retrieval.json').write_text(json.dumps(report, indent=2) + '\n')
        for stack in reversed(stacks):
            if not args.keep:
                stack.compose('down', '--volumes', '--remove-orphans')
            else:
                print('Retained disposable project: ' + stack.project, flush=True)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
