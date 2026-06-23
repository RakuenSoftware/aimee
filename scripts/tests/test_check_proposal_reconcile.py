#!/usr/bin/env python3
"""Unit tests for scripts/check-proposal-reconcile.py.

Self-contained: synthetic proposal trees in a tmpdir, no network, no live tree.
Run: python3 -m unittest discover -s scripts/tests -p test_check_proposal_reconcile.py
"""
import importlib.util
import json
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

SCRIPT = Path(__file__).resolve().parent.parent / "check-proposal-reconcile.py"

spec = importlib.util.spec_from_file_location("reconcile", SCRIPT)
rec = importlib.util.module_from_spec(spec)
spec.loader.exec_module(rec)


def _block(*entries):
    body = "\n".join(entries)
    return f"```yaml acceptance\n{body}\n```\n"


class ClassifyState(unittest.TestCase):
    def test_done_wins_over_in_flight(self):
        self.assertEqual(rec.classify_state("merged and deployed; partial rollout"),
                         "terminal_done")

    def test_in_flight(self):
        self.assertEqual(rec.classify_state("proposed; awaiting roundtable"), "in_flight")

    def test_closed(self):
        self.assertEqual(rec.classify_state("rejected in favour of X"), "terminal_closed")

    def test_unknown(self):
        self.assertEqual(rec.classify_state("some prose with no keyword"), "unknown")
        self.assertEqual(rec.classify_state(None), "unknown")

    def test_mixed_case(self):
        self.assertEqual(rec.classify_state("DONE and SHIPPED"), "terminal_done")
        self.assertEqual(rec.classify_state("Proposed"), "in_flight")

    def test_word_boundary_no_false_positive(self):
        # "abandoned" contains the substring "done" but is not the word "done".
        self.assertEqual(rec.classify_state("abandoned this approach"), "unknown")
        # "already" contains "ready" as a substring only.
        self.assertEqual(rec.classify_state("already underway"), "unknown")


class ExtractState(unittest.TestCase):
    def test_state_bullet(self):
        self.assertEqual(rec.extract_state("# T\n- **State:** done\n"), "done")

    def test_status_bold(self):
        self.assertEqual(rec.extract_state("# T\n**Status:** APPROVED\n"), "APPROVED")

    def test_status_bare(self):
        self.assertEqual(rec.extract_state("# T\nStatus: done — shipped\n"),
                         "done — shipped")

    def test_missing(self):
        self.assertIsNone(rec.extract_state("# T\n\n## Problem\nbody\n"))

    def test_multiline_bullet_first_line_only(self):
        # A bullet that wraps: only the first physical line is the state prose.
        txt = "# T\n- **State:** done — shipped\n  across many PRs\n"
        self.assertEqual(rec.extract_state(txt), "done — shipped")

    def test_space_before_colon(self):
        self.assertEqual(rec.extract_state("# T\n- **State** : done\n"), "done")


class CheckStateFolder(unittest.TestCase):
    def test_fail_pending_claims_done(self):
        b, w = rec.check_state_folder("pending", "x.md", "- **State:** done, shipped\n")
        self.assertTrue(b)
        self.assertFalse(w)

    def test_warn_done_has_in_flight(self):
        b, w = rec.check_state_folder("done", "x.md", "- **State:** proposed\n")
        self.assertFalse(b)
        self.assertTrue(w)

    def test_clean(self):
        b, w = rec.check_state_folder("done", "x.md", "- **State:** done\n")
        self.assertFalse(b)
        self.assertFalse(w)

    def test_no_bullet_warns_not_crashes(self):
        b, w = rec.check_state_folder("pending", "x.md", "# T\nbody only\n")
        self.assertFalse(b)
        self.assertTrue(w)

    def test_rejected_folder_warns_on_live_state(self):
        b, w = rec.check_state_folder("rejected", "x.md", "- **State:** approved\n")
        self.assertFalse(b)
        self.assertTrue(w)

    def test_deferred_in_flight_is_clean(self):
        b, w = rec.check_state_folder("deferred", "x.md", "- **State:** proposed\n")
        self.assertFalse(b)
        self.assertFalse(w)


class ValidateAcceptance(unittest.TestCase):
    def test_valid(self):
        self.assertEqual(
            rec.validate_acceptance("x.md",
                _block('- {id: 1, tier: mechanical, check: "make x"}')), [])

    def test_empty_list_valid(self):
        self.assertEqual(rec.validate_acceptance("x.md", "```yaml acceptance\n```\n"), [])

    def test_missing_key(self):
        errs = rec.validate_acceptance("x.md", _block('- {id: 1, tier: mechanical}'))
        self.assertTrue(any("missing 'check'" in e for e in errs))

    def test_unknown_tier(self):
        errs = rec.validate_acceptance("x.md",
            _block('- {id: 1, tier: bogus, check: "x"}'))
        self.assertTrue(any("tier" in e for e in errs))

    def test_dup_id(self):
        errs = rec.validate_acceptance("x.md", _block(
            '- {id: 1, tier: mechanical, check: "a"}',
            '- {id: 1, tier: mechanical, check: "b"}'))
        self.assertTrue(any("duplicate" in e for e in errs))

    def test_non_positive_id(self):
        errs = rec.validate_acceptance("x.md",
            _block('- {id: 0, tier: mechanical, check: "x"}'))
        self.assertTrue(any("positive int" in e for e in errs))

    def test_bool_id_rejected(self):
        # YAML `true` is a bool, not a positive int.
        errs = rec.validate_acceptance("x.md",
            _block('- {id: true, tier: mechanical, check: "x"}'))
        self.assertTrue(any("positive int" in e for e in errs))

    def test_non_list(self):
        errs = rec.validate_acceptance("x.md",
            "```yaml acceptance\nid: 1\ntier: mechanical\ncheck: x\n```\n")
        self.assertTrue(any("must be a list" in e for e in errs))

    def test_null_entry(self):
        errs = rec.validate_acceptance("x.md", "```yaml acceptance\n- null\n```\n")
        self.assertTrue(any("must be a mapping" in e for e in errs))

    def test_empty_check_string(self):
        errs = rec.validate_acceptance("x.md",
            _block('- {id: 1, tier: mechanical, check: ""}'))
        self.assertTrue(any("non-empty" in e for e in errs))

    def test_yaml_parse_error(self):
        errs = rec.validate_acceptance("x.md",
            "```yaml acceptance\n- {id: 1, tier: : : bad\n```\n")
        self.assertTrue(any("not valid YAML" in e for e in errs))

    def test_unknown_key_tolerated(self):
        self.assertEqual(
            rec.validate_acceptance("x.md",
                _block('- {id: 1, tier: mechanical, check: "x", owner: jb, '
                       'schema_version: 1}')), [])

    def test_two_blocks_ids_unique_across_file(self):
        text = (_block('- {id: 1, tier: mechanical, check: "a"}')
                + "\nprose\n"
                + _block('- {id: 1, tier: integration, check: "b"}'))
        errs = rec.validate_acceptance("x.md", text)
        self.assertTrue(any("duplicate" in e for e in errs))


class CheckDrift(unittest.TestCase):
    def setUp(self):
        self.root = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.root, ignore_errors=True)
        (self.root / "scripts").mkdir()
        (self.root / "src" / "tests").mkdir(parents=True)

    def test_missing_script_target(self):
        d = rec.check_drift("x.md",
            _block('- {id: 1, tier: mechanical, check: "python3 scripts/gone.py"}'),
            self.root, self.root / "src" / "tests")
        self.assertTrue(any("scripts/gone.py" in x for x in d))

    def test_present_script_target_clean(self):
        (self.root / "scripts" / "here.py").write_text("x")
        d = rec.check_drift("x.md",
            _block('- {id: 1, tier: mechanical, check: "python3 scripts/here.py"}'),
            self.root, self.root / "src" / "tests")
        self.assertFalse(d)

    def test_missing_test_id(self):
        d = rec.check_drift("x.md",
            _block('- {id: 1, tier: mechanical, check: "make unit-tests TEST=test_nope"}'),
            self.root, self.root / "src" / "tests")
        self.assertTrue(any("test_nope" in x for x in d))

    def test_present_test_id_clean(self):
        (self.root / "src" / "tests" / "test_real.c").write_text("void test_real(){}")
        d = rec.check_drift("x.md",
            _block('- {id: 1, tier: mechanical, check: "make unit-tests TEST=test_real"}'),
            self.root, self.root / "src" / "tests")
        self.assertFalse(d)

    def test_prose_path_drift(self):
        d = rec.check_drift("x.md", "It lives in `src/server/gone.c` today.\n",
                            self.root, self.root / "src" / "tests")
        self.assertTrue(any("src/server/gone.c" in x for x in d))

    def test_proposed_new_path_skipped(self):
        d = rec.check_drift("x.md", "- **new** `src/server/future.c` — the gate\n",
                            self.root, self.root / "src" / "tests")
        self.assertFalse(d)

    def test_fenced_code_example_skipped(self):
        d = rec.check_drift("x.md",
            "Example:\n```\nedit `src/server/gone.c` here\n```\n",
            self.root, self.root / "src" / "tests")
        self.assertFalse(d)

    def test_dotdot_escape_ignored(self):
        # A `..`-escaping path must never be flagged (or filesystem-walked).
        d = rec.check_drift("x.md", "see `src/../../etc/passwd.c`\n",
                            self.root, self.root / "src" / "tests")
        self.assertFalse(d)


class EndToEnd(unittest.TestCase):
    """Drive main() over a synthetic --proposals-dir; assert exit codes + JSON."""

    def _tree(self):
        base = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, base, ignore_errors=True)
        d = base / "proposals"
        for sub in rec.FOLDER_EXPECTED:
            (d / sub).mkdir(parents=True)
        return d

    def _run(self, proposals_dir, *extra):
        p = subprocess.run([sys.executable, str(SCRIPT),
                            "--proposals-dir", str(proposals_dir), *extra],
                           capture_output=True, text=True)
        return p

    def test_clean_tree_exit0(self):
        d = self._tree()
        (d / "done" / "a.md").write_text("# A\n- **State:** done\n")
        (d / "pending" / "b.md").write_text("# B\n- **State:** proposed\n")
        p = self._run(d)
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)

    def test_blocking_exit1(self):
        d = self._tree()
        (d / "pending" / "c.md").write_text("# C\n- **State:** done and shipped\n")
        p = self._run(d)
        self.assertEqual(p.returncode, 1)

    def test_json_has_all_three_classes(self):
        d = self._tree()
        # blocking (pending claims done), warning (done in_flight),
        # drift (bad prose src path) all at once.
        (d / "pending" / "c.md").write_text(
            "# C\n- **State:** shipped\n\nuses `src/__no__.c` now\n")
        (d / "done" / "w.md").write_text("# W\n- **State:** proposed\n")
        p = self._run(d, "--json")
        obj = json.loads(p.stdout)
        self.assertEqual(set(obj), {"blocking", "warnings", "drift"})
        self.assertTrue(obj["blocking"])
        self.assertTrue(obj["warnings"])
        self.assertTrue(obj["drift"])

    def test_strict_promotes_drift(self):
        d = self._tree()
        (d / "pending" / "c.md").write_text(
            "# C\n- **State:** proposed\n\nuses `src/__no__.c` now\n")
        self.assertEqual(self._run(d).returncode, 0)         # report-only
        self.assertEqual(self._run(d, "--strict").returncode, 1)  # promoted

    def test_plant_test(self):
        p = self._run(self._tree(), "--plant-test")
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)

    def test_missing_proposals_dir_exit2(self):
        d = self._tree().parent / "does-not-exist"
        self.assertEqual(self._run(d).returncode, 2)

    def test_non_utf8_file_does_not_crash(self):
        d = self._tree()
        (d / "done" / "bad.md").write_bytes(
            b"# T\n- **State:** done \xff\xfe garbage\n")
        p = self._run(d)
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)


if __name__ == "__main__":
    unittest.main()
