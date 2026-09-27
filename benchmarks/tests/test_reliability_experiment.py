import copy
import unittest
from benchmarks.common.reliability_experiment import POLICY, PINNED, evaluate, manifest_digest


class PairedReleaseTests(unittest.TestCase):
    def fixture(self, n=30):
        ids = [str(i) for i in range(n)]
        pins = dict.fromkeys(PINNED, 'declared-fixture')
        pins.update(dataset_sha256='a'*64, input_corpus_sha256='b'*64, answerability=dict.fromkeys(ids, True), retrieval_unit='turn')
        manifest = dict(version=1, track='protocol-compatible', case_ids=ids, seed=18, baseline=pins,
                        candidate=copy.deepcopy(pins), release_policy=POLICY, efficiency_claim=True, exploration_promotion=True)
        runs = []
        for arm in ('baseline', 'candidate'):
            runs.append(dict(arm=arm, manifest_sha256=manifest_digest(manifest), cost_kind='estimated-token-rates', cost_model='frozen-test-rates',
                             results=[dict(id=i, status='ok', success=True, latency_s=1, total_cost=.01, discovery_calls=10 if arm=='baseline' else 7) for i in ids]))
        return manifest, *runs

    def test_fixed_gates_and_missing_denominators(self):
        m, a, b = self.fixture()
        self.assertEqual(evaluate(m,a,b)['status'], 'qualified')
        b['results'][0]['status'] = 'timeout'
        self.assertEqual(evaluate(m,a,b)['status'], 'unqualified')
        b['results'].pop()
        with self.assertRaises(ValueError): evaluate(m,a,b)
        self.assertEqual(evaluate(*self.fixture(2))['status'], 'unqualified')

    def test_regression_and_manifest_drift(self):
        m,a,b=self.fixture()
        for row in b['results']: row['success']=False
        self.assertFalse(evaluate(m,a,b)['gates']['task_success_noninferiority'])
        m['candidate']['reader']='changed'
        with self.assertRaises(ValueError): evaluate(m,a,b)

    def test_unknown_cost_not_zero_and_tracks_separate(self):
        m,a,b=self.fixture()
        b['results'][0]['total_cost']=float('nan')
        with self.assertRaises(ValueError): evaluate(m,a,b)
        m['track']='combined'
        with self.assertRaises(ValueError): evaluate(m,a,b)
