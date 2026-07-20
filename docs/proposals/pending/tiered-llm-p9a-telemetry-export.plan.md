# P9a implementation plan — kb telemetry export + ingest schema (P9 §3, +ingest target)

Slice P9a of P9. Branch off `testing` (P1, P3a, P2a, P3b, P4a, P4b, P10 s1/s2/s3b merged).
P9's substantive value split: §1/§2 are the **server-side forwarder** over the always-mTLS
channel (integration — needs the P8/P2 channel, deferred); §3 is the **kb enterprise
export** — a live Prometheus `/metrics` endpoint + the telemetry INGEST schema/endpoint with
the drop-on-unknown allowlist + PII scrub. P9a ships §3 (kb-side, live, real-PG-testable) +
the ingest target the deferred forwarder will call. **PII invariant #7**: no content, no
raw OIDC `sub` — only `cert:CN`/team tags, scrubbed on write.

## Verified substrate

- Existing org state to expose: `org_spend_rollup` (P3a), `org_budget_counter` (P4a),
  `org_rate_window` (P4b), `org_model_catalog` (P2a), `kb_audit_event`. All in Postgres.
- P9 §0 gaps: Prometheus export is a textfile (`agent_write_metrics`), not an HTTP
  endpoint; lacks cost-USD/cache tokens; no live `/metrics`; no OTLP.
- kb HTTP route pattern (kb_http_*.c, kb_reqctx), ENABLE-not-FORCE RLS + definer, WORM
  audit, config keys, the real-PG gate + OpenAPI/coverage flow (P2a/P3b/P4).

## Design decisions

1. **`GET /v1/metrics` (Prometheus text format)** on kb — exposes ORG-LEVEL aggregates
   from existing state: `aimee_org_spend_usd{team}`, `aimee_org_budget_limit_usd{team,
   period}` / `_reserved_usd` / `_spend_usd`, `aimee_org_catalog_models`,
   `aimee_org_audit_events_total`, `aimee_org_teams`, etc. **No PII** — team ids + fixed
   label sets only, never content or a user `sub`. Prometheus text (simple `# HELP/# TYPE`
   + `name{labels} value` lines); no OTLP in P9a (a large protocol — documented follow-up).
2. **Scrape auth.** `/v1/metrics` is gated by a dedicated **metrics scrape token** (config
   `telemetry.metrics_token`) OR org-admin — never open (org spend is sensitive). A missing/
   wrong token → 401. (An unauthenticated /metrics would leak org spend to any scraper.)
3. **`org_telemetry` ingest schema + `POST /v1/telemetry/metrics`** — the target the
   deferred server forwarder (§1) will call. `org_telemetry(id, origin_cert_cn, team_id
   NULL, event_schema TEXT, metric_name TEXT, value NUMERIC, ts TEXT, created_at)` —
   **content-free by construction** (no free-text/payload column). The ingest is
   **allowlist-gated**: only `event_schema` on a fixed forward allowlist is stored;
   **anything unknown is DROPPED (fail-closed), not stored** (drop-on-unknown). PII scrub:
   `origin_cert_cn` is the server identity tag (from the verified caller in P2b/mTLS; in
   P9a set from the actor); a raw OIDC `sub` or any content field in the request is
   **never persisted** (the schema has no column for it). ENABLE-not-FORCE RLS
   (team-scoped read: admin OR team-lead); writes via a definer.
4. **PII scrub proof (§3 AC).** A synthetic ingest with a PII-bearing CN + a populated
   "content"/"sub" field → the content/sub is not persisted (no column), and `/v1/metrics`
   never emits it. A record whose `event_schema` is not allowlisted is dropped.

## Scope (P9a)

1. **DB2 schema** (`db2/schema.sql` + sqlite mirror + grants): `org_telemetry` (content-
   free, as above) + `org_telemetry_allowlist(event_schema TEXT PK, enabled BOOLEAN)` (or a
   fixed in-code allowlist — decide: a table lets admins manage it, WORM-audited). ENABLE
   RLS; team-scoped read; definer write. Definer `org_telemetry_ingest(origin_cn, team,
   event_schema, metric_name, value)` (drops if event_schema not allowlisted), and read
   aggregations for `/v1/metrics` via definer or direct admin query.
2. **`GET /v1/metrics`** (`kb/http/kb_http_metrics.c`) — scrape-token/admin gated,
   Prometheus text of the org aggregates (from org_spend_rollup / org_budget_counter /
   org_model_catalog / kb_audit_event + org_telemetry). No PII.
3. **`POST /v1/telemetry/metrics`** (allowlist + scrub ingest) — content-free, drop-on-
   unknown. (The server-side FORWARDER that calls it is P9 §1, DEFERRED.)
4. **Config** `telemetry.metrics_token`. **CLI** `aimee-kb telemetry {show,allow}` (operator).
5. **Tests**: unit (Prometheus text format, allowlist drop-on-unknown, no-content-column)
   + real-PG gate `scripts/p9_telemetry_rls_test.sql`: (a) ingest an allowlisted metric →
   stored; a non-allowlisted `event_schema` → dropped (0 rows); (b) the schema has NO
   content/sub column (a PII-bearing payload can't be persisted); (c) team-scoped read RLS
   (team-lead sees own team's telemetry only); (d) `/v1/metrics` aggregate reconciles with
   the seeded spend/budget rows; (e) admin-only allowlist management. Wired into
   `run-p1-rls-gate.sh`.

## Explicitly deferred

The server-side forwarder + tagging (§1/§2 — needs the P8/P2 always-mTLS channel); OTLP
(OpenTelemetry protobuf/HTTP — a large protocol, follow-up); traces/spans; the write-only
IR metrics fix (server-side); log forwarding + JSON/OTLP log export. P9a is the kb
Prometheus export + the content-free ingest target + PII scrub.

## Gate

- `make -j server` links clean; `make lint` + `make schema-sync-check` green;
  `/v1/metrics` + `/v1/telemetry/metrics` in the OpenAPI/v1 descriptor (coverage green).
- Unit + the real-PG p9 gate pass on CT103 (drop-on-unknown, no-content-column, team-read
  RLS, metrics reconciliation). Existing gates unchanged (**re-push UPDATED
  schema_grants.sql to CT103**; a definer-only table read by runtime = permission-denied,
  not RLS-filtered-0).

## Non-goals (P9a)

No server forwarder, no OTLP, no traces, no log forwarding, no unauthenticated /metrics.
Pure kb Prometheus export of org aggregates + a content-free allowlisted PII-scrubbed
ingest target + admin, tenant-isolated, real-PG-proven.
