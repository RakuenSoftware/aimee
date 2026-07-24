"""Regressor for the JSON fragment corpus defined by docs/guardrails/json_grammar.md.

Verifies that the checked-in fixtures under tests/fixtures/ match the byte
oracle promised by their headers, that JSON payloads in the json/ subdir
parse as standard JSON, that no .json file contains an inline // comment
header (an excluded fragment form), and that every fixture can drive a
TP/FP/FN/TN verdict via the declared oracle bytes.

Run:  python3 -m unittest tests/test_collapse_corpus.py  -v
"""
from __future__ import annotations

import json
import re
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).parent.parent
FIXTURE_ROOT = REPO_ROOT / "tests/fixtures"
COLLAPSE = FIXTURE_ROOT / "collapse_collapse"
LEGIT = FIXTURE_ROOT / "collapse_legit"
LEGIT_JSON = LEGIT / "json"
GRAMMAR = REPO_ROOT / "docs" / "guardrails" / "json_grammar.md"


def _read_bytes(p):
    with open(p, "rb") as f:
        return f.read()


def _parse_header_fields(header_text):
    s = header_text.decode("utf-8", errors="replace")
    m_shape = re.search(r"shape:\s*(.+?)(?:\s*;\s*expected:|\s*;?\s*$)",
                        s, re.DOTALL)
    if not m_shape:
        m_shape = re.search(r"shape:\s*([^;]+?)(?:\s*;|\s*$)", s)
    m_exp = re.search(r"expected:\s*(fire|no-fire)\b", s)
    m_off = re.search(r"expected_loop_start_offset:\s*(-?\d+)\b", s)
    m_span = re.search(r"expected_loop_span_bytes:\s*(-?\d+)\b", s)
    assert m_shape and m_exp and m_off and m_span, f"malformed header: {s!r}"
    return {
        "shape": m_shape.group(1).strip().rstrip(";").strip(),
        "expected": m_exp.group(1).strip(),
        "offset": int(m_off.group(1)),
        "span": int(m_span.group(1)),
    }


def _parse_header_for(p):
    """For .json files, look for sibling .meta; otherwise read first LF-delimited
    line of the file."""
    if p.suffix == ".json":
        meta = p.with_suffix(p.suffix + ".meta")
        if meta.exists():
            return _parse_header_fields(_read_bytes(meta))
    data = _read_bytes(p)
    i = data.find(b"\n")
    assert i >= 0, f"{p}: header missing newline"
    return _parse_header_fields(data[:i])


def _iter_fixtures():
    out = []
    for sub in (COLLAPSE, LEGIT):
        if sub.exists():
            for p in sorted(sub.rglob("*")):
                if p.is_file() and not p.name.endswith(".meta"):
                    out.append(p)
    return out


def _run_oracle_detector(data, fields):
    """Stub detector for the corpus. Returns "loop" iff the declared period
    of `fields["span"]` bytes starting at `fields["offset"]` occurs at least
    2 times (contiguously or non-contiguously) in `data`. For no-fire
    fixtures (offset=-1) it returns "no-loop". This is the oracle that
    later detector code must match."""
    if fields["expected"] == "no-fire":
        return "no-loop"
    off = fields["offset"]
    sp = fields["span"]
    if off < 0 or sp < 2 or off + sp > len(data):
        return "no-loop"
    period = data[off:off + sp]
    if len(period) != sp:
        return "no-loop"
    n, pos = 0, off
    while pos + sp <= len(data) and data[pos:pos + sp] == period:
        n += 1
        pos += sp
    if n >= 2:
        return "loop"
    body = data[off:]
    n_alt, search_pos = 0, 0
    while True:
        idx = body.find(period, search_pos)
        if idx < 0:
            break
        n_alt += 1
        search_pos = idx + sp
    if n_alt >= 2:
        return "loop"
    return "no-loop"


class TestGrammarDoc(unittest.TestCase):
    def test_grammar_exists(self):
        self.assertTrue(GRAMMAR.exists(), f"missing {GRAMMAR}")

    def test_grammar_lists_metric_definitions(self):
        text = _read_bytes(GRAMMAR).decode()
        for needle in ["TP", "FP", "FN", "TN",
                       "TP / (TP + FP)",
                       "TP / (TP + FN)",
                       "TN / (TN + FP)"]:
            self.assertIn(needle, text, f"grammar doc must define: {needle!r}")

    def test_grammar_enumerates_exclusions(self):
        text = _read_bytes(GRAMMAR).decode().lower()
        for needle in ["heterogeneous", "comment-bearing", "streaming partial token"]:
            self.assertIn(needle, text, f"grammar must enumerate {needle!r} exclusion")

    def test_grammar_documents_offset_semantics(self):
        text = _read_bytes(GRAMMAR).decode()
        self.assertIn("absolute byte offset", text.lower())
        self.assertIn("loop period", text.lower())

    def test_grammar_documents_envelope_forms(self):
        text = _read_bytes(GRAMMAR).decode()
        self.assertIn(".meta", text)
        self.assertIn("# shape:", text)


class TestCollapseCollapse(unittest.TestCase):
    """Fire fixtures: declared offset+span points to a verbatim repeated region."""

    REQUIRED_FILES = [
        "short_span.txt",
        "long_span.txt",
        "ramp_then_loop.txt",
        "nested_loop.txt",
        "interleaved.txt",
    ]

    def test_required_files_present(self):
        for fn in self.REQUIRED_FILES:
            self.assertTrue((COLLAPSE / fn).exists(), f"missing {fn}")

    def _check_fire(self, p, allow_non_contiguous=False):
        data = _read_bytes(p)
        fields = _parse_header_for(p)
        self.assertEqual(fields["expected"], "fire", f"{p}: must be fire")
        off = fields["offset"]
        sp = fields["span"]
        lf = data.find(b"\n")
        self.assertGreater(off, lf, f"{p}: offset {off} must be past header LF {lf}")
        self.assertGreaterEqual(sp, 2, f"{p}: span {sp} too small")
        self.assertLessEqual(off + sp, len(data),
                             f"{p}: offset+span exceeds file length")
        period = data[off:off + sp]
        self.assertEqual(len(period), sp, f"{p}: period slice wrong length")
        self.assertEqual(data[off:off + sp], period,
                         f"{p}: declared offset does not point at the period")
        if not allow_non_contiguous:
            n, pos = 0, off
            while pos + sp <= len(data) and data[pos:pos + sp] == period:
                n += 1
                pos += sp
            self.assertGreaterEqual(
                n, 2, f"{p}: expected at least 2 contiguous repetitions, got {n}",
            )
        else:
            body = data[off:]
            n, pos = 0, 0
            while True:
                idx = body.find(period, pos)
                if idx < 0:
                    break
                n += 1
                pos = idx + sp
            self.assertGreaterEqual(
                n, 2, f"{p}: expected at least 2 verbatim occurrences, got {n}",
            )

    def test_short_span(self):
        self._check_fire(COLLAPSE / "short_span.txt")

    def test_long_span(self):
        self._check_fire(COLLAPSE / "long_span.txt")

    def test_ramp_then_loop(self):
        """Semantic: declared offset must point at the first byte of the
        first loop-iteration line ('l' of 'loop item'), and the period
        must repeat contiguously without any preceding content overlap."""
        p = COLLAPSE / "ramp_then_loop.txt"
        data = _read_bytes(p)
        fields = _parse_header_for(p)
        off = fields["offset"]
        self.assertEqual(data[off:off + 9], b"loop item",
                         f"offset {off} does not point at 'loop item'")
        self.assertEqual(data[off - 1:off], b"\n",
                         "offset must start on a fresh line, not mid-ramp")
        self._check_fire(p)

    def test_nested_loop(self):
        """The outer block (which itself contains the inner repeat) must
        repeat verbatim, including its line terminators."""
        p = COLLAPSE / "nested_loop.txt"
        data = _read_bytes(p)
        fields = _parse_header_for(p)
        off = fields["offset"]
        sp = fields["span"]
        period = data[off:off + sp]
        self.assertTrue(period.startswith(b"outer-start\n"),
                        f"period must begin with 'outer-start\\n', got {period[:20]!r}")
        self.assertTrue(period.endswith(b"outer-end\n"),
                        f"period must end with 'outer-end\\n', got {period[-20:]!r}")
        inner = b"inner-a\ninner-b\n"
        i, count = 0, 0
        while True:
            j = period.find(inner, i)
            if j < 0:
                break
            count += 1
            i = j + len(inner)
        self.assertGreaterEqual(
            count, 2,
            f"inner sub-block must repeat >=2 times in outer period, got {count}",
        )
        n, pos = 0, off
        while pos + sp <= len(data) and data[pos:pos + sp] == period:
            n += 1
            pos += sp
        self.assertGreaterEqual(
            n, 2,
            f"outer block must repeat >=2 times contiguously, got {n}",
        )
        self._check_fire(p)

    def test_interleaved(self):
        """The connective tissue between loop iterations must be
        non-repeating; the loop period itself repeats at >= 2 positions
        in the body."""
        p = COLLAPSE / "interleaved.txt"
        data = _read_bytes(p)
        fields = _parse_header_for(p)
        off = fields["offset"]
        sp = fields["span"]
        period = data[off:off + sp]
        positions = []
        pos = 0
        while True:
            idx = data.find(period, off + pos)
            if idx < 0:
                break
            positions.append(idx)
            pos = idx - off + sp
        self.assertGreaterEqual(
            len(positions), 2,
            f"expected >= 2 period occurrences, got {len(positions)}",
        )
        connectives = []
        for a, b in zip(positions, positions[1:]):
            connectives.append(data[a + sp:b])
        unique = {c for c in connectives}
        self.assertEqual(
            len(unique), len(connectives),
            f"connective tissue between iterations must all differ; got {connectives!r}",
        )
        self._check_fire(p, allow_non_contiguous=True)


class TestCollapseLegit(unittest.TestCase):
    """No-fire fixtures: offset=-1 and span=-1, plus shape declaration."""

    REQUIRED_NON_JSON = [
        "ascii_box.txt",
        "code_boilerplate.py",
        "fenced_python.md",
        "markdown_table.md",
        "ordered_list.md",
        "short_lines.txt",
    ]

    REQUIRED_JSON = ["primitives.json", "nested.json", "objects.json", "fenced.md"]

    def test_non_json_files_present(self):
        for fn in self.REQUIRED_NON_JSON:
            self.assertTrue((LEGIT / fn).exists(), f"missing legit non-JSON {fn}")

    def test_json_files_present(self):
        for fn in self.REQUIRED_JSON:
            self.assertTrue((LEGIT_JSON / fn).exists(), f"missing legit JSON {fn}")

    def test_non_json_no_fire_headers(self):
        for fn in self.REQUIRED_NON_JSON:
            fields = _parse_header_for(LEGIT / fn)
            self.assertEqual(fields["expected"], "no-fire", f"{fn}: not no-fire")
            self.assertEqual(fields["offset"], -1)
            self.assertEqual(fields["span"], -1)
            self.assertTrue(fields["shape"], f"{fn}: shape empty")

    def test_json_no_fire_headers(self):
        for fn in self.REQUIRED_JSON:
            fields = _parse_header_for(LEGIT_JSON / fn)
            self.assertEqual(fields["expected"], "no-fire")
            self.assertEqual(fields["offset"], -1)
            self.assertEqual(fields["span"], -1)
            self.assertTrue(fields["shape"], f"{fn}: shape empty")

    def test_json_payloads_are_valid_json(self):
        for fn in ["primitives.json", "nested.json", "objects.json"]:
            with open(LEGIT_JSON / fn) as f:
                value = json.load(f)
            self.assertIsInstance(value, list,
                                 f"{fn}: top-level must be array per accepted grammar")

    def test_nested_json_is_genuinely_nested(self):
        with open(LEGIT_JSON / "nested.json") as f:
            value = json.load(f)
        self.assertIsInstance(value, list)
        fields = _parse_header_for(LEGIT_JSON / "nested.json")
        self.assertIn("nested", fields["shape"].lower())

    def test_json_payloads_have_sibling_meta(self):
        for fn in ["primitives.json", "nested.json", "objects.json"]:
            meta = LEGIT_JSON / (fn + ".meta")
            self.assertTrue(meta.exists(), f"missing sibling meta for {fn}")

    def test_json_payloads_contain_no_inline_slashslash(self):
        for fn in ["primitives.json", "nested.json", "objects.json"]:
            data = _read_bytes(LEGIT_JSON / fn)
            self.assertNotIn(b"//", data,
                             f"{fn}: contains // (excluded comment-bearing fragment)")

    def test_fenced_md_uses_documented_envelope(self):
        """Fenced.md must use the documented inline '# shape:' envelope form
        per the grammar doc; the fenced payload must be intact after the header."""
        data = _read_bytes(LEGIT_JSON / "fenced.md")
        lf = data.find(b"\n")
        self.assertGreaterEqual(lf, 0, "fenced.md: missing header newline")
        header_line = data[:lf].decode("utf-8", errors="replace")
        self.assertTrue(
            header_line.startswith("# shape:"),
            f"fenced.md header must start with '# shape:', got: {header_line!r}",
        )
        body = data[lf + 1:].decode("utf-8", errors="replace")
        self.assertIn("```json", body, "fenced.md: missing ```json fence")
        self.assertRegex(body, r"```\s*$", "fenced.md: missing closing ``` fence")


class TestAcceptedGrammarShapes(unittest.TestCase):
    """Confirm the corpus exercises each accepted-grammar shape."""

    def test_primitives_top_level_array(self):
        with open(LEGIT_JSON / "primitives.json") as f:
            value = json.load(f)
        self.assertTrue(all(isinstance(v, (int, float, str, bool, type(None)))
                            for v in value), "primitives.json must hold only primitives")

    def test_objects_uniform_with_discriminator(self):
        with open(LEGIT_JSON / "objects.json") as f:
            value = json.load(f)
        keysets = {tuple(sorted(o.keys())) for o in value}
        self.assertEqual(len(keysets), 1, "objects.json must have uniform key set")
        discriminators = {o.get("kind") for o in value}
        self.assertTrue(all(isinstance(d, str) for d in discriminators),
                        "kind values must be strings (discriminator)")

    def test_fenced_md_marks_json_block(self):
        text = _read_bytes(LEGIT_JSON / "fenced.md").decode()
        self.assertRegex(text, r"```json")


class TestDetectorExercise(unittest.TestCase):
    """Drive every fixture through the oracle detector and verify the
    declared outcome matches the detector's verdict. Accumulate TP/FP/FN/TN
    counts and assert the metric definitions from the grammar doc hold for
    the corpus as a whole.

    The oracle detector is the test-only stub `_run_oracle_detector`. For
    TP/FP/FN/TN mapping the declared `expected` is the ground truth label
    and the oracle detector verdict (driven by the declared offset+span) is
    the prediction.
    """

    EXPECTED_LABELS = {
        "short_span.txt": "fire",
        "long_span.txt": "fire",
        "ramp_then_loop.txt": "fire",
        "nested_loop.txt": "fire",
        "interleaved.txt": "fire",
        "ascii_box.txt": "no-fire",
        "code_boilerplate.py": "no-fire",
        "fenced_python.md": "no-fire",
        "markdown_table.md": "no-fire",
        "ordered_list.md": "no-fire",
        "short_lines.txt": "no-fire",
        "primitives.json": "no-fire",
        "nested.json": "no-fire",
        "objects.json": "no-fire",
        "fenced.md": "no-fire",
    }

    def test_all_fixtures_have_labels(self):
        seen = {p.name for p in _iter_fixtures()}
        missing = set(self.EXPECTED_LABELS) - seen
        unexpected = seen - set(self.EXPECTED_LABELS)
        self.assertFalse(missing,
                         f"fixtures present on disk but missing from label map: {missing}")
        self.assertFalse(unexpected,
                         f"label map references fixtures not on disk: {unexpected}")

    def test_oracle_detector_matches_declared_outcome(self):
        """For every fixture, the oracle detector's verdict must equal the
        declared expected outcome."""
        for p in _iter_fixtures():
            with self.subTest(fixture=p.name):
                data = _read_bytes(p)
                fields = _parse_header_for(p)
                declared = fields["expected"]
                verdict = _run_oracle_detector(data, fields)
                expected_verdict = "loop" if declared == "fire" else "no-loop"
                self.assertEqual(
                    verdict, expected_verdict,
                    f"{p.name}: oracle detector verdict {verdict!r} != declared {declared!r}",
                )

    def test_tp_fp_fn_tn_aggregates(self):
        """Run the oracle detector over the whole corpus and assert the
        accumulated TP/FP/FN/TN counts satisfy the metric definitions from
        the grammar doc:
            precision = TP / (TP + FP)
            recall    = TP / (TP + FN)
            specificity = TN / (TN + FP)
        The fixture corpus is designed so that the oracle detector agrees
        with every declared label: TP = number of fire fixtures,
        TN = number of no-fire fixtures, FP = FN = 0.
        """
        tp = fp = fn = tn = 0
        for p in _iter_fixtures():
            data = _read_bytes(p)
            fields = _parse_header_for(p)
            label = fields["expected"]
            verdict = _run_oracle_detector(data, fields)
            detect = (verdict == "loop")
            truth = (label == "fire")
            if detect and truth:
                tp += 1
            elif detect and not truth:
                fp += 1
            elif not detect and truth:
                fn += 1
            else:
                tn += 1
        self.assertEqual(fp, 0,
                         f"oracle detector should not mis-fire on legit corpus, got FP={fp}")
        self.assertEqual(fn, 0,
                         f"oracle detector should not miss any collapse, got FN={fn}")
        n_fire = sum(1 for v in self.EXPECTED_LABELS.values() if v == "fire")
        n_no_fire = sum(1 for v in self.EXPECTED_LABELS.values() if v == "no-fire")
        self.assertEqual(tp, n_fire,
                         f"TP should equal number of fire fixtures ({n_fire}), got {tp}")
        self.assertEqual(tn, n_no_fire,
                         f"TN should equal number of no-fire fixtures ({n_no_fire}), got {tn}")
        if tp + fp > 0:
            precision = tp / (tp + fp)
            self.assertGreaterEqual(precision, 0.0)
            self.assertLessEqual(precision, 1.0)
        if tp + fn > 0:
            recall = tp / (tp + fn)
            self.assertEqual(recall, 1.0, "recall should be 1.0 on the oracle corpus")
        if tn + fp > 0:
            specificity = tn / (tn + fp)
            self.assertEqual(specificity, 1.0,
                             "specificity should be 1.0 on the oracle corpus")


if __name__ == "__main__":
    unittest.main(verbosity=2)
