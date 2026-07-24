"""Regressor for the JSON fragment corpus defined by docs/guardrails/json_grammar.md.

Verifies that the checked-in fixtures under tests/fixtures/ match the byte
oracle promised by their headers, that JSON payloads in the json/ subdir
parse as standard JSON, that no .json file contains an inline // comment
header (an excluded fragment form), and that fire-fixture offsets, spans,
and declared repetition counts identify verbatim iteration boundaries in
the body. Detector integration is intentionally deferred until detector
code exists.

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

# Anchored regex for the canonical envelope body (after wrapper stripping).
# Field order is fixed; the only separator is "; "; no duplicates; no extras.
_CANONICAL_HEADER = re.compile(
    r"^"
    r"shape:\s*(?P<shape>[^;]+); "
    r"expected:\s*(?P<expected>fire|no-fire); "
    r"expected_loop_start_offset:\s*(?P<offset>-?\d+); "
    r"expected_loop_span_bytes:\s*(?P<span>-?\d+); "
    r"expected_repetitions:\s*(?P<reps>-?\d+)"
    r"$"
)

# Inline envelope for .txt / .py: "# shape: <body>\n"
_INLINE_HEADER_RE = re.compile(r"^# shape:\s*(.*)$")

# Inline envelope for .md: "<!-- shape: <body> -->\n"
_MD_HEADER_RE = re.compile(r"^<!-- shape:\s*(.*?)\s*-->\s*$")


def _read_bytes(p):
    with open(p, "rb") as f:
        return f.read()


def _envelope_kind(p):
    """Return ('meta', meta_path), ('md', p), or ('inline_hash', p) for a
    fixture. For .json files, the header lives in a sibling .meta if
    present."""
    if p.suffix == ".json":
        meta = p.with_suffix(p.suffix + ".meta")
        if meta.exists():
            return "meta", meta
    if p.suffix == ".md":
        return "md", p
    return "inline_hash", p


def _strip_envelope(p):
    """Return (header_bytes, body_bytes, file_bytes) for a fixture path.
    The header is one line terminated by LF; the body is everything
    after that LF, or for a `.meta` sibling the entire payload file.
    `file_bytes` is the raw bytes of the payload file (used for absolute
    byte-offset validation against declared `expected_loop_start_offset`)."""
    kind, target = _envelope_kind(p)
    if kind == "meta":
        meta = p.with_suffix(p.suffix + ".meta")
        header = _read_bytes(meta)
        file_bytes = _read_bytes(p)
        return header, file_bytes, file_bytes
    file_bytes = _read_bytes(p)
    i = file_bytes.find(b"\n")
    assert i >= 0, f"{p}: header missing newline"
    return file_bytes[:i], file_bytes[i + 1:], file_bytes


def _canonical_header_text(header_bytes, p):
    """Return the canonical header text (normalized to a single canonical
    form) for parsing against _CANONICAL_HEADER. The normalized form is:

        shape: <text>; expected: fire|no-fire; expected_loop_start_offset: <int>;
        expected_loop_span_bytes: <int>; expected_repetitions: <int>

    regardless of whether the input came from a sibling `.meta` file or
    an inline `# shape:` / `<!-- shape: ... -->` wrapper. Raises
    AssertionError on any wrapper or formatting violation."""
    s = header_bytes.decode("utf-8", errors="strict")
    # Single-line rule: no embedded LF/CR.
    assert "\n" not in s and "\r" not in s, (
        f"{p}: header must be a single line (no embedded newlines)"
    )
    kind, _ = _envelope_kind(p)
    if kind == "md":
        m = _MD_HEADER_RE.match(s)
        assert m, (
            f"{p}: .md header must use '<!-- shape: ... -->' wrapper, "
            f"got: {s!r}"
        )
        # Wrapper ate the leading "shape:" literal; prepend it so the
        # canonical form has all field names. Inside the wrapper the body
        # is just "<text>; expected: ...". We rebuild the canonical form
        # by prepending "shape: " to the wrapper-stripped text.
        body = "shape: " + m.group(1)
    elif kind == "inline_hash":
        m = _INLINE_HEADER_RE.match(s)
        assert m, (
            f"{p}: .txt/.py header must start with '# shape:', got: {s!r}"
        )
        # Rebuild canonical form: '# shape: <text>; ...' -> 'shape: <text>; ...'.
        body = "shape: " + m.group(1)
    elif kind == "meta":
        # .meta file is raw header text; canonical form starts with "shape:".
        assert s.lstrip().startswith("shape:"), (
            f"{p}: .meta file must start with 'shape:' field, got: {s!r}"
        )
        body = s.strip()
    else:
        raise AssertionError(f"unknown envelope kind {kind!r}")
    return body


def _parse_header(header_bytes, p):
    """Strict, anchored canonical-header parser. Returns a dict with keys
    shape, expected, offset, span, repetitions. Rejects duplicate fields,
    extra fields, wrong field order, malformed values, or multi-line
    headers."""
    body = _canonical_header_text(header_bytes, p)
    m = _CANONICAL_HEADER.match(body)
    assert m, (
        f"{p}: header does not match canonical grammar (anchored); "
        f"got: {body!r}"
    )
    parts = body.split("; ")
    names = [pp.split(":", 1)[0].strip() for pp in parts]
    expected_names = [
        "shape", "expected", "expected_loop_start_offset",
        "expected_loop_span_bytes", "expected_repetitions",
    ]
    assert names == expected_names, (
        f"{p}: field order or set mismatch; got {names!r}, "
        f"expected {expected_names!r}"
    )
    return {
        "shape": m.group("shape").strip(),
        "expected": m.group("expected").strip(),
        "offset": int(m.group("offset")),
        "span": int(m.group("span")),
        "repetitions": int(m.group("reps")),
    }


def _parse_header_for(p):
    """Load (header_bytes, body_bytes) for `p` and parse the header."""
    header, _body, _file = _strip_envelope(p)
    return _parse_header(header, p)


def _read_fixture(p):
    """Return (header_text, body_bytes, file_bytes) for `p`."""
    return _strip_envelope(p)


def _count_verbatim_iterations(body, off, period):
    """Walk the body forward from `off` and return the list of iteration
    offsets (byte positions whose slice of length len(period) equals
    `period`). Iterations are reached by stepping over zero or more
    connective bytes; iteration boundaries are exactly the points where
    the period matches the slice, and we never re-enter the period from
    inside its own span."""
    sp = len(period)
    iterations = []
    pos = off
    while pos + sp <= len(body):
        if body[pos:pos + sp] == period:
            iterations.append(pos)
            pos += sp
            continue
        # Not at an iteration; step one byte at a time until we find the
        # next occurrence, or until we run out.
        next_idx = body.find(period, pos + 1)
        if next_idx < 0:
            return iterations
        iterations.append(next_idx)
        pos = next_idx + sp
    return iterations


def _connectives(body, iterations, sp):
    """Return a list of connective byte slices between consecutive iterations."""
    out = []
    for a, b in zip(iterations, iterations[1:]):
        out.append(body[a + sp:b])
    return out


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

    def test_grammar_documents_iteration_oracle(self):
        text = _read_bytes(GRAMMAR).decode()
        for needle in [
            "iteration boundar",
            "expected_repetitions",
            "verbatim",
        ]:
            self.assertIn(needle, text,
                          f"grammar must document loop iteration oracle: {needle!r}")


class TestCanonicalEnvelope(unittest.TestCase):
    """Every fixture header must parse via the anchored canonical grammar
    with no duplicates, no extras, no multiline contents."""

    def test_all_headers_match_canonical_grammar(self):
        failures = []
        for p in _iter_fixtures():
            try:
                _parse_header_for(p)
            except AssertionError as exc:
                failures.append(f"{p}: {exc}")
        self.assertFalse(failures,
                         "fixture headers failed canonical envelope check:\n" +
                         "\n".join(failures))

    def test_no_duplicate_or_extra_fields_in_any_header(self):
        for p in _iter_fixtures():
            fields = _parse_header_for(p)
            self.assertTrue(fields["shape"], f"{p}: shape empty")
            self.assertIn(fields["expected"], ("fire", "no-fire"),
                          f"{p}: expected must be fire|no-fire")


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

    def _check_fire(self, p, require_distinct_connectives=False,
                    require_empty_connectives=False):
        header, body, file_bytes = _read_fixture(p)
        fields = _parse_header_for(p)
        self.assertEqual(fields["expected"], "fire", f"{p}: must be fire")
        off = fields["offset"]
        sp = fields["span"]
        reps = fields["repetitions"]
        self.assertGreaterEqual(off, 0,
                                f"{p}: fire offset must be >= 0, got {off}")
        self.assertGreaterEqual(sp, 2, f"{p}: span {sp} too small")
        # The declared offset is an absolute byte offset from the start of
        # the **file** (per the grammar doc). For inline envelopes the
        # file is `header LF body`; for `.meta` siblings the file is the
        # entire payload. Validate against file_bytes.
        self.assertLessEqual(off + sp, len(file_bytes),
                             f"{p}: offset+span exceeds file length")
        self.assertGreaterEqual(
            reps, 2,
            f"{p}: expected_repetitions {reps} must be >= 2 for fire",
        )
        period = file_bytes[off:off + sp]
        self.assertEqual(len(period), sp, f"{p}: period slice wrong length")
        self.assertEqual(file_bytes[off:off + sp], period,
                         f"{p}: declared offset does not point at the period")
        # Walk-forward iteration oracle: count verbatim iterations reachable
        # from `off` in the FILE bytes, excluding spurious substring
        # matches inside connective tissue.
        iterations = _count_verbatim_iterations(file_bytes, off, period)
        self.assertEqual(
            len(iterations), reps,
            f"{p}: expected exactly {reps} iteration boundaries, "
            f"found {len(iterations)} at {iterations}",
        )
        self.assertEqual(iterations[0], off,
                         f"{p}: first iteration must equal declared offset")
        for a, b in zip(iterations, iterations[1:]):
            self.assertGreater(b, a, f"{p}: iterations not strictly increasing")
        # Connective bytes between consecutive iterations must not contain
        # any further occurrence of the period (which would be a spurious
        # match inside connective tissue).
        for k, (a, b) in enumerate(zip(iterations, iterations[1:])):
            connective = file_bytes[a + sp:b]
            self.assertNotIn(
                period, connective,
                f"{p}: connective {k} contains period substring "
                f"(non-boundary-aligned match)",
            )
        if require_distinct_connectives:
            connectives = _connectives(file_bytes, iterations, sp)
            self.assertEqual(
                len(connectives), len(set(connectives)),
                f"{p}: connective slices must all be distinct, "
                f"got {connectives!r}",
            )
            for k, c in enumerate(connectives):
                self.assertGreater(
                    len(c), 0,
                    f"{p}: connective {k} must be non-empty for interleaved "
                    f"shape; got {c!r}",
                )
        if require_empty_connectives:
            connectives = _connectives(file_bytes, iterations, sp)
            for k, c in enumerate(connectives):
                self.assertEqual(
                    len(c), 0,
                    f"{p}: contiguous loop: connective {k} must be empty, "
                    f"got {c!r}",
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
        header, body, file_bytes = _read_fixture(p)
        fields = _parse_header_for(p)
        off = fields["offset"]
        self.assertEqual(file_bytes[off:off + 9], b"loop item",
                         f"offset {off} does not point at 'loop item'")
        self.assertEqual(file_bytes[off - 1:off], b"\n",
                         "offset must start on a fresh line, not mid-ramp")
        self._check_fire(p, require_empty_connectives=True)

    def test_nested_loop(self):
        """The outer block (which itself contains the inner repeat) must
        repeat verbatim, including its line terminators."""
        p = COLLAPSE / "nested_loop.txt"
        header, body, file_bytes = _read_fixture(p)
        fields = _parse_header_for(p)
        off = fields["offset"]
        sp = fields["span"]
        period = file_bytes[off:off + sp]
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
        # For nested loops the outer iteration must be contiguous; there
        # are no connectives between outer iterations.
        n, pos = 0, off
        while pos + sp <= len(file_bytes) and file_bytes[pos:pos + sp] == period:
            n += 1
            pos += sp
        self.assertGreaterEqual(
            n, 2,
            f"outer block must repeat >=2 times contiguously, got {n}",
        )
        self._check_fire(p)

    def test_interleaved(self):
        """The connective tissue between loop iterations must be
        non-repeating and the loop period itself must appear at exactly
        ``expected_repetitions`` ordered, boundary-aligned positions in
        the file — no spurious substring matches inside connective."""
        p = COLLAPSE / "interleaved.txt"
        header, body, file_bytes = _read_fixture(p)
        fields = _parse_header_for(p)
        off = fields["offset"]
        sp = fields["span"]
        period = file_bytes[off:off + sp]
        iterations = _count_verbatim_iterations(file_bytes, off, period)
        self.assertGreaterEqual(
            len(iterations), 2,
            f"expected >= 2 ordered iteration boundaries, got {len(iterations)}",
        )
        self.assertEqual(iterations[0], off,
                         "first iteration must equal declared offset")
        connectives = _connectives(file_bytes, iterations, sp)
        for k, c in enumerate(connectives):
            self.assertGreater(
                len(c), 0,
                f"interleaved connective {k} must be non-empty, got {c!r}",
            )
        unique = {c for c in connectives}
        self.assertEqual(
            len(unique), len(connectives),
            f"connective tissue between iterations must all differ; got {connectives!r}",
        )
        # Strict oracle: exactly `expected_repetitions` ordered iterations
        # and no extra period matches anywhere else in the file.
        self._check_fire(p, require_distinct_connectives=True)


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
        p = LEGIT_JSON / "fenced.md"
        header, body, file_bytes = _read_fixture(p)
        fields = _parse_header_for(p)
        self.assertEqual(fields["expected"], "no-fire")
        body_text = body.decode("utf-8", errors="strict")
        match = re.fullmatch(r"```json\n(.*?)\n```\n?", body_text, re.DOTALL)
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
