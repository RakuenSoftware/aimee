#!/usr/bin/env python3
"""Fail-closed validator for the canonical Gemma A:B fixture bundle."""

from __future__ import annotations

import argparse
import hashlib
import json
import sys
from collections import Counter
from pathlib import Path
from typing import Any

from build_254_fixtures import QUOTAS, SUITE_VERSION, assert_no_obvious_secrets


def load_jsonl(path: Path) -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    with path.open("r", encoding="utf-8") as handle:
        for line_number, line in enumerate(handle, 1):
            try:
                value = json.loads(line)
            except json.JSONDecodeError as exc:
                raise RuntimeError(f"{path}:{line_number}: invalid JSON: {exc}") from exc
            if not isinstance(value, dict):
                raise RuntimeError(f"{path}:{line_number}: expected object")
            rows.append(value)
    return rows


def file_sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def unique_ids(rows: list[dict[str, Any]], field: str, label: str) -> set[str]:
    values = [str(row.get(field, "")) for row in rows]
    if any(not value for value in values):
        raise RuntimeError(f"{label}: missing {field}")
    duplicates = [value for value, count in Counter(values).items() if count > 1]
    if duplicates:
        raise RuntimeError(f"{label}: duplicate {field}: {duplicates[:10]}")
    return set(values)


def validate(bundle: Path) -> dict[str, Any]:
    manifest = json.loads((bundle / "manifest.json").read_text(encoding="utf-8"))
    if manifest.get("suite_version") != SUITE_VERSION:
        raise RuntimeError("suite version mismatch")
    if manifest.get("case_count") != 10_000:
        raise RuntimeError("manifest case_count must be 10000")
    if manifest.get("strata") != {kind: QUOTAS[kind] for kind in sorted(QUOTAS)}:
        raise RuntimeError("manifest strata mismatch")
    if not manifest.get("training_exclusion", {}).get("required"):
        raise RuntimeError("training exclusion must be required")
    six_models = {
        "gemma4_e2b", "gemma4_e4b", "gemma4_12b", "gemma4_26b_a4b", "gemma4_31b", "qwen36_35b_a3b"
    }
    model_views = manifest.get("baseline_model_views")
    if not isinstance(model_views, dict) or set(model_views) != six_models | {"ettin400m"}:
        raise RuntimeError("baseline model/view matrix mismatch")
    for label in six_models:
        if model_views[label] != {
            "synthesis": "required",
            "embedding": "required_native_width",
            "reranking": "excluded_instruction_base_not_cross_encoder",
        }:
            raise RuntimeError(f"{label}: invalid required/excluded view contract")
    if model_views["ettin400m"] != {
        "synthesis": "excluded_reranker_only",
        "embedding": "excluded_reranker_only",
        "reranking": "required_incumbent_control",
    }:
        raise RuntimeError("Ettin required/excluded view contract mismatch")

    paths = [bundle / name for name in ("corpus.jsonl", "synthesis.jsonl", "embedding.jsonl", "reranking.jsonl")]
    for path in paths:
        expected = manifest["files"][path.name]
        if file_sha256(path) != expected["sha256"]:
            raise RuntimeError(f"{path.name}: sha256 mismatch")
    assert_no_obvious_secrets(paths)

    corpus = load_jsonl(bundle / "corpus.jsonl")
    synthesis = load_jsonl(bundle / "synthesis.jsonl")
    embedding = load_jsonl(bundle / "embedding.jsonl")
    reranking = load_jsonl(bundle / "reranking.jsonl")
    corpus_ids = unique_ids(corpus, "doc_id", "corpus")
    synth_ids = unique_ids(synthesis, "case_id", "synthesis")
    embed_ids = unique_ids(embedding, "case_id", "embedding")
    rerank_ids = unique_ids(reranking, "case_id", "reranking")
    if synth_ids != embed_ids or synth_ids != rerank_ids:
        raise RuntimeError("case IDs differ across synthesis/embedding/reranking")
    if len(synth_ids) != 10_000:
        raise RuntimeError("each task view must contain exactly 10000 cases")

    synth_strata = Counter(str(row.get("task", "")) for row in synthesis)
    if synth_strata != Counter(QUOTAS):
        raise RuntimeError(f"synthesis strata mismatch: {dict(synth_strata)}")
    for row in synthesis:
        if row.get("source_doc_id") not in corpus_ids:
            raise RuntimeError(f"synthesis {row['case_id']}: missing corpus source")
        if not isinstance(row.get("expected"), dict) or not row["expected"]:
            raise RuntimeError(f"synthesis {row['case_id']}: empty expected object")

    for row in embedding:
        positives = row.get("positive_doc_ids")
        if not isinstance(positives, list) or len(positives) != 1 or positives[0] not in corpus_ids:
            raise RuntimeError(f"embedding {row['case_id']}: invalid positive")
        if not str(row.get("query", "")).strip():
            raise RuntimeError(f"embedding {row['case_id']}: empty query")

    for row in reranking:
        candidates = row.get("candidate_doc_ids")
        relevance = row.get("relevance")
        if not isinstance(candidates, list) or len(candidates) != 20 or len(set(candidates)) != 20:
            raise RuntimeError(f"reranking {row['case_id']}: candidates must be 20 unique docs")
        if not set(candidates).issubset(corpus_ids):
            raise RuntimeError(f"reranking {row['case_id']}: missing corpus candidate")
        if not isinstance(relevance, dict) or len(relevance) != 1:
            raise RuntimeError(f"reranking {row['case_id']}: expected one positive label")
        positive = next(iter(relevance))
        if positive not in candidates or relevance[positive] != 1:
            raise RuntimeError(f"reranking {row['case_id']}: positive missing from candidates")

    return {
        "status": "ok",
        "suite_version": SUITE_VERSION,
        "corpus_rows": len(corpus),
        "case_rows_per_view": len(synthesis),
        "shared_case_ids": len(synth_ids),
        "strata": dict(sorted(synth_strata.items())),
        "baseline_model_views": model_views,
        "hashes": {path.name: file_sha256(path) for path in paths},
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "bundle",
        type=Path,
        nargs="?",
        default=Path("benchmarks/fixtures/gemma4-unified/ab-v1"),
    )
    result = validate(parser.parse_args().bundle)
    print(json.dumps(result, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    sys.exit(main())
