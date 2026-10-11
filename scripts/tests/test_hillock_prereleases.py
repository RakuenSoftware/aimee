"""Immutable upstream discovery and production-pin isolation for Hillock CI."""
import importlib.util
import json
import re
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]


def load(name, file):
    spec = importlib.util.spec_from_file_location(name, ROOT / 'scripts/validation/memory' / file)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


discovery = load('hillock_discovery', 'discover-hillock-prereleases.py')
runner = load('hillock_runner', 'run-hillock-contract.py')


def tip(branch, sha='a' * 40):
    return f'{sha}\trefs/heads/{branch}\n'


class DiscoveryTests(unittest.TestCase):
    def test_all_candidates_numeric_order_and_exact_tips(self):
        listing = tip('rc/v0.10.0') + tip('rc/v0.9.0') + tip('rc/v2.0.1')
        listing += tip('main') + tip('rc/v0.01.0') + tip('rc/v0.10.0-extra')
        rows = discovery.candidates(listing)
        self.assertEqual([r['branch'] for r in rows], ['rc/v0.9.0', 'rc/v0.10.0', 'rc/v2.0.1'])
        self.assertEqual(len({r['branch_key'] for r in rows}), 3)
        changed = discovery.candidates(listing.replace('a' * 40, 'b' * 40))
        self.assertEqual(rows[0]['branch_key'], changed[0]['branch_key'])
        self.assertNotEqual(rows[0]['revision'], changed[0]['revision'])
        self.assertEqual(discovery.candidates(''), [])

    def test_explicit_branch_and_fail_closed(self):
        listing = tip('rc/v0.10.0') + tip('rc/v0.11.0')
        self.assertEqual(len(discovery.candidates(listing, 'rc/v0.11.0')), 1)
        for listing, selected in [(listing, 'main'), (listing, 'rc/v9.0.0'),
                                  (tip('rc/v0.10.0', '0' * 40), ''),
                                  (tip('rc/v0.10.0', 'bad'), ''),
                                  (tip('rc/v0.10.0') * 2, ''), ('malformed', ''),
                                  (listing + tip('rc/v0.10.0'), 'rc/v0.11.0')]:
            with self.subTest(listing=listing, selected=selected), self.assertRaises(ValueError):
                discovery.candidates(listing, selected)
        many = ''.join(tip(f'rc/v1.0.{i}') for i in range(257))
        with self.assertRaises(ValueError):
            discovery.candidates(many)
        self.assertEqual(len(discovery.candidates(many, 'rc/v1.0.0')), 1)

    def test_network_failure_is_not_an_empty_success(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / 'discovery.json'
            with patch.object(sys, 'argv', ['discover', '--output', str(output)]), \
                 patch.object(discovery.subprocess, 'check_output', side_effect=subprocess.CalledProcessError(128, 'git')):
                with self.assertRaises(subprocess.CalledProcessError):
                    discovery.main()
            self.assertFalse(output.exists())


class WorkflowTriggerTests(unittest.TestCase):
    def test_scanner_is_not_triggered_by_aimee_branches_or_releases(self):
        workflow = (ROOT / '.github/workflows/hillock-prereleases.yml').read_text()
        events = workflow.split('\non:\n', 1)[1].split('\npermissions:', 1)[0]
        self.assertEqual(set(re.findall(r'^  ([a-z_]+):', events, re.MULTILINE)),
                         {'schedule', 'workflow_dispatch'})
        self.assertIn("cron: '17 * * * *'", events)

    def test_upstream_receipt_identity_ignores_aimee_commit(self):
        workflow = (ROOT / '.github/workflows/hillock-prereleases.yml').read_text()
        keys = re.findall(r'^          key: (.+)$', workflow, re.MULTILINE)
        self.assertEqual(len(keys), 2)  # Restore and save must address the same receipt.
        self.assertEqual(keys[0], keys[1])
        for key in keys:
            self.assertTrue(key.startswith('hillock-qualified-v3-'))
            self.assertIn('${{ matrix.branch_key }}', key)
            self.assertIn('${{ matrix.revision }}', key)
            self.assertNotIn('github.sha', key)
        save = workflow.split('      - uses: actions/cache/save@', 1)[1].split('      - name:', 1)[0]
        self.assertIn("if: success() && steps.passed.outputs.cache-hit != 'true'", save)
        self.assertNotIn('restore-keys:', workflow)


class QualificationTests(unittest.TestCase):
    def test_required_live_scenarios_cannot_be_skipped_or_absent(self):
        names = ('TestHillockLiveOwnerScenarios', 'TestHillockLiveRecallScenarios')
        passing = '\n'.join('--- PASS: ' + name + ' (0.10s)' for name in names)
        runner.require_live_tests(subprocess.CompletedProcess([], 0, passing), names)
        for status, output in [(1, passing), (0, ''), (0, passing.replace('PASS', 'SKIP')),
                               (0, passing.splitlines()[0]), (0, passing.replace('Scenarios ', 'ScenariosExtra ')),
                               (0, passing + '\n--- FAIL: nested (0.1s)')]:
            with self.subTest(status=status, output=output), self.assertRaises(RuntimeError):
                runner.require_live_tests(subprocess.CompletedProcess([], status, output), names)

    def test_candidate_overlay_changes_only_revision_expectation(self):
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            original_service = (ROOT / 'integrations/hillock/service.py').read_text()
            adapter = ROOT / 'server-go/modules/memory/hillock/hillock.go'
            original_adapter = adapter.read_text()
            service, flags = runner.qualification_profile(ROOT, work, 'b' * 40)
            self.assertEqual(service.read_text(), original_service.replace(runner.REVISION, 'b' * 40))
            overlay = json.loads(Path(flags[0].removeprefix('-overlay=')).read_text())
            self.assertEqual(set(overlay['Replace']), {str(adapter)})
            self.assertEqual(Path(overlay['Replace'][str(adapter)]).read_text(), original_adapter.replace(runner.REVISION, 'b' * 40))
            self.assertEqual(adapter.read_text(), original_adapter)
            self.assertEqual((ROOT / 'integrations/hillock/service.py').read_text(), original_service)
            pinned_service, flags = runner.qualification_profile(ROOT, work, runner.REVISION)
            self.assertEqual(pinned_service, ROOT / 'integrations/hillock/service.py')
            self.assertEqual(flags, [])

    def test_source_mismatch_and_modification_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory)
            with patch.object(runner.subprocess, 'check_output', return_value='b' * 40 + '\n'):
                with self.assertRaisesRegex(RuntimeError, 'revision mismatch'):
                    runner.prepare_source(source, runner.REVISION, source)
            with patch.object(runner.subprocess, 'check_output', side_effect=[runner.REVISION + '\n', 'reservoir.py\n']):
                with self.assertRaisesRegex(RuntimeError, 'modified upstream'):
                    runner.prepare_source(source, runner.REVISION, source)

    def test_failed_qualification_preserves_revision_and_error(self):
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory) / 'evidence'
            with patch.object(sys, 'argv', ['runner', '--artifacts', str(work), '--revision', 'c' * 40,
                                          '--branch', 'rc/v1.2.3', '--scale']), \
                 patch.object(runner.subprocess, 'check_output', return_value='d' * 40), \
                 patch.object(runner, 'prepare_source', return_value=Path(directory)), \
                 patch.object(runner, 'validate', side_effect=RuntimeError('contract failure')) as validate:
                with self.assertRaisesRegex(RuntimeError, 'contract failure'):
                    runner.main()
                self.assertTrue(validate.call_args.args[-1])
            summary = json.loads((work / 'summary.json').read_text())
            self.assertEqual(summary['status'], 'failed')
            self.assertEqual(summary['upstream_revision'], 'c' * 40)
            self.assertEqual(summary['upstream_branch'], 'rc/v1.2.3')
            self.assertEqual(summary['error'], 'contract failure')

    def test_mutable_or_invalid_revision_is_rejected_before_checkout(self):
        for revision in ['rc/v0.10.0', 'main', '0' * 40, 'A' * 40]:
            with patch.object(sys, 'argv', ['runner', '--artifacts', '/unused', '--revision', revision]), \
                 patch.object(runner, 'prepare_source') as checkout:
                with self.assertRaises(SystemExit):
                    runner.main()
                checkout.assert_not_called()


if __name__ == '__main__':
    unittest.main()
