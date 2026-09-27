#!/usr/bin/env python3
"""MR-07 paired task gate. No serving code consumes evaluation labels.

A manifest is frozen before collection. Every ordered case must have both arms;
missing/invalid cells prevent promotion. This scorer cannot attest the collector,
judge or provenance of supplied observations and never changes operator policy.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import math
from pathlib import Path

POLICY = {
    'task_success_noninferiority_margin': 0.01,
    'paired_confidence': 0.95,
    'maximum_p95_latency_ratio': 1.10,
    'maximum_total_cost_ratio': 1.0,
    'minimum_redundant_scan_reduction': 0.20,
}
PINNED = ('corpus_sha256', 'model', 'tokenizer', 'index_generation', 'reader', 'judge',
          'seed', 'query_class', 'context_budget', 'cost_components', 'ingestion', 'extractor',
          'renderer', 'requested_candidate_limits', 'effective_candidate_limits', 'cache_state', 'sampling_unit')


def digest(raw: bytes) -> str:
    return hashlib.sha256(raw).hexdigest()


def finite(value, *, integer=False):
    return type(value) in ((int,) if integer else (int, float)) and math.isfinite(value) and value >= 0


def binomial_cdf(k: int, total: int, p: float) -> float:
    if k < 0: return 0.0
    if k >= total or p <= 0: return 1.0
    if p >= 1: return 0.0
    if k >= total*p:
        return 1-binomial_cdf(total-k-1, total, 1-p)
    # Sum downwards from the largest term in this lower tail. Starting at
    # P(X=0) instead would underflow for large n even near the central mass.
    term = math.exp(math.lgamma(total+1)-math.lgamma(k+1)-math.lgamma(total-k+1)
                    + k*math.log(p) + (total-k)*math.log1p(-p))
    value = term
    for j in range(k, 0, -1):
        term *= j/(total-j+1)*(1-p)/p
        value += term
    return min(1.0, value)


def exact_binomial_interval(successes: int, total: int, tail: float):
    def invert(k, target):
        low, high = 0.0, 1.0
        for _ in range(64):
            mid = (low+high)/2
            if binomial_cdf(k, total, mid) > target: low = mid
            else: high = mid
        return (low+high)/2
    low = 0.0 if successes == 0 else invert(successes-1, 1-tail)
    high = 1.0 if successes == total else invert(successes, tail)
    return low, high


def paired_success_interval(wins: int, losses: int, total: int):
    # Difference is P(treatment-only success) - P(baseline-only success).
    # Bonferroni combines two exact 97.5% Clopper-Pearson intervals into a
    # conservative 95% paired interval; no independence between arms assumed.
    # Independent task pairs are the sampling unit. Repeats of one task must
    # be clustered before collection, not counted as additional independent n.
    tail = (1-POLICY['paired_confidence'])/4
    win = exact_binomial_interval(wins, total, tail)
    loss = exact_binomial_interval(losses, total, tail)
    return max(-1.0, win[0]-loss[1]), min(1.0, win[1]-loss[0])


def score(manifest: dict, results: dict, manifest_sha256: str) -> dict:
    if manifest.get('schema_version') != 1 or manifest.get('policy') != POLICY:
        raise ValueError('unsupported manifest or changed predeclared policy')
    if results.get('schema_version') != 1 or results.get('manifest_sha256') != manifest_sha256:
        raise ValueError('results do not bind the frozen manifest')
    ids = manifest.get('ordered_case_ids')
    if not isinstance(ids, list) or not ids or any(not isinstance(x, str) or not x for x in ids) or len(set(ids)) != len(ids):
        raise ValueError('manifest case IDs must be nonempty and unique')
    for key in PINNED:
        if key not in manifest or manifest[key] in (None, '', [], {}):
            raise ValueError('manifest lacks pinned ' + key)
        if results.get(key) != manifest[key]:
            raise ValueError('results changed pinned ' + key)
    if manifest['sampling_unit'] != 'independent_task':
        raise ValueError('paired interval requires independent task-level observations')
    for arm in ('baseline', 'treatment'):
        if not isinstance(manifest.get(arm), dict) or not all(manifest[arm].get(k) for k in ('implementation', 'policy_sha256', 'feature_flags')):
            raise ValueError('missing implementation/flags for ' + arm)
        if results.get(arm) != manifest[arm]:
            raise ValueError('changed implementation/flags for ' + arm)
    rows = results.get('pairs')
    if not isinstance(rows, list) or [row.get('case_id') for row in rows if isinstance(row, dict)] != ids:
        raise ValueError('missing, reordered or duplicate paired results')
    invalid = []
    for row in rows:
        for arm in ('baseline', 'treatment'):
            cell = row.get(arm)
            if not isinstance(cell, dict) or type(cell.get('eligible')) is not bool:
                raise ValueError('cell lacks explicit collection eligibility')
            if not cell['eligible']:
                invalid.append({'case_id': row['case_id'], 'arm': arm, 'reason': cell.get('invalid_reason', 'unspecified')})
                continue
            for key in ('completed', 'correct'):
                if type(cell.get(key)) is not bool:
                    raise ValueError('missing explicit task outcome')
            for key in ('raw_scans', 'redundant_scans', 'restrictions', 'false_restrictions', 'expansions'):
                if not finite(cell.get(key), integer=True):
                    raise ValueError('invalid count ' + key)
            if cell['redundant_scans'] > cell['raw_scans'] or cell['false_restrictions'] > cell['restrictions']:
                raise ValueError('invalid labeled numerator')
            if not finite(cell.get('latency_seconds')) or cell['latency_seconds'] == 0 or not finite(cell.get('total_cost')):
                raise ValueError('invalid measured latency/cost')
            if not isinstance(cell.get('evidence_sha256'), str) or len(cell['evidence_sha256']) != 64 or any(c not in '0123456789abcdef' for c in cell['evidence_sha256']):
                raise ValueError('missing raw evidence commitment')
    base = dict(schema_version=1, manifest_sha256=manifest_sha256, policy=POLICY,
                expected_pairs=len(ids), invalid_cells=invalid, decision='observe',
                paired_interval_method='bonferroni_clopper_pearson_discordant_rates_95_percent')
    if invalid:
        return dict(base, measured_pairs=None, reason='incomplete_collection', gates=None, metrics=None)
    success = lambda c: c['completed'] and c['correct']
    wins = sum(success(r['treatment']) and not success(r['baseline']) for r in rows)
    losses = sum(success(r['baseline']) and not success(r['treatment']) for r in rows)
    low, high = paired_success_interval(wins, losses, len(rows))
    aggregates = {}
    for arm in ('baseline', 'treatment'):
        cells = [r[arm] for r in rows]
        counts = {k: sum(c[k] for c in cells) for k in ('raw_scans', 'redundant_scans', 'restrictions', 'false_restrictions', 'expansions', 'total_cost')}
        counts.update(task_successes=sum(map(success, cells)),
                      p95_latency_seconds=sorted(c['latency_seconds'] for c in cells)[math.ceil(.95*len(cells))-1],
                      false_restriction_rate=counts['false_restrictions']/counts['restrictions'] if counts['restrictions'] else None,
                      expansions_per_task=counts['expansions']/len(cells))
        aggregates[arm] = counts
    b, t = aggregates['baseline'], aggregates['treatment']
    reduction = 1-t['redundant_scans']/b['redundant_scans'] if b['redundant_scans'] else None
    gates = dict(task_success_noninferior=low >= -POLICY['task_success_noninferiority_margin'],
                 p95_latency=t['p95_latency_seconds'] <= POLICY['maximum_p95_latency_ratio']*b['p95_latency_seconds'],
                 total_cost=t['total_cost'] <= POLICY['maximum_total_cost_ratio']*b['total_cost'],
                 redundant_scans=reduction is not None and 5*t['redundant_scans'] <= 4*b['redundant_scans'])
    return dict(base, measured_pairs=len(rows), gates=gates,
                decision='eligible_for_operator_review' if all(gates.values()) else 'observe',
                metrics=dict(arms=aggregates, paired_success_difference=(wins-losses)/len(rows),
                             paired_success_interval=[low, high], redundant_scan_reduction=reduction))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--manifest', type=Path, required=True)
    parser.add_argument('--results', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    raw = args.manifest.read_bytes()
    results = args.results.read_bytes()
    report = score(json.loads(raw), json.loads(results), digest(raw))
    report['results_sha256'] = digest(results)
    # Immutable reports: a later failed experiment cannot overwrite a pass.
    with args.output.open('x') as output:
        json.dump(report, output, indent=2, allow_nan=False)
        output.write('\n')
    return 0 if report['decision'] == 'eligible_for_operator_review' else 1


if __name__ == '__main__':
    raise SystemExit(main())
