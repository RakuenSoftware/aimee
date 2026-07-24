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


def test_all_phase_zero_citations_resolve_to_current_source():
    for document in DOCS:
        citations = list(CITATION.finditer(document.read_text()))
        assert citations, f"no citations in {document}"
        for match in citations:
            source = ROOT / match.group("path")
            assert source.is_file(), f"{document}: missing {source}"
            line = int(match.group("line"))
            assert line <= len(source.read_text(errors="replace").splitlines()), (
                f"{document}: citation beyond EOF: {match.group(0)}"
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
