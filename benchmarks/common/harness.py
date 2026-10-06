#!/usr/bin/env python3
"""Shared helpers for the benchmark harness v2."""

from __future__ import annotations

import atexit
import json
import os
import shutil
import subprocess
import tempfile
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any


def repo_root() -> Path:
    if root := os.environ.get("AIMEE_BENCH_ROOT"):
        return Path(root)
    return Path(__file__).resolve().parents[2]


def git_commit(root: Path | None = None) -> str:
    root = root or repo_root()
    proc = subprocess.run(
        ["git", "rev-parse", "HEAD"],
        cwd=root,
        check=True,
        capture_output=True,
        text=True,
    )
    return proc.stdout.strip()


def normalize_answer(text: str) -> str:
    chars = []
    last_space = True
    for ch in text.lower():
        if ch.isalnum():
            chars.append(ch)
            last_space = False
        elif not last_space:
            chars.append(" ")
            last_space = True
    return "".join(chars).strip()


def normalize_question(text: str) -> str:
    stopwords = {
        "a",
        "an",
        "and",
        "are",
        "as",
        "at",
        "be",
        "did",
        "do",
        "does",
        "for",
        "from",
        "go",
        "had",
        "has",
        "have",
        "he",
        "her",
        "him",
        "his",
        "how",
        "i",
        "in",
        "is",
        "it",
        "its",
        "me",
        "of",
        "on",
        "or",
        "she",
        "that",
        "the",
        "their",
        "them",
        "there",
        "they",
        "this",
        "to",
        "was",
        "were",
        "what",
        "when",
        "where",
        "which",
        "who",
        "why",
        "with",
        "would",
        "yesterday",
    }
    tokens = []
    current = []
    for ch in text.lower():
        if ch.isalnum():
            current.append(ch)
            continue
        if current:
            token = "".join(current)
            if token not in stopwords:
                tokens.append(token)
            current = []
    if current:
        token = "".join(current)
        if token not in stopwords:
            tokens.append(token)
    return " ".join(tokens) if tokens else text


def percentile(values: list[float], pct: float) -> float:
    if not values:
        return 0.0
    ordered = sorted(values)
    idx = min(len(ordered) - 1, max(0, int(round((len(ordered) - 1) * pct))))
    return ordered[idx]


def summarize_latencies(values: list[float]) -> dict[str, float]:
    if not values:
        return {"avg_s": 0.0, "p95_s": 0.0}
    return {
        "avg_s": sum(values) / len(values),
        "p95_s": percentile(values, 0.95),
    }


def summarize_costs(costs: list[float], correct_count: int) -> dict[str, float]:
    total = sum(costs)
    per_query = total / len(costs) if costs else 0.0
    per_correct = total / correct_count if correct_count > 0 else 0.0
    return {
        "total_usd": total,
        "per_query_usd": per_query,
        "per_correct_usd": per_correct,
    }


def category_accuracy(results: list[dict[str, Any]], field: str) -> dict[str, dict[str, float]]:
    buckets: dict[str, dict[str, float]] = {}
    for row in results:
        label = str(row[field])
        bucket = buckets.setdefault(label, {"correct": 0.0, "total": 0.0})
        bucket["total"] += 1.0
        if row["verdict"] == "CORRECT":
            bucket["correct"] += 1.0
    for label, bucket in buckets.items():
        bucket["accuracy"] = bucket["correct"] / bucket["total"] if bucket["total"] else 0.0
    return dict(sorted(buckets.items(), key=lambda item: item[0]))


def print_breakdown(title: str, buckets: dict[str, dict[str, float]]) -> None:
    print(title)
    for label, bucket in buckets.items():
        print(
            f"  {label}: accuracy={bucket['accuracy']:.3f} "
            f"({int(bucket['correct'])}/{int(bucket['total'])})"
        )


def token_estimate_cost(model: str, input_tokens: int, output_tokens: int) -> float:
    prices = [
        ("claude-opus-4", 15.00, 75.00),
        ("claude-sonnet-4", 3.00, 15.00),
        ("claude-haiku-4", 0.80, 4.00),
        ("claude-3-7-sonnet", 3.00, 15.00),
        ("claude-3-5-sonnet", 3.00, 15.00),
        ("claude-3-5-haiku", 0.80, 4.00),
        ("claude-3-opus", 15.00, 75.00),
        ("claude-3-sonnet", 3.00, 15.00),
        ("claude-3-haiku", 0.25, 1.25),
        ("gpt-4o-mini", 0.15, 0.60),
        ("gpt-4o", 2.50, 10.00),
        ("o3-mini", 1.10, 4.40),
        ("o3", 10.00, 40.00),
        ("o1-mini", 1.10, 4.40),
        ("o1", 15.00, 60.00),
        ("gpt-4-turbo", 10.00, 30.00),
        ("gpt-4", 30.00, 60.00),
        ("gpt-3.5-turbo", 0.50, 1.50),
    ]
    lowered = (model or "").lower()
    for needle, in_price, out_price in prices:
        if needle in lowered:
            return input_tokens * in_price / 1_000_000.0 + output_tokens * out_price / 1_000_000.0
    return 0.0


def collect_vector_runtime_metadata(root: Path | None = None) -> dict[str, Any]:
    """Return best-effort memory-index metadata for benchmark artifacts.

    Vectors live in pgvector inside KB_STORE (Postgres) since #1575; this hook
    is left in place so future runs can route a real metadata RPC through
    the kb client without changing the artifact schema."""
    root = root or repo_root()
    client = root / "aimee-client"
    server = root / "aimee-server"
    kb = root / "aimee-kb"
    missing = [str(path.name) for path in (client, server, kb) if not path.exists()]
    if missing:
        return {"status": "not_collected", "reason": f"missing shipped artifact(s): {', '.join(missing)}"}
    return {
        "status": "not_collected",
        "reason": "memory-index metadata is not exposed through a routed benchmark RPC",
    }


@dataclass
class AgentExecution:
    agent: str
    response: str
    prompt_tokens: int
    completion_tokens: int
    latency_s: float


class AimeeHarness:
    """Run benchmark cases against the shipped aimee client/server artifacts."""

    def __init__(self, root: Path | None = None) -> None:
        self.root = root or repo_root()
        self.binary = Path(os.environ.get("AIMEE_BENCH_CLIENT", str(self.root / "aimee-client")))
        self.server_binary = self.root / "aimee-server"
        self.fake_agent = os.environ.get("AIMEE_BENCH_FAKE_AGENT") == "1"
        self.source_home = Path(os.environ.get("AIMEE_BENCH_SOURCE_HOME", str(Path.home())))
        # When set, the aimee target's memory store/search run against a live
        # server reachable from this HOME's socket (a scratch stack on a scratch
        # DB) instead of a per-case temp home that has no socket. Isolation then
        # comes from the per-sample --session id; pair with a small --max-samples.
        self._server_home = os.environ.get("AIMEE_BENCH_SERVER_HOME")
        self.current_model = self._load_execute_model()
        self._fake_memories: dict[str, list[dict[str, Any]]] = {}
        self._server_procs: list[subprocess.Popen[str]] = []
        atexit.register(self._cleanup_servers)

    def _run(
        self,
        args: list[str],
        *,
        home: Path,
        capture_json: bool = True,
        prefer_server: bool = False,
    ) -> Any:
        env = os.environ.copy()
        env["HOME"] = str(home)
        env["AIMEE_NO_CACHE"] = "1"
        sock = home / ".config" / "aimee" / "aimee.sock"
        if sock.exists():
            env["AIMEE_SOCK"] = str(sock)
        cmd = [str(self.binary), *args]
        if prefer_server and self.server_binary.exists():
            cmd = [str(self.binary), *args]
        proc = subprocess.run(
            cmd,
            cwd=self.root,
            check=True,
            capture_output=True,
            text=True,
            env=env,
        )
        if not capture_json:
            return proc.stdout
        return json.loads(proc.stdout)

    def _copy_user_agents(self, home: Path) -> None:
        src_dir = self.source_home / ".config" / "aimee"
        dst_dir = home / ".config" / "aimee"
        dst_dir.mkdir(parents=True, exist_ok=True)
        for name in ("agents.json",):
            src = src_dir / name
            if src.exists():
                shutil.copy2(src, dst_dir / name)

    def prepare_home(self) -> tuple[tempfile.TemporaryDirectory[str], Path]:
        tmp = tempfile.TemporaryDirectory(prefix="aimee-bench-")
        home = Path(tmp.name)
        (home / ".config" / "aimee").mkdir(parents=True, exist_ok=True)
        self._copy_user_agents(home)
        return tmp, home

    def _cleanup_servers(self) -> None:
        for proc in self._server_procs:
            if proc.poll() is None:
                proc.terminate()
        for proc in self._server_procs:
            try:
                proc.wait(timeout=2)
            except subprocess.TimeoutExpired:
                proc.kill()
        self._server_procs.clear()

    def _fake_store_memory(
        self,
        home: Path,
        *,
        key: str,
        content: str,
        session: str,
        tier: str,
        kind: str,
    ) -> dict[str, Any]:
        bucket = self._fake_memories.setdefault(str(home), [])
        memory_id = len(bucket) + 1
        row = {
            "id": memory_id,
            "key": key,
            "content": content,
            "session": session,
            "tier": tier,
            "kind": kind,
        }
        bucket.append(row)
        return {"status": "ok", "id": memory_id}

    def _fake_search_facts(self, home: Path, query: str, limit: int) -> list[dict[str, Any]]:
        rows = self._fake_memories.get(str(home), [])
        query_terms = {term for term in normalize_answer(query).split() if len(term) > 2}

        def score(row: dict[str, Any]) -> tuple[int, int]:
            content_terms = set(normalize_answer(str(row.get("content", ""))).split())
            overlap = len(query_terms & content_terms)
            return (overlap, -int(row.get("id", 0)))

        ranked = sorted(rows, key=score, reverse=True)
        if any(score(row)[0] > 0 for row in ranked):
            ranked = [row for row in ranked if score(row)[0] > 0]
        return ranked[:limit]

    def store_memory(
        self,
        home: Path,
        *,
        key: str,
        content: str,
        session: str,
        tier: str = "L2",
        kind: str = "fact",
    ) -> dict[str, Any]:
        if self.fake_agent:
            return self._fake_store_memory(
                home, key=key, content=content, session=session, tier=tier, kind=kind
            )
        args = [
            "--json", "memory", "store", key, content,
            "--tier", tier, "--kind", kind, "--session", session,
        ]
        store_home = Path(self._server_home) if self._server_home else home
        # Memory ingest fires many stores in a row; a single transient server
        # hiccup must not abort the whole benchmark. Retry a few times.
        attempts = int(os.environ.get("AIMEE_BENCH_STORE_RETRIES", "3"))
        last_exc: Exception | None = None
        for attempt in range(max(1, attempts)):
            try:
                return self._run(args, home=store_home, prefer_server=True)
            except subprocess.CalledProcessError as exc:
                last_exc = exc
                time.sleep(0.5 * (attempt + 1))
        raise last_exc  # type: ignore[misc]

    def search_facts(self, home: Path, query: str, limit: int) -> tuple[list[dict[str, Any]], float]:
        started = time.perf_counter()
        if self.fake_agent:
            facts = self._fake_search_facts(home, query, limit)
            return facts, time.perf_counter() - started
        payload = self._run(
            ["--json", "memory", "search", query, "--limit", str(limit)],
            home=Path(self._server_home) if self._server_home else home,
            prefer_server=True,
        )
        elapsed = time.perf_counter() - started
        return payload.get("facts", []), elapsed

    def _fake_agent_run(self, prompt: str, system: str) -> AgentExecution:
        if "grading benchmark answers" in system.lower():
            gold = ""
            candidate = ""
            for line in prompt.splitlines():
                if line.startswith("Gold answer: "):
                    gold = line.split(": ", 1)[1]
                elif line.startswith("Candidate answer: "):
                    candidate = line.split(": ", 1)[1]
            norm_gold = normalize_answer(gold)
            norm_candidate = normalize_answer(candidate)
            score = 1 if (
                norm_gold == norm_candidate
                or (norm_gold and norm_gold in norm_candidate)
                or (norm_candidate and norm_candidate in norm_gold)
            ) else 0
            response = json.dumps({"score": score})
        else:
            response = "Unknown"
            marker = "Retrieved memory context:\n"
            if marker in prompt:
                context = prompt.split(marker, 1)[1].split("\nFinal answer:", 1)[0]
                first_line = next((line for line in context.splitlines() if line.strip()), "")
                if ": " in first_line:
                    response = first_line.split(": ", 1)[1].strip()
                elif first_line:
                    response = first_line.strip()
        return AgentExecution(
            agent="fake-benchmark-agent",
            response=response,
            prompt_tokens=max(1, len(prompt) // 4),
            completion_tokens=max(1, len(response) // 4),
            latency_s=0.01,
        )

    def agent_run(
        self, home: Path, *, prompt: str, system: str | None = None, max_tokens: int
    ) -> AgentExecution:
        if self.fake_agent:
            return self._fake_agent_run(prompt, system or "")
        return self._delegate_execute(prompt, system, max_tokens)

    def _delegate_execute(
        self, prompt: str, system: str | None, max_tokens: int
    ) -> AgentExecution:
        """Run one delegate turn via the durable job queue and return its output.

        Foreground delegate is unsupported over the default /v1 transport (it
        blocks), so we queue with ``--background`` and poll ``jobs status`` until
        the job reaches a terminal state. The server requires a ``--persona``
        (``AIMEE_BENCH_PERSONA``, default ``engineer``). HOME points at
        ``source_home`` so the client reaches the running server and its
        configured execute-role agent, and so benchmark calls never write to a
        throwaway per-case home that has no socket.

        Token counts are estimated from text length (the job record exposes no
        token usage); cost estimates downstream are therefore approximate.
        """
        persona = os.environ.get("AIMEE_BENCH_PERSONA", "engineer")
        timeout_s = float(os.environ.get("AIMEE_BENCH_DELEGATE_TIMEOUT", "600"))
        poll_s = float(os.environ.get("AIMEE_BENCH_DELEGATE_POLL", "5"))

        env = os.environ.copy()
        env["HOME"] = str(self.source_home)
        env["AIMEE_NO_CACHE"] = "1"

        def _client(args: list[str], stdin_text: str | None = None) -> dict[str, Any]:
            proc = subprocess.run(
                [str(self.binary), *args],
                cwd=self.root,
                check=True,
                capture_output=True,
                text=True,
                env=env,
                input=stdin_text,
            )
            return json.loads(proc.stdout)

        # Pass the prompt over stdin, not as an argv string: benchmark prompts
        # (e.g. model_only stuffing a full conversation transcript) routinely
        # exceed the kernel's per-argument limit (E2BIG / "Argument list too long").
        queue_cmd = [
            "--json", "delegate", "execute", "--prompt-stdin",
            "--persona", persona, "--max-tokens", str(max_tokens), "--background",
        ]
        if system:
            queue_cmd += ["--system", system]
        queued = _client(queue_cmd, stdin_text=prompt)
        job_id = queued.get("job_id")
        if job_id is None:
            raise RuntimeError(f"delegate queue failed: {queued}")

        # Server job terminal states: done (success), failed, cancelled, error.
        terminal = {"done", "failed", "cancelled", "error"}
        started = time.perf_counter()
        job: dict[str, Any] = {}
        while True:
            job = _client(["--json", "jobs", "status", str(job_id)]).get("job", {})
            if job.get("status") in terminal:
                break
            if time.perf_counter() - started > timeout_s:
                raise TimeoutError(f"delegate job {job_id} timed out after {timeout_s:.0f}s")
            time.sleep(poll_s)

        latency_s = time.perf_counter() - started
        if job.get("status") != "done":
            raise RuntimeError(
                f"delegate job {job_id} {job.get('status')}: {job.get('result', '')}"
            )
        response = str(job.get("result", ""))
        return AgentExecution(
            agent=str(job.get("agent_name", "")),
            response=response,
            prompt_tokens=max(1, len(prompt) // 4),
            completion_tokens=max(1, len(response) // 4),
            latency_s=latency_s,
        )

    def _load_execute_model(self) -> str:
        agent_path = self.source_home / ".config" / "aimee" / "agents.json"
        if not agent_path.exists():
            return ""
        try:
            data = json.loads(agent_path.read_text())
        except json.JSONDecodeError:
            return ""
        agents = data.get("agents", [])
        for agent in agents:
            roles = set(agent.get("roles", [])) | set(agent.get("exec_roles", []))
            if "execute" in roles and agent.get("enabled", True):
                return str(agent.get("model", ""))
        return ""
