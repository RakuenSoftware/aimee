#!/usr/bin/env python3
"""S0 — transport verification matrix for the AGENTIC supervised SWE-bench benchmark.

BLOCKING gate (proposal `docs/proposals/pending/agentic-supervised-swebench.md`, S0).
Before any graded agentic run we must PROVE that, for the multi-turn agentic loop, the
`token_audit` ledger attributes tokens correctly — otherwise the primary-token-reduction
headline is measuring the wrong thing. Single-turn scoping (PR #986) is necessary but not
sufficient once both arms run tool-using loops across many turns.

The ledger's contract (`src/db1/token_audit.h`): a row's `delegation_id` is EMPTY for a
primary-agent turn and NON-EMPTY for a delegate child; `session_id` is shared by parent and
child; `tool_name` names the turn's tool; `cache_read_tokens`/`cache_write_tokens` split the
prompt-cache. This module submits a KNOWN multi-turn agentic loop through each candidate
primary transport, then asserts FIVE properties over the resulting rows (scoped by the run's
session_id). A transport is only sanctioned for arms A/C if it passes ALL five; if none pass,
`main()` exits non-zero — fail closed.

The five assertions (map to the roundtable's B2 leak-list + the polarity check):
  P1 primary_captured     — the primary's OWN turns land with delegation_id EMPTY, model=
                            primary, usage_kind='realized', count >= expected primary turns
                            (proves the multi-turn primary loop is measured at all).
  P2 worker_attributed    — worker turns land with delegation_id NON-EMPTY (so they are
                            excluded from the primary bill and priced $0).
  P3 no_cross_bill        — a large worker return (>=5k tokens) does NOT inflate the primary's
                            prompt_tokens (B2b: worker tool-result content billed to worker).
  P4 cache_split          — cached vs uncached primary tokens are separable, so the headline
                            can use UNCACHED tokens (B2a: cache re-reads must not skew A/C).
  P5 primary_tools_billed — the primary's own tool turns (non-empty tool_name) carry
                            delegation_id EMPTY, i.e. they are billed to the primary and can be
                            allowlist-gated (B2c: primary-side tools must not bypass the split).

The assertions are pure functions over row dicts so CI exercises them in FAKE mode with
synthesized ledgers (AIMEE_BENCH_FAKE_AGENT=1); the live path fills the rows from a real
`token_audit` after driving each transport against a running server (the .254 fleet).
"""
from __future__ import annotations

import argparse
import json
import os
import sqlite3
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Callable

_FAKE = os.environ.get("AIMEE_BENCH_FAKE_AGENT") == "1"

# A worker return at/above this size is the "big return" probe for P3.
_BIG_WORKER_RETURN_TOKENS = 5000
# The primary's prompt for the probe task is small; if its prompt_tokens exceed this the
# worker's return has leaked into the primary bill. Generous so a real short loop passes.
_PRIMARY_PROMPT_CEILING = 2500


# ------------------------------------------------------------ ledger rows -----
def _row(model, delegation_id="", tool_name="", prompt=0, completion=0,
         cache_read=0, cache_write=0, usage_kind="realized", session_id="s0"):
    """One token_audit row as a dict (the shape both the live query and FAKE mode produce)."""
    return {"model": model, "delegation_id": delegation_id or "", "tool_name": tool_name or "",
            "prompt_tokens": prompt, "completion_tokens": completion,
            "cache_read_tokens": cache_read, "cache_write_tokens": cache_write,
            "usage_kind": usage_kind, "session_id": session_id}


def _primary_rows(rows, primary_model):
    """Realized primary turns: delegation_id EMPTY (the ledger's primary-agent marker)."""
    return [r for r in rows if r["model"] == primary_model
            and not r["delegation_id"] and r["usage_kind"] == "realized"]


def _worker_rows(rows):
    """Realized delegate turns: delegation_id NON-EMPTY."""
    return [r for r in rows if r["delegation_id"] and r["usage_kind"] == "realized"]


# ------------------------------------------------------------ assertions ------
# Each returns (ok: bool, detail: str). Pure — unit-tested without a server.
def assert_primary_captured(rows, primary_model, min_turns) -> tuple[bool, str]:
    pr = _primary_rows(rows, primary_model)
    n = len(pr)
    ok = n >= min_turns and any(r["completion_tokens"] > 0 for r in pr)
    return ok, f"{n} primary (delegation_id EMPTY) realized turns; need >= {min_turns} with output"


def assert_worker_attributed(rows, primary_model) -> tuple[bool, str]:
    wr = _worker_rows(rows)
    # Worker turns must exist AND none may be billed under the primary model with EMPTY
    # delegation_id (that would be cross-attribution).
    stray = [r for r in wr if r["model"] == primary_model]
    ok = len(wr) >= 1 and not stray
    return ok, (f"{len(wr)} worker (delegation_id NON-EMPTY) turns"
                + (f"; {len(stray)} billed under primary model!" if stray else ""))


def assert_no_cross_bill(rows, primary_model, big_return_tokens) -> tuple[bool, str]:
    pr = _primary_rows(rows, primary_model)
    max_primary_prompt = max((r["prompt_tokens"] for r in pr), default=0)
    # If a >=5k worker return had leaked into the primary's context bill, the primary's
    # per-turn prompt would jump by roughly big_return_tokens. It must stay under the ceiling.
    ok = big_return_tokens >= _BIG_WORKER_RETURN_TOKENS and max_primary_prompt <= _PRIMARY_PROMPT_CEILING
    return ok, (f"max primary prompt_tokens={max_primary_prompt} (ceiling {_PRIMARY_PROMPT_CEILING}); "
                f"worker return was {big_return_tokens} tok")


def assert_cache_split(rows, primary_model) -> tuple[bool, str]:
    pr = _primary_rows(rows, primary_model)
    cache_read = sum(r["cache_read_tokens"] for r in pr)
    prompt = sum(r["prompt_tokens"] for r in pr)
    # The columns must be present and the uncached headline (prompt - cache_read) must be
    # computable and non-negative. A multi-turn primary re-reads context, so >0 cache_read
    # is expected; we require the split to be SEPARABLE, not a specific hit rate.
    uncached = prompt - cache_read
    ok = all("cache_read_tokens" in r and "cache_write_tokens" in r for r in pr) and uncached >= 0
    return ok, f"prompt={prompt} cache_read={cache_read} -> uncached headline={uncached}"


def assert_primary_tools_billed(rows, primary_model) -> tuple[bool, str]:
    tool_rows = [r for r in rows if r["tool_name"] and r["usage_kind"] == "realized"
                 and r["model"] == primary_model]
    # A primary-side tool turn MUST carry delegation_id EMPTY (billed to primary). Any
    # primary-model tool turn with a NON-EMPTY delegation_id is a bypass of the split.
    bypass = [r for r in tool_rows if r["delegation_id"]]
    ok = len(tool_rows) >= 1 and not bypass
    return ok, (f"{len(tool_rows)} primary tool turns"
                + (f"; {len(bypass)} bypass the split (non-empty delegation_id)!" if bypass else ""))


def verify_rows(rows, primary_model, expected_primary_turns, big_return_tokens) -> dict:
    """Run all five assertions; return {name: {ok, detail}} plus an overall `passed`."""
    checks = {
        "P1_primary_captured": assert_primary_captured(rows, primary_model, expected_primary_turns),
        "P2_worker_attributed": assert_worker_attributed(rows, primary_model),
        "P3_no_cross_bill": assert_no_cross_bill(rows, primary_model, big_return_tokens),
        "P4_cache_split": assert_cache_split(rows, primary_model),
        "P5_primary_tools_billed": assert_primary_tools_billed(rows, primary_model),
    }
    out = {k: {"ok": ok, "detail": d} for k, (ok, d) in checks.items()}
    out["passed"] = all(v["ok"] for v in out.values())
    return out


# ------------------------------------------------------------ live probe ------
@dataclass
class TransportProbe:
    """A candidate primary transport and how to drive a short multi-turn agentic loop
    through it. `invoke` returns the run's session_id (all turns share it)."""
    name: str
    invoke: Callable[["ProbeCtx"], str]
    expected_primary_turns: int = 2


@dataclass
class ProbeCtx:
    aimee_bin: str
    worker: str
    workdir: str
    big_return_tokens: int = _BIG_WORKER_RETURN_TOKENS
    extra: dict = field(default_factory=dict)


# --- LIVE TRANSPORT WIRING IS PENDING (resolved against the running server) -------------
# The five assertions above + their FAKE-mode tests are the CI-ready deliverable of S0.
# Driving a real multi-turn agentic loop is the part that genuinely needs the .254 server,
# and the entrypoint is NOT a CLI subcommand: `/v1/runs` is an OpenAI-compatible HTTP route
# (see src/cli_v1_routes.c, test_openai_runs_store.c), and "agent_shell" is a server-side
# execution mode, not `aimee run`. Rather than call a phantom CLI, each probe raises a
# descriptive error so `run_matrix` records an honest FAIL with the exact wiring to do next.
# When the live transport lands, replace the body with the real HTTP submit + poll and
# return the run's session_id (all turns share it). The task must force >=2 primary turns
# and one delegated worker turn whose return is padded to `big_return_tokens` (the P3 probe).
_PROBE_TASK = ("Read PROBE.txt, delegate a summary of it to a worker, then write the "
               "length of the worker's reply into PROBE_OUT.txt. Use tools.")


def _invoke_v1_runs(ctx: ProbeCtx) -> str:
    raise NotImplementedError(
        "live /v1/runs probe not wired: POST an agentic run to /v1/runs on the server, poll "
        "to completion, return its session_id. Task: " + _PROBE_TASK)


def _invoke_agent_shell(ctx: ProbeCtx) -> str:
    raise NotImplementedError(
        "live agent_shell probe not wired: submit the probe task via the server-side "
        "agent_shell execution mode and return its session_id. Task: " + _PROBE_TASK)


DEFAULT_PROBES = [
    TransportProbe("v1_runs", _invoke_v1_runs),
    TransportProbe("agent_shell", _invoke_agent_shell),
]


_COLS = ("model", "delegation_id", "tool_name", "prompt_tokens", "completion_tokens",
         "cache_read_tokens", "cache_write_tokens", "usage_kind", "session_id")


def collect_rows(db: str, session_id: str) -> list[dict]:
    """Read all token_audit rows for one run (scoped by the shared session_id)."""
    if not db or not Path(db).exists() or not session_id:
        return []
    con = sqlite3.connect(db)
    con.row_factory = sqlite3.Row
    q = f"SELECT {', '.join(_COLS)} FROM token_audit WHERE session_id=?"
    return [dict(r) for r in con.execute(q, (session_id,)).fetchall()]


# ------------------------------------------------------------ fake ledgers ----
def _fake_rows(scenario: str, primary="gpt-5.5", worker="glm-5.2"):
    """Synthesize a token_audit for CI. 'pass' satisfies all five; the others each break
    exactly one assertion so the tests prove the checks actually catch regressions."""
    did = "deleg-1-1730000000-job42"
    ok = [
        _row(primary, prompt=800, completion=120, cache_read=200, tool_name="read_file"),
        _row(primary, prompt=1400, completion=90, cache_read=600, tool_name="apply_patch"),
        _row(worker, delegation_id=did, prompt=1200, completion=_BIG_WORKER_RETURN_TOKENS),
    ]
    if scenario == "pass":
        return ok
    if scenario == "no_primary":            # breaks P1
        return [r for r in ok if r["model"] != primary]
    if scenario == "no_worker":             # breaks P2
        return [r for r in ok if not r["delegation_id"]]
    if scenario == "worker_under_primary":  # breaks P2 (stray primary-model delegate)
        return ok + [_row(primary, delegation_id=did, prompt=10, completion=10)]
    if scenario == "cross_bill":            # breaks P3 (worker return leaked into primary)
        return [_row(primary, prompt=_PRIMARY_PROMPT_CEILING + _BIG_WORKER_RETURN_TOKENS,
                     completion=120, tool_name="read_file")] + ok[1:]
    if scenario == "tool_bypass":           # breaks P5 (primary tool turn with delegation_id)
        return [_row(primary, delegation_id=did, tool_name="bash", prompt=50, completion=5)] + ok
    raise ValueError(scenario)


# ------------------------------------------------------------ matrix ----------
def run_matrix(db, primary_model, probes, ctx: ProbeCtx) -> dict:
    results = {}
    for pr in probes:
        try:
            sid = pr.invoke(ctx)
            rows = collect_rows(db, sid)
            v = verify_rows(rows, primary_model, pr.expected_primary_turns, ctx.big_return_tokens)
            v["session_id"] = sid
            v["n_rows"] = len(rows)
        except Exception as e:  # a transport that errors is a FAIL, not a crash
            v = {"passed": False, "error": f"{type(e).__name__}: {e}"}
        results[pr.name] = v
    return results


def render(results: dict) -> str:
    lines = ["transport verification matrix (S0):"]
    for name, v in results.items():
        verdict = "PASS" if v.get("passed") else "FAIL"
        lines.append(f"  [{verdict}] {name}" + (f"  ({v['error']})" if v.get("error") else ""))
        for k in ("P1_primary_captured", "P2_worker_attributed", "P3_no_cross_bill",
                  "P4_cache_split", "P5_primary_tools_billed"):
            if k in v:
                mark = "ok" if v[k]["ok"] else "XX"
                lines.append(f"      {mark} {k}: {v[k]['detail']}")
    sanctioned = [n for n, v in results.items() if v.get("passed")]
    lines.append(f"sanctioned transports: {sanctioned or 'NONE — blocked'}")
    return "\n".join(lines)


def main() -> None:
    ap = argparse.ArgumentParser(description="S0 transport verification matrix")
    ap.add_argument("--token-db", default=os.environ.get("AIMEE_DB", ""))
    ap.add_argument("--primary-model", default=os.environ.get("AIMEE_BENCH_PRIMARY_MODEL", "gpt-5.5"))
    ap.add_argument("--worker", default=os.environ.get("AIMEE_BENCH_POOL", "glm-5.2").split(",")[0])
    ap.add_argument("--aimee-bin", default=os.environ.get("AIMEE_BENCH_CLIENT", "./aimee"))
    ap.add_argument("--workdir", default=".")
    ap.add_argument("--output", default="")
    args = ap.parse_args()

    if _FAKE:
        results = {p.name: {**verify_rows(_fake_rows("pass", args.primary_model, args.worker),
                                          args.primary_model, p.expected_primary_turns,
                                          _BIG_WORKER_RETURN_TOKENS),
                            "session_id": "fake", "n_rows": 3} for p in DEFAULT_PROBES}
    else:
        if not args.token_db:
            raise SystemExit("--token-db is required for the live matrix")
        ctx = ProbeCtx(args.aimee_bin, args.worker, args.workdir)
        results = run_matrix(args.token_db, args.primary_model, DEFAULT_PROBES, ctx)

    print(render(results), file=sys.stderr)
    if args.output:
        Path(args.output).write_text(json.dumps(results, indent=2))
    # Fail closed: exit non-zero unless at least one transport passes ALL assertions.
    if not any(v.get("passed") for v in results.values()):
        raise SystemExit(2)


if __name__ == "__main__":
    main()
