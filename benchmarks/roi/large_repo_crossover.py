#!/usr/bin/env python3
"""Paired full-repository agent runs for Aimee context-capacity evidence.

The control and treatment use the same model, task, checkout, tools, limits,
and hidden grader. The treatment alone passes each accumulated canonical
history through the production Go economizer handler. Generated artifacts keep
every provider usage object, tool result, reduction decision, patch, and grade.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import random
import shlex
import shutil
import subprocess
import tempfile
import time
import urllib.error
import urllib.request
import uuid
from dataclasses import dataclass, field
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

from benchmarks.roi.current_stack_pilot import EconomizerProbe, build_probe, usage_buckets


SEED = 20260827
PROGRESS_INITIAL_CALLS = 12
PROGRESS_FOLLOWUP_CALLS = 8
PROGRESS_DUPLICATE_HITS = 2
PROGRESS_WINDOW = 12
SYSTEM_PROMPT = """You are a coding agent working in a large repository at a known buggy revision.
Diagnose and implement the user's complete request. Inspect the repository rather than guessing.
Use the provided tools, preserve existing contracts, add regression tests when appropriate, and
run focused verification. Do not reveal or search git history for the later fix. Call submit only
when the work is ready for an independent hidden grader. Tool output can be recovered exactly by
reference with tool_output_get."""

TOOLS = [
    {"type": "function", "function": {"name": "search", "description": "Search tracked workspace text with ripgrep.", "parameters": {"type": "object", "properties": {"query": {"type": "string"}, "path": {"type": "string", "default": "."}}, "required": ["query"]}}},
    {"type": "function", "function": {"name": "read_file", "description": "Read a line range from a workspace file.", "parameters": {"type": "object", "properties": {"path": {"type": "string"}, "start": {"type": "integer", "minimum": 1}, "end": {"type": "integer", "minimum": 1}}, "required": ["path"]}}},
    {"type": "function", "function": {"name": "run", "description": "Run a bounded build, test, search, or git inspection command in the workspace.", "parameters": {"type": "object", "properties": {"command": {"type": "string"}, "timeout_seconds": {"type": "integer", "minimum": 1, "maximum": 900}}, "required": ["command"]}}},
    {"type": "function", "function": {"name": "apply_patch", "description": "Apply a unified patch to the workspace.", "parameters": {"type": "object", "properties": {"patch": {"type": "string"}}, "required": ["patch"]}}},
    {"type": "function", "function": {"name": "git_diff", "description": "Inspect the current workspace diff and status.", "parameters": {"type": "object", "properties": {}}}},
    {"type": "function", "function": {"name": "tool_output_get", "description": "Recover the exact full output of an earlier tool result by its ref.", "parameters": {"type": "object", "properties": {"ref": {"type": "string"}}, "required": ["ref"]}}},
    {"type": "function", "function": {"name": "submit", "description": "Finish the task and hand the workspace to the hidden grader.", "parameters": {"type": "object", "properties": {"summary": {"type": "string"}}, "required": ["summary"]}}},
]

ALLOWED_COMMANDS = {
    "cc", "clang", "cmake", "go", "make", "ninja", "python3", "rg",
    "git", "sed", "head", "tail", "wc", "find", "ls", "test",
}
FORBIDDEN_COMMAND_TEXT = (
    "rm -rf", "git reset", "git checkout", "git clean", "sudo ", "ssh ",
    "scp ", "curl ", "wget ", "mkfs", "shutdown", "reboot", "> /",
    "git log", "git show", "git branch", "git reflog", "git rev-list",
)


@dataclass(frozen=True)
class Task:
    task_id: str
    fix_commit: str
    prompt: str
    languages: list[str]
    selection_stratum: str
    hidden_test_files: list[str]
    grader_commands: list[str]
    setup_commands: list[str] = field(default_factory=list)
    setup_artifact_paths: list[str] = field(default_factory=list)


class ProviderError(RuntimeError):
    def __init__(self, message: str, *, context_limit: bool = False):
        super().__init__(message)
        self.context_limit = context_limit


class ProgressController:
    """Experiment mirror of the production write-role progress guard."""

    def __init__(self) -> None:
        self.window: list[tuple[str, str, int | None, int | None]] = []
        self.calls_since_mutation = 0
        self.calls_since_checkpoint = 0
        self.duplicate_hits = 0
        self.checkpoints = 0
        self.successful_mutations = 0

    @staticmethod
    def retrieval(name: str, args: dict[str, Any]) -> tuple[str, str, int | None, int | None]:
        if name == "read_file":
            start = max(1, int(args.get("start") or 1))
            end = max(start, min(start + 799, int(args.get("end") or start + 399)))
            return name, str(args.get("path") or ""), start, end
        return name, json.dumps(args, sort_keys=True, separators=(",", ":")), None, None

    @staticmethod
    def overlaps(left: tuple[str, str, int | None, int | None],
                 right: tuple[str, str, int | None, int | None]) -> bool:
        if left[:2] != right[:2]:
            return False
        if left[2] is None or right[2] is None:
            return True
        return right[2] <= left[3] and left[2] <= right[3]

    def observe(self, name: str, args: dict[str, Any], *, usable: bool,
                mutation: bool) -> dict[str, Any]:
        if not usable:
            return self.event("none")
        if mutation:
            mutations = self.successful_mutations + 1
            self.__init__()
            self.successful_mutations = mutations
            return self.event("mutation_reset")

        self.calls_since_mutation += 1
        if self.checkpoints:
            self.calls_since_checkpoint += 1
        current = self.retrieval(name, args)
        if any(self.overlaps(prior, current) for prior in self.window):
            self.duplicate_hits += 1
        self.window.append(current)
        self.window = self.window[-PROGRESS_WINDOW:]

        action = "none"
        if self.checkpoints == 0 and (
            self.calls_since_mutation >= PROGRESS_INITIAL_CALLS
            or self.duplicate_hits >= PROGRESS_DUPLICATE_HITS
        ):
            self.checkpoints = 1
            self.calls_since_checkpoint = 0
            action = "checkpoint"
        elif self.checkpoints and self.calls_since_checkpoint >= PROGRESS_FOLLOWUP_CALLS:
            self.checkpoints += 1
            self.calls_since_checkpoint = 0
            action = "abort" if self.checkpoints >= 3 else "escalate"
        return self.event(action)

    def event(self, action: str) -> dict[str, Any]:
        return {
            "action": action,
            "calls_since_mutation": self.calls_since_mutation,
            "calls_since_checkpoint": self.calls_since_checkpoint,
            "duplicate_hits": self.duplicate_hits,
            "checkpoints": self.checkpoints,
            "successful_mutations": self.successful_mutations,
        }

    def hint(self, action: str) -> str:
        if action == "escalate":
            return (
                f"No-progress escalation: this write-capable task has completed "
                f"{self.calls_since_mutation} successful tool calls without an edit "
                f"({self.duplicate_hits} repeated or overlapping retrievals). You must now make "
                "the smallest justified edit and run focused verification, or return a specific "
                "blocker. Do not spend more calls on broad repository exploration or reread "
                "ranges already present in the transcript."
            )
        return (
            f"Progress checkpoint: this write-capable task has completed "
            f"{self.calls_since_mutation} successful tool calls without an edit "
            f"({self.duplicate_hits} repeated or overlapping retrievals). Stop gathering broadly. "
            "State a concrete defect hypothesis, then make the smallest justified edit or run "
            "the decisive test. If blocked, return the blocker explicitly. Reuse earlier results "
            "instead of rereading them."
        )


def tool_result_usable(name: str, content: str) -> bool:
    if content.startswith(("tool error:", "unknown tool:", "read_file: no such", "run: command refused")):
        return False
    if "[exit_code=" in content and "[exit_code=0" not in content:
        return False
    return name != "submit"


def sha256_text(value: str) -> str:
    return hashlib.sha256(value.encode()).hexdigest()


def sha256_json(value: Any) -> str:
    return sha256_text(json.dumps(value, sort_keys=True, separators=(",", ":")))


def load_tasks(path: Path, selected: list[str]) -> tuple[dict[str, Any], list[Task]]:
    manifest = json.loads(path.read_text())
    tasks = {row["task_id"]: Task(**row) for row in manifest["tasks"]}
    missing = sorted(set(selected) - set(tasks))
    if missing:
        raise SystemExit(f"unknown tasks: {', '.join(missing)}")
    return manifest, [tasks[name] for name in selected]


def http_json(url: str, payload: dict[str, Any] | None = None, timeout: int = 900) -> dict[str, Any]:
    data = None if payload is None else json.dumps(payload, separators=(",", ":")).encode()
    request = urllib.request.Request(
        url, data=data, headers={"Content-Type": "application/json"},
        method="GET" if data is None else "POST",
    )
    try:
        with urllib.request.urlopen(request, timeout=timeout) as response:
            return json.load(response)
    except urllib.error.HTTPError as error:
        detail = error.read().decode(errors="replace")
        lowered = detail.lower()
        is_context = any(word in lowered for word in (
            "context window", "context length", "too many tokens", "exceeds the available context",
            "exceed context", "prompt is too long", "requested tokens", "maximum context",
        ))
        raise ProviderError(f"HTTP {error.code}: {detail[:4000]}", context_limit=is_context) from error
    except (urllib.error.URLError, TimeoutError) as error:
        raise ProviderError(f"provider transport failed: {error}") from error


def resolve_workspace_path(root: Path, raw: str) -> Path:
    path = (root / raw).resolve()
    if path != root and root not in path.parents:
        raise ValueError("path escapes workspace")
    return path


def bounded_output(text: str, limit: int) -> tuple[str, bool]:
    if len(text.encode()) <= limit:
        return text, False
    half = max(1, limit // 2)
    head = text.encode()[:half].decode(errors="replace")
    tail = text.encode()[-half:].decode(errors="replace")
    return head + "\n[... middle omitted from visible result; recover by ref ...]\n" + tail, True


def command_allowed(command: str) -> bool:
    lowered = command.lower()
    if any(item in lowered for item in FORBIDDEN_COMMAND_TEXT) or "\n" in command:
        return False
    if "`" in command or "$(" in command:
        return False
    try:
        lexer = shlex.shlex(command, posix=True, punctuation_chars=";&|")
        lexer.whitespace_split = True
        words = list(lexer)
    except ValueError:
        return False
    if not words:
        return False
    command_heads = [words[0]]
    for index, word in enumerate(words[:-1]):
        if word in {";", "&&", "||", "|"}:
            command_heads.append(words[index + 1])
    return all(
        Path(head).name in ALLOWED_COMMANDS or Path(head).name.startswith("unit-test-")
        for head in command_heads
    )


def run_process(command: list[str] | str, cwd: Path, timeout: int, *, shell: bool = False,
                stdin: str | None = None) -> dict[str, Any]:
    started = time.monotonic()
    try:
        proc = subprocess.run(
            command, cwd=cwd, input=stdin, capture_output=True, text=True,
            timeout=timeout, shell=shell,
        )
        return {
            "exit_code": proc.returncode,
            "stdout": proc.stdout,
            "stderr": proc.stderr,
            "wall_seconds": time.monotonic() - started,
            "timed_out": False,
        }
    except subprocess.TimeoutExpired as error:
        stdout = error.stdout.decode(errors="replace") if isinstance(error.stdout, bytes) else (error.stdout or "")
        stderr = error.stderr.decode(errors="replace") if isinstance(error.stderr, bytes) else (error.stderr or "")
        return {
            "exit_code": 124, "stdout": stdout, "stderr": stderr,
            "wall_seconds": time.monotonic() - started, "timed_out": True,
        }


def execute_tool(name: str, args: dict[str, Any], root: Path,
                 outputs: dict[str, str], output_limit: int) -> tuple[str, bool]:
    if name == "tool_output_get":
        ref = str(args.get("ref", ""))
        return outputs.get(ref, f"unknown or expired tool output ref: {ref}"), False
    if name == "read_file":
        path = resolve_workspace_path(root, str(args.get("path", "")))
        if not path.is_file():
            raw = f"read_file: no such file: {path.relative_to(root) if path != root else '.'}"
        else:
            start = max(1, int(args.get("start") or 1))
            end = max(start, min(start + 799, int(args.get("end") or start + 399)))
            lines = path.read_text(errors="replace").splitlines()
            raw = "\n".join(f"{number}:{lines[number - 1]}" for number in range(start, min(end, len(lines)) + 1))
    elif name == "search":
        query = str(args.get("query", ""))
        path = resolve_workspace_path(root, str(args.get("path") or "."))
        result = run_process(["rg", "-n", "--hidden", "--glob", "!.git/**", "--", query, str(path)], root, 90)
        raw = result["stdout"] + result["stderr"] + f"\n[exit_code={result['exit_code']}]"
    elif name == "run":
        command = str(args.get("command", ""))
        if not command_allowed(command):
            raw = "run: command refused by benchmark allowlist"
        else:
            timeout = max(1, min(900, int(args.get("timeout_seconds") or 300)))
            result = run_process(command, root, timeout, shell=True)
            raw = result["stdout"] + result["stderr"] + (
                f"\n[exit_code={result['exit_code']} wall_seconds={result['wall_seconds']:.3f} timed_out={str(result['timed_out']).lower()}]"
            )
    elif name == "apply_patch":
        patch = str(args.get("patch", ""))
        result = run_process(["git", "apply", "--whitespace=nowarn", "-"], root, 90, stdin=patch)
        raw = result["stdout"] + result["stderr"] + f"\n[exit_code={result['exit_code']}]"
    elif name == "git_diff":
        status = run_process(["git", "status", "--short"], root, 30)
        diff = run_process(["git", "diff", "--no-ext-diff"], root, 60)
        raw = status["stdout"] + diff["stdout"] + diff["stderr"]
    elif name == "submit":
        raw = "submission accepted; hidden grading begins after the turn"
    else:
        raw = f"unknown tool: {name}"

    ref = "out_" + sha256_text(raw)[:20]
    outputs[ref] = raw
    visible, truncated = bounded_output(raw, output_limit)
    suffix = f"\n[tool_output_ref={ref} full_bytes={len(raw.encode())}]"
    return visible + suffix, truncated


def economize(probe: EconomizerProbe, messages: list[dict[str, Any]], state_key: str,
              system_prompt: str = SYSTEM_PROMPT) -> dict[str, Any]:
    request = {
        "messages": messages,
        "system_prompt": system_prompt,
        "seam": "delegate",
        "state_key": state_key,
        "history_fold": True,
        "compress": True,
        "retained_msgs": 10,
        "min_fold_msgs": 4,
        "excerpt_bytes": 768,
        "register_enabled": True,
        "compact_head_bytes": 1024,
        "compact_tail_bytes": 1024,
        "closet_enabled": True,
        "closet_budget_bytes": 8192,
        "closet_max_ratio_pct": 35,
        "recall_enabled": True,
        "recall_ttl_turns": 40,
        "recall_inject": True,
        "freeze_guard_enabled": True,
        "freeze_guard_horizon": 8,
    }
    return probe.reduce_request(request)


def learned_failure_context(artifact: dict[str, Any], task_id: str) -> tuple[str, dict[str, Any]]:
    """Render the same task-scoped no-progress lesson the product injects on retry."""
    candidates = [
        cell for cell in artifact.get("cells", [])
        if cell.get("task_id") == task_id
        and cell.get("condition") == "aimee_progress"
        and cell.get("terminal_reason") == "progress_abort"
    ]
    if len(candidates) != 1:
        raise ValueError(
            f"expected one sealed aimee_progress/progress_abort cell for {task_id}, "
            f"found {len(candidates)}"
        )
    cell = candidates[0]
    progress = cell.get("progress_controller") or {}
    calls = int(progress.get("calls_since_mutation") or 0)
    duplicates = int(progress.get("duplicate_hits") or 0)
    if calls <= 0:
        raise ValueError("sealed failure has no successful retrieval count")
    failure = (
        f"no-progress circuit breaker tripped after {calls} successful calls without an edit "
        f"({duplicates} repeated or overlapping retrievals)"
    )
    approach = (
        "broad repository exploration with repeated or overlapping retrievals and no edit"
    )
    block = (
        "<prior_failure_learning>\n"
        "This is durable evidence from earlier attempts at a sufficiently similar goal.\n"
        "Approaches already tried for a goal like this, and how they went:\n"
        f"- {approach} -> {failure} (seen 1 time)\n"
        "Choose a materially different plan before using tools. When the prior approach was a "
        "no-progress retrieval loop, form a concrete defect hypothesis and attempt the smallest "
        "justified edit or decisive test before broadening exploration.\n"
        "</prior_failure_learning>"
    )
    return block, {
        "source_run_id": cell.get("run_id"),
        "source_terminal_reason": cell.get("terminal_reason"),
        "approach": approach,
        "failure": failure,
        "calls_without_mutation": calls,
        "duplicate_hits": duplicates,
        "context_sha256": sha256_text(block),
    }


def call_provider(base_url: str, model: str, messages: list[dict[str, Any]],
                  max_output_tokens: int, seed: int) -> tuple[dict[str, Any], float]:
    payload = {
        "model": model,
        "messages": messages,
        "tools": TOOLS,
        "tool_choice": "auto",
        "temperature": 0,
        "seed": seed,
        "max_tokens": max_output_tokens,
        "chat_template_kwargs": {"enable_thinking": False},
    }
    started = time.monotonic()
    response = http_json(base_url.rstrip("/") + "/v1/chat/completions", payload)
    return response, time.monotonic() - started


def add_worktree(repo: Path, target: Path, revision: str) -> None:
    subprocess.run(["git", "worktree", "add", "--detach", str(target), revision], cwd=repo, check=True,
                   stdout=subprocess.DEVNULL)


def remove_worktree(repo: Path, target: Path) -> None:
    subprocess.run(["git", "worktree", "remove", "--force", str(target)], cwd=repo, check=False,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def prepare_task_workspace(task: Task, root: Path) -> list[dict[str, Any]]:
    records = []
    for command in task.setup_commands:
        result = run_process(command, root, 1200, shell=True)
        records.append({"command": command, **result})
        if result["exit_code"]:
            raise RuntimeError(f"task setup failed: {command}\n{result['output']}")
    return records


def prepare_diff(root: Path, excluded_paths: list[str] | None = None) -> None:
    """Make untracked files visible to diff without staging their contents."""
    pathspecs = [".", *[f":(exclude){path}" for path in (excluded_paths or [])]]
    subprocess.run(["git", "add", "--intent-to-add", "--", *pathspecs], cwd=root, check=True,
                   stdout=subprocess.DEVNULL)


def is_test_path(name: str) -> bool:
    parts = Path(name).parts
    basename = Path(name).name.lower()
    return (
        any(part.lower() in {"test", "tests"} for part in parts)
        or basename.startswith("test_")
        or basename.endswith(("_test.go", ".test.js", ".test.ts", ".spec.js", ".spec.ts"))
    )


def diff_metrics(root: Path) -> dict[str, Any]:
    names = subprocess.check_output(["git", "diff", "HEAD", "--name-only"], cwd=root, text=True).splitlines()
    numstat = subprocess.check_output(["git", "diff", "HEAD", "--numstat"], cwd=root, text=True).splitlines()
    added = deleted = 0
    for row in numstat:
        fields = row.split("\t")
        if len(fields) >= 2 and fields[0].isdigit() and fields[1].isdigit():
            added += int(fields[0]); deleted += int(fields[1])
    test_files = [name for name in names if is_test_path(name)]
    return {"files": names, "test_files": test_files, "added": added, "deleted": deleted}


def grade(task: Task, root: Path) -> dict[str, Any]:
    before_hidden = []
    for command in task.grader_commands:
        result = run_process(command, root, 1200, shell=True)
        before_hidden.append({"command": command, **result})
        if result["exit_code"]:
            break
    for path in task.hidden_test_files:
        subprocess.run(["git", "restore", f"--source={task.fix_commit}", "--", path], cwd=root, check=True)
        os.utime(root / path, None)
    hidden = []
    for command in task.grader_commands:
        result = run_process(command, root, 1200, shell=True)
        hidden.append({"command": command, **result})
        if result["exit_code"]:
            break
    return {
        "visible_commands_passed": bool(before_hidden) and all(row["exit_code"] == 0 for row in before_hidden),
        "hidden_passed": bool(hidden) and all(row["exit_code"] == 0 for row in hidden),
        "visible_commands": before_hidden,
        "hidden_commands": hidden,
    }


def authored_test_sensitivity(repo: Path, task: Task, parent: str, test_patch: str,
                              candidate_visible_passed: bool, worktree_root: Path) -> dict[str, Any]:
    """Check whether authored tests pass on the patch and fail on the buggy parent."""
    if not test_patch.strip():
        return {
            "status": "no_authored_test_patch", "candidate_passed": candidate_visible_passed,
            "buggy_parent_failed": None, "regression_sensitive": False, "commands": [],
        }
    workspace = worktree_root / f"test-sensitivity-{uuid.uuid4().hex[:16]}"
    add_worktree(repo, workspace, parent)
    try:
        setup = prepare_task_workspace(task, workspace)
        applied = run_process(["git", "apply", "--whitespace=nowarn", "-"], workspace, 90,
                              stdin=test_patch)
        if applied["exit_code"]:
            return {
                "status": "test_patch_apply_failed", "candidate_passed": candidate_visible_passed,
                "buggy_parent_failed": None, "regression_sensitive": False,
                "setup": setup, "apply": applied, "commands": [],
            }
        commands = []
        for command in task.grader_commands:
            result = run_process(command, workspace, 1200, shell=True)
            commands.append({"command": command, **result})
            if result["exit_code"]:
                break
        parent_failed = bool(commands) and any(row["exit_code"] != 0 for row in commands)
        return {
            "status": "measured", "candidate_passed": candidate_visible_passed,
            "buggy_parent_failed": parent_failed,
            "regression_sensitive": candidate_visible_passed and parent_failed,
            "setup": setup, "commands": commands,
        }
    finally:
        remove_worktree(repo, workspace)


def run_cell(repo: Path, task: Task, condition: str, repeat: int, base_url: str, model: str,
             max_turns: int, max_output_tokens: int, output_limit: int,
             probe: EconomizerProbe, worktree_root: Path,
             learned_contexts: dict[str, tuple[str, dict[str, Any]]] | None = None) -> dict[str, Any]:
    run_id = f"large-repo-{uuid.uuid4().hex[:16]}"
    workspace = worktree_root / run_id
    parent = task.fix_commit + "^"
    add_worktree(repo, workspace, parent)
    try:
        setup = prepare_task_workspace(task, workspace)
    except Exception:
        remove_worktree(repo, workspace)
        raise
    messages: list[dict[str, Any]] = [{"role": "user", "content": task.prompt}]
    learned_context, learned_evidence = (learned_contexts or {}).get(task.task_id, ("", {}))
    if condition == "aimee_learned_retry" and not learned_context:
        raise ValueError(f"no sealed prior-failure learning for {task.task_id}")
    system_prompt = SYSTEM_PROMPT
    if condition == "aimee_learned_retry":
        system_prompt = SYSTEM_PROMPT + "\n\n" + learned_context
    outputs: dict[str, str] = {}
    progress = ProgressController()
    progress_enabled = condition in {"aimee_progress", "aimee_learned_retry"}
    turns = []
    terminal_reason = "max_turns"
    submitted = False
    started = time.monotonic()
    try:
        for turn in range(max_turns):
            activation: dict[str, Any]
            view = messages
            if condition in {"aimee", "aimee_progress", "aimee_learned_retry"}:
                activation = economize(probe, messages, run_id, system_prompt)
                if activation.get("mutated"):
                    view = activation["messages"]
            else:
                activation = {"mutated": False, "reason": "off", "byte_identical": True}
            provider_messages = [{"role": "system", "content": system_prompt}, *view]
            request_record = {
                "turn": turn + 1,
                "canonical_message_count": len(messages),
                "provider_message_count": len(provider_messages),
                "canonical_bytes": len(json.dumps(messages, separators=(",", ":")).encode()),
                "provider_bytes": len(json.dumps(provider_messages, separators=(",", ":")).encode()),
                "request_sha256": sha256_json(provider_messages),
                "economizer": activation,
                "progress_controller_before": progress.event("none") if progress_enabled else None,
            }
            try:
                response, wall = call_provider(
                    base_url, model, provider_messages, max_output_tokens,
                    SEED + repeat * 1000 + turn,
                )
            except ProviderError as error:
                request_record.update({
                    "provider_error": str(error), "context_limit": error.context_limit,
                })
                turns.append(request_record)
                terminal_reason = "context_limit" if error.context_limit else "provider_error"
                break
            choices = response.get("choices") or []
            if len(choices) != 1 or not isinstance(choices[0].get("message"), dict):
                request_record["provider_error"] = "provider returned no unique assistant message"
                turns.append(request_record)
                terminal_reason = "malformed_response"
                break
            assistant = choices[0]["message"]
            usage = usage_buckets(response)
            request_record.update({
                "provider_response_id": response.get("id"), "usage": usage,
                "raw_usage": response.get("usage"), "wall_seconds": wall,
                "finish_reason": choices[0].get("finish_reason"),
            })
            messages.append(assistant)
            tool_calls = assistant.get("tool_calls") or []
            tool_records = []
            progress_events = []
            if not tool_calls:
                turns.append(request_record)
                terminal_reason = "assistant_final"
                break
            for call in tool_calls:
                function = call.get("function") or {}
                name = str(function.get("name") or "")
                try:
                    arguments = json.loads(function.get("arguments") or "{}")
                    if not isinstance(arguments, dict):
                        raise ValueError("tool arguments are not an object")
                    content, truncated = execute_tool(name, arguments, workspace, outputs, output_limit)
                except Exception as error:  # tool errors are model-visible outcomes
                    arguments = {}
                    content, truncated = f"tool error: {error}", False
                messages.append({"role": "tool", "tool_call_id": call.get("id", ""), "content": content})
                tool_records.append({
                    "tool_call_id": call.get("id"), "name": name, "arguments": arguments,
                    "result_bytes": len(content.encode()), "visible_truncated": truncated,
                })
                if progress_enabled:
                    event = progress.observe(
                        name, arguments, usable=tool_result_usable(name, content),
                        mutation=name == "apply_patch",
                    )
                    progress_events.append(event)
                if name == "submit":
                    submitted = True
            request_record["tool_calls"] = tool_records
            request_record["progress_events"] = progress_events
            turns.append(request_record)
            if submitted:
                terminal_reason = "submitted"
                break
            if progress_enabled and any(row["action"] == "abort" for row in progress_events):
                terminal_reason = "progress_abort"
                break
            if progress_enabled:
                actions = [row["action"] for row in progress_events]
                action = "escalate" if "escalate" in actions else (
                    "checkpoint" if "checkpoint" in actions else "none"
                )
                if action != "none":
                    messages.append({"role": "user", "content": progress.hint(action)})

        prepare_diff(workspace, task.setup_artifact_paths)
        patch = subprocess.check_output(["git", "diff", "HEAD", "--no-ext-diff"], cwd=workspace, text=True)
        metrics = diff_metrics(workspace)
        test_patch = ""
        if metrics["test_files"]:
            test_patch = subprocess.check_output(
                ["git", "diff", "HEAD", "--no-ext-diff", "--", *metrics["test_files"]],
                cwd=workspace, text=True,
            )
        grader = grade(task, workspace)
        test_sensitivity = authored_test_sensitivity(
            repo, task, parent, test_patch, grader["visible_commands_passed"], worktree_root,
        )
        usage_rows = [row["usage"] for row in turns if "usage" in row]
        return {
            "run_id": run_id,
            "task_id": task.task_id,
            "condition": condition,
            "repeat": repeat,
            "parent_commit": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=workspace, text=True).strip(),
            "fix_commit": task.fix_commit,
            "languages": task.languages,
            "selection_stratum": task.selection_stratum,
            "setup": setup,
            "terminal_reason": terminal_reason,
            "submitted": submitted,
            "resolved": grader["hidden_passed"],
            "turns": turns,
            "usage": {
                key: sum(row[key] for row in usage_rows)
                for key in ("input_tokens", "input_uncached_tokens", "input_cached_tokens", "output_tokens", "total_tokens")
            },
            "max_provider_input_tokens": max((row["input_tokens"] for row in usage_rows), default=0),
            "economizer_activated_turns": sum(
                bool(row["economizer"].get("mutated")) for row in turns
            ),
            "economizer_reused_boundary_turns": sum(
                bool(row["economizer"].get("reused_boundary")) for row in turns
            ),
            "tool_output_recoveries": sum(
                call["name"] == "tool_output_get" for row in turns for call in row.get("tool_calls", [])
            ),
            "progress_controller": {
                "enabled": progress_enabled,
                **progress.event("none"),
                "checkpoint_turns": sum(
                    any(event["action"] == "checkpoint" for event in row.get("progress_events", []))
                    for row in turns
                ),
                "escalation_turns": sum(
                    any(event["action"] == "escalate" for event in row.get("progress_events", []))
                    for row in turns
                ),
            },
            "prior_failure_learning": learned_evidence if condition == "aimee_learned_retry" else None,
            "patch": patch,
            "patch_sha256": sha256_text(patch),
            "diff": metrics,
            "authored_test_patch": test_patch,
            "authored_test_patch_sha256": sha256_text(test_patch),
            "authored_test_sensitivity": test_sensitivity,
            "grader": grader,
            "wall_seconds": time.monotonic() - started,
            "canonical_transcript": messages,
            "full_tool_outputs": outputs,
        }
    finally:
        remove_worktree(repo, workspace)


def summarize(cells: list[dict[str, Any]]) -> dict[str, Any]:
    by_condition = {}
    for condition in sorted({cell["condition"] for cell in cells}):
        rows = [cell for cell in cells if cell["condition"] == condition]
        resolved = sum(bool(cell["resolved"]) for cell in rows)
        by_condition[condition] = {
            "cells": len(rows), "resolved": resolved,
            "context_limit_failures": sum(cell["terminal_reason"] == "context_limit" for cell in rows),
            "authored_test_cells": sum(bool(cell["diff"]["test_files"]) for cell in rows),
            "regression_sensitive_test_cells": sum(
                bool(cell["authored_test_sensitivity"]["regression_sensitive"]) for cell in rows
            ),
            "input_tokens": sum(cell["usage"]["input_tokens"] for cell in rows),
            "output_tokens": sum(cell["usage"]["output_tokens"] for cell in rows),
            "total_tokens": sum(cell["usage"]["total_tokens"] for cell in rows),
            "tokens_per_resolved_task": (
                sum(cell["usage"]["total_tokens"] for cell in rows) / resolved if resolved else None
            ),
        }
    off = {(c["task_id"], c["repeat"]): c for c in cells if c["condition"] == "off"}
    treatments = {
        condition: {(c["task_id"], c["repeat"]): c for c in cells if c["condition"] == condition}
        for condition in by_condition if condition != "off"
    }
    crossovers = []
    for condition, rows in treatments.items():
        crossovers.extend([
            {"task_id": key[0], "repeat": key[1], "condition": condition,
             "off_reason": off[key]["terminal_reason"]}
            for key in sorted(set(off) & set(rows))
            if not off[key]["resolved"] and rows[key]["resolved"]
        ])
    context_crossovers = [
        row for row in crossovers
        if off[(row["task_id"], row["repeat"])]["terminal_reason"] == "context_limit"
    ]
    return {"by_condition": by_condition, "completion_crossovers": crossovers,
            "context_capacity_crossovers": context_crossovers}


def write_json_atomic(path: Path, value: dict[str, Any]) -> None:
    temporary = path.with_name(path.name + ".tmp")
    temporary.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n")
    os.replace(temporary, path)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--base-url", required=True)
    parser.add_argument("--model", required=True)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--task-manifest", type=Path,
                        default=Path(__file__).with_name("large_repo_tasks.json"))
    parser.add_argument("--tasks", default="clone_fd_and_owner,trust_bundle_readiness,pool_lease_attribution,db1_outcome_codes")
    parser.add_argument("--conditions", default="off,aimee")
    parser.add_argument("--progress-preregistration", type=Path,
                        default=Path(__file__).with_name("large_repo_progress_preregistration.json"))
    parser.add_argument("--learned-failure-artifact", type=Path)
    parser.add_argument("--repeats", type=int, default=1)
    parser.add_argument("--max-turns", type=int, default=50)
    parser.add_argument("--max-output-tokens", type=int, default=2048)
    parser.add_argument("--context-cap", type=int, default=65536)
    parser.add_argument("--tool-output-max-bytes", type=int, default=32768)
    parser.add_argument("--budget-limit-usd", type=float, required=True)
    parser.add_argument("--marginal-input-usd-per-million", type=float, default=0.0)
    parser.add_argument("--marginal-output-usd-per-million", type=float, default=0.0)
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    if args.repeats < 1 or args.max_turns < 1 or args.max_output_tokens < 1:
        raise SystemExit("repeats, max-turns, and max-output-tokens must be positive")
    conditions = [item.strip() for item in args.conditions.split(",") if item.strip()]
    if set(conditions) - {"off", "aimee", "aimee_progress", "aimee_learned_retry"} or not conditions:
        raise SystemExit(
            "conditions must be off, aimee, aimee_progress, and/or aimee_learned_retry"
        )
    selected = [item.strip() for item in args.tasks.split(",") if item.strip()]
    manifest, tasks = load_tasks(args.task_manifest, selected)
    progress_preregistration = json.loads(args.progress_preregistration.read_text())
    expected_progress = {
        "initial_calls": PROGRESS_INITIAL_CALLS,
        "followup_calls": PROGRESS_FOLLOWUP_CALLS,
        "duplicate_hits": PROGRESS_DUPLICATE_HITS,
        "window": PROGRESS_WINDOW,
    }
    if progress_preregistration.get("controller") != expected_progress:
        raise SystemExit("progress preregistration does not match the implemented controller")
    learned_contexts: dict[str, tuple[str, dict[str, Any]]] = {}
    learned_failure_sha256 = None
    if "aimee_learned_retry" in conditions:
        if not args.learned_failure_artifact:
            raise SystemExit("aimee_learned_retry requires --learned-failure-artifact")
        failure_bytes = args.learned_failure_artifact.read_bytes()
        learned_failure_sha256 = hashlib.sha256(failure_bytes).hexdigest()
        registered_sha = progress_preregistration.get("source_failure_artifact_sha256")
        if learned_failure_sha256 != registered_sha:
            raise SystemExit("learned failure artifact does not match the preregistered SHA-256")
        failure_artifact = json.loads(failure_bytes)
        for task in tasks:
            try:
                learned_contexts[task.task_id] = learned_failure_context(
                    failure_artifact, task.task_id
                )
            except ValueError as error:
                raise SystemExit(str(error)) from error
    repo = Path(__file__).resolve().parents[2]
    dirty = subprocess.check_output(["git", "status", "--porcelain", "--untracked-files=all"], cwd=repo, text=True)
    if dirty:
        raise SystemExit("source worktree is dirty; commit the runner before recording lineage")
    source_commit = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=repo, text=True).strip()
    catalog = http_json(args.base_url.rstrip("/") + "/v1/models")
    if args.model not in [row.get("id") for row in catalog.get("data", [])]:
        raise SystemExit(f"model {args.model!r} absent from endpoint catalog")

    calls = len(tasks) * len(conditions) * args.repeats * args.max_turns
    expected_calls = len(tasks) * len(conditions) * args.repeats * min(20, args.max_turns)
    expected_input = expected_calls * min(args.context_cap // 2, 32_000)
    expected_output = expected_calls * min(args.max_output_tokens, 512)
    hard_input = calls * args.context_cap
    hard_output = calls * args.max_output_tokens
    expected_spend = (
        expected_input * args.marginal_input_usd_per_million
        + expected_output * args.marginal_output_usd_per_million
    ) / 1_000_000
    hard_spend = (
        hard_input * args.marginal_input_usd_per_million
        + hard_output * args.marginal_output_usd_per_million
    ) / 1_000_000
    budget = {
        "pricing_contract": "operator-owned local endpoint; configured marginal rates",
        "planned_cells": len(tasks) * len(conditions) * args.repeats,
        "expected_provider_calls": expected_calls, "hard_provider_call_cap": calls,
        "expected_input_tokens": expected_input, "hard_input_token_cap": hard_input,
        "expected_output_tokens": expected_output, "hard_output_token_cap": hard_output,
        "expected_spend_usd": expected_spend, "hard_maximum_spend_usd": hard_spend,
        "budget_limit_usd": args.budget_limit_usd,
    }
    print(json.dumps({"budget_preflight": budget}, indent=2), flush=True)
    if args.budget_limit_usd < hard_spend:
        raise SystemExit("budget limit is below hard maximum; no inference dispatched")

    args.output.parent.mkdir(parents=True, exist_ok=True)
    preflight = args.output.with_suffix(".preflight.json")
    preflight_record = {
        "schema_version": 1, "created_at": datetime.now(timezone.utc).isoformat(),
        "source_commit": source_commit, "model": args.model,
        "task_manifest_sha256": sha256_json(manifest), "tasks": selected,
        "progress_preregistration_sha256": sha256_json(progress_preregistration),
        "learned_failure_artifact_sha256": learned_failure_sha256,
        "conditions": conditions, "repeats": args.repeats, "budget": budget,
        "dispatch_started": False,
    }
    preflight.write_text(json.dumps(preflight_record, indent=2, sort_keys=True) + "\n")

    temporary = tempfile.TemporaryDirectory(prefix="aimee-large-repo-roi-")
    temp = Path(temporary.name)
    probe_path = temp / "aimee-economizer-probe"
    build_probe(repo, probe_path)
    probe = EconomizerProbe(probe_path)
    plan = [(task, condition, repeat) for task in tasks for repeat in range(args.repeats) for condition in conditions]
    random.Random(SEED).shuffle(plan)
    preflight_record["dispatch_started"] = True
    preflight_record["dispatch_started_at"] = datetime.now(timezone.utc).isoformat()
    preflight.write_text(json.dumps(preflight_record, indent=2, sort_keys=True) + "\n")
    cells = []
    checkpoint = args.output.with_suffix(".checkpoint.json")
    try:
        for ordinal, (task, condition, repeat) in enumerate(plan, 1):
            cell = run_cell(
                repo, task, condition, repeat, args.base_url, args.model,
                args.max_turns, args.max_output_tokens, args.tool_output_max_bytes,
                probe, temp, learned_contexts,
            )
            cells.append(cell)
            write_json_atomic(checkpoint, {
                "schema_version": 1, "complete": False,
                "created_at": datetime.now(timezone.utc).isoformat(),
                "source_commit": source_commit, "model": args.model,
                "tasks": selected, "conditions": conditions, "repeats": args.repeats,
                "completed_cells": len(cells), "planned_cells": len(plan),
                "cells": cells, "summary": summarize(cells),
            })
            print(json.dumps({
                "completed": ordinal, "planned": len(plan), "task": task.task_id,
                "condition": condition, "resolved": cell["resolved"],
                "terminal_reason": cell["terminal_reason"], "usage": cell["usage"],
            }), flush=True)
    finally:
        probe.close()
        temporary.cleanup()

    artifact = {
        "schema_version": 1, "claim_status": "exploratory_capacity_pilot",
        "created_at": datetime.now(timezone.utc).isoformat(), "source_commit": source_commit,
        "model": args.model, "context_cap": args.context_cap,
        "execution_path": "full git worktree; OpenAI-compatible tool loop; production Go economizer handler with process-local StateStore",
        "task_manifest_sha256": sha256_json(manifest), "seed": SEED,
        "progress_preregistration": progress_preregistration,
        "progress_preregistration_sha256": sha256_json(progress_preregistration),
        "learned_failure_artifact_sha256": learned_failure_sha256,
        "budget": budget, "cells": cells, "summary": summarize(cells),
    }
    write_json_atomic(args.output, artifact)
    write_json_atomic(checkpoint, {**artifact, "complete": True})
    print(json.dumps({"artifact": str(args.output), "summary": artifact["summary"]}, indent=2))


if __name__ == "__main__":
    main()
