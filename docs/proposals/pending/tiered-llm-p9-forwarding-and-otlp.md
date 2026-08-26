# P9 residual: telemetry forwarding and OTLP

- **State:** PENDING. Residual scope only.

**Archived parent:** [`tiered-llm-p9-telemetry-tiering.md`](../done/tiered-llm-p9-telemetry-tiering.md)

## Remaining deliverables

- Forward server telemetry to the configured authority with tenant and trace identity preserved.
- Export supported metrics, traces, and logs through OTLP with bounded queues and backpressure.
- Complete IR-stage latency, token, error, and fallback metrics.
- Define redaction, sampling, outage, retry, and drop-accounting behavior.
- Add interoperability and failure-mode tests against an OTLP collector.
