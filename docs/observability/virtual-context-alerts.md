# Virtual-context alerts

Virtual context replaces old tool-event chains with compact references while keeping raw turns
recoverable. Alert on loss of recovery, not merely on low compression.

## Signals

| Signal | Healthy |
| --- | --- |
| pending events | returns toward zero after a burst |
| segments created vs. stubbed | track each other |
| bytes saved | grows during long tool sessions |
| compression ratio | above the configured quality gate |
| expansion errors | below 1% over a useful window |
| assembly latency | within the interactive budget |

## Pages

- Page when raw expansion fails for more than 5% of attempts over five minutes. Users can lose tool
  evidence.
- Warn when pending events stay far above the auto-flush threshold for ten minutes while no segments
  are created. The assembler is stalled or starved.
- Warn when compression collapses below its gate after a config/model change, but do not page if raw
  recovery and latency remain healthy.

Use the metric names emitted by `session_context_status`; do not copy a PromQL name into deployment
alerts until the exporter exposes it with that exact prefix and labels.

## Roll back

Disable `session.virtual_context.enabled` and restart the server. Raw turns remain the source of
truth, so rollback trades token savings for direct context without deleting session data.

Before re-enabling, reproduce expansion, backlog, and assembly latency with the failed session shape.
