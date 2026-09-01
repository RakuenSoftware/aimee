#!/usr/bin/env python3
"""Checked Codex CLI runner for the S1 live semantic context paired study."""

from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import platform
import random
import shutil
import subprocess
import tempfile
import time
from typing import Any
import uuid


ROOT = Path(__file__).resolve().parents[2]
BASE = Path(__file__).resolve().parent
CONTRACT = BASE / "s1-experiment-contract.json"
MANIFEST = BASE / "s1-task-manifest.json"
SYSTEM_PROMPT = BASE / "prompts" / "s1-agent-system-v1.md"
RESULT_SCHEMA = BASE / "schemas" / "s1-result-v1.json"
MCP_SERVER = BASE / "s1_mcp_server.py"
BRIDGE_SOURCE = BASE / "s1_lsp_bridge.c"
TOOL_SCHEMAS = json.loads((BASE / "tools" / "s1-tool-schemas-v1.json").read_text())
ARMS = ("production", "location_only", "batched_context")

LINK_OBJECTS = [
    "modules/lsp/lsp_client.o", "modules/lsp/lsp_manager.o", "modules/lsp/lsp_context.o",
    "aimee_sha256.o", "tests/aimee_pg_sqlite_shim.o", "db2/db2_test_shim.o",
    "config_client.o", "config_client_contract.o",
    *[f"config_client_accessors_{index}.o" for index in range(8)],
    "tests/support/config_module_stub.o", "session_id.o", "yaml.o", "dstr.o",
    "aimee_home.o", "util.o", "text.o", "platform_random.o", "log.o",
    "posix/platform_ipc.o", "posix/platform_path.o", "posix/platform_process.o",
    "posix/platform_random.o", "posix/platform_net.o", "linux/platform_event.o",
    "linux/platform_ipc.o", "linux/platform_process.o", "linux/secret_store.o",
    "posix/util.o", "posix/pam_auth.o", "cJSON.o", "modules/vault/runtime_secret.o",
]


def run(command: list[str], **kwargs: Any) -> subprocess.CompletedProcess[str]:
    return subprocess.run(command, text=True, check=True, **kwargs)


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def git_output(*arguments: str, cwd: Path = ROOT) -> str:
    return run(["git", *arguments], cwd=cwd, capture_output=True).stdout.strip()


def validate_lineage(codex: Path, contract: dict[str, Any]) -> dict[str, str]:
    if platform.system() != "Linux" or platform.machine() != "x86_64":
        raise SystemExit("the frozen model executable digest is Linux x86_64 only")
    pin = contract["model_execution_pin"]
    observed_digest = sha256(codex)
    if observed_digest != pin["linux_x86_64_executable_sha256"]:
        raise SystemExit(f"Codex digest mismatch: {observed_digest}")
    version = run([str(codex), "--version"], capture_output=True).stdout.strip()
    if version != f"codex-cli {pin['client_version']}":
        raise SystemExit(f"Codex version mismatch: {version}")
    candidate = contract["candidate_commit_pin"]
    src_tree = git_output("rev-parse", "HEAD:src")
    if src_tree != candidate["runtime_src_tree"]:
        raise SystemExit(f"candidate src tree mismatch: {src_tree}")
    if subprocess.run(["git", "diff", "--quiet", "HEAD", "--", "src"], cwd=ROOT).returncode:
        raise SystemExit("candidate src contains tracked changes")
    untracked = git_output("ls-files", "--others", "--exclude-standard", "--", "src")
    if untracked:
        raise SystemExit("candidate src contains untracked files")
    login = run([str(codex), "login", "status"], capture_output=True)
    if "Logged in using ChatGPT" not in login.stdout + login.stderr:
        raise SystemExit("Codex is not authenticated with the frozen ChatGPT execution contract")
    return {
        "codex_version": version,
        "codex_sha256": observed_digest,
        "candidate_commit": candidate["commit"],
        "candidate_src_tree": src_tree,
        "runner_commit": git_output("rev-parse", "HEAD"),
    }


def validate_non_lsp_tools(args: argparse.Namespace, contract: dict[str, Any]) -> dict[str, str]:
    observed = {}
    for name, path in (("ripgrep", args.rg.resolve()), ("ast_grep", args.ast_grep.resolve())):
        pin = contract["non_lsp_execution_pins"][name]
        if not path.is_file() or sha256(path) != pin["linux_x86_64_executable_sha256"]:
            raise SystemExit(f"{name} executable does not match the frozen digest")
        version = run([str(path), "--version"], capture_output=True).stdout.splitlines()[0]
        if version != pin["version_output"]:
            raise SystemExit(f"{name} version mismatch: {version}")
        observed[f"{name}_version"] = version
        observed[f"{name}_sha256"] = sha256(path)
    return observed


def build_bridge(output_dir: Path) -> Path:
    run(["make", "-C", "src", "build/obj/tests/unit-test-lsp"], cwd=ROOT,
        stdout=subprocess.DEVNULL)
    obj_root = ROOT / "src" / "build" / "obj"
    missing = [item for item in LINK_OBJECTS if not (obj_root / item).is_file()]
    if missing:
        raise RuntimeError(f"candidate build did not produce bridge dependencies: {missing}")
    bridge_object = output_dir / "s1_lsp_bridge.o"
    bridge = output_dir / "s1-lsp-bridge"
    run([
        "cc", "-c", "-O2", "-Wall", "-Wextra", "-Werror",
        f"-I{ROOT / 'src'}", f"-I{ROOT / 'src' / 'headers'}",
        f"-I{ROOT / 'src' / 'vendor' / 'headers'}",
        f"-I{ROOT / 'src' / 'modules' / 'config'}",
        f"-I{ROOT / 'src' / 'modules' / 'lsp'}",
        str(BRIDGE_SOURCE), "-o", str(bridge_object),
    ], cwd=ROOT)
    objects = [str(obj_root / item) for item in LINK_OBJECTS]
    run([
        "cc", "-flto", "-o", str(bridge), str(bridge_object), *objects,
        "-lsqlite3", "-lpq", "-lm", "-lpthread", "-lzstd", "-lz",
        str(obj_root / "libaimee-core-connection.a"), "-lssl", "-lcrypto", "-lpam",
    ], cwd=ROOT)
    return bridge


def provider_config(task: dict[str, Any], args: argparse.Namespace) -> tuple[Path, str, str]:
    if task["provider"] == "gopls":
        return args.gopls.resolve(), "-", ".go"
    return args.pyright_langserver.resolve(), "--stdio", ".py"


def arm_plan(tasks: list[dict[str, Any]], seed: int) -> list[tuple[dict[str, Any], str]]:
    plan = []
    for task in tasks:
        arms = list(ARMS)
        random.Random(f"{seed}:{task['id']}").shuffle(arms)
        plan.extend((task, arm) for arm in arms)
    return plan


def prepare_checkout(task: dict[str, Any], destination: Path) -> None:
    run(["git", "clone", "--quiet", "--shared", "--no-checkout", str(ROOT), str(destination)])
    run(["git", "switch", "--quiet", "--create", "s1-cell", task["repository_commit"]],
        cwd=destination)
    if git_output("rev-parse", "HEAD", cwd=destination) != task["repository_commit"]:
        raise RuntimeError("cell checkout does not match the immutable task commit")
    setup = task.get("setup") or {}
    mutation = setup.get("saved_file_mutation")
    if mutation:
        path = destination / mutation["path"]
        if mutation["operation"] != "insert_before" or mutation["line"] != 1:
            raise RuntimeError("unrecognized checked mutation")
        path.write_text(mutation["text"] + path.read_text())


def cell_prompt(task: dict[str, Any]) -> str:
    anchors = (task.get("setup") or {}).get("post_edit_anchors", task["anchors"])
    supplied = {
        "task_id": task["id"],
        "authority": task["authority"],
        "anchors": anchors,
        "provider": task["provider"],
    }
    return (
        task["prompt"]
        + "\n\nSupplied checked task data:\n"
        + json.dumps(supplied, sort_keys=True, separators=(",", ":"))
        + "\nUse only the task MCP tools. Built-in Codex utility/resource tools are inert "
          "and are not eligible evidence. Return the required JSON envelope."
    )


def toml_string(value: str) -> str:
    return json.dumps(value)


def mcp_env_override(values: dict[str, str]) -> str:
    body = ",".join(f"{key}={toml_string(value)}" for key, value in values.items())
    return "mcp_servers.s1.env={" + body + "}"


def parse_events(path: Path) -> tuple[list[dict[str, Any]], dict[str, Any]]:
    events = []
    with path.open(encoding="utf-8") as stream:
        for number, line in enumerate(stream, 1):
            try:
                events.append(json.loads(line))
            except json.JSONDecodeError as error:
                raise RuntimeError(f"invalid Codex JSONL event {number}: {error}") from error
    terminal = [event for event in events if event.get("type") == "turn.completed"]
    if len(terminal) != 1 or not isinstance(terminal[0].get("usage"), dict):
        raise RuntimeError("cell does not have exactly one terminal usage event")
    return events, terminal[0]["usage"]


def grade(task: dict[str, Any], arm: str, answer: dict[str, Any],
          failure_active: bool = False) -> dict[str, Any]:
    expected = {
        (item["file"], item["line"], item["symbol"])
        for item in (task.get("setup") or {}).get("post_edit_targets", task["oracle"]["targets"])
    }
    observed = {
        (item.get("file"), item.get("line"), item.get("symbol"))
        for item in answer.get("targets", []) if isinstance(item, dict)
    }
    authority = answer.get("authority")
    authority_ok = isinstance(authority, str) and "local_checkout" in authority
    failure = task.get("failure_overlay") if failure_active and arm != "production" else None
    if failure:
        expected_status = failure["expected_status"]
        typed_failure_preserved = answer.get("answer_status") == expected_status and not observed
        task_success = typed_failure_preserved and authority_ok
        exact_target_correctness = None
        expected_behavior = "typed_failure"
    else:
        expected_status = "ok"
        typed_failure_preserved = None
        task_success = answer.get("answer_status") == "ok" and observed == expected and authority_ok
        exact_target_correctness = observed == expected
        expected_behavior = "exact_targets"
    return {
        "task_success": task_success,
        "expected_behavior": expected_behavior,
        "expected_status": expected_status,
        "typed_failure_preserved": typed_failure_preserved,
        "exact_target_correctness": exact_target_correctness,
        "authority_cited": authority_ok,
        "false_empty_count": int(answer.get("answer_status") == "empty"),
        "false_ok_empty_count": int(
            answer.get("answer_status") == "ok" and not observed and bool(expected)
        ),
        "stale_result_count": int(answer.get("answer_status") == "stale" and expected_status != "stale"),
        "false_current_results": int(bool(failure) and answer.get("answer_status") == "ok"),
        "expected_targets": sorted(expected),
        "observed_targets": sorted(observed),
    }


def run_cell(task: dict[str, Any], arm: str, args: argparse.Namespace, bridge: Path,
             lineage: dict[str, str], run_id: str, ordinal: int,
             failure_active: bool = False) -> dict[str, Any]:
    cell_root = Path(tempfile.mkdtemp(prefix=f"aimee-s1-{task['id']}-{arm}-"))
    workspace = cell_root / "work"
    raw_events = args.output / "raw" / f"{ordinal:03d}-{task['id']}-{arm}.jsonl"
    tool_log = args.output / "tools" / f"{ordinal:03d}-{task['id']}-{arm}.jsonl"
    last_message = cell_root / "last-message.json"
    try:
        prepare_checkout(task, workspace)
        provider, provider_arg, extension = provider_config(task, args)
        if not provider.is_file():
            raise RuntimeError(f"pinned provider is missing: {provider}")
        env_values = {
            "S1_WORKSPACE": str(workspace), "S1_ARM": arm, "S1_TOOL_LOG": str(tool_log),
            "S1_BRIDGE": str(bridge), "S1_PROVIDER_COMMAND": str(provider),
            "S1_PROVIDER_ARG": provider_arg, "S1_PROVIDER_EXTENSION": extension,
            "S1_RG": str(args.rg.resolve()), "S1_AST_GREP": str(args.ast_grep.resolve()),
        }
        failure = (task.get("failure_overlay") or {}) if failure_active else {}
        if failure:
            env_values["S1_FAILURE_INJECTION"] = failure["injection"]
            env_values["S1_FAILURE_STATUS"] = failure["expected_status"]
        command = [
            str(args.codex), "exec", "--ephemeral", "--json", "--ignore-user-config",
            "--ignore-rules", "--model", args.model, "--sandbox", "read-only",
            "--output-schema", str(RESULT_SCHEMA), "--output-last-message", str(last_message),
            "-C", str(workspace), "-c", f"model_reasoning_effort={toml_string(args.reasoning)}",
            "-c", f"model_instructions_file={toml_string(str(SYSTEM_PROMPT))}",
            "-c", f"mcp_servers.s1.command={toml_string('python3')}",
            "-c", "mcp_servers.s1.args=[" + toml_string(str(MCP_SERVER)) + "]",
            "-c", mcp_env_override(env_values),
            "-c", "mcp_servers.s1.default_tools_approval_mode=\"approve\"",
            "--disable", "shell_tool", "--disable", "view_image", "--disable", "multi_agent",
            "--disable", "multi_agent_v2", "--disable", "apps", "--disable", "browser_use",
            "--disable", "computer_use", "--disable", "skill_search", "--disable", "plugins",
            "--disable", "memories", "--disable", "token_budget", "--disable", "tool_suggest",
            cell_prompt(task),
        ]
        started = time.monotonic()
        completed = subprocess.run(command, text=True, capture_output=True, timeout=args.timeout)
        wall_seconds = time.monotonic() - started
        raw_events.write_text(completed.stdout)
        if completed.returncode:
            return {
                "task_id": task["id"], "arm": arm, "ordinal": ordinal,
                "infrastructure_failure": True, "exit_code": completed.returncode,
                "stderr": completed.stderr[:4000], "wall_seconds": wall_seconds, **lineage,
            }
        events, usage = parse_events(raw_events)
        answer = json.loads(last_message.read_text())
        calls = [json.loads(line) for line in tool_log.read_text().splitlines()] if tool_log.exists() else []
        completed_tool_items = [
            event.get("item") or {} for event in events
            if event.get("type") == "item.completed"
            and (event.get("item") or {}).get("type") not in ("agent_message", "reasoning")
        ]
        cell_eligible = len(completed_tool_items) == len(calls)
        decisive = answer.get("decisive_evidence") or {}
        eligible_tool_names = {item["tool"] for item in calls}
        decisive_matches_log = any(
            item["tool"] == "lsp_context"
            and decisive.get("timestamp_ms") == (item.get("result") or {}).get("observed_at_monotonic_ms")
            for item in calls
        )
        cell_grade = grade(task, arm, answer, failure_active)
        cell_grade["task_success"] = cell_grade["task_success"] and cell_eligible
        lsp_calls = [item for item in calls if item["tool"].startswith("lsp_")]
        tool_input_bytes = sum(len(json.dumps(item.get("arguments"), sort_keys=True,
                                                    separators=(",", ":")).encode())
                               for item in calls)
        tool_output_bytes = sum(len(json.dumps(item.get("result"), sort_keys=True,
                                                     separators=(",", ":")).encode())
                                for item in calls)
        tool_latencies_ms = [
            item["completed_at_monotonic_ms"] - item["started_at_monotonic_ms"]
            for item in calls
        ]
        lsp_latencies_ms = [
            item["completed_at_monotonic_ms"] - item["started_at_monotonic_ms"]
            for item in lsp_calls
        ]
        tool_schema_bytes = len(json.dumps(
            TOOL_SCHEMAS["non_lsp_tools"] + TOOL_SCHEMAS["arm_lsp_tools"][arm],
            sort_keys=True, separators=(",", ":"),
        ).encode())
        preparation_bytes = (
            len(cell_prompt(task).encode()) + len(SYSTEM_PROMPT.read_bytes())
            + len(RESULT_SCHEMA.read_bytes()) + tool_schema_bytes
        )
        return {
            "run_id": run_id, "task_id": task["id"], "family": task["family"], "arm": arm,
            "ordinal": ordinal, "semantic_eligible": task["semantic_eligible"],
            "infrastructure_failure": False, "cell_eligible": cell_eligible,
            "ineligible_completed_tool_items": [] if cell_eligible else completed_tool_items,
            "answer": answer, "grade": cell_grade,
            "usage": usage, "wall_seconds": wall_seconds, "agent_turns": 1,
            "tool_calls": len(calls), "tool_log": str(tool_log), "raw_events": str(raw_events),
            "measurement": {
                "preparation_bytes": preparation_bytes,
                "tool_schema_bytes": tool_schema_bytes,
                "tool_input_bytes": tool_input_bytes,
                "tool_output_bytes": tool_output_bytes,
                "tool_latency_ms": tool_latencies_ms,
                "provider_cold_latency_ms": lsp_latencies_ms[0] if lsp_latencies_ms else None,
                "provider_warm_latency_ms": lsp_latencies_ms[1:],
                "provider_setup_success": (
                    None if failure_active or not lsp_calls else all(
                        (item.get("result") or {}).get("status") != "unavailable"
                        for item in lsp_calls
                    )
                ),
                "provider_process_count": None,
                "provider_peak_rss_kib": None,
                "reference_recall": None,
                "reference_false_positive_rate": None,
            },
            "candidate_used_before_decisive_edit": (
                arm == "batched_context" and "lsp_context" in eligible_tool_names
                and str(decisive.get("tool", "")).endswith("lsp_context") and decisive_matches_log
            ),
            "stderr": completed.stderr[:4000], **lineage,
        }
    finally:
        shutil.rmtree(cell_root)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--codex", required=True, type=Path)
    parser.add_argument("--gopls", required=True, type=Path)
    parser.add_argument("--pyright-langserver", required=True, type=Path)
    parser.add_argument("--rg", required=True, type=Path)
    parser.add_argument("--ast-grep", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--task", action="append")
    parser.add_argument("--arm", choices=ARMS, action="append")
    parser.add_argument("--max-cells", type=int)
    parser.add_argument("--timeout", type=int, default=600)
    parser.add_argument("--model", default="gpt-5.6-sol")
    parser.add_argument("--reasoning", default="medium")
    parser.add_argument("--execute", action="store_true")
    parser.add_argument(
        "--failure-suite", action="store_true",
        help="run the 12 LSP-only adversarial overlay cells instead of the paired value study",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    contract = json.loads(CONTRACT.read_text())
    manifest = json.loads(MANIFEST.read_text())
    pin = contract["model_execution_pin"]
    if args.model != pin["model_identifier"] or args.reasoning != pin["reasoning"]:
        raise SystemExit("model and reasoning must match the frozen contract")
    lineage = validate_lineage(args.codex.resolve(), contract)
    lineage.update(validate_non_lsp_tools(args, contract))
    selected = manifest["tasks"]
    if args.failure_suite:
        selected = [task for task in selected if task.get("failure_overlay")]
    if args.task:
        wanted = set(args.task)
        selected = [task for task in selected if task["id"] in wanted]
        if {task["id"] for task in selected} != wanted:
            raise SystemExit("unknown task id selected")
    plan = arm_plan(selected, contract["run_order"]["seed"])
    if args.failure_suite:
        plan = [cell for cell in plan if cell[1] != "production"]
    if args.arm:
        wanted_arms = set(args.arm)
        plan = [cell for cell in plan if cell[1] in wanted_arms]
    if args.max_cells is not None:
        if args.max_cells < 1:
            raise SystemExit("--max-cells must be positive")
        plan = plan[:args.max_cells]
    preflight = {
        "schema_version": 1, "created_at": datetime.now(timezone.utc).isoformat(),
        "cells": [{"task_id": task["id"], "arm": arm} for task, arm in plan],
        "cell_count": len(plan), "seed": contract["run_order"]["seed"], **lineage,
        "study_kind": "adversarial_failure" if args.failure_suite else "paired_value",
    }
    print(json.dumps({"preflight": preflight}, indent=2), flush=True)
    if not args.execute:
        return 0
    args.output.mkdir(parents=True, exist_ok=True)
    (args.output / "raw").mkdir(exist_ok=True)
    (args.output / "tools").mkdir(exist_ok=True)
    (args.output / "preflight.json").write_text(json.dumps(preflight, indent=2, sort_keys=True) + "\n")
    run_id = "live-semantic-s1-" + uuid.uuid4().hex[:16]
    rows = []
    with tempfile.TemporaryDirectory(prefix="aimee-s1-bridge-") as bridge_dir:
        bridge = build_bridge(Path(bridge_dir))
        for ordinal, (task, arm) in enumerate(plan, 1):
            row = run_cell(
                task, arm, args, bridge, lineage, run_id, ordinal,
                failure_active=args.failure_suite,
            )
            rows.append(row)
            print(json.dumps({"completed": ordinal, "task": task["id"], "arm": arm,
                              "success": (row.get("grade") or {}).get("task_success"),
                              "infrastructure_failure": row["infrastructure_failure"]}), flush=True)
    artifact = {
        "schema_version": 1,
        "study_kind": "adversarial_failure" if args.failure_suite else "paired_value",
        "claim_status": "complete" if len(plan) == (12 if args.failure_suite else 135)
        else "calibration_only",
        "created_at": datetime.now(timezone.utc).isoformat(), "run_id": run_id,
        "contract_sha256": sha256(CONTRACT), "manifest_sha256": sha256(MANIFEST),
        "cells": rows, "lineage": lineage,
    }
    (args.output / "results.json").write_text(json.dumps(artifact, indent=2, sort_keys=True) + "\n")
    return 1 if any(row["infrastructure_failure"] for row in rows) else 0


if __name__ == "__main__":
    raise SystemExit(main())
