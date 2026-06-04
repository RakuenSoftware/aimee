# Virtual-Context Assembly: Rollout Validation Report

Closes the operational acceptance criteria of
[virtual-context-assembly-rollout-validation.md](../proposals/done/virtual-context-assembly-rollout-validation.md).
The parent feature
([virtual-context-assembly-and-tool-chain-paging.md](../proposals/done/virtual-context-assembly-and-tool-chain-paging.md))
shipped the assembler, deterministic stubbing, the `session_context_*` tools,
and the benchmark gate; this validates them for real and flips the default on.

## Summary

| Acceptance criterion | Status | Evidence |
|---|---|---|
| Gate passes on a real (non-synthetic) tool-heavy transcript | ✅ | §1, `fixture_real_session.json` |
| Manual inspection: compacted signal, not raw duplicate traffic; expand recovers raw | ✅ | §2, `make virtual-context-inspect` |
| Operational metrics collected, no late-turn regression beyond 0.01 | ✅ | §1 (accuracy) + §3 (metrics surface) |
| Default flipped to on with documented rollback | ✅ | §4 |
| Dashboard + rebuild-backlog / expansion-failure alert thresholds | ✅ | §5 |

## §1: Real-session benchmark validation (AC#1)

`run_eval.py` now takes a repeatable `--fixture` flag and the
`virtual-context-eval-check` make target runs **two** fixtures: the original
synthetic one and a new non-synthetic `fixture_real_session.json` captured from
a real aimee coding session (each chain is a real Read/Bash/Edit chain over real
repository files; every oracle answer is a symbol that exists in this repo:
`run_eval.py`, `virtual_context_enabled`, `build_stub`, `db1_conv_list_chains`).

```
$ make virtual-context-eval-check
===== fixture_tool_heavy.json =====
reduction       : 97.7%  (required >= 40%)
PASS: compression criterion (97.7% >= 40%)
... 4/4 oracles PASS ...
PASS: accuracy criterion (overall +0.50 >= -0.01, long-context +1.00 >= +0.05)

===== fixture_real_session.json =====
reduction       : 96.2%  (required >= 40%)
PASS: compression criterion (96.2% >= 40%)
... 6/6 oracles PASS ...
baseline acc    : 0.50  (all tasks)
compacted acc   : 1.00  (all tasks)
overall delta   : +0.50  (required >= -0.01)
long-ctx delta  : +0.75  (required >= +0.05)
PASS: accuracy criterion (overall +0.50 >= -0.01, long-context +0.75 >= +0.05)

All acceptance criteria met across 2 fixture(s).
```

The accuracy gate is the late-turn-quality check: under a fixed live-prompt
budget, budget-truncated raw history drops the old long-context answers
(`baseline_long = 0.25`) while compacted stubs retain them (`compacted_long =
1.00`), with **no overall regression** (compacted ≥ baseline by +0.50, well
inside the 0.01 tolerance). AC#3's "no regression beyond 0.01" holds.

## §2: Manual inspection (AC#2)

`make virtual-context-inspect` builds and runs `inspect_real_session.c`, which
drives the **real** public path with the flag on
(`conv_ctx_record_event` → `conv_ctx_flush_pending` → `conv_ctx_assemble`, plus
`db1_conv_search_chains` / `db1_conv_chain_events`, the same functions the
`session_context_search` / `_expand` / `_status` MCP tools wrap) over a
realistic 7-event tool-heavy sequence. No LLM or network required, so it is a
reproducible inspection artifact rather than a one-off screenshot.

Observed (verbatim):

```
--- Chains (compaction is real, stub_bytes < raw_bytes) ---
  chain #2 [read_file,bash]  raw=3900 stub=210
  chain #1 [read_file,bash]  raw=12000 stub=233

--- Assembled working set (conv_ctx_assemble): compacted stubs, NOT replayed raw traffic ---
# Session Activity
- [chain 1] Tools: read_file,bash. Files: src/config.c,src/conversation_context.c,src/db1/conv_context.h. Excerpt: config_load parses aimee.yaml; the session.virtual_context block sets virtual_context_enabled...

--- session_context_search('virtual_context') hit ---
  1 chain(s) matched; first stub: Tools: read_file,bash. Files: src/config.c,...

--- session_context_expand: raw recovery for chain #2 ---
  recovered 2 raw event(s):
    [read_file] run_eval.py is the deterministic gate ...
    [bash] All acceptance criteria met across 2 fixture(s).
```

This confirms the three things the parent Test Plan asked a human to check:

1. **Compacted signal, not raw duplicate traffic.** The assembled working set is
   query-relevant chain stubs (`# Session Activity` list), not replayed raw
   read/search/bash output. Every chain's `stub_bytes` ≪ `raw_bytes`.
2. **Search works on stubs.** `session_context_search` returns the matching
   chain.
3. **Expand recovers raw on demand.** `session_context_expand` (via
   `db1_conv_chain_events`) returns the raw events behind a stub.

## §3: Operational metrics (AC#3)

`tool_session_context_status` now returns a `metrics` object derived from the
session's chains, giving the dashboard real data:

```
--- Operational metrics (session_context_status.metrics) ---
  segments_total      : 2
  chains_stubbed_total: 2
  raw_bytes_total     : 15900
  stub_bytes_total    : 443
  bytes_saved         : 15457
  compression_ratio   : 35.9x
  reduction           : 97.2%
```

Exposed keys: `session_context_segments_total`,
`session_tool_chains_stubbed_total`, `raw_bytes_total`, `stub_bytes_total`,
`session_context_bytes_saved`, `compression_ratio` (plus the existing
`event_count` / `chain_count` / `pending_events` fields).

## §4: Default flip + rollback (AC#4)

`config.c` now initializes `virtual_context_enabled = 1` (was `0`); the header
documents the new default. The full unit suite (`make unit-tests`, ~200
binaries) passes with the flag on by default; the only test that assumed the
compiled-off default (`test_assemble_disabled`) was updated to disable the flag
explicitly via config.

**Rollback:** set `session.virtual_context.enabled = false` in the active
`aimee.yaml` and restart the server. Raw turns remain the source of truth, so
the feature reverts with no schema loss. Blast radius is prompt-assembly quality
for long sessions; failure degrades to larger prompts, not data loss.

## §5: Dashboard + alerts (AC#5)

- [`docs/observability/virtual-context-dashboard.json`](../observability/virtual-context-dashboard.json)
  is a 7-panel Grafana dashboard over the metrics above (segments, stubbed,
  compression ratio, pending events, bytes saved, assembly latency p50/p95, and
  stub-expansion ok/error rate).
- [`docs/observability/virtual-context-alerts.md`](../observability/virtual-context-alerts.md)
  defines the **RebuildBacklog** alert (pending events accumulate while no segments
  emit → auto-flush stalled) and the **ExpandFailure** alert (>5% expand error
  rate → raw recovery broken), each with PromQL, for-duration, severity, and
  rationale, plus the rollback note.

## Reproducing

```
cd src
make virtual-context-eval-check    # §1 gate on both fixtures
make virtual-context-inspect       # §2/§3 live inspection + metrics
make unit-tests                    # §4 full suite green with default-on
```
