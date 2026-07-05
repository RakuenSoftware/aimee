#!/usr/bin/env python3
"""S2 — arm A (agentic-solo) runner for the agentic supervised SWE-bench benchmark (#987).

Arm A: the expensive primary drives the S1 agentic loop itself (explore/edit/[test]/verify) and
we count ITS tokens. This module composes the already-merged pieces into one per-instance record
in the shape `supervised_report.build_report` consumes, plus the agentic-arm honesty fields the
roundtables ratified:
  - token accounting from the token_audit ledger with S0's polarity (primary = delegation_id
    EMPTY); the HEADLINE input is UNCACHED (prompt_tokens - cache_read_tokens) so a multi-turn
    primary re-reading context across turns cannot skew the A vs C comparison (proposal B2a);
  - two wall-clocks per instance (proposal Q5 / issue #987): total (queue+work) and work-only,
    with queue = total - work; the report's p95 gate reads the headline `wall_s`;
  - the S1 canonical patch + its fingerprint, secret-redacted before it can reach a ledger.

Pure surface (unit-tested, no live server/docker/network): the token math, the wall-clock
decomposition, and record-schema conformance. The live loop is S1's marked stub. Arm C's
supervision loop (S3) reuses this record schema and token accounting — the SAME core for both
arms so a transport difference can never masquerade as an algorithmic one (roundtable Q3).

KNOWN LIMITATION (surfaced, not faked): token_audit has no separate reasoning/thinking-token
column (only prompt/completion/cache_read/cache_write), so the proposal's thinking-vs-text split
is NOT extractable today. `thinking_tokens` is reported as None until the ledger carries it.
"""
from __future__ import annotations

import os
from dataclasses import dataclass, field
from pathlib import Path

import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from benchmarks.coding import swebench_transport_verify as V
from benchmarks.coding import swebench_agentic_harness as H

_FAKE = os.environ.get("AIMEE_BENCH_FAKE_AGENT") == "1"


# ------------------------------------------------------------ token totals -----
@dataclass
class PrimaryTokens:
    input_uncached: int   # HEADLINE: prompt_tokens - cache_read_tokens
    input_cached: int     # cache_read_tokens (re-read context; excluded from headline)
    output: int
    turns: int
    thinking: int | None = None  # unavailable from token_audit today (see module docstring)

    @property
    def input_total(self) -> int:
        return self.input_uncached + self.input_cached

    @property
    def total_headline(self) -> int:
        """The number the reduction headline uses: uncached input + output."""
        return self.input_uncached + self.output


def primary_token_totals(rows, primary_model) -> PrimaryTokens:
    """Sum the primary's realized turns (delegation_id EMPTY, per S0's verified polarity)."""
    pr = V._primary_rows(rows, primary_model)
    prompt = sum(r["prompt_tokens"] for r in pr)
    cache_read = sum(r["cache_read_tokens"] for r in pr)
    return PrimaryTokens(
        input_uncached=max(0, prompt - cache_read),
        input_cached=cache_read,
        output=sum(r["completion_tokens"] for r in pr),
        turns=len(pr),
    )


def worker_token_totals(rows) -> tuple[int, int]:
    """(input, output) across delegate children (delegation_id NON-EMPTY); priced $0 (honesty)."""
    wr = V._worker_rows(rows)
    return sum(r["prompt_tokens"] for r in wr), sum(r["completion_tokens"] for r in wr)


# ------------------------------------------------------------ wall-clock -------
@dataclass
class WallClock:
    """Per-instance wall-clock. t0 = instance prompt enters the harness dispatch queue (pinned
    IDENTICALLY for both arms, proposal Q5); first_work = first worker/primary turn starts;
    t1 = patch ready (grader resolves downstream). We report total and work-only; queue is the
    gap. Fleet throughput is a SEPARATE number, never conflated here."""
    t0: float
    first_work: float
    t1: float

    @property
    def total_s(self) -> float:
        return max(0.0, self.t1 - self.t0)

    @property
    def work_s(self) -> float:
        return max(0.0, self.t1 - self.first_work)

    @property
    def queue_s(self) -> float:
        return max(0.0, self.first_work - self.t0)


# ------------------------------------------------------------ arm record -------
def build_arm_record(instance_id, arm, primary_model, *, tokens: PrimaryTokens,
                     worker_in: int, worker_out: int, wall: WallClock, resolved,
                     patch: str, base_commit: str, repo: str, n_workers: int = 1,
                     redactions: int = 0, escalations: int = 0, compaction: bool = False,
                     invalid: bool = False) -> dict:
    """One record in supervised_report's schema + the agentic-arm honesty fields."""
    return {
        # --- schema consumed by supervised_report.build_report ---
        "instance_id": instance_id,
        "arm": arm,
        "compaction": compaction,
        "supervisor_input_tokens": tokens.input_uncached,   # headline = UNCACHED
        "supervisor_output_tokens": tokens.output,
        "worker_input_tokens": worker_in,
        "worker_output_tokens": worker_out,
        "wall_s": round(wall.total_s, 3),
        "resolved": resolved,
        "n_workers": n_workers,
        "invalid": invalid,
        # --- agentic-arm honesty extensions (S4 report surfaces these) ---
        "supervisor_input_cached_tokens": tokens.input_cached,
        "supervisor_thinking_tokens": tokens.thinking,      # None until ledger carries it
        "primary_turns": tokens.turns,
        "wall_work_s": round(wall.work_s, 3),
        "wall_queue_s": round(wall.queue_s, 3),
        "escalations": escalations,
        "patch_fingerprint": H.workspace_fingerprint(repo, base_commit, patch),
        "patch_redactions": redactions,
    }


# ------------------------------------------------------------ fake mode --------
def _fake_arm_a_record(instance, primary_model):
    iid = instance["instance_id"]
    rows = V._fake_rows("pass", primary_model, "glm-5.2")
    tok = primary_token_totals(rows, primary_model)
    wi, wo = worker_token_totals(rows)
    wall = WallClock(t0=0.0, first_work=0.5, t1=12.5)
    patch, red = H.scan_and_redact_secrets("diff --git a/x b/x\n+ok\n")
    return build_arm_record(iid, "A", primary_model, tokens=tok, worker_in=wi, worker_out=wo,
                            wall=wall, resolved=True, patch=patch,
                            base_commit=instance.get("base_commit", "0"*40),
                            repo=instance.get("repo", "x/y"), redactions=red)


# ------------------------------------------------------------ live entry -------
def run_arm_a(instance: dict, primary_model: str, *, token_db: str, base_repo: str,
              allocator: "H.EnvAllocator", budget: "H.LoopBudget") -> dict:
    """Live arm-A run for one instance. Provisions a workspace, drives the primary agentic loop
    via the S0-verified /v1/runs transport, extracts the patch, and reads the primary's tokens
    from token_audit scoped to the run's session_id. Marked live (needs a server)."""
    if _FAKE:
        return _fake_arm_a_record(instance, primary_model)
    raise NotImplementedError(
        "live arm-A run not wired: provision via H.provision_workspace, drive H.run_agentic_loop "
        "(arm='A', /v1/runs transport), extract via H.extract_patch, then read tokens with "
        "V.collect_rows(token_db, session_id) + primary_token_totals(). Requires a live server.")
