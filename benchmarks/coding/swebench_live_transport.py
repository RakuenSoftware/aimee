#!/usr/bin/env python3
"""Live delegate transport for the agentic supervised SWE-bench benchmark (#987).

The single place the live arm-A / arm-C / S0 paths talk to the fleet. It supersedes the
theoretical `/v1/runs` + EMPTY-delegation-polarity model of `swebench_transport_verify.py`
with the DEPLOYMENT-VERIFIED model established in `swebench_live_attribution.py` (live .254
run, 2026-07-05):

  * the "primary" and the workers are BOTH dispatched as `aimee delegate` jobs — a top-level
    tmux-Claude primary does not reliably emit EMPTY-delegation rows (FINDING 1), so we
    distinguish primary from worker by WHICH delegation_id the harness dispatched, read from
    `delegation_spawns` (FINDING 2);
  * agentic `--tools` needs `--worktree` or the parent-worktree write-guard blocks every tool
    (FINDING 4) — so a tools-enabled dispatch here ALWAYS carries a worktree branch;
  * only `usage_kind='realized'` rows are spend (FINDING 3).

Design (roundtable-ratified 2026-07-08, arm-C transport):
  - argv construction and status parsing are PURE and unit-tested;
  - dispatch/poll take an INJECTED `runner` (argv -> CompletedRun) so the whole loop is
    exercised in CI with a fake fleet, and the live path just passes a subprocess runner;
  - delegation_id is captured with a BEFORE-dispatch watermark over `delegation_spawns`
    (max id for the session), then the newest row after dispatch (H7: bound by session_id +
    watermark so a concurrent sibling dispatch can't be misattributed).

The DB read (token attribution) lives on the fleet host (`--token-db`); it is an operator-
supplied path (`$AIMEE_HOME/aimee.db`, or `/mnt/media/.plugins/aimee-server/server/home/aimee.db`
on .254). This module never assumes it is local.
"""
from __future__ import annotations

import json
import os
import shlex
import subprocess
import sqlite3
import time
from dataclasses import dataclass, field
from pathlib import Path

import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from benchmarks.coding import swebench_live_attribution as LA

_FAKE = os.environ.get("AIMEE_BENCH_FAKE_AGENT") == "1"

# Terminal job_status values from agent_jobs (verified live): success vs failure.
_TERMINAL_OK = frozenset({"done"})
_TERMINAL_FAIL = frozenset({"failed", "error", "cancelled"})


def is_terminal(status: str) -> bool:
    return status in _TERMINAL_OK or status in _TERMINAL_FAIL


def is_ok(status: str) -> bool:
    return status in _TERMINAL_OK


# ------------------------------------------------------------ argv construction -
def build_delegate_argv(role: str, prompt: str, *, aimee_bin: str = "aimee", via: str | None = None,
                        persona: str = "engineer", tools: bool = False, worktree: str | None = None,
                        model: str | None = None, max_turns: int | None = None,
                        timeout_ms: int | None = None, durable: bool = True,
                        extra: list[str] | None = None) -> list[str]:
    """Build the `aimee delegate <role> ...` argv (PURE). Enforces FINDING 4: a tools-enabled
    dispatch MUST carry a worktree branch, else the write-guard silently blocks every tool."""
    if tools and not worktree:
        raise ValueError("tools-enabled dispatch requires a worktree branch (FINDING 4: the "
                         "parent-worktree write guard blocks a plain --tools delegate)")
    argv = [aimee_bin, "delegate", role, prompt, "--persona", persona, "--json"]
    if via:
        argv += ["--via", via]
    if tools:
        argv.append("--tools")
    if worktree:
        argv += ["--worktree", worktree]
    if model:
        argv += ["--model", model]
    if max_turns is not None:
        argv += ["--max-turns", str(max_turns)]
    if timeout_ms is not None:
        argv += ["--timeout", str(timeout_ms)]
    if durable:
        argv.append("--durable")
    if extra:
        argv += list(extra)
    return argv


def build_status_argv(job_id, *, aimee_bin: str = "aimee") -> list[str]:
    return [aimee_bin, "delegate", "status", str(job_id), "--json"]


# ------------------------------------------------------------ status parsing ----
@dataclass
class DelegateStatus:
    job_id: int | None
    status: str            # pending | running | done | failed | error | cancelled
    result: str            # the delegate's final text (empty until terminal)
    agent_name: str
    api_calls: int
    raw: dict = field(default_factory=dict)

    @property
    def terminal(self) -> bool:
        return is_terminal(self.status)

    @property
    def ok(self) -> bool:
        return is_ok(self.status)


def parse_status(stdout: str) -> DelegateStatus:
    """Parse `aimee delegate status --json` output (PURE). Tolerates a trailing/leading log line
    by scanning for the first JSON object."""
    obj = _first_json_object(stdout)
    return DelegateStatus(
        job_id=obj.get("job_id"),
        status=str(obj.get("job_status") or obj.get("status") or "unknown"),
        result=str(obj.get("result") or ""),
        agent_name=str(obj.get("agent_name") or ""),
        api_calls=int(obj.get("api_call_count") or 0),
        raw=obj,
    )


def parse_dispatch(stdout: str) -> int | None:
    """Parse the `{"job_id":N,"job_status":"pending"}` a dispatch prints (PURE)."""
    obj = _first_json_object(stdout)
    jid = obj.get("job_id")
    return int(jid) if jid is not None else None


def _first_json_object(text: str) -> dict:
    """Return the first well-formed top-level JSON object in `text`, or {}."""
    if not text:
        return {}
    # Fast path: the whole thing is JSON.
    t = text.strip()
    try:
        v = json.loads(t)
        return v if isinstance(v, dict) else {}
    except Exception:
        pass
    # Scan for a balanced {...} span.
    depth = 0
    start = -1
    for i, ch in enumerate(text):
        if ch == "{":
            if depth == 0:
                start = i
            depth += 1
        elif ch == "}":
            depth -= 1
            if depth == 0 and start >= 0:
                try:
                    v = json.loads(text[start:i + 1])
                    if isinstance(v, dict):
                        return v
                except Exception:
                    start = -1
    return {}


# ------------------------------------------------------------ runner protocol ---
@dataclass
class CompletedRun:
    rc: int
    stdout: str
    stderr: str = ""


def subprocess_runner(argv: list[str], *, timeout_s: float = 900.0) -> CompletedRun:
    """The live runner: run argv, capture output. Never raises on non-zero rc (a delegate error
    is data, not a crash — S0 discipline)."""
    try:
        p = subprocess.run(argv, capture_output=True, text=True, timeout=timeout_s)
        return CompletedRun(p.returncode, p.stdout, p.stderr)
    except subprocess.TimeoutExpired as e:
        return CompletedRun(124, e.stdout or "", f"timeout after {timeout_s}s")
    except Exception as e:  # pragma: no cover - defensive
        return CompletedRun(1, "", f"{type(e).__name__}: {e}")


# ------------------------------------------------------------ dispatch + poll ----
@dataclass
class DispatchOutcome:
    job_id: int | None
    status: str
    result: str
    agent_name: str
    delegation_id: str | None
    api_calls: int
    polls: int
    error: str = ""

    @property
    def ok(self) -> bool:
        return is_ok(self.status) and not self.error


def _spawn_watermark(token_db: str, session_id: str) -> int:
    """Max delegation_spawns.id for a session BEFORE dispatch (0 if unreadable). The newest row
    with id > watermark after dispatch is this dispatch's delegation_id (H7 race guard)."""
    if not token_db or not session_id or not Path(token_db).exists():
        return 0
    try:
        con = sqlite3.connect(token_db)
        r = con.execute(f"SELECT COALESCE(MAX(id),0) FROM {LA.SPAWNS} WHERE session_id=?",
                        (session_id,)).fetchone()
        return int(r[0]) if r else 0
    except Exception:
        return 0


def _capture_delegation(token_db: str, session_id: str, watermark: int) -> str | None:
    if not token_db or not session_id or not Path(token_db).exists():
        return None
    try:
        con = sqlite3.connect(token_db)
        return LA.capture_dispatch_delegation(con, session_id, watermark)
    except Exception:
        return None


def dispatch_and_wait(role: str, prompt: str, *, runner=subprocess_runner,
                      poll_interval_s: float = 6.0, max_polls: int = 300,
                      token_db: str = "", session_id: str = "", sleep=time.sleep,
                      **argv_kwargs) -> DispatchOutcome:
    """Dispatch one delegate and poll to a terminal state. Captures the dispatch's delegation_id
    from `delegation_spawns` (if `token_db`/`session_id` are given) using a before/after watermark.

    `runner` is injected so CI drives a fake fleet; the live path uses `subprocess_runner`. All
    fleet/DB errors are folded into the outcome (never raised) so a benchmark cell degrades to a
    recorded failure rather than crashing the suite."""
    aimee_bin = argv_kwargs.get("aimee_bin", "aimee")
    watermark = _spawn_watermark(token_db, session_id)
    argv = build_delegate_argv(role, prompt, **argv_kwargs)
    disp = runner(argv)
    job_id = parse_dispatch(disp.stdout)
    if job_id is None:
        return DispatchOutcome(None, "error", "", "", None, 0, 0,
                               error=f"dispatch produced no job_id (rc={disp.rc}): "
                                     f"{(disp.stderr or disp.stdout)[:200]}")
    delegation_id = _capture_delegation(token_db, session_id, watermark)
    st = DelegateStatus(job_id, "pending", "", "", 0)
    for polls in range(1, max_polls + 1):
        s = runner(build_status_argv(job_id, aimee_bin=aimee_bin))
        st = parse_status(s.stdout)
        if st.terminal:
            # delegation_id may only materialize once the job registers its spawn row.
            if delegation_id is None:
                delegation_id = _capture_delegation(token_db, session_id, watermark)
            return DispatchOutcome(job_id, st.status, st.result, st.agent_name, delegation_id,
                                   st.api_calls, polls)
        sleep(poll_interval_s)
    return DispatchOutcome(job_id, st.status or "timeout", st.result, st.agent_name, delegation_id,
                           st.api_calls, max_polls, error="poll budget exhausted")


# ------------------------------------------------------------ token attribution -
def read_split(token_db: str, primary_did: str | None, worker_dids: list[str]) -> dict | None:
    """Primary-vs-worker token split for a completed run (delegation_id partitioned, realized
    only). Returns None if the DB is unreadable or the primary delegation was never captured."""
    if not token_db or not primary_did or not Path(token_db).exists():
        return None
    try:
        con = sqlite3.connect(token_db)
        return LA.primary_worker_split(con, primary_did, [w for w in worker_dids if w])
    except Exception:
        return None


def read_realized_by_jobs(token_db: str, model: str, job_ids: list) -> tuple[int, int, int, int]:
    """(input_uncached, input_cached, output, rows) for one agent's realized turns over the given
    dispatch job_ids. Reuses PR #986's job-id scoping: a delegate turn's delegation_id ends in
    `-<job_id>` (the CLI job_id), so `delegation_id LIKE '%-<job_id>'` selects exactly this run's
    turns for `model` — no time window, no cross-run/cross-agent contamination. UNCACHED input
    (prompt - cache_read) is the headline denominator (B2a: multi-turn re-reads must not skew A/C).

    This is the LIVE-deployment attribution (delegation_id NON-EMPTY, partitioned by job_id),
    superseding the EMPTY-polarity model for real runs (swebench_live_attribution FINDING 1)."""
    jids = [j for j in (job_ids or []) if j is not None]
    if not token_db or not model or not jids or not Path(token_db).exists():
        return (0, 0, 0, 0)
    try:
        con = sqlite3.connect(token_db)
        likes = " OR ".join(["delegation_id LIKE ?"] * len(jids))
        params = [model] + [f"%-{j}" for j in jids]
        r = con.execute(
            f"SELECT COALESCE(SUM(prompt_tokens),0), COALESCE(SUM(cache_read_tokens),0), "
            f"COALESCE(SUM(completion_tokens),0), COUNT(*) FROM {LA.AUDIT} "
            f"WHERE model=? AND usage_kind='realized' AND ({likes})", params).fetchone()
        prompt, cache_read, completion, rows = r
        return (max(0, prompt - cache_read), cache_read, completion, rows)
    except Exception:
        return (0, 0, 0, 0)


def read_worker_tokens_by_jobs(token_db: str, job_ids: list) -> tuple[int, int]:
    """(input, output) summed over the workers' dispatch job_ids, MODEL-AGNOSTIC (arm-C workers
    span many models). Priced $0 downstream — reported only for the honesty panel."""
    jids = [j for j in (job_ids or []) if j is not None]
    if not token_db or not jids or not Path(token_db).exists():
        return (0, 0)
    try:
        con = sqlite3.connect(token_db)
        likes = " OR ".join(["delegation_id LIKE ?"] * len(jids))
        params = [f"%-{j}" for j in jids]
        r = con.execute(
            f"SELECT COALESCE(SUM(prompt_tokens),0), COALESCE(SUM(completion_tokens),0) "
            f"FROM {LA.AUDIT} WHERE usage_kind='realized' AND ({likes})", params).fetchone()
        return (int(r[0]), int(r[1]))
    except Exception:
        return (0, 0)


def supervisor_tool_call_rows(token_db: str, delegation_id: str) -> int:
    """H1/H2 runtime assertion support: count token_audit rows for a delegation that name a tool.
    For the arm-C supervisor (dispatched with NO --tools) this MUST be 0 — any non-zero value
    means the 'no raw code' structural guarantee was violated. Returns -1 if unreadable."""
    if not token_db or not delegation_id or not Path(token_db).exists():
        return -1
    try:
        con = sqlite3.connect(token_db)
        r = con.execute(
            f"SELECT COUNT(*) FROM {LA.AUDIT} WHERE delegation_id=? AND usage_kind='realized' "
            f"AND tool_name IS NOT NULL AND tool_name<>''", (delegation_id,)).fetchone()
        return int(r[0]) if r else 0
    except Exception:
        return -1


def argv_str(argv: list[str]) -> str:
    """A copy-pasteable shell rendering of a dispatch argv (for runbook/logging)."""
    return " ".join(shlex.quote(a) for a in argv)
