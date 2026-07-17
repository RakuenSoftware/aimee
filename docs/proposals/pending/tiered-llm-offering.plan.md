# Implementation plan — Tiered LLM offering

- **State:** draft plan; tracks `tiered-llm-offering.md` (proposed). Each packet runs its own build → live-test → unit-tests → lint → roundtable → PR → merge cycle, against `testing`.
- **Nothing shipped yet.** All packets below are *not started*.

## Build graph (dependencies, not calendar)

```
P1 tenancy+identity ─┬─> P2 kb egress + catalog ──┬─> P3 attribution
                     │        (P7 vault rides here)│   (P3a schema can precede P2;
                     │                             │    P3b write rides P2b)
                     │                             ├─> P4 budgets+limits
                     │                             └─> P9 telemetry tiering
                     └─> P5 control plane (kb manages servers)
P6 Bedrock ── independent (needs P2 egress seam for the org path)
P8 thin-client mTLS ── independent (default-flip; anytime)
P7 hardened kb vault ── MUST land with/before P2 holds live org keys
```

**Recommended merge order:** P1 → **P3a (schema + pricing)** → (P2 + P7 together — **P2b's egress path writes the mandatory `org_token_audit` row**, so live org egress never ships without authoritative cost rows) → **P3b (read/rollup/reporting surfaces)** → P4 → P5 → P9 → P6, with P8 slotted whenever. P3's *schema/pricing* lands right after P1; the **attribution write is part of P2b itself** (not deferred to P3b), so an unattributed org call is impossible; the hard edge remains **P7 gates real org keys in P2**.

## Cross-cutting guardrails (apply to every packet)

- **Uphold the ten invariants** in the master plan's "Security & transport invariants" — reviewers check each PR against them, **including #9 (stateless kb / Postgres source of truth / atomic-on-primary) and #10 (Postgres hardened, ciphertext-only at rest)**.
- **Postgres hardening (invariant #10) is delivered by P1 as foundation** and extended by later migrations: `verify-full` + replica TLS on every kb↔Postgres connection, separate migration vs. runtime DB roles (runtime role = DML on its own schemas, no DDL/superuser), mandatory team-scoped predicates / row-level security on tenant tables, and encrypted backups + WAL. Every packet that adds a DB2 table (P1 teams, P3 audit, P4 budgets, P5 registry, P7 vault) ships its tenant-isolation predicate and its migration/runtime-role grants with it, tested.
- **kb tier = Postgres/DB2; server tier = SQLite/DB1.** Never put multi-tenant state on the server. New shared tables are DB2, behind the KB service (server/CLI must not touch DB2 directly).
- **Migrations are first-class and reversible.** Additive columns, backfill, and a down path. Follow the `agent_load_config` identity-cache pattern (PR #1372) so a refreshed roster/config is never served stale.
- **Every new `/v1` route** is added to OpenAPI and `v1-method-coverage` (conformance-tested), not a side door, and gated by an explicit capability.
- **Streaming:** build kb egress + Bedrock stream-clean from the start — honor the buffered-replay vs. true-stream distinction (`stream=false` on the non-responses buffered-replay wire); add a deterministic streaming probe per packet that touches egress.

## P1 — Tenancy + identity  *(detailed in `tiered-llm-p1-tenancy-identity.plan.md`)*

Foundation. Team/project entities (DB2), identity→team binding (`kb_identity_resolve`), OIDC promoted to a first-class *optional* data-plane authenticator, `/v1/team/*` + `aimee team` CLI + console. Sliced into 4 PRs — see the P1 plan. **AC:** team CRUD + membership over CLI/`/v1`; OIDC/cert/owner callers all resolve to a team; unknown identity → deny (no admin fallback).

## P2 — kb egress authority + org model catalog  *(largest packet; pairs with P7)*

Slices (each a safe PR):
- **P2a — org catalog + entitlement read.** DB2 catalog `{model_id, provider, wire, entitled_teams[]}`; `GET /v1/models/entitled` resolving via P1; admin CRUD. No egress yet. *AC:* an entitled user's catalog lists only their teams' models.
- **P2b — kb egress endpoint (non-streaming).** `POST /v1/llm/egress`: authenticate (mTLS `cert:CN` + optional OIDC), entitlement gate, attach org key **via the P7 use-in-place primitive**, dispatch through the existing IR backend adapters, buffered response, **writing the mandatory `org_token_audit` attribution row in the same settlement transaction (P3a schema)**. *AC:* entitled call returns a completion and produces exactly one audit row (no live org call is unattributed); unentitled denied at kb; **assert no org key on the server** — both a runtime integration assertion (vault/agents.json scanned after a call) **and a static CI guard** (a test asserts `egress: via-kb` agents carry an empty `api_key` end-to-end; a build/grep check asserts no server code path writes an org-provider key to the local vault or agents.json).
- **P2c — streaming egress.** Add the streaming path stream-clean (probe on Anthropic + OpenAI wires). *AC:* SSE round-trips; buffered-replay flag correct.
- **P2d — blended roster on the server.** Server pulls `entitled` on session start and merges into the agent list with `egress: via-kb` (no key) alongside personal `egress: direct`; namespacing + precedence; refresh without stale-cache. *AC:* `aimee` shows both kinds; `primary` can select either; kb-down disables org models but personal keep working (typed error, no hang).

**Depends on P1 and the full P7 hardened-vault minimum — not use-in-place alone:** external custody + seal enforcement, memory hygiene, ciphertext-only Postgres persistence, and fail-closed **default-on** WORM admission must all be in place before P2b (the first slice that touches a real key) ships. Only P2a (catalog-only, no keys) may precede P7.

## P3 — Per-team/project cost attribution  *(cheap; do right after P1)*

- **P3a** — create the **DB2 `org_token_audit`** table (team/project/model/tokens/cost + pricing `version`) and the **DB2 `org_model_pricing`** table (reversible migrations); the server's DB1 `token_audit` stays **unchanged** for personal calls. Org rows are written at the P2 egress point once P2b exists.
- **P3b** — kb-native `org_token_audit_by_team`/`_by_project` aggregations over DB2 (not the server's DB1 `_by_model` symbols) + rollup; `GET /v1/insights/spend` (org-admin/team-lead scoped) + `aimee spend` + `CostPanel` team view; `--json`.
- **Note:** org cost rows live on the **kb tier** (decided). *AC:* per-team spend reconciles with `_by_model`; a team lead cannot read another team's spend.

## P4 — Budgets + rate limits

- **P4a** — budget model + fast per-period `spend_counter`; pre-flight budget check at egress; typed ≥1000 refusal code; soft-limit operator signal.
- **P4b** — replace the single global `server_http_rate_check` bucket with a keyed (team / `cert:CN`) limiter at kb egress. *AC:* over-budget team refused, others unaffected; refusal never reaches the vendor or leaks a key.

## P5 — OIDC control plane (kb manages servers)

- **P5a** — server registry (DB2) + enroll-a-server-into-registry (reuse `aimee://`); heartbeat.
- **P5b** — kb→server mgmt channel (reverse of `kb_client_mtls.c`, always mTLS); server honors it under existing `remote_writes`/caps.
- **P5c** — OIDC (or owner/bearer) identity propagation → real actor in the server audit log.
- **P5d** — console Fleet view + per-server drill-down; allowlist extension. *AC:* operator lists fleet + drives an agent enable through kb, recorded as the operator; `remote_writes: off` blocks the write.

## P6 — AWS Bedrock

- **P6a** — Bedrock driver + SigV4/STS signer; creds from the P7 kb vault.
- **P6b** — IR↔Bedrock envelope + eventstream (streaming); catalog + pricing wiring so P3/P4 cover Bedrock. *AC:* org Bedrock call end-to-end, no AWS key on the server, spend priced (no `(unattributed)` bucket).

## P7 — Hardened kb vault  *(rides with P2)*

- **P7a** — link the server vault crypto core into kb; kb-owned org-scoped vault; move the **CA key** behind it (kill plaintext PKCS#8).
- **P7b** — **use-in-place** egress primitive (the one P2b calls) + WORM-audited use.
- **P7c** — seal/unseal barrier + KEK-custody provider seam (`file` default; `tpm2`/`pkcs11`/`kms` behind flags) + `mlock`/`MADV_DONTDUMP`.
- **P7d** — rotation (value + per-DEK). *AC (per master invariant 1/6):* no route ever returns an org key; sealed kb refuses egress until unsealed; core dump has no key bytes.

## P8 — Thin-client mTLS  *(default-flip; independent)*

- **P8a** — per-client enrollment + client presents cert (`aimee_tls.c`).
- **P8b** — server ramp `0→1→2`; `cert:CN` cap elevation; shipped configs default to `1`. *AC:* two clients get distinct certs; revoke one, other still connects; `required` refuses bearer-only.

## P9 — Telemetry tiering

- **P9a** — server→kb forwarder (mTLS, batched, back-pressure-safe) + `POST /v1/telemetry/{logs,metrics}`; **origin tags stamped from mTLS identity**.
- **P9b** — kb enterprise export: Prometheus `/metrics` + OTLP (cost/token/latency from `token_audit`, team/provider/model labels); fix write-only IR metrics.
- **P9c** — PII posture: shared redactor; permissive local server display (best-effort secret scrub) vs. scrubbed forward. *AC:* forwarded records tagged + secret-free; telemetry loss never blocks egress.

## Roundtable / review notes

Each packet carries its own roundtable sign-off. The security-sensitive packets (P2b, P7*, P8, P9c) should draw the security lens explicitly; P2c/P6b the streaming correctness lens (the buffered-replay caveat). Behavior-change notes go in each PR where a default flips (P8 mTLS ramp; any config default change in shipped compose).
