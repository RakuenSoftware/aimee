#!/usr/bin/env python3
"""Run one provider-backed E6 coding cell in an isolated Codex checkout."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import subprocess
import tempfile
import time
import urllib.parse
from pathlib import Path


PINNED_COMMIT = "aa8c40e9d75449774c9b0b630bb8f1037efb8097"
MODEL = "gpt-5.6-sol"
REASONING = "medium"
CODEX = Path(os.environ.get("AIMEE_E6_CODEX", "/home/virant/.local/bin/codex"))
AUTH = Path(os.environ.get("AIMEE_E6_CODEX_AUTH", "/home/virant/.codex/auth.json"))
PROMPT_FIXTURE = Path(__file__).with_name("prompts") / "e6-agent-task-v1.md"
HARNESS = Path(os.environ.get(
    "AIMEE_E6_FIXTURE_HARNESS", "/home/virant/dev/ponytail-codex-benchmark/battery"
))
FIXTURE = HARNESS / "fixture"
HIDDEN = HARNESS / "hidden_tests"
TASKS = {
    "c01": ("t06_semver", "best_match", "app/deps.py"),
    "c02": ("t50_toposort", "install_order", "app/graph.py"),
    "c03": ("t43_config_precedence", "resolve_config", "app/configuration.py"),
    "c04": ("t09_root_cause", "billing_period_days", "app/billing.py"),
    "c05": ("t27_sort_missing", "stable_sort", "app/textutils.py"),
    "c06": ("t38_iso8601", "parse_timestamp", "app/timeutils.py"),
    "c07": ("t46_upstream_errors", "upstream_response", "app/api.py"),
    "c08": ("t40_cache_stampede", "stampede_get", "app/cache.py"),
}
ARMS = ("standard", "observe", "on", "ceiling")


def run(command: list[str | Path], **kwargs) -> subprocess.CompletedProcess:
    return subprocess.run([str(part) for part in command], text=True, capture_output=True, **kwargs)


def ticket_map() -> dict[str, str]:
    result = {}
    for line in (HARNESS / "arms" / "tasks.tsv").read_text().splitlines():
        if line.strip():
            key, value = line.split("\t", 1)
            result[key] = value
    return result


def packet_for(symbol: str) -> tuple[dict, float]:
    query = urllib.parse.urlencode({"query": symbol, "symbol": symbol,
                                    "project": "e6-aa8c40e-fixture"})
    remote = (
        "pct exec 331 -- docker exec aimee989c4fe-aimee-kb-1 "
        f"curl -fsS 'http://127.0.0.1:8741/v1/code/context?{query}'"
    )
    started = time.monotonic()
    response = run(["ssh", "root@192.168.1.253", remote], timeout=30)
    elapsed = time.monotonic() - started
    if response.returncode:
        raise RuntimeError(f"packet retrieval failed: {response.stderr}")
    packet = json.loads(response.stdout)
    if packet.get("status") != "ok" or not packet.get("results"):
        raise RuntimeError(f"packet is not answerable: {response.stdout}")
    return packet, elapsed


def codex_home(root: Path) -> tuple[Path, dict[str, str]]:
    # Trusted-operator-only harness: the disposable Codex home contains provider credentials.
    home = root / "codex-home"
    home.mkdir()
    shutil.copy2(AUTH, home / "auth.json")
    (home / "config.toml").write_text(
        f'model = "{MODEL}"\nmodel_reasoning_effort = "{REASONING}"\n\n[agents]\nenabled = false\n'
    )
    environment = {
        "HOME": str(root), "USER": "benchmark", "LOGNAME": "benchmark", "SHELL": "/bin/bash",
        "PATH": "/usr/local/bin:/usr/bin:/bin", "LANG": "C.UTF-8", "LC_ALL": "C.UTF-8",
        "TERM": "dumb", "CODEX_HOME": str(home),
    }
    return home, environment


def parse_stream(path: Path) -> tuple[dict, bool]:
    usage, messages, edit_index = {}, [], None
    events = []
    for line in path.read_text(errors="replace").splitlines():
        try:
            events.append(json.loads(line))
        except json.JSONDecodeError:
            continue
    for index, event in enumerate(events):
        if event.get("type") == "turn.completed":
            usage = event.get("usage") or {}
        item = event.get("item")
        if not isinstance(item, dict):
            continue
        if item.get("type") == "agent_message":
            messages.append((index, str(item.get("text", ""))))
        if edit_index is None and item.get("type") in ("file_change", "command_execution"):
            blob = json.dumps(item).lower()
            if item.get("type") == "file_change" or any(marker in blob for marker in
                    ("apply_patch", "sed -i", "tee ", "> app/")):
                edit_index = index
    consumed = edit_index is not None and any(index < edit_index and
                                               "aimee-context consumed" in text.lower()
                                               for index, text in messages)
    return usage, consumed


def hidden_test(workspace: Path, task: str) -> bool:
    result = run(["python3", HIDDEN / "runner.py", workspace, HIDDEN / f"{task}.py"], timeout=60)
    try:
        rows = json.loads(result.stdout)
    except json.JSONDecodeError:
        return False
    return result.returncode == 0 and bool(rows) and all(row.get("ok") for row in rows.values())


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--arm", choices=ARMS, required=True)
    parser.add_argument("--task", choices=TASKS, required=True)
    parser.add_argument("--artifacts", type=Path, required=True)
    args = parser.parse_args()
    task, symbol, oracle_path = TASKS[args.task]
    args.artifacts.mkdir(parents=True, exist_ok=False)
    with tempfile.TemporaryDirectory(prefix=f"e6-{args.arm}-{args.task}-") as temporary:
        root = Path(temporary)
        workspace = root / "source"
        shutil.copytree(FIXTURE, workspace)
        initialized = run(["git", "init", "-q"], cwd=workspace, timeout=30)
        if initialized.returncode:
            raise RuntimeError(initialized.stderr)
        run(["git", "config", "user.name", "E6 Benchmark"], cwd=workspace)
        run(["git", "config", "user.email", "benchmark@example.invalid"], cwd=workspace)
        run(["git", "add", "."], cwd=workspace)
        committed = run(["git", "commit", "-q", "-m", "fixture seed"], cwd=workspace)
        if committed.returncode:
            raise RuntimeError(committed.stderr)
        ticket = ticket_map()[task]
        (workspace / "TICKET.txt").write_text(ticket + "\n")
        _, environment = codex_home(root)
        packet = None
        retrieval_latency = 0.0
        if args.arm in ("observe", "on"):
            packet, retrieval_latency = packet_for(symbol)
        base_prompt = (
            PROMPT_FIXTURE.read_text() + "\n\nTask:\n" + ticket +
            "\n\nRead README.md and the existing code. Fix the task in this checkout. "
            "Do not inspect parent or sibling directories.\n"
        )
        if args.arm == "on":
            base_prompt += ("\n<aimee-context>\n" + json.dumps(packet, sort_keys=True) +
                            "\n</aimee-context>\nBefore editing, state `AIMEE-CONTEXT consumed` and "
                            "name the result path if you use this packet.\n")
        elif args.arm == "ceiling":
            base_prompt += ("\n<oracle-context>\n" + (workspace / oracle_path).read_text() +
                            "\n</oracle-context>\n")
        stream_path = args.artifacts / "codex.jsonl"
        stderr_path = args.artifacts / "codex.stderr"
        # These bypasses are intentional: every cell uses a disposable tempdir checkout in the
        # explicitly authorized provider-test environment, and no repository credentials are copied.
        command = [
            CODEX, "exec", "--ephemeral", "--json", "--color", "never",
            "--dangerously-bypass-approvals-and-sandbox", "--dangerously-bypass-hook-trust",
            "-m", MODEL, "-c", f'model_reasoning_effort="{REASONING}"',
            "-c", "agents.enabled=false", "-C", workspace, base_prompt,
        ]
        started = time.monotonic()
        with stream_path.open("w") as stdout, stderr_path.open("w") as stderr:
            completed = subprocess.run([str(part) for part in command], cwd=workspace, env=environment,
                                       text=True, stdout=stdout, stderr=stderr, timeout=900)
        wall = time.monotonic() - started
        usage, consumed = parse_stream(stream_path)
        patch = run(["git", "diff", "--binary"], cwd=workspace).stdout
        (args.artifacts / "patch.diff").write_text(patch)
        result = {
            "schema_version": 1, "pinned_commit": PINNED_COMMIT,
            "prompt_fixture": str(PROMPT_FIXTURE.relative_to(Path(__file__).parent)),
            "arm": args.arm, "task": args.task, "fixture_task": task,
            "score_eligible": completed.returncode == 0, "task_success": hidden_test(workspace, task),
            "answerable": True, "consumed_before_edit": consumed if args.arm == "on" else False,
            "uncached_input_tokens": max(0, int(usage.get("input_tokens") or 0) -
                                           int(usage.get("cached_input_tokens") or 0)),
            "total_wall_s": wall, "retrieval_latency_s": retrieval_latency,
            "packet_tokens": len(json.dumps(packet).split()) if packet else 0,
            "codex_exit": completed.returncode, "model": MODEL, "reasoning": REASONING,
            "packet_sha256": hashlib.sha256(json.dumps(packet, sort_keys=True).encode()).hexdigest()
                             if packet else None,
        }
        (args.artifacts / "cell-result.json").write_text(json.dumps(result, indent=2, sort_keys=True) + "\n")
        print(json.dumps(result, sort_keys=True))
        return 0 if completed.returncode == 0 else 2


if __name__ == "__main__":
    raise SystemExit(main())
