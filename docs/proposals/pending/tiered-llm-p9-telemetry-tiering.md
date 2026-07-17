# Proposal: P9 — Telemetry tiering: logs & metrics to aimee-kb, enterprise export, PII posture

- **State:** proposed (pending — not started). Part of `tiered-llm-offering.md`.
- **Author:** JBailes (drafted by the engineer agent, 2026-07-17).
- **Depends on:** P1 (identity/teams — for tagging), the always-mTLS server↔kb
  channel (P2). Feeds P3 (attribution) and P5 (fleet view).

## Thesis

Telemetry follows the same tiering invariant as everything else: **user-specific
detail stays on aimee-server; the org-wide, aggregated, scrubbed view lives on
aimee-kb** — and from kb it must be exportable to the enterprise's own observability
stack. Two properties follow from the trust model: (a) aimee-server is guaranteed
single-user, so its *local* log view may show that user's PII; (b) anything
**forwarded to kb is multi-tenant**, so it must be scrubbed of secrets and **tagged
with the originating user/server**. Today no such pipeline exists: metrics are split
between a write-only counter set and a textfile export, and logs go to stderr plus
a local audit file with no forwarding.

## Goal

1. Forward all aimee-server logs and metrics to **aimee-kb** over the mTLS channel,
   tagged with the originating user + server (`cert:CN` / OIDC `sub`).
2. Have **aimee-kb expose and export** logs and metrics in a standardized form for
   enterprise ingestion: OpenTelemetry OTLP, a Prometheus `/metrics` endpoint, and
   structured JSON log export.
3. Apply a **split PII posture**: aimee-server may display PII-bearing logs locally
   (single user) but best-effort redacts plaintext secrets even there; data
   forwarded to kb is scrubbed of PII and secrets.

## §0 What already exists (and its gaps)

- **Rich cost telemetry** — `token_audit` DB1 table with per-request tokens→USD,
  attributed by model/source/role/tool/session/principal/delegation
  (`src/db1/token_audit.c`). Strong, but server-local; no team tag (P3 adds it), no
  forwarding.
- **Prometheus export is a textfile, not an endpoint** — `agent_write_metrics()`
  (`src/server/agent_policy.c:702-752`) writes `aimee.prom` from the *older*
  `agent_log` table, so it **lacks cost-USD and cache tokens**. No live `/metrics`
  HTTP endpoint; no OTLP.
- **`aimee_ir_metrics` counters are WRITE-ONLY** — `aimee_ir_metric_get` and friends
  (`src/headers/aimee_ir_metrics.h`) have **zero non-test callers**; the counters
  are incremented but never read, dumped, or exported. This dead-end observability
  must be made readable as part of this work.
- **Logs** — `aimee_log()` → stderr (`src/log.c`); a rotated JSON audit log at
  `~/.aimee/audit.log` (`log.c`); the WORM ledger for privileged actions. None are
  forwarded anywhere.

## §1 Server → kb telemetry forwarding

A forwarder on aimee-server ships logs and metrics to a kb ingest endpoint
(`POST /v1/telemetry/{logs,metrics}`) over the **always-mTLS** channel (P2).
Transport is batched, buffered, back-pressure-safe, and best-effort: telemetry
loss must never block egress or the user. kb aggregates fleet-wide, feeding the
P3 spend rollups and the P5 fleet view.

## §2 Mandatory user/server tagging

**Every** record forwarded to kb is tagged with its origin: the server's `cert:CN`
(always present — the per-server mTLS identity) and, when OIDC is configured, the
user `sub`; plus team (P1). Tagging happens at the forwarder and is **not
caller-supplied** — kb stamps and validates the tag from the authenticated mTLS
identity, so a record can never forge a different origin. No untagged record is
accepted.

## §3 Enterprise export from kb

kb exposes standardized surfaces for the org's own observability stack:
- **Metrics:** a live Prometheus **`/metrics`** endpoint and **OTLP** (OpenTelemetry)
  push/pull, covering the real cost/token/latency/egress dimensions (sourced from
  `token_audit`, not the thin `agent_log`) — labeled by team/provider/model.
- **Logs:** structured JSON export plus OTLP logs, filterable by team/user/time.
- **Traces (optional):** OTLP spans for the egress path (server → kb → vendor) so an
  org can see end-to-end latency.
This is the "plug aimee into our Datadog/Grafana/Splunk" story an enterprise expects.

## §4 Fix the write-only IR metrics

Wire a reader/exporter for `aimee_ir_metrics`, or fold its counters into the OTLP
and Prometheus surface, so IR passthrough-vs-fallback parity is observable in the
field. The roundtable-mandated shadow metrics are currently undiagnosable because
nothing reads them.

## §5 PII posture + redaction

- **aimee-server local display** may show PII (the single guaranteed user's own
  data), but a **best-effort secret redactor** scrubs plaintext passwords, API
  keys, bearer tokens, and known secret shapes from what is *displayed* —
  defence against shoulder-surfing, screen-share, and log-file leakage.
  Best-effort, explicitly not a guarantee.
- **Forwarded-to-kb data is scrubbed** of PII and secrets before it leaves the
  server — kb (multi-tenant, org-visible) must not receive the user's raw PII or
  any secret. The redaction boundary is the forwarder: local view permissive,
  forwarded view minimized and tagged.
- Reuse the vault's existing discipline of *never logging the secret itself*
  (`vault_service.c`); promote it into a shared redaction helper that both the
  display path and the forwarder call.

## Acceptance criteria

- A server log line and a metric sample arrive at kb, each **tagged** with the
  server `cert:CN` (plus user `sub`/team when present); an untagged or
  origin-forged record is rejected.
- kb `/metrics` returns Prometheus text with cost-USD and cache-token dimensions
  (not just the old `agent_log` subset); OTLP export produces equivalent data.
- A password or API key appearing in a server log is redacted in the local
  display and absent from anything forwarded to kb.
- IR parity metrics are readable via the export surface (no longer write-only).
- Telemetry backpressure or kb unavailability never blocks egress or user
  interaction (loss is acceptable; blocking is not).

## Testing

**Unit:** tag stamping from mTLS identity plus forge rejection; redactor on known
secret shapes (present in local view, scrubbed in forward view);
Prometheus/OTLP serialization including cost dimensions; IR-metric reader.
**Integration:** server→kb forward over mTLS with tags asserted; scrape kb
`/metrics` and an OTLP collector; kill kb and prove egress continues while
telemetry buffers and drops gracefully.

## Non-goals

No guaranteed PII elimination (server local display is best-effort by design, per
the single-user guarantee). No replacement of the enterprise's observability stack —
this exposes standardized feeds *into* it. No new metrics store on the server (the
server keeps `token_audit`; kb is the aggregation and export tier).
