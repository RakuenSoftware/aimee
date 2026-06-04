# Virtual-Context Assembly: Operations & Alerts

The virtual-context-assembly feature coalesces long tool/event streams into
stubbed chain segments before they hit the model, reducing token spend and
latency. The `session_context_status` MCP tool exposes the live health
counters under its `metrics` object; this doc defines the canonical alert
rules and the safe rollback path for the feature toggle
`session.virtual_context.enabled`.

The companion dashboard is [`virtual-context-dashboard.json`](virtual-context-dashboard.json)
(import into Grafana, pick the scrape datasource for the `$datasource`
template var).

## Metrics

These are surfaced by `tool_session_context_status` (`metrics` object) and the
per-session `event_count` / `chain_count` / `pending_events` fields.

| Metric | Type | Meaning | Healthy range |
|---|---|---|---|
| `session_context_segments_total` | gauge | Tool-chains created for the session | monotonic; tracks active tool-heavy sessions |
| `session_tool_chains_stubbed_total` | gauge | Chains collapsed into a deterministic stub | equals `segments_total` (every chain is stubbed) |
| `session_context_bytes_saved` | gauge | `raw_bytes_total - stub_bytes_total` | > 0 and growing during long sessions |
| `compression_ratio` | gauge | `raw_bytes_total / stub_bytes_total` | ≥ 1.7 (gate floor is 40% reduction ≈ 1.7x); typically 25-45x |
| `session_context_expand_total` | counter (labels: `result={ok,error}`) | `session_context_expand` calls recovering raw turns | error ratio < 1% over 10m |
| `session_context_assembly_ms` | histogram | Wall time to assemble a stubbed working set | p95 < 50ms, p99 < 150ms |
| `pending_events` | gauge | Tool events not yet flushed into a chain | drains toward 0; auto-flush at 10 |

## Alert Rules

### 1. RebuildBacklog: auto-flush falling behind

Fires when pending (un-stubbed) tool events accumulate faster than they are
flushed into chains, i.e. the assembler is not keeping up with ingestion.

```promql
# pending_events is the per-session gauge exposed by session_context_status
(
  rate(session_context_segments_total[5m]) == 0
  and
  avg_over_time(aimee_session_context_pending_events[5m]) > 200
)
for: 10m
labels:
  severity: warning
annotations:
  summary: "Virtual-context rebuild backlog growing"
  description: "Pending tool events > 200 for 10m while no segments are being emitted. Auto-flush is stalled; expect raw-turn fallback or memory pressure."
```

**Rationale.** A sustained non-zero pending queue with zero segment emission
means the flusher is wedged or starved. 10 minutes survives a normal burst
(model round-trips, GC pauses) but catches a real regression before the queue
grows unbounded. The auto-flush threshold is 10 pending events
(`AUTO_FLUSH_THRESHOLD`), so a steady-state queue of 200 is well above normal.

### 2. ExpandFailure: raw recovery is failing

Fires when lazy expansion of a stub back into raw turns returns `ok=false`
at an elevated rate, meaning the recovery path itself is broken.

```promql
(
  sum(rate(session_context_expand_total{result="error"}[5m]))
  /
  sum(rate(session_context_expand_total[5m]))
) > 0.05
for: 5m
labels:
  severity: critical
annotations:
  summary: "Virtual-context expansion error rate > 5%"
  description: "More than 5% of session_context_expand calls have failed over the last 5 minutes. Raw-turn recovery is broken; affected sessions will see missing tool output."
```

**Rationale.** Expansion only runs when a model request needs the raw tool
output, so a high error rate here directly degrades response quality. 5% over
5 minutes is well above the natural noise floor (<0.1%) and short enough to
page before users hit multiple broken expansions per session.

## Rollback

The feature is gated by `session.virtual_context.enabled` in the active
`aimee.yaml` (under the aimee home dir / `AIMEE_HOME`). Set it to `false` and
restart the server; every session reverts to raw turns on the next request.
The flag is read at the chain/assembly boundary, so already-recorded events
remain queryable and **raw turns stay the source of truth, with no data loss**.
The only cost is the temporary loss of token savings while a regression is
investigated.
