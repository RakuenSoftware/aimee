"""Mathematical/invalid-run tests; these synthetic cells are NOT quality evidence."""
import copy
import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location('exploration_gate', Path(__file__).parents[1]/'memory/exploration_gate.py')
gate = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gate)


def fixture(n):
    manifest = dict(schema_version=1, policy=gate.POLICY, ordered_case_ids=[f'case-{i}' for i in range(n)])
    manifest.update({key: 'pinned-fixture' for key in gate.PINNED})
    manifest['sampling_unit'] = 'independent_task'
    for arm in ('baseline', 'treatment'):
        manifest[arm] = dict(implementation=arm, policy_sha256=arm, feature_flags={'exploration': arm})
    results = dict(manifest, manifest_sha256='manifest-fixture')
    cell = dict(eligible=True, completed=True, correct=True, raw_scans=10, redundant_scans=10,
                restrictions=0, false_restrictions=0, expansions=0, latency_seconds=1,
                total_cost=1, evidence_sha256='a'*64)
    results['pairs'] = [dict(case_id=case, baseline=dict(cell), treatment=dict(cell, redundant_scans=8))
                        for case in manifest['ordered_case_ids']]
    return manifest, results


class ExplorationGateTest(unittest.TestCase):
    def test_exact_interval_endpoints_and_cdf(self):
        self.assertAlmostEqual(gate.binomial_cdf(1, 2, .5), .75)
        self.assertAlmostEqual(gate.binomial_cdf(0, 2, .5), .25)
        low, high = gate.exact_binomial_interval(0, 8, .0125)
        self.assertEqual(low, 0)
        self.assertAlmostEqual(high, 1-.0125**(1/8))
        self.assertAlmostEqual(gate.exact_binomial_interval(1, 1, .0125)[0], .0125)
        self.assertAlmostEqual(gate.exact_binomial_interval(1, 2, .0125)[0], 1-(1-.0125)**.5)

    def test_small_perfect_sample_does_not_prove_noninferiority(self):
        m, r = fixture(8)
        report = gate.score(m, r, 'manifest-fixture')
        self.assertEqual(report['decision'], 'observe')
        self.assertFalse(report['gates']['task_success_noninferior'])
        self.assertLess(report['metrics']['paired_success_interval'][0], -.01)

    def test_frozen_efficiency_boundary_and_uncertainty(self):
        m, r = fixture(600)
        report = gate.score(m, r, 'manifest-fixture')
        self.assertEqual(report['decision'], 'eligible_for_operator_review')
        self.assertAlmostEqual(report['metrics']['redundant_scan_reduction'], .2)
        self.assertIsNone(report['metrics']['arms']['treatment']['false_restriction_rate'])
        self.assertGreater(report['metrics']['paired_success_interval'][0], -.01)
        self.assertLess(report['metrics']['paired_success_interval'][0], 0)
        r['pairs'][0]['treatment']['redundant_scans'] += 1
        self.assertFalse(gate.score(m, r, 'manifest-fixture')['gates']['redundant_scans'])

    def test_paired_loss_is_not_hidden_by_completion(self):
        m, r = fixture(600)
        for row in r['pairs'][:12]:
            row['treatment']['correct'] = False
        report = gate.score(m, r, 'manifest-fixture')
        self.assertEqual(report['decision'], 'observe')
        self.assertAlmostEqual(report['metrics']['paired_success_difference'], -.02)

    def test_missing_invalid_and_mismatched_runs_cannot_promote(self):
        m, original = fixture(600)
        for change in ('missing', 'duplicate', 'generation', 'manifest', 'nan', 'numerator', 'boolean'):
            with self.subTest(change=change):
                r = copy.deepcopy(original)
                if change == 'missing': r['pairs'].pop()
                elif change == 'duplicate': r['pairs'][1]['case_id'] = r['pairs'][0]['case_id']
                elif change == 'generation': r['index_generation'] = 'different'
                elif change == 'manifest': r['manifest_sha256'] = 'different'
                elif change == 'nan': r['pairs'][0]['treatment']['total_cost'] = float('nan')
                elif change == 'numerator': r['pairs'][0]['treatment']['false_restrictions'] = 1
                else: r['pairs'][0]['treatment']['completed'] = 1
                with self.assertRaises(ValueError): gate.score(m, r, 'manifest-fixture')
        r = copy.deepcopy(original)
        r['pairs'][0]['treatment'] = dict(eligible=False, invalid_reason='provider_timeout')
        report = gate.score(m, r, 'manifest-fixture')
        self.assertEqual(report['decision'], 'observe')
        self.assertIsNone(report['metrics'])
        self.assertEqual(len(report['invalid_cells']), 1)

    def test_no_redundant_baseline_has_no_claimed_improvement(self):
        m, r = fixture(600)
        for row in r['pairs']:
            row['baseline']['redundant_scans'] = row['treatment']['redundant_scans'] = 0
        report = gate.score(m, r, 'manifest-fixture')
        self.assertIsNone(report['metrics']['redundant_scan_reduction'])
        self.assertFalse(report['gates']['redundant_scans'])

    def test_latency_cost_and_label_rates(self):
        m, r = fixture(600)
        for row in r['pairs']:
            row['treatment'].update(latency_seconds=1.11, total_cost=1.001, restrictions=2, false_restrictions=1, expansions=3)
        report = gate.score(m, r, 'manifest-fixture')
        self.assertFalse(report['gates']['p95_latency'])
        self.assertFalse(report['gates']['total_cost'])
        self.assertEqual(report['metrics']['arms']['treatment']['false_restriction_rate'], .5)
        self.assertEqual(report['metrics']['arms']['treatment']['expansions_per_task'], 3)


if __name__ == '__main__':
    unittest.main()
