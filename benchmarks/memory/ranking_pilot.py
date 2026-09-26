#!/usr/bin/env python3
"""Frozen, controlled MR-09 routing/exposure fit and paired model-answer pilot.

This pilot cannot authorize native serving promotion: its candidate arms/ranks
and prior serving counts are controlled features, not native owner observations.
The adapter is an operator-supplied module exposing request(path, body); credentials
are never stored in the manifest, requests or result files.
"""
import argparse
import hashlib
import importlib.util
import json
import math
import time
from pathlib import Path


def select(case, route, penalty, manifest):
    candidates = [c for c in case['candidates'] if route == 'all' or c['arm'] == route]
    ordered = sorted(candidates, key=lambda c: (not c['mandatory'], c['base_rank'] + min(penalty, c['served_count']), c['base_rank']))
    kept, used = [], 0
    for c in ordered:
        size = len((c['path'] + '\n' + c['source']).encode())
        if len(kept) < manifest['max_items'] and used + size <= manifest['max_context_bytes']:
            kept.append(c)
            used += size
    return kept


def fit(manifest):
    rows = []
    cases = [c for c in manifest['cases'] if c['split'] == 'fit']
    for route in manifest['grid']['routes']:
        for penalty in manifest['grid']['exposure_penalties']:
            score = sum(any(r['symbol'] == c['gold_symbol'] for r in select(c, route, penalty, manifest)) for c in cases)
            rows.append(dict(route=route, exposure_penalty=penalty, gold_retained=score, cases=len(cases)))
    winner = max(rows, key=lambda r: (r['gold_retained'], -r['exposure_penalty'], r['route'] == 'all'))
    return rows, winner


def run(manifest, adapter):
    grid, winner = fit(manifest)
    cells = []
    for index, case in enumerate(c for c in manifest['cases'] if c['split'] == 'heldout'):
        arms = [('baseline', 'all', 0), ('fitted', winner['route'], winner['exposure_penalty'])]
        if index % 2:
            arms.reverse()
        for arm, route, penalty in arms:
            started = time.monotonic()
            evidence = select(case, route, penalty, manifest)
            context = '\n\n'.join(c['path'] + '\n' + c['source'] for c in evidence)
            body = dict(model='codex', stream=False, max_tokens=256,
                        messages=[dict(role='system', content='Answer this code-navigation question using only the supplied excerpts. Return one JSON object with symbol and path. If the answer is absent, use null for both. Do not call tools.'),
                                  dict(role='user', content=case['question'] + '\n\n' + context)],
                        tools=[dict(type='function', function=dict(name='unavailable_source', description='No additional source is available in this bounded evaluation.', parameters=dict(type='object', properties={}, additionalProperties=False)))])
            status, response = adapter.request('/v1/chat/completions', body)
            elapsed = time.monotonic() - started
            message = response.get('choices', [{}])[0].get('message', {}) if isinstance(response, dict) else {}
            usage = response.get('usage', {}) if isinstance(response, dict) else {}
            valid = status == 200 and all(isinstance(usage.get(k), int) and usage[k] >= 0 for k in ['prompt_tokens', 'completion_tokens']) and not message.get('tool_calls')
            text = message.get('content') or ''
            try:
                answer = json.loads(text)
            except (ValueError, TypeError):
                answer = None
            correct = valid and isinstance(answer, dict) and answer.get('symbol') == case['gold_symbol'] and answer.get('path') == case['gold_path']
            model = manifest['estimated_cost_model']
            estimate = (usage.get('prompt_tokens', 0) * model['input_per_million_tokens'] + usage.get('completion_tokens', 0) * model['output_per_million_tokens']) / 1_000_000 if valid else None
            cell = dict(case=case['id'], arm=arm, valid=valid, task_success=correct, latency_seconds=elapsed, estimated_cost_usd=estimate,
                        usage=usage, model=response.get('model') if isinstance(response, dict) else None, request_sha256=hashlib.sha256(json.dumps(body, sort_keys=True).encode()).hexdigest(),
                        context_sha256=hashlib.sha256(context.encode()).hexdigest(), retained=[c['id'] for c in evidence], answer=answer)
            cells.append(cell)
            print(json.dumps(dict(case=case['id'], arm=arm, valid=valid, task_success=correct)), flush=True)
    return grid, winner, cells


def report(manifest, grid, winner, cells):
    metrics = {}
    for arm in ['baseline', 'fitted']:
        rows = [c for c in cells if c['arm'] == arm]
        latency = sorted(c['latency_seconds'] for c in rows)
        metrics[arm] = dict(pairs=len(rows), successes=sum(c['task_success'] for c in rows), valid=all(c['valid'] for c in rows),
                            p95_latency_seconds=latency[math.ceil(.95 * len(latency)) - 1] if latency else None,
                            estimated_cost_usd=sum(c['estimated_cost_usd'] or 0 for c in rows))
    base, fitted = metrics['baseline'], metrics['fitted']
    limits = manifest['gates']
    baseline_fit = next(r for r in grid if r['route'] == 'all' and r['exposure_penalty'] == 0)
    gates = dict(native_representative_workload=manifest['representative_native_workload'],
                 sufficient_heldout_pairs=base['pairs'] >= limits['minimum_heldout_pairs'] and base['pairs'] == fitted['pairs'],
                 all_cells_valid=base['valid'] and fitted['valid'] and bool(cells),
                 strict_fit_improvement=winner['gold_retained'] > baseline_fit['gold_retained'],
                 observed_success_noninferior=fitted['successes'] >= base['successes'],
                 p95_latency=bool(base['p95_latency_seconds']) and fitted['p95_latency_seconds'] <= limits['maximum_p95_latency_ratio'] * base['p95_latency_seconds'],
                 estimated_cost=fitted['estimated_cost_usd'] <= limits['maximum_estimated_cost_ratio'] * base['estimated_cost_usd'])
    # Point success counts are not a confidence interval or production quality
    # certificate. A controlled pilot is unconditionally ineligible for promotion.
    return dict(schema_version=1, kind=manifest['kind'], decision='not_eligible_for_promotion',
                reason='controlled feature pilot; no production exposure or routing claim', fitted_policy=winner, fit_grid=grid,
                gates=gates, metrics=metrics, estimated_cost_model=manifest['estimated_cost_model'], cells=cells)


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--manifest', required=True)
    p.add_argument('--adapter', required=True)
    p.add_argument('--output', required=True)
    args = p.parse_args()
    raw = Path(args.manifest).read_bytes()
    manifest = json.loads(raw)
    spec = importlib.util.spec_from_file_location('operator_adapter', args.adapter)
    adapter = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(adapter)
    grid, winner, cells = run(manifest, adapter)
    result = report(manifest, grid, winner, cells)
    result['manifest_sha256'] = hashlib.sha256(raw).hexdigest()
    Path(args.output).write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(dict(decision=result['decision'], measured_cells=len(cells), valid_cells=sum(c['valid'] for c in cells))), flush=True)
    return 0 if result['gates']['all_cells_valid'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
