"""Repository contracts for the guardrail-collapse Phase 0 packet."""
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DOCS = [
    ROOT / "docs/guardrails/collapse_recon.md",
    ROOT / "docs/guardrails/sampling_capability_matrix.md",
    ROOT / "docs/guardrails/collapse_anchors.md",
]
CITATION = re.compile(r"(?P<path>(?:src|scripts)/[A-Za-z0-9_./-]+):(?P<line>[0-9]+)")
# C block-comment continuation/closer lines: `* foo`, `*/`, `/* foo`.
# A citation that lands on one of these is a documentation drift bug -- the
# cited line is supposed to anchor a declaration, definition, or call-site
# signature, not a comment.
# Recognise a C/C++ comment line in any of the common shapes:
# ``// ...``, ``/* ...``, ``* ...`` (block-comment continuation), ``*/``,
# or a bare ``*`` (sometimes used mid-block).  A citation that lands on one
# of these is a documentation drift bug -- the cited line is supposed to
# anchor a declaration, definition, or call-site signature.
_C_COMMENT_LINE = re.compile(
    r"^\s*(?://|/\*|\*(?:[^/]|$)|\*/)"
)


def test_all_phase_zero_citations_resolve_to_current_source():
    # F05 closure: a citation that points at a line which is entirely
    # blank, whitespace-only, or sits inside an unrelated comment block
    # with no nearby code is a documentation drift bug.  The previous
    # implementation only checked that the cited line was within the file
    # (and within EOF), which let the F01/F02/F03 citations drift past the
    # actual symbol without failing.  This test extracts the cited line
    # and the surrounding window (line-2 .. line+2) and requires at least
    # one of those lines to be a non-blank, non-comment line.  A citation
    # like ``src/db1/webchat_live.c:18-21`` is still accepted because the
    # cited line 18 is a comment that documents the SQL block immediately
    # following it (line 21 in the same window is the SQL declaration).
    _window = 2
    for document in DOCS:
        citations = list(CITATION.finditer(document.read_text()))
        assert citations, f"no citations in {document}"
        for match in citations:
            source = ROOT / match.group("path")
            assert source.is_file(), f"{document}: missing {source}"
            line_no = int(match.group("line"))
            lines = source.read_text(errors="replace").splitlines()
            assert line_no <= len(lines), (
                f"{document}: citation beyond EOF: {match.group(0)}"
            )
            window_start = max(0, line_no - 1 - _window)
            window_end = min(len(lines), line_no - 1 + _window + 1)
            window = lines[window_start:window_end]
            has_code = any(
                ln.strip() and not _C_COMMENT_LINE.match(ln)
                for ln in window
            )
            assert has_code, (
                f"{document}: citation {match.group(0)} lands in a window "
                f"(lines {window_start + 1}-{window_end}) that has no "
                f"non-comment code line; window:\n"
                + "\n".join(window)
            )


def test_recon_covers_every_required_surface_and_binding_verdict():
    text = DOCS[0].read_text()
    for surface in ("`/v1/messages`", "`/v1/responses`",
                    "`/v1/chat/completions`", "webchat ingest",
                    "delegate relay", "roundtable relay"):
        assert surface in text
    assert text.count("**Decision: paths diverge and Phase 2 must be split per handler.**") == 1
    assert "AIMEE_DELTA_BLOCK_DELTA" in text
    assert "Invented constants" in text


def test_sampling_matrix_has_required_controls_and_backends():
    text = DOCS[1].read_text()
    header = next(line for line in text.splitlines() if line.startswith("| backend |"))
    for field in ("temperature", "top_p", "max_tokens", "stop",
                  "repetition_penalty", "presence_penalty",
                  "frequency_penalty", "min_p", "stop_sequences",
                  "continuation/prefix"):
        assert f"| {field} " in header
    for backend in ("OpenAI Chat", "OpenAI Responses", "Anthropic",
                    "Bedrock Converse", "Ollama/llama-compatible"):
        assert f"| {backend} |" in text
    assert "## Phase 4.0 missing plumbing" in text


def test_anchor_document_contains_exactly_six_decisions_and_prerequisites():
    text = DOCS[2].read_text()
    assert re.findall(r"^## Decision ([1-6]) ", text, re.MULTILINE) == list("123456")
    for phase in ("Phase 2.3.0", "Phase 4.0", "Phase 5.0"):
        assert phase in text
    assert "Phase 1 implementation starts only after this document is merged" in text
