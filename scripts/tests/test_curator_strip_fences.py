#!/usr/bin/env python3
"""Unit tests for strip_fences() in scripts/curator-extract.py.

Regression guard for a production failure on the .254 appliance: the sidecar
stripped a reasoning model's <think> preamble by splitting on the LAST
"</think>" anywhere in the response. Any answer that merely MENTIONED the tag
inside its JSON was cut mid-string, so a perfectly valid response became
"LLM returned non-JSON: Expecting value: line 1 column 1 (char 0)" and the job
died after 3 attempts.

It reproduced exactly when the curator summarised the functions whose own job is
stripping think blocks (strip_think in curator-synthesize.py,
strip_thinking_blocks in agent_bridge.c): the model quotes the tag, and the
stripper ate the payload.

Pure string handling — no model, no network.
"""
import importlib.util
import json
import os
import sys
import unittest

_HERE = os.path.dirname(os.path.abspath(__file__))
_EXTRACT = os.path.join(_HERE, "..", "curator-extract.py")


def _load():
    spec = importlib.util.spec_from_file_location("curator_extract", _EXTRACT)
    mod = importlib.util.module_from_spec(spec)
    argv = sys.argv
    sys.argv = ["curator-extract.py"]          # the module must not parse our argv
    try:
        spec.loader.exec_module(mod)
    except SystemExit:                          # tolerate a __main__ guard exiting
        pass
    finally:
        sys.argv = argv
    return mod


class StripFencesTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.mod = _load()

    def test_mention_of_close_tag_in_payload_survives(self):
        """THE REGRESSION: </think> inside a JSON string must not truncate it."""
        resp = (
            '{"status":"ok","artifacts":[{"kind":"code_unit","payload":'
            '{"summary":"Drops the <think></think> preamble; if the tag exists, '
            'the desired content follows it.","side_effects":[]}}]}'
        )
        out = self.mod.strip_fences(resp)
        parsed = json.loads(out)                # would raise before the fix
        self.assertEqual(parsed["status"], "ok")
        self.assertIn("</think>", parsed["artifacts"][0]["payload"]["summary"])

    def test_real_reasoning_preamble_is_stripped(self):
        """A genuine leading <think> block must still be removed."""
        resp = '<think>I should emit JSON. Maybe {"a":1}?</think>\n{"status":"ok","artifacts":[]}'
        self.assertEqual(json.loads(self.mod.strip_fences(resp))["status"], "ok")

    def test_preamble_and_mention_together(self):
        """Strip the preamble, keep a mention in the payload."""
        resp = (
            '<think>reasoning...</think>\n'
            '{"status":"ok","artifacts":[{"payload":{"summary":"handles </think> tags"}}]}'
        )
        parsed = json.loads(self.mod.strip_fences(resp))
        self.assertEqual(parsed["artifacts"][0]["payload"]["summary"], "handles </think> tags")

    def test_unterminated_preamble_is_left_alone(self):
        """No close tag: don't guess — let the JSON extractor try."""
        resp = '<think>never closed {"status":"ok","artifacts":[]}'
        self.assertIsInstance(self.mod.strip_fences(resp), str)   # must not raise

    def test_code_fences_still_handled(self):
        resp = '```json\n{"status":"ok","artifacts":[]}\n```'
        self.assertEqual(json.loads(self.mod.strip_fences(resp))["status"], "ok")

    def test_prose_around_the_object_is_dropped(self):
        resp = 'Sure! Here you go:\n{"status":"ok","artifacts":[]}\nHope that helps.'
        self.assertEqual(json.loads(self.mod.strip_fences(resp))["status"], "ok")


if __name__ == "__main__":
    unittest.main()
