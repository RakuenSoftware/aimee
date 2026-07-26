#!/usr/bin/env python3
"""Build paired, de-identified Gemma baseline fixtures from the .254 KB.

The source database is queried read-only over SSH. Raw rows are streamed through
memory and are never written to disk. The resulting corpus and three 10,000-case
views share stable case/document identifiers and are safe to compare A:B.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import heapq
import json
import math
import re
import shlex
import subprocess
import sys
from collections import Counter, defaultdict
from pathlib import Path
from typing import Any, Iterable


SUITE_VERSION = "gemma4-ab-v1"
DEFAULT_HOST = "admin@192.168.1.254"
PSQL_SOURCE = "/proc/785747/root/usr/lib/postgresql/17/bin/psql"
PG_SOCKET = "/proc/785747/root/run/postgresql"
QUOTAS = {
    "claim": 2500,
    "code_unit": 2500,
    "doc_summary": 2000,
    "entity": 1500,
    "synthesis": 1500,
}
REQUIRED_KEYS = {
    "claim": {"subject", "attribute", "value", "text"},
    "code_unit": {"symbol", "signature", "summary", "def_kind"},
    "doc_summary": {"summary", "status", "priority"},
    "entity": {"name", "context"},
    "synthesis": {"text", "topic_name"},
}
ALLOWED_FIELDS = {
    "claim": ("subject", "attribute", "value", "text", "claim_kind"),
    "code_unit": ("symbol", "signature", "summary", "def_kind", "invariants", "side_effects", "domain_concepts"),
    "doc_summary": ("summary", "status", "priority", "components"),
    "entity": ("name", "entity_kind", "context"),
    "synthesis": ("topic_name", "text"),
}
TASK_INSTRUCTIONS = {
    "claim": "Extract the supported claim as subject, attribute, value, text, and claim_kind JSON.",
    "code_unit": "Extract the code unit as symbol, signature, summary, def_kind, invariants, side_effects, and domain_concepts JSON.",
    "doc_summary": "Summarize the document as summary, status, priority, and components JSON.",
    "entity": "Extract the entity as name, entity_kind, and context JSON.",
    "synthesis": "Synthesize the cited topic as topic_name, text, and citations JSON.",
}

EMAIL_RE = re.compile(r"(?i)\b[A-Z0-9._%+-]+@[A-Z0-9.-]+\.[A-Z]{2,}\b")
IP_RE = re.compile(r"(?<![\w.])(?:\d{1,3}\.){3}\d{1,3}(?![\w.])")
HOME_RE = re.compile(r"(?<!\w)/(?:home|Users)/[^/\s]+")
SECRET_RE = re.compile(
    r"(?i)(?:AKIA[0-9A-Z]{16}|gh[pousr]_[A-Za-z0-9_]{20,}|"
    r"(?:api[_-]?key|token|password|passwd|secret)\s*[:=]\s*[^\s,;]{8,})"
)
PEM_RE = re.compile(r"-----BEGIN [A-Z ]*(?:PRIVATE KEY|CERTIFICATE)-----.*?-----END [A-Z ]+-----", re.S)
TOKEN_RE = re.compile(r"[A-Za-z_][A-Za-z0-9_./:-]*|\d+(?:\.\d+)?")


def stable_id(namespace: str, value: str) -> str:
    return hashlib.sha256(f"{SUITE_VERSION}\0{namespace}\0{value}".encode()).hexdigest()[:24]


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def sanitize_text(value: str) -> str:
    value = value.replace("\x00", "")
    value = PEM_RE.sub("<REDACTED_PEM>", value)
    value = SECRET_RE.sub("<REDACTED_SECRET>", value)
    value = EMAIL_RE.sub("<EMAIL>", value)
    value = IP_RE.sub("<IP_ADDRESS>", value)
    value = HOME_RE.sub("/home/<USER>", value)
    return value


def sanitize_json(value: Any) -> Any:
    if isinstance(value, str):
        return sanitize_text(value)
    if isinstance(value, list):
        return [sanitize_json(item) for item in value]
    if isinstance(value, dict):
        return {sanitize_text(str(key)): sanitize_json(item) for key, item in value.items()}
    return value


def project_payload(kind: str, payload: dict[str, Any]) -> dict[str, Any]:
    projected = {field: sanitize_json(payload[field]) for field in ALLOWED_FIELDS[kind] if field in payload}
    if kind == "synthesis":
        citations = payload.get("citations")
        count = len(citations) if isinstance(citations, list) else 0
        projected["citations"] = list(range(1, count + 1))
    return projected


def canonical_json(value: Any) -> str:
    return json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(",", ":"))


def remote_csv(host: str, sql: str) -> Iterable[dict[str, str]]:
    copy_sql = f"COPY ({sql}) TO STDOUT WITH (FORMAT CSV, HEADER TRUE)"
    remote = (
        "set -eu; "
        "probe=/tmp/aimee-fixture-psql-$$; "
        f"cp {shlex.quote(PSQL_SOURCE)} \"$probe\"; chmod 700 \"$probe\"; "
        "trap 'rm -f \"$probe\"' EXIT; "
        f"\"$probe\" -h {shlex.quote(PG_SOCKET)} -U aimee -d aimee_shared "
        f"-v ON_ERROR_STOP=1 -c {shlex.quote(copy_sql)}"
    )
    process = subprocess.Popen(
        ["ssh", "-o", "BatchMode=yes", host, remote],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    assert process.stdout is not None
    reader = csv.DictReader(process.stdout)
    try:
        yield from reader
    finally:
        stderr = process.stderr.read() if process.stderr else ""
        return_code = process.wait()
        if return_code:
            raise RuntimeError(f"remote psql failed ({return_code}): {stderr.strip()}")


def corpus_sql() -> str:
    return """
        SELECT 'kb_document' AS source_type,
               d.id::text AS source_id,
               COALESCE(d.doc_kind, '') AS doc_kind,
               COALESCE(d.content, '') AS content
          FROM kb_documents d
         WHERE length(COALESCE(d.content, '')) BETWEEN 64 AND 24000
           AND COALESCE(d.quarantine_state, '') NOT IN ('quarantined', 'blocked')
           AND COALESCE(d.sensitivity_class, '') NOT IN ('sensitive', 'restricted', 'secret')
        UNION ALL
        SELECT 'code_unit_source' AS source_type,
               ranked.id AS source_id,
               'code_unit_body' AS doc_kind,
               COALESCE(ranked.payload->>'body_excerpt', '') AS content
          FROM (
              SELECT a.id, a.payload,
                     row_number() OVER (ORDER BY md5(a.id)) AS rn
                FROM artifacts a
               WHERE a.kind = 'code_unit'
                 AND a.state = 'committed'
                 AND length(COALESCE(a.payload->>'body_excerpt', '')) BETWEEN 64 AND 24000
                 AND a.payload ?& ARRAY['symbol', 'signature', 'summary', 'def_kind']
          ) ranked
         WHERE ranked.rn <= 12000
        UNION ALL
        SELECT 'synthesis_source' AS source_type,
               a.id AS source_id,
               'cited_artifacts' AS doc_kind,
               jsonb_agg(jsonb_build_object('kind', b.kind, 'payload', b.payload)
                         ORDER BY citation.ordinality)::text AS content
          FROM artifacts a
          CROSS JOIN LATERAL jsonb_array_elements_text(a.payload->'citations')
               WITH ORDINALITY AS citation(cited_id, ordinality)
          JOIN artifacts b ON b.id = citation.cited_id
         WHERE a.kind = 'synthesis'
           AND a.state = 'committed'
           AND jsonb_typeof(a.payload->'citations') = 'array'
           AND a.payload ?& ARRAY['text', 'topic_name', 'citations']
         GROUP BY a.id
        ORDER BY source_type, source_id
    """


def cases_sql() -> str:
    # Oversample each stratum so sanitizer-induced duplicates can be removed locally.
    return """
        WITH document_cases AS (
            SELECT 'kb_document' AS source_type,
                   d.id::text AS source_id,
                   a.id AS artifact_id,
                   a.kind,
                   a.payload::text AS payload,
                   row_number() OVER (
                       PARTITION BY a.kind
                       ORDER BY md5(a.id || ':' || d.id::text)
                   ) AS rn
              FROM artifact_citations c
              JOIN kb_documents d
                ON d.id = CASE WHEN c.source_id ~ '^[0-9]+$' THEN c.source_id::bigint END
              JOIN artifacts a ON a.id = c.artifact_id
             WHERE c.source_kind = 'kb_document'
               AND a.state = 'committed'
               AND a.kind IN ('claim', 'doc_summary', 'entity')
               AND length(COALESCE(d.content, '')) BETWEEN 64 AND 24000
               AND COALESCE(d.quarantine_state, '') NOT IN ('quarantined', 'blocked')
               AND COALESCE(d.sensitivity_class, '') NOT IN ('sensitive', 'restricted', 'secret')
        ), direct_cases AS (
            SELECT CASE a.kind
                       WHEN 'code_unit' THEN 'code_unit_source'
                       WHEN 'synthesis' THEN 'synthesis_source'
                   END AS source_type,
                   a.id AS source_id,
                   a.id AS artifact_id,
                   a.kind,
                   a.payload::text AS payload,
                   row_number() OVER (PARTITION BY a.kind ORDER BY md5(a.id)) AS rn
              FROM artifacts a
             WHERE a.state = 'committed'
               AND a.kind IN ('code_unit', 'synthesis')
               AND (
                   (a.kind = 'code_unit'
                    AND length(COALESCE(a.payload->>'body_excerpt', '')) BETWEEN 64 AND 24000
                    AND a.payload ?& ARRAY['symbol', 'signature', 'summary', 'def_kind'])
                   OR
                   (a.kind = 'synthesis'
                    AND jsonb_typeof(a.payload->'citations') = 'array'
                    AND jsonb_array_length(a.payload->'citations') > 0
                    AND a.payload ?& ARRAY['text', 'topic_name', 'citations'])
               )
        ), eligible AS (
            SELECT * FROM document_cases
            UNION ALL
            SELECT * FROM direct_cases
        )
        SELECT source_type, source_id, artifact_id, kind, payload
          FROM eligible
         WHERE rn <= 12000
         ORDER BY kind, rn
    """


def tokenize(text: str) -> list[str]:
    return [token.lower() for token in TOKEN_RE.findall(text) if len(token) > 1]


class BM25:
    def __init__(self, docs: dict[str, str]) -> None:
        self.doc_ids = sorted(docs)
        self.lengths: dict[str, int] = {}
        self.postings: dict[str, list[tuple[str, int]]] = defaultdict(list)
        for doc_id in self.doc_ids:
            counts = Counter(tokenize(docs[doc_id]))
            self.lengths[doc_id] = sum(counts.values())
            for token, count in counts.items():
                self.postings[token].append((doc_id, count))
        self.avg_len = sum(self.lengths.values()) / max(1, len(self.lengths))

    def top(self, query: str, excluded: set[str], limit: int) -> list[str]:
        scores: defaultdict[str, float] = defaultdict(float)
        total = len(self.doc_ids)
        for token in set(tokenize(query)):
            posting = self.postings.get(token, [])
            if not posting:
                continue
            idf = math.log(1.0 + (total - len(posting) + 0.5) / (len(posting) + 0.5))
            for doc_id, frequency in posting:
                if doc_id in excluded:
                    continue
                norm = frequency + 1.5 * (1 - 0.75 + 0.75 * self.lengths[doc_id] / self.avg_len)
                scores[doc_id] += idf * (frequency * 2.5) / norm
        ranked = heapq.nsmallest(limit, scores, key=lambda doc_id: (-scores[doc_id], doc_id))
        if len(ranked) < limit:
            ranked.extend(doc_id for doc_id in self.doc_ids if doc_id not in excluded and doc_id not in scores)
        return ranked[:limit]


def query_from_payload(kind: str, payload: dict[str, Any]) -> str:
    fields = {
        "claim": ("text", "subject", "attribute", "value"),
        "code_unit": ("summary", "signature", "symbol", "domain_concepts"),
        "doc_summary": ("summary", "components", "status"),
        "entity": ("context", "name", "entity_kind"),
        "synthesis": ("text", "topic_name"),
    }[kind]
    parts: list[str] = []
    for field in fields:
        value = payload.get(field)
        if isinstance(value, list):
            parts.extend(str(item) for item in value)
        elif value not in (None, ""):
            parts.append(str(value))
    return sanitize_text(" ".join(parts)).strip()


def write_jsonl(path: Path, rows: Iterable[dict[str, Any]]) -> tuple[int, str]:
    count = 0
    digest = hashlib.sha256()
    with path.open("w", encoding="utf-8", newline="\n") as handle:
        for row in rows:
            # A second pass catches credential-like syntax exposed only after
            # JSON escaping. Fail if the replacement ever breaks JSON.
            serialized = sanitize_text(canonical_json(row))
            json.loads(serialized)
            line = serialized + "\n"
            handle.write(line)
            digest.update(line.encode())
            count += 1
    return count, digest.hexdigest()


def assert_no_obvious_secrets(paths: Iterable[Path]) -> None:
    failures: list[str] = []
    for path in paths:
        with path.open("r", encoding="utf-8") as handle:
            for line_number, line in enumerate(handle, 1):
                if EMAIL_RE.search(line) or SECRET_RE.search(line) or PEM_RE.search(line):
                    failures.append(f"{path}:{line_number}")
                    if len(failures) >= 20:
                        break
    if failures:
        raise RuntimeError("secret/de-identification scan failed: " + ", ".join(failures))


def build(host: str, output: Path) -> None:
    output.mkdir(parents=True, exist_ok=True)

    corpus: dict[str, str] = {}
    corpus_kind: dict[str, str] = {}
    raw_doc_fingerprint = hashlib.sha256()
    for row in remote_csv(host, corpus_sql()):
        source_key = f"{row['source_type']}:{row['source_id']}"
        public_id = stable_id("document", source_key)
        content = sanitize_text(row["content"])
        if row["source_type"] == "synthesis_source":
            try:
                cited = json.loads(content)
                content = canonical_json(
                    [
                        {
                            "kind": str(item.get("kind", "")),
                            "payload": project_payload(str(item.get("kind", "")), item.get("payload", {})),
                        }
                        for item in cited
                        if isinstance(item, dict)
                        and str(item.get("kind", "")) in ALLOWED_FIELDS
                        and isinstance(item.get("payload"), dict)
                    ]
                )
            except json.JSONDecodeError:
                continue
        if len(content.strip()) < 64:
            continue
        corpus[public_id] = content
        corpus_kind[public_id] = sanitize_text(row["doc_kind"])
        raw_doc_fingerprint.update(f"{source_key}\0{sha256_bytes(row['content'].encode())}\n".encode())

    selected: dict[str, list[dict[str, Any]]] = {kind: [] for kind in QUOTAS}
    seen: set[str] = set()
    source_fingerprint = hashlib.sha256()
    for row in remote_csv(host, cases_sql()):
        kind = row["kind"]
        if kind not in QUOTAS or len(selected[kind]) >= QUOTAS[kind]:
            continue
        source_key = f"{row['source_type']}:{row['source_id']}"
        public_doc_id = stable_id("document", source_key)
        if public_doc_id not in corpus:
            continue
        try:
            payload_raw = json.loads(row["payload"])
            payload = project_payload(kind, payload_raw)
        except (TypeError, json.JSONDecodeError):
            continue
        if not REQUIRED_KEYS[kind].issubset(payload):
            continue
        query = query_from_payload(kind, payload)
        if len(query) < 16:
            continue
        fingerprint = sha256_bytes(f"{kind}\0{public_doc_id}\0{canonical_json(payload)}".encode())
        if fingerprint in seen:
            continue
        seen.add(fingerprint)
        case_id = stable_id("case", f"{row['artifact_id']}:{source_key}:{kind}")
        selected[kind].append(
            {
                "case_id": case_id,
                "kind": kind,
                "doc_id": public_doc_id,
                "query": query,
                "expected": payload,
            }
        )
        source_fingerprint.update(
            f"{row['artifact_id']}\0{source_key}\0{kind}\0{sha256_bytes(row['payload'].encode())}\n".encode()
        )

    short = {kind: (len(selected[kind]), quota) for kind, quota in QUOTAS.items() if len(selected[kind]) != quota}
    if short:
        raise RuntimeError(f"unable to fill fixture quotas: {short}")

    cases = sorted((case for group in selected.values() for case in group), key=lambda row: row["case_id"])
    if len(cases) != 10_000:
        raise RuntimeError(f"expected 10000 cases, got {len(cases)}")

    bm25 = BM25(corpus)
    synthesis_rows: list[dict[str, Any]] = []
    embedding_rows: list[dict[str, Any]] = []
    rerank_rows: list[dict[str, Any]] = []
    for case in cases:
        synthesis_rows.append(
            {
                "case_id": case["case_id"],
                "task": case["kind"],
                "instruction": TASK_INSTRUCTIONS[case["kind"]],
                "source_doc_id": case["doc_id"],
                "expected": case["expected"],
                "label_provenance": "committed_artifact_citation_silver",
            }
        )
        embedding_rows.append(
            {
                "case_id": case["case_id"],
                "query": case["query"],
                "positive_doc_ids": [case["doc_id"]],
                "label_provenance": "committed_artifact_citation_silver",
            }
        )
        negatives = bm25.top(case["query"], {case["doc_id"]}, 19)
        candidates = [case["doc_id"], *negatives]
        candidates.sort(key=lambda doc_id: sha256_bytes(f"{case['case_id']}:{doc_id}".encode()))
        rerank_rows.append(
            {
                "case_id": case["case_id"],
                "query": case["query"],
                "candidate_doc_ids": candidates,
                "relevance": {case["doc_id"]: 1},
                "label_provenance": "committed_artifact_citation_silver",
            }
        )

    file_results: dict[str, dict[str, Any]] = {}
    outputs = {
        "corpus.jsonl": (
            {"doc_id": doc_id, "doc_kind": corpus_kind[doc_id], "content": corpus[doc_id]}
            for doc_id in sorted(corpus)
        ),
        "synthesis.jsonl": synthesis_rows,
        "embedding.jsonl": embedding_rows,
        "reranking.jsonl": rerank_rows,
    }
    paths: list[Path] = []
    for name, rows in outputs.items():
        path = output / name
        count, digest = write_jsonl(path, rows)
        file_results[name] = {"rows": count, "sha256": digest}
        paths.append(path)

    assert_no_obvious_secrets(paths)
    model_views = {
        label: {
            "synthesis": "required",
            "embedding": "required_native_width",
            "reranking": "excluded_instruction_base_not_cross_encoder",
        }
        for label in (
            "gemma4_e2b", "gemma4_e4b", "gemma4_12b", "gemma4_26b_a4b", "gemma4_31b", "qwen36_35b_a3b"
        )
    }
    for label in ("ettin68m", "ettin400m"):
        model_views[label] = {
            "synthesis": "excluded_reranker_only",
            "embedding": "excluded_reranker_only",
            "reranking": "required_incumbent_control",
        }
    manifest = {
        "suite_version": SUITE_VERSION,
        "source": {
            "deployment": ".254",
            "database": "aimee_shared",
            "access": "read_only_stream",
            "raw_rows_persisted": False,
            "document_snapshot_sha256": raw_doc_fingerprint.hexdigest(),
            "case_snapshot_sha256": source_fingerprint.hexdigest(),
        },
        "case_count": len(cases),
        "strata": {kind: len(selected[kind]) for kind in sorted(selected)},
        "label_provenance": "silver: committed artifacts with kb_document citations",
        "baseline_model_views": model_views,
        "training_exclusion": {
            "required": True,
            "keys": ["suite_version", "document_snapshot_sha256", "case_snapshot_sha256"],
        },
        "deidentification": {
            "emails": True,
            "ip_addresses": True,
            "home_usernames": True,
            "credential_patterns": True,
            "pem_blocks": True,
            "source_ids_hashed": True,
        },
        "files": file_results,
    }
    manifest_path = output / "manifest.json"
    manifest_path.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(json.dumps(manifest, indent=2, sort_keys=True))


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", default=DEFAULT_HOST)
    parser.add_argument(
        "--output",
        type=Path,
        default=Path("benchmarks/fixtures/gemma4-unified/ab-v1"),
    )
    args = parser.parse_args()
    build(args.host, args.output)
    return 0


if __name__ == "__main__":
    sys.exit(main())
