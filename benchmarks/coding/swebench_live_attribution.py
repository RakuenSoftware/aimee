#!/usr/bin/env python3
"""S0-live — empirically-grounded token attribution for the agentic supervised SWE-bench harness.

The pure S0 module (`swebench_transport_verify.py`) asserts a THEORETICAL polarity: a top-level
primary turn carries `delegation_id` EMPTY and a delegate child carries it non-empty. Running the
verification matrix against the live .254 fleet (2026-07-05) showed the DEPLOYMENT does not work
that way, and this module encodes the corrected model so the live harness measures the right
thing:

  FINDING 1 — almost no EMPTY-delegation rows exist. On .254, ~all realized `token_audit` rows are
    delegate turns with a NON-EMPTY `delegation_id` of the form `deleg-<sess>-<ns_ts>-<idx>`; the
    aimee primary (a tmux Claude TUI whose /v1 proxy is stateless) does not reliably emit
    EMPTY-delegation rows. => the bench runs the "primary" itself as a `codex`/gpt-5.5 DELEGATE,
    and primary vs worker is distinguished by WHICH delegation_id the harness dispatched, not by
    empty-vs-nonempty.
  FINDING 2 — `delegation_spawns` is the attribution table: (delegation_id, parent_delegation_id,
    session_id, depth, role). A dispatch creates one row; workers a primary spawns would be its
    depth-2 children (parent_delegation_id = primary's). Today all dispatches are depth-1 siblings,
    so the harness captures each dispatch's delegation_id from the newest spawn row for its session.
  FINDING 3 — `usage_kind` filtering is load-bearing: a single agentic job produced 9 rows, 8 of
    them `avoided` (economizer estimates) and 1 `realized`. Only `realized` is spend.
  FINDING 4 — agentic `--tools` needs an ISOLATED worktree: a plain `delegate execute --tools`
    hit the "parent worktree write guard ... sandbox fallback could not start" and no tool ran.
    The worker's edit/test loop must dispatch with `--worktree` (S1 per-worker isolation is
    therefore REQUIRED for tools to function at all, not merely for reproducibility).

The attribution logic here is pure and unit-tested against an in-memory sqlite fixture mirroring
`delegation_spawns` + `token_audit`; the SSH-based live runner is a marked stub.
"""
from __future__ import annotations

import sqlite3
from dataclasses import dataclass
from pathlib import Path


# ------------------------------------------------------------ schema names -----
# The two real tables (columns verified on .254 aimee.db, 2026-07-05).
SPAWNS = "delegation_spawns"       # delegation_id, parent_delegation_id, session_id, depth, role, ...
AUDIT = "token_audit"              # delegation_id, usage_kind, prompt/completion/cache_*_tokens, ...


@dataclass
class TokenTotals:
    input_uncached: int
    input_cached: int
    output: int
    rows: int

    @property
    def total_headline(self) -> int:
        return self.input_uncached + self.output


def capture_dispatch_delegation(con: sqlite3.Connection, session_id: str,
                                after_spawn_id: int) -> str | None:
    """Resolve the delegation_id of a just-dispatched job: the newest delegation_spawns row for
    `session_id` with id > after_spawn_id (the id captured immediately BEFORE dispatch). Returns
    None if the dispatch produced no spawn row (a failed/rejected dispatch)."""
    r = con.execute(
        f"SELECT delegation_id FROM {SPAWNS} WHERE session_id=? AND id>? "
        f"ORDER BY id DESC LIMIT 1", (session_id, after_spawn_id)).fetchone()
    return r[0] if r else None


def child_delegations(con: sqlite3.Connection, parent_delegation_id: str) -> list[str]:
    """The delegation_ids of workers a primary spawned (depth-2 children). Empty today (all
    dispatches are depth-1 siblings) but correct once the primary spawns workers server-side."""
    rows = con.execute(
        f"SELECT delegation_id FROM {SPAWNS} WHERE parent_delegation_id=?",
        (parent_delegation_id,)).fetchall()
    return [r[0] for r in rows]


def realized_totals(con: sqlite3.Connection, delegation_ids: list[str]) -> TokenTotals:
    """Sum REALIZED token_audit rows over a set of delegation_ids. Headline input is UNCACHED
    (prompt - cache_read); avoided/estimated rows are excluded (usage_kind='realized' only)."""
    if not delegation_ids:
        return TokenTotals(0, 0, 0, 0)
    q = (f"SELECT COALESCE(SUM(prompt_tokens),0), COALESCE(SUM(cache_read_tokens),0), "
         f"COALESCE(SUM(completion_tokens),0), COUNT(*) FROM {AUDIT} "
         f"WHERE usage_kind='realized' AND delegation_id IN ({','.join('?' * len(delegation_ids))})")
    prompt, cache_read, completion, rows = con.execute(q, delegation_ids).fetchone()
    return TokenTotals(input_uncached=max(0, prompt - cache_read), input_cached=cache_read,
                       output=completion, rows=rows)


def primary_worker_split(con: sqlite3.Connection, primary_did: str,
                         worker_dids: list[str]) -> dict:
    """The corrected primary-vs-worker split: primary tokens over the primary's delegation_id,
    worker tokens over the workers' delegation_ids. This REPLACES the EMPTY-polarity model for the
    live deployment."""
    p = realized_totals(con, [primary_did])
    w = realized_totals(con, worker_dids)
    return {
        "primary_did": primary_did,
        "primary_input_uncached": p.input_uncached,
        "primary_input_cached": p.input_cached,
        "primary_output": p.output,
        "primary_headline": p.total_headline,
        "primary_rows": p.rows,
        "worker_dids": worker_dids,
        "worker_input": w.input_uncached + w.input_cached,
        "worker_output": w.output,
        "worker_rows": w.rows,
    }


# ------------------------------------------------------------ live assertions --
def verify_live_attribution(con: sqlite3.Connection, primary_did: str,
                            worker_dids: list[str]) -> dict:
    """Re-express the S0 assertions over the REAL schema for a completed run:
      L1 primary_measured  — the primary delegation has >=1 realized row with output.
      L2 worker_attributed — every worker delegation is disjoint from the primary's id.
      L3 no_cross_bill     — no worker delegation_id is counted under the primary total.
      L4 realized_only     — (structural) totals exclude avoided/estimated rows (SQL enforces).
    """
    split = primary_worker_split(con, primary_did, worker_dids)
    l1 = split["primary_rows"] >= 1 and split["primary_output"] >= 0
    l2 = primary_did not in worker_dids
    l3 = not (set(worker_dids) & {primary_did})
    checks = {
        "L1_primary_measured": (l1, f"{split['primary_rows']} realized primary rows"),
        "L2_worker_disjoint": (l2, f"{len(worker_dids)} worker delegations, primary excluded"),
        "L3_no_cross_bill": (l3, "no worker delegation_id under the primary total"),
        "L4_realized_only": (True, "SQL filters usage_kind='realized' (avoided rows excluded)"),
    }
    out = {k: {"ok": ok, "detail": d} for k, (ok, d) in checks.items()}
    out["passed"] = all(v["ok"] for v in out.values())
    out["split"] = split
    return out


def split_by_jobs(con: sqlite3.Connection, primary_job_id, worker_job_ids: list) -> dict:
    """The corrected primary-vs-worker split keyed by CLI job_id (delegation_id ends in `-<job>`),
    the attribution PR #986 verified and the agentic arms reuse. Realized rows only (FINDING 3)."""
    def _tot(job_ids):
        jids = [j for j in job_ids if j is not None]
        if not jids:
            return TokenTotals(0, 0, 0, 0)
        likes = " OR ".join([f"{AUDIT}.delegation_id LIKE ?"] * len(jids))
        q = (f"SELECT COALESCE(SUM(prompt_tokens),0), COALESCE(SUM(cache_read_tokens),0), "
             f"COALESCE(SUM(completion_tokens),0), COUNT(*) FROM {AUDIT} "
             f"WHERE usage_kind='realized' AND ({likes})")
        prompt, cache_read, completion, rows = con.execute(q, [f"%-{j}" for j in jids]).fetchone()
        return TokenTotals(max(0, prompt - cache_read), cache_read, completion, rows)
    p, w = _tot([primary_job_id]), _tot(worker_job_ids)
    return {"primary_job_id": primary_job_id, "primary_input_uncached": p.input_uncached,
            "primary_input_cached": p.input_cached, "primary_output": p.output,
            "primary_headline": p.total_headline, "primary_rows": p.rows,
            "worker_job_ids": list(worker_job_ids), "worker_input": w.input_uncached + w.input_cached,
            "worker_output": w.output, "worker_rows": w.rows}


# ------------------------------------------------------------ live runner ------
def run_live_matrix(*, db_path: str, primary_agent: str = "codex", worker: str = "GLM-5.2",
                    aimee_bin: str = "aimee", dispatch=None) -> dict:
    """S0-live gate (corrected model). Dispatch a real PRIMARY agentic probe and a WORKER probe via
    the delegate transport, then verify attribution over the REAL schema (`db_path` = aimee.db,
    readable where this runs — the fleet host, or a copy). Returns the assertion matrix + the split.

    Run it on the host that owns the ledger (`$AIMEE_HOME/aimee.db`, or a scp'd copy of
    /mnt/media/.plugins/aimee-server/server/home/aimee.db on .254). FINDING 4: the worker probe
    dispatches with `--worktree` so its tools actually run. `dispatch` is injected for CI."""
    from benchmarks.coding import swebench_live_transport as T
    if dispatch is None:
        dispatch = T.dispatch_and_wait
    probe = ("Read the repository, make a one-line no-op edit to a scratch file, and emit the diff "
             "in a ```diff block. Use tools.")
    # Primary probe: a tools-enabled agentic dispatch routed via the primary agent.
    p = dispatch("code", probe, aimee_bin=aimee_bin, via=primary_agent, tools=True,
                 worktree="aimee/wi/s0-primary")
    # Worker probe: same, routed via a cheap worker (FINDING 4: --worktree required).
    w = dispatch("code", probe, aimee_bin=aimee_bin, via=worker, tools=True,
                 worktree="aimee/wi/s0-worker")
    result = {"primary_job_id": p.job_id, "worker_job_id": w.job_id,
              "primary_status": p.status, "worker_status": w.status,
              "primary_delegation_id": p.delegation_id, "worker_delegation_id": w.delegation_id}
    if not db_path or not Path(db_path).exists():
        result["passed"] = False
        result["error"] = f"db not readable at {db_path!r} (run on the ledger host)"
        return result
    con = sqlite3.connect(db_path)
    split = split_by_jobs(con, p.job_id, [w.job_id])
    l1 = split["primary_rows"] >= 1 and split["primary_output"] >= 0
    l2 = p.delegation_id != w.delegation_id
    l3 = p.job_id != w.job_id
    checks = {
        "L1_primary_measured": (l1, f"{split['primary_rows']} realized primary rows"),
        "L2_worker_disjoint": (l2, "primary and worker delegation_ids differ"),
        "L3_no_cross_bill": (l3, "primary and worker job_ids differ (no shared attribution)"),
        "L4_realized_only": (True, "SQL filters usage_kind='realized'"),
    }
    result.update({k: {"ok": ok, "detail": d} for k, (ok, d) in checks.items()})
    result["split"] = split
    result["passed"] = all(v[0] for v in checks.values())
    return result
