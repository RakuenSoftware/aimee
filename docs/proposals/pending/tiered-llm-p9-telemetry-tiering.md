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

1. Forward aimee-server telemetry to **aimee-kb** over the mTLS channel as an
   **allowlisted set of structured events and metric samples** (not free-form log
   text): only events whose schema is on the forward allowlist are eligible, and
   **anything unknown is dropped, not forwarded** (drop-on-unknown, fail-closed).
   Each record is tagged with the originating user + server (`cert:CN` / OIDC `sub`).
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

A forwarder on aimee-server ships an **allowlisted set of structured log events
and metric samples** to a kb ingest endpoint (`POST /v1/telemetry/{logs,metrics}`)
over the **always-mTLS** channel (P2). Only allowlisted event schemas are eligible;
unknown event types are dropped, not forwarded (fail-closed). Free-form
`aimee_log()` stderr / local audit text stays server-local and is never forwarded.
Transport is batched, buffered, back-pressure-safe, and best-effort: telemetry
loss must never block egress or the user. Any of the N stateless kb instances may
accept a batch; kb aggregates fleet-wide in **shared Postgres** (invariant #9), so
observability views are complete regardless of which instance ingested — feeding
**dashboards** and the P5 fleet view. **Authoritative billing and attribution is the
synchronous P3 egress audit row (DB2), written at egress — never this best-effort,
lossy telemetry stream.** A dropped telemetry batch changes a dashboard, never a
billed total or a budget decision; P9 telemetry enriches observability but is not a
source of truth for spend.

## §2 Mandatory user/server tagging

**Every** record forwarded to kb is tagged with its origin: the server's `cert:CN`
(always present — the per-server mTLS identity) and, when OIDC is configured, the
user `sub`; plus team (P1). The origin tag is **authoritative at kb, not the
forwarder**: kb derives `cert:CN` from the client cert it verified on the mTLS
handshake (the existing `kb_tls_peer_cn` path, `src/kb/http/kb_tls.c`), and any
origin field a server includes is advisory only — kb overwrites it and rejects the
record if a supplied origin disagrees with the authenticated `cert:CN`. The
forwarder may attach payload/team labels, but the server-identity tag is never
trusted from the wire. No untagged record is accepted (rejecting a forged/untagged
record is *validation*, not the "loss" the best-effort data plane tolerates). **If a
valid, non-revoked cert resolves to zero teams**, the record is tagged with `cert:CN`
and **no team**, and is retained only in an untenanted operator view — never attributed
to a team. **Per-request cert-revocation** applies to telemetry ingest and export too
(a revoked cert stops being accepted on its next request). **Ingest is bounded against
a hostile/looping server:** per-origin admission limits on batch size, queue depth,
payload size, and label cardinality, so one user-controlled server cannot exhaust kb
memory/connections/Postgres and degrade other tenants. The **actor token** (§2) has the
same concrete contract as P5's kb-signed JWT — `iss=kb`, `aud=kb`, subject, expiry,
`jti`, bound to the server `cert:CN` + session — with **replay state in shared
Postgres** (fleet-wide, not instance-local) bounded by the token TTL. Any authoritative
`sub` is **mapped to an opaque surrogate id before persistence/export** — the raw OIDC
`sub` is not stored in forwarded telemetry (keeping the opaque-id guarantee of §5) — and
because a raw `cert:CN` can itself contain a hostname/username, the **exported** origin is
also an **opaque server id**, not the raw CN. Replay state is a concrete `jti` table
`(jti PRIMARY KEY, exp)` in shared Postgres — dedup is an `INSERT … ON CONFLICT DO NOTHING`
that rejects a re-used `jti`, TTL-pruned; it is fleet-wide, not instance-local. The two
tag paths do **not** contradict: the *server-origin* tag is overwritten from the
authenticated `cert:CN` (and a disagreeing supplied origin rejects the record), while an
unverified *`sub`* is stripped — different fields, different handling. When a cert/composite
identity resolves to **multiple teams**, telemetry is attributed to the **single resolved
composite billing team** (P1) or, if none is unambiguous, tagged **untenanted** — never
spread ambiguously across teams. Each ingested event carries a **unique event id**; ingest
dedups on it (`ON CONFLICT DO NOTHING`), so a batch retry after a crash cannot double-count.
Per-origin admission limits are a **shared-Postgres token bucket keyed by origin** (atomic,
fleet-wide), so routing a hostile server's requests across many stateless instances cannot
evade the cap.

**Only `cert:CN` is transport-authoritative; the human `sub` needs its own proof.**
The telemetry mTLS connection authenticates the *server*, not the user behind it, so
kb cannot derive the originating human `sub` from the handshake — and a
user-controlled server could otherwise forge human attribution. A `sub` is therefore
recorded as **authoritative** only when the record (or batch) carries a
kb-verifiable, audience-bound OIDC token, or a short-lived kb-issued **actor token
bound to the server's `cert:CN`** and session — a *distinct* token with **audience =
kb** (not the P5 §3 management JWT, which is audience-bound to a server for the
opposite kb→server direction and is not valid as a telemetry actor proof at kb) —
checked for issuer/audience/expiry/replay. Absent that proof, a server-supplied `sub`/user label is **dropped at ingest —
not stored even as advisory** (a stored "advisory" field would let a user-run server
pin arbitrary attacker-controlled `sub` values that then surface on enterprise
exports); the record is tagged with `cert:CN` only. AC: a forwarded record carrying a
`sub` with no verified actor token is rejected or stripped of `sub` before
persistence.

**The team tag is resolved at kb, not taken from the payload.** kb stamps the
authoritative team by running P1's composite-identity resolution over the
authenticated `cert:CN` (and any verified actor token) — the same intersection rule
as egress — not from a team label the server placed in the record. A payload-supplied
team is advisory only; the stamped team is what any team-scoped telemetry query or
rollup uses.

## §3 Enterprise export from kb

kb exposes standardized surfaces for the org's own observability stack:
- **Metrics:** a live Prometheus **`/metrics`** endpoint and **OTLP** (OpenTelemetry)
  push/pull, covering the real cost/token/latency/egress dimensions (sourced from
  `token_audit`, not the thin `agent_log`) — labeled by team/provider/model.
- **Logs:** structured JSON export plus OTLP logs, filterable by team/user/time.
- **Traces (optional):** OTLP spans for the egress path (server → kb → vendor) so an
  org can see end-to-end latency.
The export projection is **explicit and content-free**: only token counts, USD cost,
latency, model id, team id, and `cert_cn` are exported; content-bearing `org_token_audit`
columns (prompt/completion bodies, message text, tool args) are **excluded by projection,
not by redaction**, so an OTLP/Prometheus consumer cannot be configured to ingest content
by accident (AC: a synthetic record with content columns populated never surfaces them on
any export). These export surfaces are **authenticated, authorized, and tenant-isolated** — not
open scrape targets: `/metrics`, OTLP, and log export require mTLS or OIDC, are gated
by an org-admin/team-lead capability, and a team-scoped consumer sees **only its own
team's series** (the same team-scoped predicate / RLS as the P3 spend reads, invariant
#10) — so the export tier cannot leak one tenant's cost/latency to another. This is the
"plug aimee into our Datadog/Grafana/Splunk" story an enterprise expects.
Export is **fail-closed authenticated even without OIDC**: `/metrics`, OTLP, and log
export require mTLS (`cert:CN`) *or* OIDC when configured, gated by an org-admin/team-lead
capability — never an open/unauthenticated scrape target, and never dependent on OIDC
being on. OTLP is offered as an **authenticated pull (scrape) or a push to an operator-configured
authenticated collector** — the direction and its trust boundary are explicit, not
ambiguous. The **retention/deletion path is the only post-ingest mutation of telemetry**:
it is org-admin-authorized, tenant-scoped, and **WORM-audited on the invariant-#6
hash-chained ledger** (who deleted what, when), backed by **append-only table permissions**
(the runtime role can INSERT telemetry but not UPDATE/DELETE rows except via the audited
retention path), so it cannot be a silent tamper vector. OTLP export follows the **standard
OpenTelemetry contract** — a **push exporter to an operator-configured authenticated
collector** — with per-shard ownership + checkpoint so N stateless instances do not
double-export the same rows and a crash resumes from the checkpoint; the Prometheus
`/metrics` surface is the authenticated **pull/scrape** path. Each stateless instance
periodically **flushes its per-process IR-metric counters to a shared Postgres table keyed
by instance id**, and the reader sums across instances — so no per-process counter is lost
and the fleet total is correct.

Forwarded telemetry carries a configurable **retention** on kb (default-bounded, with
per-tenant override) and a deletion path, so the org controls how long tagged records
live — telemetry is not an unbounded store.

## §4 Fix the write-only IR metrics

Wire a reader/exporter for `aimee_ir_metrics`, or fold its counters into the OTLP
and Prometheus surface, so IR passthrough-vs-fallback parity is observable in the
field. The counters exist on **both tiers** (server and kb); each tier exposes its own
via its local export path (the server forwards its IR-metric samples as allowlisted
structured events, kb reads its own from shared Postgres, invariant #9) — the reader is
not a single-process assumption. The roundtable-mandated shadow metrics are currently undiagnosable because
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
- **Structural boundary, not just redaction.** The forwarder accepts only a typed
  telemetry record (enumerated metric samples + structured log events with fixed
  fields); it has **no free-text field that can carry a raw prompt, completion, or
  message body**. Raw content is dropped at the type boundary *before* redaction
  runs, so redaction is defence-in-depth over a channel that structurally cannot
  carry the payload — this is what makes invariant #7's "kb never receives raw PII
  or secrets" a guarantee rather than a best-effort hope. The allowlist (§1) bounds
  *which* events leave; the typed record bounds *which fields*; redaction is the
  last-resort scrub within allowed fields. Crucially the allowlist covers **each
  field's type**, not just the event type: allowed fields are bounded enums, ids,
  counters, durations, or bounded low-cardinality labels — **no allowed field is a
  free-text carrier** (a "no free-text field" record could still leak PII if an
  *allowed* field were a free string). Any field that could carry user text is
  excluded, or reduced to a bounded id, or passed through the redactor with enforced
  cardinality bounds — so "kb receives no raw PII or secrets" holds structurally, not
  by hoping the redactor catches everything. Allowed **id** fields are **opaque
  surrogate keys** (an internal team/server id, a hashed/tokenized reference), **never
  a natural identifier that is itself PII** (email, raw username, IP) — any natural
  identifier is mapped to an opaque id before forwarding, so even a bounded id field
  cannot smuggle PII.
- Reuse the vault's existing discipline of *never logging the secret itself*
  (`vault_service.c`); promote it into a shared redaction helper that both the
  display path and the forwarder call.

## Acceptance criteria

- A server **allowlisted structured log event** and a metric sample arrive at kb,
  each **tagged** with the server `cert:CN` (plus user `sub`/team when present); an
  untagged or origin-forged record is rejected, and an arbitrary free-form
  `aimee_log()` / audit line is **not** forwarded (negative test).
- kb `/metrics` returns Prometheus text with cost-USD and cache-token dimensions
  (not just the old `agent_log` subset); OTLP export produces equivalent data.
- A password or API key appearing in a server log is redacted in the local
  display and absent from anything forwarded to kb.
- IR parity metrics are readable via the export surface (no longer write-only).
- Telemetry backpressure or kb unavailability never blocks egress or user
  interaction (loss is acceptable; blocking is not).
- An event of an unrecognized / unallowlisted type is dropped at the forwarder and
  never reaches kb; the forward record type has no free-text field able to carry a
  raw prompt or completion (asserted structurally, not by redaction alone).

## Testing

**Unit:** tag stamping from mTLS identity plus forge rejection; redactor on known
secret shapes (present in local view, scrubbed in forward view);
Prometheus/OTLP serialization including cost dimensions; IR-metric reader.
**Integration:** server→kb forward over mTLS with tags asserted; scrape kb
`/metrics` and an OTLP collector; kill kb and prove egress continues while
telemetry buffers and drops gracefully.

## Non-goals

No guaranteed PII elimination **on the server's local display** (best-effort by
design, per the single-user guarantee). This does **not** apply to the forward
path: what reaches kb is bounded structurally (the typed, allowlisted record of
§1/§5), so kb receives no raw PII or secrets — that boundary is a guarantee, not
best-effort. No replacement of the enterprise's observability stack —
this exposes standardized feeds *into* it. No new metrics store on the server (the
server keeps `token_audit`; kb is the aggregation and export tier).
