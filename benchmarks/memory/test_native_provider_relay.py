import importlib.util
import json
from pathlib import Path
import sys
import tempfile
import unittest

spec = importlib.util.spec_from_file_location('relay', Path(__file__).with_name('native_provider_relay.py'))
relay = importlib.util.module_from_spec(spec)
spec.loader.exec_module(relay)


class RelayTests(unittest.TestCase):
    def test_missing_tools_never_reaches_provider(self):
        with tempfile.TemporaryDirectory() as directory:
            command = [sys.executable, '-c', 'import json;print("EVAL_RELAY "+json.dumps(dict(id="1",body={})))']
            def forbidden(body):
                self.fail('unscoped request reached paired production endpoint')
            with self.assertRaises(ValueError):
                relay.collect(command, Path(directory) / 'out', 'model', 1, forbidden)

    def test_stderr_backpressure_and_usage_evidence(self):
        with tempfile.TemporaryDirectory() as directory:
            packet = dict(id='1', body=dict(model='native', stream=True, max_tokens=8000,
                                           tools=[dict(type='function', function=dict(name='read_file'))]))
            code = ('import json,sys;sys.stderr.write("x"*262144);sys.stderr.flush();'
                    'print("EVAL_RELAY "+' + repr(json.dumps(packet)) + ',flush=True);'
                    'reply=json.loads(input());assert reply["id"]=="1" and len(reply["evidence_sha256"])==64')
            def provider(body):
                self.assertEqual(body['max_tokens'], 4096)
                self.assertEqual(body['model'], 'paired')
                self.assertFalse(body['stream'])
                return 200, dict(usage=dict(prompt_tokens=5, completion_tokens=2, total_tokens=7))
            output = Path(directory) / 'out'
            summary = relay.collect([sys.executable, '-c', code], output, 'paired', 1, provider)
            self.assertEqual(summary['actual_exit'], 0)
            self.assertFalse(summary['billing_cost_available'])
            evidence = output / 'call-00001.json'
            self.assertEqual(evidence.stat().st_mode & 0o777, 0o600)
            self.assertEqual(output.stat().st_mode & 0o777, 0o700)
            saved = json.loads(evidence.read_text())
            self.assertEqual(saved['native_request'], packet['body'])
            self.assertEqual(saved['request']['model'], 'paired')

    def test_missing_usage_retains_failure_without_fabricating_cost(self):
        with tempfile.TemporaryDirectory() as directory:
            packet = dict(id='1', body=dict(tools=[dict(type='function')]))
            code = 'print("EVAL_RELAY "+' + repr(json.dumps(packet)) + ',flush=True);input()'
            output = Path(directory) / 'out'
            with self.assertRaises(ValueError):
                relay.collect([sys.executable, '-c', code], output, 'model', 1, lambda body: (200, {}))
            self.assertTrue((output / 'call-00001.json').exists())
            self.assertFalse((output / 'collector-summary.json').exists())

    def test_usage_requires_complete_consistent_integer_counts(self):
        for usage in ({}, dict(prompt_tokens=1, completion_tokens=1, total_tokens=3),
                      dict(prompt_tokens=True, completion_tokens=1, total_tokens=2),
                      dict(prompt_tokens=-1, completion_tokens=2, total_tokens=1)):
            with self.subTest(usage=usage):
                self.assertFalse(relay.valid_usage(dict(usage=usage)))


    def test_call_ceiling_stops_before_another_provider_charge(self):
        with tempfile.TemporaryDirectory() as directory:
            packet = dict(id='1', body=dict(tools=[dict(type='function')]))
            code = ('import json;packet=' + repr(json.dumps(packet)) + ';'
                    'print("EVAL_RELAY "+packet,flush=True);input();'
                    'print("EVAL_RELAY "+packet,flush=True);input()')
            calls = []
            def provider(body):
                calls.append(body)
                return 200, dict(usage=dict(prompt_tokens=1, completion_tokens=1, total_tokens=2))
            output = Path(directory) / 'out'
            with self.assertRaises(ValueError):
                relay.collect([sys.executable, '-c', code], output, 'model', 1, provider)
            self.assertEqual(len(calls), 1)
            self.assertFalse((output / 'collector-summary.json').exists())


if __name__ == '__main__':
    unittest.main()
