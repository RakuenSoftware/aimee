# P9a implementation plan — kb telemetry export + ingest schema (P9 §3, +ingest target)

> **Archived proposal.** This records the design as it was agreed, not the
> system as it behaves today; parts of it have since diverged. For current
> behaviour see `docs/`, or the code.

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

## v2 refinements (roundtable-converged; separation + PII-structural + token hygiene)

- **`/v1/metrics` exports AUTHORITATIVE PG STATE ONLY; `org_telemetry` is a WRITE-ONLY ingest
  target, never read back through /metrics in P9a.** This is the key separation — it kills
  the double-counting / name-collision / self-referential-ingest concerns. `/v1/metrics`
  aggregates `org_spend_rollup` / `org_budget_counter` / `org_model_catalog` /
  `kb_audit_event` (things that exist and are exercised). `org_telemetry` is the target the
  deferred forwarder (§1) will populate; in P9a it is shippable-but-unexercised (documented:
  no P9a producer). When the forwarder lands, forwarded metrics get a **distinct namespace**
  (`aimee_fwd_*`) so they never collide with the authoritative `aimee_org_*` series.
- **PII structural, not just "no content column".** `metric_name` is NOT free text: it is
  validated against the **allowlisted metric-name set carried by the `event_schema`** (an
  unknown metric_name for a known schema is dropped, same fail-closed rule). All caller TEXT
  (`metric_name`) is length-capped (≤128) + charset-restricted (`[a-zA-Z0-9_:]`) so a `sub`/
  email/free content cannot be smuggled into it. `origin_cert_cn` is **server-set from the
  authenticated principal** (the ingest caller's verified identity), NEVER free caller text —
  the ingest request body carries no CN/sub field the definer reads. The HTTP parser routes
  ONLY the known fields; there is no generic `jsonb`/`extra`/`payload` column and unknown
  JSON fields are ignored, not stored (proven by the no-content-column gate).
- **Ingest auth boundary (even without mTLS).** `POST /v1/telemetry/metrics` requires an
  authenticated principal — in P9a **org-admin OR the ingest token** (the forwarder's mTLS
  identity is the deferred §1/§2 upgrade). It is NEVER open. `team_id` is server-resolved
  from the authenticated origin, not trusted from the body (closes cross-team mis-attribution).
- **Idempotency + forwarder shaping.** `org_telemetry` carries `source_event_id TEXT`
  (UNIQUE) — the forwarder is at-least-once, so ingest is `ON CONFLICT (source_event_id) DO
  NOTHING` (a retried forward is deduped, not double-counted). Add `metric_kind TEXT
  CHECK(metric_kind IN('counter','gauge'))` and accept `ts` as a definer param (BIGINT epoch,
  not a TEXT the row stores raw). This gives the deferred forwarder a stable contract now.
- **Allowlist = admin-managed table, WORM-audited.** `org_telemetry_allowlist(event_schema
  TEXT PK, metric_names TEXT[] , enabled BOOLEAN, updated_at)` — admin-only mutation via a
  definer that `kb_audit_worm_append`s each change (so the forwarder's contract is durable +
  auditable, not a moving in-code target). Read: admin-only.
- **Token hygiene.** `telemetry.metrics_token` (also the ingest token) is **stored hashed**
  (SHA-256, compared constant-time against the presented bearer); a wrong/missing token → 401
  with **no token echo** in the body or logs. Presented as a bearer header (never a URL query
  param — avoids access-log leakage). Rotation = set a new config value (a follow-up can
  support two active hashes for zero-downtime rotation; P9a = single).
- **Prometheus text safety.** Hand-built output escapes label values per the Prometheus text
  format (`\\`, `\"`, `\n`); labels are **bounded, fixed-cardinality** only (`team` = numeric
  id, `period` ∈ {day,month}, `model` from the catalog) — no ingested/free-text value ever
  becomes a label or metric name. A gate test parses the output for format conformance.
- **One-org-per-kb observability scope (explicit invariant).** The kb RLS model is
  team-within-one-org (`kb_principal_is_admin` = org admin); a kb instance serves one org, so
  `/v1/metrics` exposing all of that org's teams to the org's own scrape token is the intended
  org-observability view, NOT cross-tenant. Documented as the deployment invariant; the token
  grants org-scoped observability to that org's operators.
- **Retention/scale.** `org_telemetry` gets an index on `(team_id, created_at)` and a
  documented retention/prune follow-up (`org_telemetry_prune(older_than)` — a stub note, not
  built in P9a since there is no producer yet). `/v1/metrics` aggregates are bounded queries
  over the existing rollup tables (already indexed), not a full scan.

### Gate additions

- (f) ingest with an unknown `metric_name` for a KNOWN `event_schema` → dropped (0 rows);
  (g) a duplicate `source_event_id` re-ingest → deduped (still 1 row, ON CONFLICT);
  (h) `metric_name` with a disallowed charset / over-length → rejected;
  (i) `/v1/metrics` output passes a Prometheus text-format parse (format conformance);
  (j) a wrong metrics token → 401 with the token not present in the response body;
  (k) org_telemetry is NEVER read back through /v1/metrics (the ingested test rows do not
  appear in the /metrics output — the write-only-target invariant).
