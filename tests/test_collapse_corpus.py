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

_CANONICAL_HEADER = re.compile(
    r"^"
    r"shape:\s*(?P<shape>[^;]+); "
    r"expected:\s*(?P<expected>fire|no-fire); "
    r"expected_loop_start_offset:\s*(?P<offset>-?\d+); "
    r"expected_loop_span_bytes:\s*(?P<span>-?\d+); "
    r"expected_repetitions:\s*(?P<reps>-?\d+)"
    r"$"
)

_INLINE_HEADER_RE = re.compile(r"^# shape:\s*(.*)$")
_MD_HEADER_RE = re.compile(r"^<!-- shape:\s*(.*?)\s*-->\s*$")


def _read_bytes(p):
    with open(p, "rb") as f:
        return f.read()


def _envelope_kind(p):
    if p.suffix == ".json":
        meta = p.with_suffix(p.suffix + ".meta")
        if meta.exists():
            return "meta", meta
    if p.suffix == ".md":
        return "md", p
    return "inline_hash", p


def _strip_envelope(p):
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
    s = header_bytes.decode("utf-8", errors="strict")
    kind, _ = _envelope_kind(p)
    if kind == "meta":
        # .meta files are LF-terminated for consistency with the inline
        # envelope form (canonical "single line, terminated by LF"). Strip
        # the trailing LF before enforcing "no embedded newline" and before
        # passing to the canonical parser.
        if s.endswith("\n"):
            s = s[:-1]
    assert "\n" not in s and "\r" not in s, (
        f"{p}: header must be a single line (no embedded newlines)"
    )
    if kind == "md":
        m = _MD_HEADER_RE.match(s)
        assert m, (
            f"{p}: .md header must use '<!-- shape: ... -->' wrapper, "
            f"got: {s!r}"
        )
        body = "shape: " + m.group(1)
    elif kind == "inline_hash":
        m = _INLINE_HEADER_RE.match(s)
        assert m, (
            f"{p}: .txt/.py header must start with '# shape:', got: {s!r}"
        )
        body = "shape: " + m.group(1)
    elif kind == "meta":
        assert s.lstrip().startswith("shape:"), (
            f"{p}: .meta file must start with 'shape:' field, got: {s!r}"
        )
        body = s.strip()
    else:
        raise AssertionError(f"unknown envelope kind {kind!r}")
    return body


def _parse_header(header_bytes, p):
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
    header, _body, _file = _strip_envelope(p)
    return _parse_header(header, p)


def _read_fixture(p):
    return _strip_envelope(p)


def _header_length(p, file_bytes):
    """Return the byte length of the metadata envelope for fixture `p`.
    The envelope covers the LF-terminated header line for inline
    envelopes, and is zero for sibling `.meta` envelopes (where the
    payload file is offset 0)."""
    kind, _ = _envelope_kind(p)
    if kind == "meta":
        return 0
    i = file_bytes.find(b"\n")
    assert i >= 0, f"{p}: header missing newline"
    return i + 1  # include the trailing LF


def _count_verbatim_iterations(body, off, period):
    """Walk the body forward from `off` and return ordered iteration
    offsets. After accepting an iteration at offset p, the next
    iteration must start at a position strictly greater than p (no
    overlap with the current iteration's bytes). The body is scanned
    byte-by-byte so that an incidental substring match of the period
    inside a longer connective region cannot be silently mistaken for
    a new iteration; ``_check_fire`` enforces the
    connective-must-not-contain-P precondition by validating that no
    connective region contains the period as a substring before
    trusting the count."""
    sp = len(period)
    iterations = []
    pos = off
    while pos + sp <= len(body):
        if body[pos:pos + sp] == period:
            iterations.append(pos)
            pos += sp
            continue
        pos += 1
    return iterations


def _connectives(body, iterations, sp):
    out = []
    for a, b in zip(iterations, iterations[1:]):
        out.append(body[a + sp:b])
    return out


def _has_genuine_verbatim_loop(body, min_period=60):
    """Return (period_bytes, offset_a, offset_b) if the body contains a
    verbatim period of length >= min_period that is "substantive" (i.e.
    contains word-bearing content beyond pure markup) and that repeats
    >= 2 times at non-overlapping, boundary-aligned positions with a
    connective that does NOT contain the period as a substring. Returns
    (None, None, None) if no such loop exists.

    The substantive-content threshold (>= 60 bytes and containing at
    least one alphabetic byte) is what separates "legitimate
    structural repeats" (which the detector must suppress) from
    "genuine collapse loops" (which the detector must fire on).
    Structural markers such as "+-----+", "| OK  |", "| Name | Status |",
    "1. configure", and short import stubs are below the threshold
    and are not flagged. A repeated paragraph or repeated multi-line
    code block with prose content IS flagged. This framing matches
    the corpus intent: every no-fire fixture must remain honestly
    suppressable (no paragraph-level collapse lurking inside).
    """
    max_sp = len(body) // 2
    ascii_letters = set(range(b"a"[0], b"z"[0] + 1)) | set(
        range(b"A"[0], b"Z"[0] + 1)
    )
    for sp in range(min_period, max_sp + 1):
        first = {}
        for pos in range(0, len(body) - sp + 1):
            slice_ = body[pos:pos + sp]
            if not any(b in ascii_letters for b in slice_):
                continue
            if slice_ in first:
                first_off = first[slice_]
                connective = body[first_off + sp:pos]
                if slice_ not in connective:
                    return slice_, first_off, pos
            else:
                first[slice_] = pos
    return None, None, None


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
    REQUIRED_FILES = [
        "short_span.txt",
        "long_span.txt",
        "ramp_then_loop.txt",
        "nested_loop.txt",
        "interleaved.txt",
        # near_miss_fire.txt exercises the FN category: a fire fixture
        # whose period embeds smaller structural markers a naive
        # detector could mistake for the unit of repetition and
        # therefore miss the outer scale. The detector must fire on
        # it; failing to do so counts as a false negative.
        "near_miss_fire.txt",
    ]

    def test_required_files_present(self):
        for fn in self.REQUIRED_FILES:
            self.assertTrue((COLLAPSE / fn).exists(), f"missing {fn}")

    # Shape kinds decide the connective rule the validator enforces.
    # The grammar doc defines two iteration-boundary styles:
    #
    #   contiguous  - the connective between consecutive iterations is
    #                 empty; iterations advance by exactly `sp` bytes.
    #                 Used by short_span, long_span, ramp_then_loop,
    #                 and nested_loop.
    #   interleaved - the connective is non-empty and typically varies
    #                 between iterations. Used by the interleaved
    #                 fixture.
    #
    # The validator scans the *body* (post-envelope bytes) so the
    # iteration oracle is computed against the byte range the grammar
    # doc defines. The metadata offset is translated from
    # file-relative to body-relative by subtracting header_len.
    SHAPE_CONTIGUOUS = "contiguous"
    SHAPE_INTERLEAVED = "interleaved"

    def _check_fire(self, p, shape_kind=SHAPE_CONTIGUOUS):
        header, body, file_bytes = _read_fixture(p)
        fields = _parse_header_for(p)
        self.assertEqual(fields["expected"], "fire", f"{p}: must be fire")
        file_off = fields["offset"]
        sp = fields["span"]
        reps = fields["repetitions"]
        header_len = _header_length(p, file_bytes)
        off = file_off - header_len
        self.assertGreaterEqual(
            off, 0,
            f"{p}: declared offset {file_off} is inside the header "
            f"(header_len={header_len}); the period must live in the body",
        )
        self.assertGreaterEqual(sp, 2, f"{p}: span {sp} too small")
        self.assertLessEqual(off + sp, len(body),
                             f"{p}: offset+span exceeds body length "
                             f"(body_len={len(body)}, off={off}, sp={sp})")
        self.assertGreaterEqual(
            reps, 2,
            f"{p}: expected_repetitions {reps} must be >= 2 for fire",
        )
        period = body[off:off + sp]
        self.assertTrue(
            any(b not in (b"\n", b" ", b"\t", b"\r") for b in period),
            f"{p}: period slice is whitespace-only; "
            f"the corpus must declare substantive periods",
        )
        iterations = _count_verbatim_iterations(body, off, period)
        self.assertEqual(
            len(iterations), reps,
            f"{p}: expected exactly {reps} iteration boundaries in body, "
            f"found {len(iterations)} at body offsets {iterations}",
        )
        self.assertEqual(
            iterations[0], off,
            f"{p}: first iteration body offset {iterations[0]} must "
            f"equal declared offset {off} (file offset {file_off}, "
            f"header_len {header_len})",
        )
        for a, b in zip(iterations, iterations[1:]):
            self.assertGreater(b, a, f"{p}: iterations not strictly increasing")
        if shape_kind == self.SHAPE_CONTIGUOUS:
            for a, b in zip(iterations, iterations[1:]):
                self.assertEqual(
                    a + sp, b,
                    f"{p}: contiguous loop expected empty connective, "
                    f"but iteration gap at body offsets {a}->{b} has "
                    f"connective {body[a + sp:b]!r}; the grammar "
                    f"defines contiguous loops as boundary-aligned",
                )
        for k, (a, b) in enumerate(zip(iterations, iterations[1:])):
            connective = body[a + sp:b]
            self.assertNotIn(
                period, connective,
                f"{p}: connective {k} contains period substring "
                f"(non-boundary-aligned match)",
            )
        if shape_kind == self.SHAPE_INTERLEAVED:
            connectives = _connectives(body, iterations, sp)
            self.assertEqual(
                len(connectives), len(set(connectives)),
                f"{p}: connective slices must all be distinct, "
                f"got {connectives!r}",
            )
            for k, c in enumerate(connectives):
                self.assertGreater(
                    len(c), 0,
                    f"{p}: connective {k} must be non-empty for "
                    f"interleaved shape; got {c!r}",
                )
                self.assertTrue(
                    any(b not in (b"\n", b" ", b"\t", b"\r") for b in c),
                    f"{p}: interleaved connective {c!r} must contain "
                    f"non-whitespace",
                )

    def test_short_span(self):
        self._check_fire(COLLAPSE / "short_span.txt")

    def test_long_span(self):
        self._check_fire(COLLAPSE / "long_span.txt")

    def test_ramp_then_loop(self):
        p = COLLAPSE / "ramp_then_loop.txt"
        header, body, file_bytes = _read_fixture(p)
        fields = _parse_header_for(p)
        header_len = _header_length(p, file_bytes)
        off_body = fields["offset"] - header_len
        sp = fields["span"]
        period = body[off_body:off_body + sp]
        self.assertEqual(period, b"loop item\n",
                         f"declared offset does not point at the loop "
                         f"period; got {period!r}")
        self.assertEqual(body[off_body - 1:off_body], b"\n",
                         "offset must start on a fresh line, not mid-ramp")
        ramp_window = body[:off_body]
        self.assertIn(b"setup one\n", ramp_window)
        self.assertIn(b"setup two\n", ramp_window)
        self.assertIn(b"setup three\n", ramp_window)
        self._check_fire(p, shape_kind=self.SHAPE_CONTIGUOUS)

    def test_nested_loop(self):
        p = COLLAPSE / "nested_loop.txt"
        header, body, file_bytes = _read_fixture(p)
        fields = _parse_header_for(p)
        header_len = _header_length(p, file_bytes)
        off_body = fields["offset"] - header_len
        sp = fields["span"]
        period = body[off_body:off_body + sp]
        self.assertTrue(period.startswith(b"outer-start\n"),
                        f"period must begin with 'outer-start\\n', "
                        f"got {period[:20]!r}")
        self.assertTrue(period.endswith(b"outer-end\n"),
                        f"period must end with 'outer-end\\n', "
                        f"got {period[-20:]!r}")
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
            f"inner sub-block must repeat >=2 times in outer period, "
            f"got {count}",
        )
        self._check_fire(p, shape_kind=self.SHAPE_CONTIGUOUS)

    def test_interleaved(self):
        p = COLLAPSE / "interleaved.txt"
        self._check_fire(p, shape_kind=self.SHAPE_INTERLEAVED)

    def test_near_miss_fire(self):
        # FN-risk fixture: the outer period contains inner markers
        # (marker-a, marker-b) a naive detector might split on. The
        # detector must fire on the outer scale anyway. Period must
        # be boundary-aligned, span includes the markers verbatim,
        # and the connective rule is contiguous.
        p = COLLAPSE / "near_miss_fire.txt"
        header, body, file_bytes = _read_fixture(p)
        fields = _parse_header_for(p)
        header_len = _header_length(p, file_bytes)
        off_body = fields["offset"] - header_len
        sp = fields["span"]
        period = body[off_body:off_body + sp]
        # Outer period must contain the inner marker pair verbatim,
        # confirming this fixture genuinely nests.
        self.assertIn(
            b"marker-a\n", period,
            "near_miss_fire: outer period must include 'marker-a\n'",
        )
        self.assertIn(
            b"marker-b\n", period,
            "near_miss_fire: outer period must include 'marker-b\n'",
        )
        self._check_fire(p, shape_kind=self.SHAPE_CONTIGUOUS)

    def test_fire_bodies_contain_genuine_verbatim_loop(self):
        # Symmetric to the no-fire negative check on
        # _has_genuine_verbatim_loop: every fire fixture body must
        # itself contain a substantive verbatim period that repeats
        # at least twice. This guards against a regression where a
        # "fire" fixture degenerates into a structural-only repeat
        # (e.g. a body composed entirely of single-byte or
        # pure-marker tokens) and the oracle silently accepts it as
        # a fire target.
        #
        # The probe uses min_period=4 because the corpus
        # intentionally exercises both small (short_span: 6-byte
        # period) and large (long_span: 127-byte period) verbatim
        # loops; the substantive-content filter (period must
        # contain at least one alphabetic byte) is what
        # disqualifies marker-only bodies.
        for fn in self.REQUIRED_FILES:
            p = COLLAPSE / fn
            header, body, file_bytes = _strip_envelope(p)
            period, a, b = _has_genuine_verbatim_loop(body, min_period=4)
            self.assertIsNotNone(
                period,
                f"{p}: declared fire, but body has no substantive "
                f"verbatim loop of period >= 4 bytes; a fire fixture "
                f"must genuinely repeat to exercise the detector",
            )
            self.assertGreaterEqual(
                len(period), 4,
                f"{p}: degenerate verbatim period {len(period)} bytes",
            )
            self.assertLess(
                a, b,
                f"{p}: verbatim loop offsets not strictly ordered: {a}, {b}",
            )

    def test_fire_declared_offsets_match_file_position(self):
        # File-level offset honesty: the declared
        # expected_loop_start_offset must equal the absolute byte
        # offset of the first period byte in the file (not merely
        # the body-relative offset, which the boundary-alignment
        # check already verifies). A regression where the declared
        # offset is a placeholder copy-pasted across fixtures would
        # be caught here.
        ascii_letters = set(range(b"a"[0], b"z"[0] + 1)) | set(
            range(b"A"[0], b"Z"[0] + 1)
        )
        for fn in self.REQUIRED_FILES:
            p = COLLAPSE / fn
            header, body, file_bytes = _strip_envelope(p)
            fields = _parse_header_for(p)
            header_len = _header_length(p, file_bytes)
            off_body = fields["offset"] - header_len
            sp = fields["span"]
            period = body[off_body:off_body + sp]
            self.assertTrue(
                any(b in ascii_letters for b in period),
                f"{p}: period slice has no alphabetic byte; "
                f"declared period is degenerate",
            )
            file_off_first_period = header_len + off_body
            self.assertEqual(
                file_off_first_period, fields["offset"],
                f"{p}: declared offset {fields['offset']} does not "
                f"equal header_len ({header_len}) + body_off "
                f"({off_body}) = {file_off_first_period}; "
                f"the declared offset must be the absolute byte "
                f"position of the first period byte in the file",
            )


class TestCollapseLegit(unittest.TestCase):
    REQUIRED_NON_JSON = [
        "ascii_box.txt",
        "code_boilerplate.py",
        "fenced_python.md",
        "markdown_table.md",
        "ordered_list.md",
        "short_lines.txt",
        # fp_risk_long_code.md exercises the FP category: a no-fire
        # fixture whose structural repeat is unusually long (a
        # full fenced python block, not a one-line marker), so a
        # naive detector with a fixed threshold might mistakenly
        # fire. The detector must suppress it; failing to do so
        # counts as a false positive.
        "fp_risk_long_code.md",
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

    def test_no_fire_bodies_have_no_paragraph_style_repeat(self):
        """Every no-fire fixture declares its own periodic structure
        as the suppression target the detector must learn to ignore
        (e.g. ASCII box edges, markdown table rows, code boilerplate
        stubs). That structural repeat is intentional and is exactly
        what makes the fixture a legitimate no-fire target. We do NOT
        want to assert "no verbatim repeat at all" — that would
        contradict the corpus intent.

        What we DO want to assert: no no-fire fixture body may
        contain a *paragraph-style* verbatim repeat: a continuous
        block of substantive prose content (>= 40 bytes containing
        several distinct words) that happens to repeat. Such a repeat
        would be a content collapse the detector should fire on, and
        smuggling one into a no-fire fixture would be dishonest.
        Smaller structural markers (ASCII box edges, list items,
        import statements, table separators) are below the prose-
        length threshold and are explicitly allowed.

        Following the review's alternative recommendation: the
        paragraphs-vs-structural distinction is fundamentally a
        detector policy choice. The corpus test pins the *upper*
        limit (no prose-style repeats) and leaves the detector
        responsible for the lower bound (which structural markers
        count as suppressable)."""
        for sub in (LEGIT, LEGIT_JSON):
            for p in sorted(sub.rglob("*")):
                if not p.is_file():
                    continue
                if p.name.endswith(".meta"):
                    continue
                header, body, file_bytes = _strip_envelope(p)
                period, a, b = _has_genuine_verbatim_loop(body)
                if period is not None:
                    self.fail(
                        f"{p}: declared no-fire, but body contains a "
                        f"paragraph-style verbatim repeat of length "
                        f"{len(period)} at offsets {a} and {b}; such a "
                        f"content-level collapse should live under "
                        f"collapse_collapse/, not collapse_legit/"
                    )

    def test_no_fire_bodies_do_exhibit_a_structural_repeat(self):
        """Companion check: each no-fire fixture body that the corpus
        says demonstrates a "repeated shape" actually exhibits at
        least one verbatim repeat at a small period size (a
        structural marker such as a row, line, fence, or box).
        This protects against a regression where a no-fire fixture
        becomes so generic that it has no repetition at all, in
        which case it would not be exercising the suppression code
        path the detector must implement.

        Fixtures that are accepted as one-off examples (e.g. fenced
        markdown whose body has just one fence, or primitives.json
        which is a primitive array with periodic numbers) are
        exempted from this check via the ``EXPECTED_STRUCTURAL_REPEAT``
        set below; the remaining fixtures must exhibit at least one
        boundary-aligned repeat of period length in [2, 20)."""
        expected_no_repeat = {
            ("collapse_legit", "json", "fenced.md"),
            ("json", "primitives.json"),
            ("json", "nested.json"),
            ("json", "objects.json"),
        }
        for sub in (LEGIT, LEGIT_JSON):
            for p in sorted(sub.rglob("*")):
                if not p.is_file() or p.name.endswith(".meta"):
                    continue
                rel = p.relative_to(LEGIT)
                key = (sub.name,) + rel.parts if sub == LEGIT else (sub.name, p.name)
                if key in expected_no_repeat:
                    # No structural-repeat expectation; nothing to check.
                    continue
                header, body, file_bytes = _strip_envelope(p)
                self.assertTrue(
                    self._body_has_short_repeat(body),
                    f"{p}: declared no-fire, but body does not exhibit "
                    f"any boundary-aligned verbatim repeat of period in "
                    f"[2, 20); the fixture should demonstrate a structural "
                    f"repeat the detector must suppress.",
                )

    @staticmethod
    def _body_has_short_repeat(body, min_period=2, max_period=20):
        # Pure-whitespace marker periods (single newline) are too common
        # to count, so require at least one byte that is neither whitespace
        # nor a pure ASCII art character. A row separator "| Name |" or
        # "1. " or "+-----+" qualifies because it contains "|"/" " among
        # other characters.
        ws = (b"\n", b" ", b"\t", b"\r")
        for sp in range(min_period, max_period + 1):
            seen = {}
            for pos in range(0, len(body) - sp + 1):
                slice_ = body[pos:pos + sp]
                if slice_ in ws:
                    continue
                if slice_ in seen:
                    return True
                seen[slice_] = pos
        return False

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
        """The .md legit fixtures must actually use the
        '<!-- shape: ... -->' HTML-comment envelope on their first line,
        not '# shape:'. A regression to '# shape:' on a .md payload
        would be detected here."""
        md_fixtures = [
            "fenced_python.md",
            "markdown_table.md",
            "ordered_list.md",
            "json/fenced.md",
        ]
        for fn in md_fixtures:
            p = LEGIT / fn
            file_bytes = _read_bytes(p)
            first_nl = file_bytes.find(b"\n")
            assert first_nl >= 0, f"{p}: missing newline"
            first_line = file_bytes[:first_nl].decode("utf-8")
            self.assertTrue(
                first_line.startswith("<!-- shape:"),
                f"{p}: .md header must start with '<!-- shape:', got: {first_line!r}",
            )
            self.assertTrue(
                first_line.endswith(" -->"),
                f"{p}: .md header must end with ' -->', got: {first_line!r}",
            )
            fields = _parse_header_for(p)
            self.assertEqual(fields["expected"], "no-fire")

    def test_non_md_payloads_use_hash_envelope(self):
        """The .txt and .py fixtures must use the '# shape: ...' envelope
        form on the first line; a regression to '<!-- shape: ... -->'
        on these would be detected here."""
        non_md_fixtures = [
            "ascii_box.txt",
            "code_boilerplate.py",
            "short_lines.txt",
        ]
        for fn in non_md_fixtures:
            p = LEGIT / fn
            file_bytes = _read_bytes(p)
            first_nl = file_bytes.find(b"\n")
            assert first_nl >= 0, f"{p}: missing newline"
            first_line = file_bytes[:first_nl].decode("utf-8")
            self.assertTrue(
                first_line.startswith("# shape:"),
                f"{p}: .txt/.py header must start with '# shape:', got: {first_line!r}",
            )
            self.assertFalse(
                first_line.startswith("<!--"),
                f"{p}: .txt/.py must not use the .md '<!--' envelope",
            )

    def test_meta_files_terminate_with_lf(self):
        """The grammar doc states the canonical header occupies one line
        terminated by LF; .meta files store the entire header as their
        content, so they too are LF-terminated for consistency with the
        inline envelope form."""
        for fn in ["primitives.json", "nested.json", "objects.json"]:
            meta = LEGIT_JSON / (fn + ".meta")
            data = _read_bytes(meta)
            self.assertTrue(
                data.endswith(b"\n"),
                f"{meta}: must end with LF to match canonical single-line rule",
            )
            self.assertNotIn(
                b"\r", data,
                f"{meta}: must not contain CR (Windows-style line terminator)",
            )


class TestAcceptedGrammarShapes(unittest.TestCase):
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

    def test_fenced_python_md_has_multiple_fences(self):
        """fenced_python.md must contain multiple distinct fenced code
        blocks so it exercises the 'repeated fenced block'
        suppression shape (per the acceptance criteria)."""
        text = _read_bytes(LEGIT / "fenced_python.md").decode()
        fence_open = text.count("```python")
        self.assertGreaterEqual(
            fence_open, 2,
            f"fenced_python.md must contain >= 2 fenced python blocks, "
            f"got {fence_open}",
        )


if __name__ == "__main__":
    unittest.main(verbosity=2)
