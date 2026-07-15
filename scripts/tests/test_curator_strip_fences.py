#!/usr/bin/env python3
"""Unit tests for strip_fences() in scripts/curator-extract.py.

CONTRACT: strip_fences does NOT know about reasoning. llm-chat.py splits it off
at the wire boundary (split_reasoning), so anything reaching here is the answer
— including a "</think>" that is genuinely part of it. strip_fences only removes
code fences and extracts the JSON object.

Regression guard for a production failure on .254: curator-extract.py used to
split on the LAST "</think>" anywhere in the response, so summarising the very
functions whose job is stripping think blocks (strip_think in
curator-synthesize.py, strip_thinking_blocks in agent_bridge.c) cut the JSON
mid-string. A valid answer became "LLM returned non-JSON: Expecting value:
line 1 column 1 (char 0)" and the job died after three attempts.

The reasoning split itself is covered by scripts/check-sidecar-clients.py
(check_llm_chat_reasoning_split), against stub servers of both shapes.

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

    def test_close_tag_in_payload_survives(self):
        """THE REGRESSION: </think> inside the answer is CONTENT, not a delimiter.

        Reasoning was already removed upstream, so a tag here belongs to the
        summarised source (e.g. a docstring about stripping think blocks) and
        must reach the artifact intact.
        """
        resp = (
            '{"status":"ok","artifacts":[{"kind":"code_unit","payload":'
            '{"summary":"Drops the <think></think> preamble; if the tag exists, '
            'the desired content follows it.","side_effects":[]}}]}'
        )
        parsed = json.loads(self.mod.strip_fences(resp))   # would raise before the fix
        self.assertEqual(parsed["status"], "ok")
        self.assertIn("</think>", parsed["artifacts"][0]["payload"]["summary"])

    def test_tag_bearing_answer_is_not_truncated(self):
        """A tag late in the payload must not cut everything before it."""
        resp = '{"status":"ok","artifacts":[{"payload":{"summary":"handles </think> tags"}}]}'
        parsed = json.loads(self.mod.strip_fences(resp))
        self.assertEqual(parsed["artifacts"][0]["payload"]["summary"], "handles </think> tags")

    def test_code_fences_still_handled(self):
        resp = '```json\n{"status":"ok","artifacts":[]}\n```'
        self.assertEqual(json.loads(self.mod.strip_fences(resp))["status"], "ok")

    def test_prose_around_the_object_is_dropped(self):
        resp = 'Sure! Here you go:\n{"status":"ok","artifacts":[]}\nHope that helps.'
        self.assertEqual(json.loads(self.mod.strip_fences(resp))["status"], "ok")

    def test_nested_braces_and_strings_survive(self):
        """_first_json_object is string-aware: braces inside strings don't end it."""
        resp = '{"status":"ok","artifacts":[{"payload":{"body_excerpt":"if (x) { y(\\"}\\"); }"}}]}'
        parsed = json.loads(self.mod.strip_fences(resp))
        self.assertIn("{", parsed["artifacts"][0]["payload"]["body_excerpt"])


if __name__ == "__main__":
    unittest.main()
