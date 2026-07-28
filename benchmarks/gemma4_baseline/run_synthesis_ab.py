#!/usr/bin/env python3
"""Run the frozen 10k synthesis suite against one OpenAI-compatible model."""

from __future__ import annotations

import argparse
import concurrent.futures
import hashlib
import json
import math
import re
import statistics
import sys
import time
import urllib.error
import urllib.request
from collections import Counter, defaultdict
from pathlib import Path
from typing import Any

try:
    from .build_254_fixtures import sanitize_text
except ImportError:  # Executed directly from benchmarks/gemma4_baseline.
    from build_254_fixtures import sanitize_text


JSON_GBNF = r'''
root   ::= object
value  ::= object | array | string | number | ("true" | "false" | "null") ws
object ::= "{" ws ( string ":" ws value ("," ws string ":" ws value)* )? "}" ws
array  ::= "[" ws ( value ("," ws value)* )? "]" ws
string ::= "\"" ( [^"\\\x7F\x00-\x1F] | "\\" (["\\bfnrt/] | "u" [0-9a-fA-F] [0-9a-fA-F] [0-9a-fA-F] [0-9a-fA-F]) )* "\"" ws
number ::= ("-"? ([0-9] | [1-9] [0-9]{0,15})) ("." [0-9]+)? ([eE] [-+]? [0-9]{1,16})? ws
ws ::= | " " | "\n" [ \t]{0,20}
'''

SCHEMAS = {
    "claim": {
        "subject": "string", "attribute": "string", "value": "string", "text": "string",
        "claim_kind": "fact|requirement|constraint|decision|behavior",
    },
    "code_unit": {
        "symbol": "string", "signature": "string", "summary": "string", "def_kind": "string",
        "invariants": "string or list", "side_effects": "list", "domain_concepts": "list",
    },
    "doc_summary": {
        "summary": "string", "status": "draft|accepted|done|rejected|deferred",
        "priority": "low|medium|high", "components": "list",
    },
    "entity": {"name": "string", "entity_kind": "string", "context": "string"},
    "synthesis": {"topic_name": "string", "text": "string", "citations": "list of 1-based source indices"},
}
TOKEN_RE = re.compile(r"[A-Za-z_][A-Za-z0-9_./:-]*|\d+(?:\.\d+)?")


def load_jsonl(path: Path) -> list[dict[str, Any]]:
    with path.open("r", encoding="utf-8") as handle:
        return [json.loads(line) for line in handle if line.strip()]


def percentile(values: list[float], p: float) -> float:
    if not values:
        return 0.0
    ordered = sorted(values)
    position = (len(ordered) - 1) * p
    lower = math.floor(position)
    upper = math.ceil(position)
    if lower == upper:
        return ordered[lower]
    return ordered[lower] * (upper - position) + ordered[upper] * (position - lower)


def tokens(value: str) -> Counter[str]:
    return Counter(token.lower() for token in TOKEN_RE.findall(value))


def f1(expected: Any, actual: Any) -> float:
    if isinstance(expected, str):
        if not isinstance(actual, str):
            return 0.0
        left, right = tokens(expected), tokens(actual)
        if not left and not right:
            return 1.0
        overlap = sum((left & right).values())
        precision = overlap / max(1, sum(right.values()))
        recall = overlap / max(1, sum(left.values()))
        return 2 * precision * recall / (precision + recall) if precision + recall else 0.0
    if isinstance(expected, list):
        if not isinstance(actual, list):
            return 0.0
        left = {json.dumps(item, sort_keys=True) for item in expected}
        right = {json.dumps(item, sort_keys=True) for item in actual}
        if not left and not right:
            return 1.0
        overlap = len(left & right)
        precision = overlap / max(1, len(right))
        recall = overlap / max(1, len(left))
        return 2 * precision * recall / (precision + recall) if precision + recall else 0.0
    return 1.0 if expected == actual else 0.0


def unwrap(value: Any, task: str) -> dict[str, Any]:
    if not isinstance(value, dict):
        return {}
    if isinstance(value.get("payload"), dict):
        return value["payload"]
    artifacts = value.get("artifacts")
    if isinstance(artifacts, list):
        for artifact in artifacts:
            if isinstance(artifact, dict) and artifact.get("kind") == task and isinstance(artifact.get("payload"), dict):
                return artifact["payload"]
    return value


def score(expected: dict[str, Any], actual: dict[str, Any]) -> dict[str, Any]:
    required = set(expected)
    present = {key for key in required if key in actual and actual[key] not in (None, "")}
    field_scores = {key: f1(expected[key], actual.get(key)) for key in sorted(required)}
    return {
        "schema_valid": bool(required) and required.issubset(actual),
        "required_field_recall": len(present) / max(1, len(required)),
        "content_f1": statistics.fmean(field_scores.values()) if field_scores else 0.0,
        "extra_key_count": len(set(actual) - required),
        "field_scores": field_scores,
    }


def persisted_row(row: dict[str, Any]) -> dict[str, Any]:
    """Redact generated credential/PII-like text after scoring but before persistence."""
    result = dict(row)
    response = result.get("response")
    if isinstance(response, str):
        sanitized = sanitize_text(response)
        if sanitized != response:
            result["response"] = "<REDACTED_GENERATED_RESPONSE>"
            result["response_redacted"] = True
            result["response_sha256"] = hashlib.sha256(response.encode()).hexdigest()
    return result


def prompt_for(case: dict[str, Any], content: str) -> str:
    task = case["task"]
    schema = json.dumps(SCHEMAS[task], sort_keys=True)
    source_label = "Cited source artifacts" if task == "synthesis" else "Source"
    return (
        "Perform the requested structured extraction using only the supplied source. "
        "Return exactly one JSON object containing the requested payload fields; no wrapper, "
        "markdown, commentary, or invented evidence.\n"
        f"Task: {case['instruction']}\nRequired schema: {schema}\n"
        f"{source_label}:\n{content}"
    )


def call(endpoint: str, model: str, case: dict[str, Any], content: str, timeout: int) -> dict[str, Any]:
    request_body = {
        "model": model,
        "messages": [{"role": "user", "content": prompt_for(case, content)}],
        "temperature": 0,
        "seed": 1,
        "max_tokens": 1536,
        "chat_template_kwargs": {"enable_thinking": False},
        "grammar": JSON_GBNF,
    }
    encoded = json.dumps(request_body).encode()
    last_error = ""
    for attempt in range(3):
        started = time.perf_counter()
        try:
            request = urllib.request.Request(
                endpoint.rstrip("/") + "/v1/chat/completions",
                data=encoded,
                headers={"Content-Type": "application/json"},
            )
            with urllib.request.urlopen(request, timeout=timeout) as response:
                body = json.load(response)
            latency = time.perf_counter() - started
            choice = body["choices"][0]
            text = choice["message"].get("content") or ""
            parse_error = ""
            try:
                parsed = json.loads(text)
            except json.JSONDecodeError as exc:
                parsed = {}
                parse_error = str(exc)
            actual = unwrap(parsed, case["task"])
            metrics = score(case["expected"], actual)
            usage = body.get("usage", {})
            timings = body.get("timings", {})
            return {
                "case_id": case["case_id"],
                "task": case["task"],
                "ok": True,
                "attempts": attempt + 1,
                "latency_s": latency,
                "finish_reason": choice.get("finish_reason", ""),
                "raw_parse": not parse_error,
                "parse_error": parse_error,
                "empty": not bool(text.strip()),
                "truncated": choice.get("finish_reason") == "length",
                "usage": usage,
                "timings": timings,
                "metrics": metrics,
                "response": text,
            }
        except (OSError, KeyError, ValueError, urllib.error.URLError) as exc:
            last_error = f"{type(exc).__name__}: {exc}"
            if attempt < 2:
                time.sleep(2**attempt)
    return {
        "case_id": case["case_id"], "task": case["task"], "ok": False,
        "attempts": 3, "error": last_error, "latency_s": 0.0,
        "raw_parse": False, "empty": True, "truncated": False,
        "metrics": {"schema_valid": False, "required_field_recall": 0.0, "content_f1": 0.0, "extra_key_count": 0},
    }


def summarize(rows: list[dict[str, Any]], label: str, model: str, suite_hash: str) -> dict[str, Any]:
    def aggregate(group: list[dict[str, Any]]) -> dict[str, Any]:
        latencies = [float(row.get("latency_s", 0)) for row in group if row.get("ok")]
        values = lambda key: [float(row.get("metrics", {}).get(key, 0)) for row in group]
        completion = [int(row.get("usage", {}).get("completion_tokens", 0)) for row in group]
        prompt = [int(row.get("usage", {}).get("prompt_tokens", 0)) for row in group]
        predicted_ms = sum(float(row.get("timings", {}).get("predicted_ms", 0)) for row in group)
        prompt_ms = sum(float(row.get("timings", {}).get("prompt_ms", 0)) for row in group)
        return {
            "n": len(group),
            "success_rate": sum(bool(row.get("ok")) for row in group) / max(1, len(group)),
            "raw_parse_rate": sum(bool(row.get("raw_parse")) for row in group) / max(1, len(group)),
            "schema_valid_rate": statistics.fmean(values("schema_valid")),
            "required_field_recall": statistics.fmean(values("required_field_recall")),
            "content_f1": statistics.fmean(values("content_f1")),
            "empty_rate": sum(bool(row.get("empty")) for row in group) / max(1, len(group)),
            "truncated_rate": sum(bool(row.get("truncated")) for row in group) / max(1, len(group)),
            "latency_s": {"p50": percentile(latencies, 0.50), "p95": percentile(latencies, 0.95), "p99": percentile(latencies, 0.99)},
            "completion_tokens": sum(completion),
            "prompt_tokens": sum(prompt),
            "decode_tokens_per_second": sum(completion) / (predicted_ms / 1000) if predicted_ms else 0.0,
            "prompt_tokens_per_second": sum(prompt) / (prompt_ms / 1000) if prompt_ms else 0.0,
            "requests_retried": sum(int(row.get("attempts", 1)) > 1 for row in group),
        }

    by_task: defaultdict[str, list[dict[str, Any]]] = defaultdict(list)
    for row in rows:
        by_task[row["task"]].append(row)
    return {
        "label": label,
        "model": model,
        "suite_manifest_sha256": suite_hash,
        "overall": aggregate(rows),
        "by_task": {task: aggregate(group) for task, group in sorted(by_task.items())},
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--endpoint", required=True)
    parser.add_argument("--model", required=True)
    parser.add_argument("--label", required=True)
    parser.add_argument("--bundle", type=Path, default=Path("benchmarks/fixtures/gemma4-unified/ab-v1"))
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--workers", type=int, default=1)
    parser.add_argument("--timeout", type=int, default=300)
    parser.add_argument("--max-cases", type=int, default=0)
    args = parser.parse_args()

    corpus = {row["doc_id"]: row["content"] for row in load_jsonl(args.bundle / "corpus.jsonl")}
    cases = load_jsonl(args.bundle / "synthesis.jsonl")
    if args.max_cases:
        cases = cases[: args.max_cases]
    manifest_path = args.bundle / "manifest.json"
    suite_hash = hashlib.sha256(manifest_path.read_bytes()).hexdigest()
    args.output_dir.mkdir(parents=True, exist_ok=True)
    raw_path = args.output_dir / f"raw_{args.label}.jsonl"
    done: dict[str, dict[str, Any]] = {}
    if raw_path.exists():
        for row in load_jsonl(raw_path):
            done[row["case_id"]] = row
    pending = [case for case in cases if not done.get(case["case_id"], {}).get("ok", False)]

    with raw_path.open("a", encoding="utf-8", newline="\n") as handle:
        with concurrent.futures.ThreadPoolExecutor(max_workers=args.workers) as executor:
            futures = {
                executor.submit(call, args.endpoint, args.model, case, corpus[case["source_doc_id"]], args.timeout): case
                for case in pending
            }
            for completed, future in enumerate(concurrent.futures.as_completed(futures), 1):
                row = persisted_row(future.result())
                done[row["case_id"]] = row
                handle.write(json.dumps(row, ensure_ascii=False, sort_keys=True) + "\n")
                handle.flush()
                if completed % 100 == 0:
                    print(f"{args.label}: {len(done)}/{len(cases)}", flush=True)

    rows = [done[case["case_id"]] for case in cases]
    summary = summarize(rows, args.label, args.model, suite_hash)
    summary_path = args.output_dir / f"summary_{args.label}.json"
    summary_path.write_text(json.dumps(summary, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(json.dumps(summary, indent=2, sort_keys=True))
    return 0 if summary["overall"]["n"] == len(cases) and all(row.get("ok", False) for row in rows) else 1


if __name__ == "__main__":
    sys.exit(main())
