"""Versioned paired task-quality gates, separate from deterministic retrieval tests.

The manifest is frozen before execution; gold labels stay in the evaluator.
Missing, invalid and timed-out attempts never disappear from the denominator.
"""
from __future__ import annotations
import hashlib
import json
import math
import random

POLICY = {"version": 1, "minimum_pairs": 30, "bootstrap_repetitions": 10000,
          "task_success_paired_95ci_lower_min": -.01, "p95_latency_ratio_max": 1.10,
          "total_cost_ratio_max": 1.0, "exploration_discovery_ratio_max": .8}
PINNED = ("implementation", "dataset_sha256", "input_corpus_sha256", "retrieval_unit",
          "answerability", "model", "tokenizer", "index_generations", "ingestion",
          "extractor", "policy", "feature_flags", "reader", "judge", "seed",
          "requested_candidate_limit", "effective_candidate_limit", "final_payload_budget")


def manifest_digest(manifest):
    return hashlib.sha256(json.dumps(manifest, sort_keys=True, separators=(',', ':'), allow_nan=False).encode()).hexdigest()


def validate_manifest(manifest):
    if manifest.get('version') != 1 or manifest.get('track') not in ('protocol-compatible', 'product-optimized'):
        raise ValueError('experiment requires version 1 and a separate declared track')
    ids = manifest.get('case_ids')
    if not isinstance(ids, list) or not ids or any(not isinstance(x, str) or not x for x in ids) or len(set(ids)) != len(ids):
        raise ValueError('experiment requires unique ordered case IDs')
    if manifest.get('release_policy') != POLICY:
        raise ValueError('release policy changed; version and review a new experiment')
    if type(manifest.get('seed')) is not int:
        raise ValueError('experiment seed required')
    for arm in ('baseline', 'candidate'):
        pins = manifest.get(arm, {})
        if any(key not in pins or pins[key] is None for key in PINNED):
            raise ValueError(arm + ': incomplete experiment identity')
        if pins['answerability'].keys() != set(ids) or any(type(x) is not bool for x in pins['answerability'].values()):
            raise ValueError('complete explicit answerability labels required')
        for key in ('dataset_sha256', 'input_corpus_sha256'):
            value = pins[key]
            if not isinstance(value, str) or len(value) != 64 or any(c not in '0123456789abcdef' for c in value):
                raise ValueError('invalid corpus digest')
        if pins['retrieval_unit'] not in ('turn', 'claim', 'chunk', 'session', 'document'):
            raise ValueError('retrieval unit required')
    for key in ('dataset_sha256', 'input_corpus_sha256', 'answerability', 'model', 'tokenizer', 'reader', 'judge', 'seed', 'final_payload_budget'):
        if manifest['baseline'][key] != manifest['candidate'][key]:
            raise ValueError('unmatched paired input: ' + key)


def validate_arm(manifest, arm, payload):
    if payload.get('manifest_sha256') != manifest_digest(manifest) or payload.get('arm') != arm:
        raise ValueError('run does not bind the frozen experiment/arm')
    rows = payload.get('results', [])
    if [r.get('id') for r in rows] != manifest['case_ids']:
        raise ValueError('missing, duplicate or reordered paired results')
    for row in rows:
        if row.get('status') not in ('ok', 'invalid', 'timeout', 'infrastructure_error'):
            raise ValueError('explicit run status required')
        if type(row.get('success')) is not bool:
            raise ValueError('reviewed task-success result required')
        for key in ('latency_s', 'total_cost', 'discovery_calls'):
            value = row.get(key)
            if type(value) not in (int, float) or not math.isfinite(value) or value < 0:
                raise ValueError('finite measured nonnegative ' + key + ' required')
    if payload.get('cost_kind') not in ('billed', 'estimated-token-rates') or not payload.get('cost_model'):
        raise ValueError('declared stage-total cost source/model required')
    return rows


def evaluate(manifest, baseline, candidate):
    validate_manifest(manifest)
    a, b = validate_arm(manifest, 'baseline', baseline), validate_arm(manifest, 'candidate', candidate)
    if (baseline['cost_kind'], baseline['cost_model']) != (candidate['cost_kind'], candidate['cost_model']):
        raise ValueError('paired cost accounting differs')
    failures = [{'arm': arm, 'id': r['id'], 'status': r['status']} for arm, rows in [('baseline', a), ('candidate', b)] for r in rows if r['status'] != 'ok']
    report = {'version': 1, 'manifest_sha256': manifest_digest(manifest), 'status': 'unqualified',
              'pairs': len(a), 'failed_attempts': failures, 'cost_kind': baseline['cost_kind'],
              'policy': POLICY, 'reason': 'insufficient pairs or invalid attempts'}
    if len(a) < POLICY['minimum_pairs'] or failures:
        return report
    differences = [int(y['success']) - int(x['success']) for x, y in zip(a, b)]
    rng = random.Random(manifest['seed'])
    means = sorted(sum(rng.choices(differences, k=len(a))) / len(a) for _ in range(POLICY['bootstrap_repetitions']))
    interval = [means[int(len(means) * .025)], means[int(len(means) * .975) - 1]]
    def p95(rows):
        return sorted(r['latency_s'] for r in rows)[math.ceil(len(rows) * .95) - 1]
    def ratio(new, old):
        return new / old if old else (1.0 if new == 0 else None)
    latency = ratio(p95(b), p95(a))
    cost = ratio(sum(r['total_cost'] for r in b), sum(r['total_cost'] for r in a))
    discovery = ratio(sum(r['discovery_calls'] for r in b), sum(r['discovery_calls'] for r in a))
    gates = {'task_success_noninferiority': interval[0] >= POLICY['task_success_paired_95ci_lower_min'],
             'p95_latency': latency is not None and latency <= POLICY['p95_latency_ratio_max']}
    if manifest.get('efficiency_claim'):
        gates['total_task_cost'] = cost is not None and cost <= POLICY['total_cost_ratio_max']
    if manifest.get('exploration_promotion'):
        gates['redundant_discovery'] = discovery is not None and discovery <= POLICY['exploration_discovery_ratio_max']
    report.update(status='qualified' if all(gates.values()) else 'unqualified', reason='fixed paired thresholds',
                  success_difference_95ci=interval, uncertainty='paired percentile bootstrap',
                  p95_latency_ratio=latency, total_cost_ratio=cost, discovery_ratio=discovery, gates=gates)
    return report
