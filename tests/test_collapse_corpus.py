"""Regressor for the JSON fragment corpus defined by docs/guardrails/json_grammar.md.

Verifies that the checked-in fixtures under tests/fixtures/ match the byte
oracle promised by their headers, that JSON payloads in the json/ subdir
parse as standard JSON, that no .json file contains an inline // comment
header (an excluded fragment form), and that fire-fixture offsets, spans,
and declared repetition counts identify repeated byte periods. Detector
integration is intentionally deferred until detector code exists.

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
    m_reps = re.search(r"expected_repetitions:\s*(-?\d+)\b", s)
    assert m_shape and m_exp and m_off and m_span, f"malformed header: {s!r}"
    assert m_reps, (
        f"malformed header: missing expected_repetitions: field in {s!r}"
    )
    return {
        "shape": m_shape.group(1).strip().rstrip(";").strip(),
        "expected": m_exp.group(1).strip(),
        "offset": int(m_off.group(1)),
        "span": int(m_span.group(1)),
        "repetitions": int(m_reps.group(1)),
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
        self.assertIn("<!-- shape:", text,
                      "grammar must enumerate <!-- shape: --> form for .md")

    def test_grammar_documents_repetition_field(self):
        text = _read_bytes(GRAMMAR).decode()
        self.assertIn("expected_repetitions", text,
                      "grammar must document the expected_repetitions field")


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
        reps = fields["repetitions"]
        lf = data.find(b"\n")
        self.assertGreater(off, lf, f"{p}: offset {off} must be past header LF {lf}")
        self.assertGreaterEqual(sp, 2, f"{p}: span {sp} too small")
        self.assertLessEqual(off + sp, len(data),
                             f"{p}: offset+span exceeds file length")
        self.assertGreaterEqual(
            reps, 2,
            f"{p}: expected_repetitions {reps} must be >= 2 for fire fixtures",
        )
        period = data[off:off + sp]
        self.assertEqual(len(period), sp, f"{p}: period slice wrong length")
        self.assertEqual(data[off:off + sp], period,
                         f"{p}: declared offset does not point at the period")
        first_occurrence = data.find(period, lf + 1)
        self.assertEqual(
            first_occurrence, off,
            f"{p}: declared offset must identify the first period occurrence; "
            f"found first occurrence at {first_occurrence}",
        )
        if not allow_non_contiguous:
            n, pos = 0, off
            while pos + sp <= len(data) and data[pos:pos + sp] == period:
                n += 1
                pos += sp
            self.assertGreaterEqual(
                n, reps,
                f"{p}: expected at least {reps} contiguous repetitions, got {n}",
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
                n, reps,
                f"{p}: expected at least {reps} verbatim occurrences, got {n}",
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
            self.assertEqual(fields["repetitions"], 0,
                             f"{fn}: no-fire must declare expected_repetitions: 0")
            self.assertTrue(fields["shape"], f"{fn}: shape empty")

    def test_json_no_fire_headers(self):
        for fn in self.REQUIRED_JSON:
            fields = _parse_header_for(LEGIT_JSON / fn)
            self.assertEqual(fields["expected"], "no-fire")
            self.assertEqual(fields["offset"], -1)
            self.assertEqual(fields["span"], -1)
            self.assertEqual(fields["repetitions"], 0,
                             f"{fn}: no-fire must declare expected_repetitions: 0")
            self.assertTrue(fields["shape"], f"{fn}: shape empty")

    def test_json_payloads_are_valid_json(self):
        for fn in ["primitives.json", "nested.json", "objects.json"]:
            with open(LEGIT_JSON / fn) as f:
                value = json.load(f)
            self.assertIsInstance(value, list,
                                 f"{fn}: top-level must be array per accepted grammar")

    def test_nested_json_is_genuinely_nested(self):
        """Structural: nested.json must contain sub-arrays whose elements
        are themselves arrays (a nested-array shape), not just a flat
        homogeneous array whose shape description happens to mention
        'nested'."""
        with open(LEGIT_JSON / "nested.json") as f:
            value = json.load(f)
        self.assertIsInstance(value, list, "top-level must be list")
        self.assertTrue(
            any(isinstance(el, list) for el in value),
            "nested.json must contain at least one sub-array (nested structure)",
        )
        sub_array = next(el for el in value if isinstance(el, list))
        self.assertTrue(
            len(sub_array) >= 1,
            "nested.json sub-array must contain elements per grammar",
        )
        for item in sub_array:
            self.assertIsInstance(
                item, dict,
                "nested.json sub-array elements must be objects",
            )
            self.assertTrue(
                all(isinstance(v, (str, int, float, bool, type(None)))
                    for v in item.values()),
                "nested.json object leaves must be primitive",
            )

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
        """Fenced.md must use the documented '<!-- shape: ... -->' HTML-comment
        envelope form for .md payloads per the grammar doc; the fenced JSON
        payload must be intact after the header and conform to the accepted
        object-array grammar (uniform keys, string discriminator, primitive
        leaves)."""
        data = _read_bytes(LEGIT_JSON / "fenced.md")
        lf = data.find(b"\n")
        self.assertGreaterEqual(lf, 0, "fenced.md: missing header newline")
        header_line = data[:lf].decode("utf-8", errors="replace").strip()
        self.assertTrue(
            header_line.startswith("<!-- shape:") and header_line.endswith("-->"),
            f"fenced.md header must use '<!-- shape: ... -->' form, got: {header_line!r}",
        )
        fields = _parse_header_for(LEGIT_JSON / "fenced.md")
        self.assertEqual(fields["expected"], "no-fire")
        body = data[lf + 1:].decode("utf-8", errors="strict")
        match = re.fullmatch(r"```json\n(.*?)\n```\n?", body, re.DOTALL)
        self.assertIsNotNone(match, "fenced.md: expected one paired json fence")
        value = json.loads(match.group(1))
        self.assertIsInstance(value, list, "fenced JSON must be an object array")
        self.assertTrue(value, "fenced JSON object array must not be empty")
        self.assertTrue(all(isinstance(item, dict) for item in value))
        keysets = {tuple(sorted(item)) for item in value}
        self.assertEqual(len(keysets), 1, "fenced objects must have uniform keys")
        self.assertTrue(all(isinstance(item.get("kind"), str) for item in value),
                        "fenced objects require a string kind discriminator")
        primitive_types = (str, int, float, bool, type(None))
        self.assertTrue(all(isinstance(leaf, primitive_types)
                            for item in value for leaf in item.values()),
                        "fenced object leaves must be primitive")

    def test_md_fixtures_use_html_comment_envelope(self):
        """The .md legit fixtures (fenced_python.md, markdown_table.md,
        ordered_list.md, json/fenced.md) must use the <!-- shape: -->
        HTML-comment form documented in the grammar, not a # shape: line,
        so that the surrounding Markdown is not misinterpreted as an ATX
        heading."""
        md_fixtures = [
            "fenced_python.md",
            "markdown_table.md",
            "ordered_list.md",
            "json/fenced.md",
        ]
        for fn in md_fixtures:
            p = LEGIT / fn
            data = _read_bytes(p)
            lf = data.find(b"\n")
            self.assertGreaterEqual(lf, 0, f"{fn}: missing header newline")
            header_line = data[:lf].decode("utf-8", errors="replace").strip()
            self.assertTrue(
                header_line.startswith("<!-- shape:") and header_line.endswith("-->"),
                f"{fn}: .md header must use '<!-- shape: ... -->' form, got: {header_line!r}",
            )
            fields = _parse_header_for(p)
            self.assertEqual(fields["expected"], "no-fire")


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
        primitive_types = (str, int, float, bool, type(None))
        self.assertTrue(
            all(isinstance(leaf, primitive_types)
                for item in value for leaf in item.values()),
            "objects.json values must all be primitive leaves",
        )

    def test_fenced_md_marks_json_block(self):
        text = _read_bytes(LEGIT_JSON / "fenced.md").decode()
        self.assertRegex(text, r"```json")



if __name__ == "__main__":
    unittest.main(verbosity=2)
