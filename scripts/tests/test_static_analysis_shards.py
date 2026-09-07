"""Ensure parallel analysis covers every source and keeps the combined debt gate."""
import collections
import importlib.util
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch
import xml.etree.ElementTree as ET

import yaml

ROOT = Path(__file__).resolve().parents[2]


def load_runner(name):
    spec = importlib.util.spec_from_file_location(name, ROOT / 'scripts' / name)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


tidy = load_runner('run-clang-tidy.py')
cppcheck = load_runner('run-cppcheck-ratchet.py')
WORKFLOW = yaml.safe_load((ROOT / '.github/workflows/ci.yml').read_text())['jobs']


def write_report(path, identifier='known', count=1):
    root = ET.Element('results')
    errors = ET.SubElement(root, 'errors')
    for line in range(count):
        error = ET.SubElement(errors, 'error', id=identifier, msg='fixture diagnostic')
        ET.SubElement(error, 'location', file='fixture.c', line=str(line + 1))
    ET.ElementTree(root).write(path)


class AnalysisShardsTests(unittest.TestCase):
    def test_ci_batches_cover_each_source_once(self):
        sources = [f'source-{i:04}.c' for i in range(1081)]
        for runner, job in ((tidy, 'clang-tidy-shards'), (cppcheck, 'cppcheck-shards')):
            indices = WORKFLOW[job]['strategy']['matrix']['shard']
            self.assertEqual(indices, list(range(len(indices))))
            batches = [runner.shard_sources(list(reversed(sources)) + sources[:5], i, len(indices))
                       for i in indices]
            self.assertEqual(collections.Counter(s for batch in batches for s in batch),
                             collections.Counter(sources))
            self.assertLessEqual(max(map(len, batches)) - min(map(len, batches)), 1)
            self.assertEqual(runner.shard_sources(sources, 0, 1), sources)
            command = next(s['run'] for s in WORKFLOW[job]['steps'] if s.get('name') == 'Analyze source batch')
            self.assertIn(f'--shard-count {len(indices)}', command)

    def test_invalid_or_empty_batches_fail(self):
        for runner in (tidy, cppcheck):
            for index, count in ((0, 0), (-1, 8), (8, 8), (1, 2)):
                with self.subTest(runner=runner.__name__, index=index, count=count):
                    with self.assertRaises(ValueError):
                        runner.shard_sources(['one.c'], index, count)

    def test_clang_failure_survives_partitioning_and_full_database_is_retained(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            src = root / 'src'
            src.mkdir()
            files = [src / f'sample-{i}.c' for i in range(16)]
            for file in files:
                file.touch()
            plan = '\n'.join(f'gcc -c {file.name} -o {file.stem}.o' for file in files)
            seen = []
            def analyze(executable, database, source):
                import json
                entries = json.loads((database / 'compile_commands.json').read_text())
                self.assertEqual(len(entries), len(files))
                seen.append(source)
                return (17, 'error: planted failure\n') if source == str(files[3]) else (0, '')
            outcomes = []
            for index in range(8):
                argv = ['tidy', '--shard-index', str(index), '--shard-count', '8', '--log', str(root / 'tidy.log')]
                with patch.object(tidy, 'ROOT', root), patch.object(sys, 'argv', argv), \
                     patch.object(tidy.subprocess, 'run', return_value=subprocess.CompletedProcess([], 0, plan, '')), \
                     patch.object(tidy, 'analyze', side_effect=analyze):
                    outcomes.append(tidy.main())
            self.assertEqual(collections.Counter(seen), collections.Counter(map(str, files)))
            self.assertEqual(collections.Counter(outcomes), {0: 7, 1: 1})

    def test_cppcheck_combines_counts_before_applying_ceiling(self):
        with tempfile.TemporaryDirectory() as temp:
            directory = Path(temp)
            for index in range(2):
                write_report(directory / f'cppcheck-{index}.xml', count=2)
            actual, details = cppcheck.merge_reports(directory, 2)
            self.assertEqual(actual[('known', 'fixture.c')], 4)
            baseline = collections.Counter({('known', 'fixture.c'): 3})
            self.assertEqual(cppcheck.check_diagnostics(actual, details, baseline), 1)
            baseline[('known', 'fixture.c')] = 4
            self.assertEqual(cppcheck.check_diagnostics(actual, details, baseline), 0)
            write_report(directory / 'cppcheck-1.xml', identifier='newFinding')
            self.assertEqual(cppcheck.check_diagnostics(*cppcheck.merge_reports(directory, 2), baseline), 1)

    def test_missing_extra_and_malformed_cppcheck_reports_fail(self):
        with tempfile.TemporaryDirectory() as temp:
            directory = Path(temp)
            write_report(directory / 'cppcheck-0.xml')
            with self.assertRaises(ValueError):
                cppcheck.merge_reports(directory, 2)
            write_report(directory / 'cppcheck-1.xml')
            write_report(directory / 'cppcheck-2.xml')
            with self.assertRaises(ValueError):
                cppcheck.merge_reports(directory, 2)
            (directory / 'cppcheck-2.xml').unlink()
            (directory / 'cppcheck-1.xml').write_text('<results/>')
            with self.assertRaises(ValueError):
                cppcheck.merge_reports(directory, 2)

    def test_required_gate_rejects_non_successful_dependencies(self):
        gate = WORKFLOW['secure-development-gates']
        self.assertEqual(set(gate['needs']), {'cppcheck-shards', 'clang-tidy-shards', 'secret-scanning'})
        self.assertEqual(gate['if'], '${{ always() }}')
        step = gate['steps'][0]
        for result in ('success', 'failure', 'cancelled', 'skipped', ''):
            for key in step['env']:
                env = {name: 'success' for name in step['env']}
                env[key] = result
                run = subprocess.run(['bash', '-e', '-c', step['run']], env=env, capture_output=True)
                self.assertEqual(run.returncode == 0, result == 'success')
        self.assertNotIn('needs', WORKFLOW['secret-scanning'])
        checkout = WORKFLOW['secret-scanning']['steps'][0]
        self.assertEqual(checkout['with']['fetch-depth'], 0)


if __name__ == '__main__':
    unittest.main()
