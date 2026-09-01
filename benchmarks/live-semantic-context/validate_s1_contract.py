#!/usr/bin/env python3
"""Validate the immutable S1 corpus, prompt, tool, and experiment pins."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import re
import subprocess


ROOT = Path(__file__).resolve().parents[2]
BASE = Path(__file__).resolve().parent
MANIFEST = BASE / "s1-task-manifest.json"
CONTRACT = BASE / "s1-experiment-contract.json"
PROMPT = BASE / "prompts" / "s1-agent-system-v1.md"
TOOLS = BASE / "tools" / "s1-tool-schemas-v1.json"
REQUIRED_FAMILIES = {
    "semantic_disambiguation",
    "reference_backed_change",
    "fresh_saved_edit",
    "batched_anchors",
    "control_literal_or_short_file",
    "failure_injection",
}


def sha256_file(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def committed_text(commit: str, path: str) -> str:
    completed = subprocess.run(
        ["git", "show", f"{commit}:{path}"],
        cwd=ROOT,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    if completed.returncode:
        raise ValueError(f"missing immutable source {commit}:{path}: {completed.stderr.strip()}")
    return completed.stdout


def validate_path(value: object, label: str) -> str:
    if not isinstance(value, str) or not value:
        raise ValueError(f"{label}: path must be a non-empty string")
    path = PurePosixPath(value)
    if path.is_absolute() or ".." in path.parts or str(path) != value:
        raise ValueError(f"{label}: path is not canonical and workspace-relative")
    return value


def validate() -> dict[str, str]:
    manifest = json.loads(MANIFEST.read_text())
    contract = json.loads(CONTRACT.read_text())
    tools = json.loads(TOOLS.read_text())
    if contract.get("state") == "candidate-pinned":
        candidate = (contract.get("candidate_commit_pin") or {}).get("commit")
        if not isinstance(candidate, str) or not re.fullmatch(r"[0-9a-f]{40}", candidate):
            raise ValueError("candidate implementation commit is not fully pinned")
        subprocess.run(["git", "cat-file", "-e", f"{candidate}^{{commit}}"], cwd=ROOT, check=True)
    tasks = manifest.get("tasks")
    if not isinstance(tasks, list) or len(tasks) != 45:
        raise ValueError("manifest must contain exactly 45 tasks")
    ids = [task.get("id") for task in tasks]
    if len(set(ids)) != 45 or any(not isinstance(task_id, str) for task_id in ids):
        raise ValueError("task ids must be 45 unique strings")
    semantic = sum(task.get("semantic_eligible") is True for task in tasks)
    controls = sum(task.get("semantic_eligible") is False for task in tasks)
    if (semantic, controls) != (30, 15):
        raise ValueError("manifest must contain 30 semantic tasks and 15 controls")
    if manifest.get("counts") != {"total": 45, "semantic_eligible": 30, "controls": 15}:
        raise ValueError("declared task counts do not match the checked corpus")

    repository = manifest.get("repository") or {}
    commit = repository.get("commit")
    if not isinstance(commit, str) or len(commit) != 40:
        raise ValueError("repository commit must be a full immutable git object id")
    subprocess.run(["git", "cat-file", "-e", f"{commit}^{{commit}}"], cwd=ROOT, check=True)
    observed_families: set[str] = set()
    source_cache: dict[str, list[str]] = {}
    failure_count = 0
    for task in tasks:
        label = str(task["id"])
        if task.get("repository_commit") != commit:
            raise ValueError(f"{label}: does not inherit the immutable repository commit")
        if task.get("authority") != {
            "kind": "local_checkout", "workspace_root": ".", "detached": False
        }:
            raise ValueError(f"{label}: authority is not the checked local checkout")
        if task.get("provider") not in ("gopls", "pyright"):
            raise ValueError(f"{label}: provider is not pinned")
        expected_language = {"gopls": "go", "pyright": "python"}[task["provider"]]
        if task.get("language") != expected_language:
            raise ValueError(f"{label}: language/provider mismatch")
        if not isinstance(task.get("prompt"), str) or not task["prompt"].strip():
            raise ValueError(f"{label}: prompt is absent")
        tags = task.get("tags")
        if not isinstance(tags, list) or task.get("family") not in tags:
            raise ValueError(f"{label}: primary family must be present in tags")
        observed_families.update(tags)
        anchors = task.get("anchors")
        oracle = task.get("oracle") or {}
        if not isinstance(anchors, list) or not anchors or len(anchors) > 16:
            raise ValueError(f"{label}: must have one to sixteen checked anchors")
        if oracle.get("status") != "ok" or oracle.get("exact_target_count") != len(anchors):
            raise ValueError(f"{label}: oracle target count does not match anchors")
        targets = oracle.get("targets")
        if not isinstance(targets, list) or len(targets) != len(anchors):
            raise ValueError(f"{label}: oracle targets are incomplete")
        for index, (anchor, target) in enumerate(zip(anchors, targets, strict=True)):
            path = validate_path(anchor.get("file"), f"{label}.anchors[{index}]")
            if path not in source_cache:
                source_cache[path] = committed_text(commit, path).splitlines()
            lines = source_cache[path]
            line = anchor.get("line")
            column = anchor.get("column")
            symbol = anchor.get("symbol")
            if not isinstance(line, int) or not 1 <= line <= len(lines):
                raise ValueError(f"{label}: anchor line is outside immutable source")
            if not isinstance(symbol, str) or not symbol:
                raise ValueError(f"{label}: anchor symbol is absent")
            observed_column = lines[line - 1].find(symbol) + 1
            if observed_column <= 0 or column != observed_column:
                raise ValueError(
                    f"{label}: {path}:{line} pins column {column}, observed {observed_column}"
                )
            target_path = validate_path(target.get("file"), f"{label}.targets[{index}]")
            if target_path not in source_cache:
                source_cache[target_path] = committed_text(commit, target_path).splitlines()
            target_lines = source_cache[target_path]
            target_line = target.get("line")
            if not isinstance(target_line, int) or not 1 <= target_line <= len(target_lines):
                raise ValueError(f"{label}: target line is outside immutable source")
            if target.get("symbol") != symbol:
                raise ValueError(f"{label}: anchor and target symbols differ")
            definition = target_lines[target_line - 1]
            expected = rf"\b(?:def|func)\s+{re.escape(symbol)}\s*\("
            if not re.search(expected, definition):
                raise ValueError(f"{label}: oracle does not name the canonical definition")
            same_location = path == target_path and line == target_line
            if task["semantic_eligible"] is same_location:
                raise ValueError(
                    f"{label}: semantic tasks require a real use site; controls require direct targets"
                )
        if task.get("family") == "fresh_saved_edit":
            mutation = (task.get("setup") or {}).get("saved_file_mutation") or {}
            validate_path(mutation.get("path"), f"{label}.saved_file_mutation")
            if mutation.get("operation") != "insert_before" or mutation.get("line") != 1:
                raise ValueError(f"{label}: saved edit is not the checked insertion")
            setup = task.get("setup") or {}
            expected_anchors = [{**item, "line": item["line"] + 1} for item in anchors]
            expected_targets = [{**item, "line": item["line"] + 1} for item in targets]
            if setup.get("post_edit_anchors") != expected_anchors:
                raise ValueError(f"{label}: post-edit use-site coordinates are not checked")
            if setup.get("post_edit_targets") != expected_targets:
                raise ValueError(f"{label}: post-edit definition coordinates are not checked")
        failure = task.get("failure_overlay")
        if failure is not None:
            failure_count += 1
            allowed = {
                "missing_binary": "unavailable", "unsupported_method": "unsupported",
                "request_timeout": "unavailable", "provider_crash": "unavailable",
                "stale_document": "stale", "returned_path_escape": "unauthorized",
            }
            if allowed.get(failure.get("injection")) != failure.get("expected_status"):
                raise ValueError(f"{label}: failure overlay weakens the typed outcome")
    if observed_families != REQUIRED_FAMILIES:
        raise ValueError(f"task families differ: {sorted(observed_families)}")
    if failure_count != 6:
        raise ValueError("exactly six scored tasks must carry adversarial failure overlays")

    non_lsp = tools.get("non_lsp_tools")
    lsp_arms = tools.get("arm_lsp_tools")
    if not isinstance(non_lsp, list) or len(non_lsp) != 6:
        raise ValueError("the six non-LSP tool schemas are not frozen")
    if set(lsp_arms or {}) != {"production", "location_only", "batched_context"}:
        raise ValueError("tool schema arms do not match the experiment")
    if lsp_arms["production"]:
        raise ValueError("production arm unexpectedly exposes an LSP tool")
    hashes = {
        "task_manifest_sha256": sha256_file(MANIFEST),
        "agent_system_prompt_sha256": sha256_file(PROMPT),
        "tool_schemas_sha256": sha256_file(TOOLS),
    }
    pins = contract.get("content_pins") or {}
    for name, observed in hashes.items():
        expected = pins.get(name)
        if expected is not None and expected != observed:
            raise ValueError(f"{name}: contract {expected} != observed {observed}")
    return hashes


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--print-hashes", action="store_true")
    args = parser.parse_args()
    hashes = validate()
    if args.print_hashes:
        print(json.dumps(hashes, indent=2, sort_keys=True))
    else:
        print("live-semantic-context S1 contract: ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
