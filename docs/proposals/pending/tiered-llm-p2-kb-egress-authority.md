# Proposal: P2 — aimee-kb as egress authority + org model catalog (blended roster)

- **State:** proposed (pending — not started). Part of `tiered-llm-offering.md`.
- **Author:** JBailes (drafted by the engineer agent, 2026-07-17).
- **Depends on:** P1 (team identity). **Blocks:** P3, P4 (they meter/enforce at the egress point this packet creates). **This is the core of the series.**

## Thesis

Org vendor keys must never touch the user-run aimee-server (see master plan). For org models, aimee-kb is therefore the **egress authority**: the server forwards the model call to kb, kb attaches the org key and calls the vendor. Users keep their own models local. The result is a **blended roster** — org models offered by kb, personal models configured on the server — distinguished by a single `egress` field per agent (`via-kb` | `direct`).

## Goal

1. aimee-kb *offers* an entitlement catalog of org models to an authenticated server.
2. aimee-server *merges* that catalog with the user's personal agents into one roster; `aimee primary` can point at either kind uniformly.
3. An `egress: via-kb` request flows server → kb → vendor with the org key held only in kb; an `egress: direct` request stays local and unchanged.

## §0 What already exists

- **Canonical IR + provider drivers** — `src/headers/aimee_ir.h`, `src/server/aimee_ir_serve.c`, `delegate_driver.c`. The Anthropic/OpenAI/Responses/Gemini egress machinery already exists (the universal-gateway work, done). P2 gives kb an egress path built from these adapters.
- **Vault** — `src/server/vault_service.c` already stores creds encrypted-at-rest keyed by principal, but it is **server-side** and holds per-user keys today; **kb has no vault yet** — P7 builds the kb-side org vault. No org keys exist on either tier at this point: P2 introduces them, and **P7 must land alongside P2** before any real org key is entrusted to kb. The point is that the *envelope-encryption core* is reusable, not that a kb org vault already exists.
- **Keyless roster + attested credential handling** — the shipped `deploy/container/agents.json` carries no keys. The old client-push `POST /v1/session/credentials` was **retired**; secrets now live in the server vault (`/v1/vault/*`) and vault writes require an attested transport (UDS or confidential TLS+bearer; never plaintext TCP — `server_cert.c:18-22`). P2 moves the credential authority up a tier: **kb** holds the org keys and *is* the egress point, so an org key never needs to reach the server's vault at all.
- **Per-agent config** — `agent_t` (`src/headers/agent_types.h:214-277`) already carries `provider`, `endpoint`, `backend`; adding `egress` is a natural field.

## §1 Org model catalog on kb

A kb-side catalog: `{model_id, display_name, provider, wire, entitled_teams[], cred_slot_ref}`. The org key for each provider lives in the kb vault, never in the catalog payload; `cred_slot_ref` is a **server-invisible credential-slot reference scoped by `(org, billing_team, provider, model)`** and is resolved to a vault slot only *after* authoritative team + entitlement resolution — so with multiple orgs/teams holding creds there is never ambiguity about which key is attached, and the vault ciphertext's AAD binds the same `(team|provider|cred)` tuple.

- `GET /v1/models/entitled` returns the caller's entitled models, resolved via P1's `kb_identity_resolve` — a user sees only models their team may use.
- Admin CRUD at `/v1/models/org/{add,remove,set}` requires a **narrow org-admin capability checked from composite identity on the primary** for every mutation, is tenant-scoped (RLS predicates, cross-org identifiers rejected), and appends a **WORM-audited before/after** record — it is not merely "console-managed".

## §2 Server merges a blended roster

On session start (and on refresh), aimee-server calls `GET /v1/models/entitled` under the **composite identity** (P1 §2) — the authenticated per-server `cert:CN` always, plus a kb-verified actor token when OIDC is configured; **when OIDC is absent the certificate principal alone resolves the catalog**, so the flow works with no IdP — and merges the result into its agent list:

- Org models become agents with `egress: via-kb`, `provider`/`wire` from the catalog, and **no `api_key`** (the field stays empty on the server, by design).
- Personal models remain `egress: direct` with the user's key in the local vault.
- **Pre-tier org keys in a user's own vault:** a user who today holds what is really an *org* vendor key in their personal server vault (the old "personal model with my key" affordance) is not disturbed automatically — it stays a personal `direct` model — but the org tier's intent is that such keys move to kb. A migration note/tool **flags** a personal key whose provider matches an org-catalog entry so the org can reclaim it into kb; kb never auto-exfiltrates a user's personal key.
- **Namespacing:** org models fall under an `org/` prefix the user cannot edit; personal models keep the user's own names. On collision, the org entry is read-only and wins on its own name; the user's own name stays theirs. Precedence is explicit.
- This extends `agent_load_config` merge logic. The mtime/identity cache fix (PR #1372) is the pattern to follow so a refreshed roster is never served stale.

## §3 kb egress endpoint

`POST /v1/llm/egress` on aimee-kb accepts a canonical request (IR or a supported wire), authenticates the caller (OIDC / cert), resolves the caller's **billing team** via the P1 resolver — a team named on the request (e.g. `team_id`) is accepted only if it is in the caller's resolved set (rejected outright if not, never downgraded); an *absent* team uses the caller's `default` — and denies if **that** team is not entitled to the requested org model, so entitlement, attribution (P3), and budget (P4) all key off the same single resolved team; attaches the **org** vendor key from the kb vault, dispatches through the existing IR backend adapter for the model's `wire`, and streams the response back. This is the single point where P3 meters cost and P4 enforces budget/limits — one seam, not scattered.

- **Every request revalidates authorization on the primary** (read-your-writes): cert revocation (per-request, P1 §2), composite identity, selected/`default` team, catalog entitlement, and credential-slot status — never cached for the connection's life. These checks are **one atomic authorization decision** in a single primary transaction (not five independent reads that could interleave with a mutation), and the JWT is verified by **kb's own OIDC verifier** (`kb_oidc_verify_jwt`, `aud=kb`), never trusted from a header. Primary reads at ~20+ instances rely on **HA Postgres** (replicated primary + failover), so "primary" is not a scaling SPOF. Catalog-fetch entitlement is a **roster hint only**; egress re-checks entitlement authoritatively, so an entitlement revoked between fetch and egress is caught at the egress gate (no TOCTOU).
- **Sequencing:** P2b enables a live org-key path, so it must **not ship an uncapped spend surface before P4** — either P4's budget/rate enforcement lands with P2b, or P2b enforces a conservative interim per-team cap until P4, so an entitled/compromised server cannot run unbounded org spend in the gap.
- **Strict trust boundary — nothing authoritative comes from the untrusted server.** Provider, endpoint, wire, model, credential slot, and region are derived **exclusively from the authoritative catalog + vault**; the server supplies only a catalog `model_id` and the canonical IR payload + validated generation params. A raw vendor wire, if accepted, is parsed into a typed structure and the upstream request **reconstructed** from catalog/vault — a server can never override the endpoint, credential, or auth headers (no SSRF / credential-redirection).
- **Crash-safe audit lifecycle around the non-transactional vendor call:** a globally unique request/attempt id anchors a durable **`started`** record on the primary *before* credential use or upstream transmission (the P7 dispatch record), and a terminal `success`/`denied`/`failed`/`uncertain` record after — so a crash or ambiguous retry can never produce an unrecorded call, a double charge, or a double dispatch (idempotent on the id, per P4 T1/T2 + P7 §6).
- **Invariant #7 scope:** kb *is* the egress authority, so the egress request path **necessarily carries prompt content to kb** — kb processes it **in-flight and never persists it** (the `org_token_audit` row is counts/USD only, no content, P9). Invariant #7's "kb never receives raw PII or secrets" governs the **telemetry/observability tier (P9)** and the enterprise export — not the egress request, which the org's own kb must see to make the vendor call.

- Reuse `aimee_ir_build_provider_body` and the backend adapters (`aimee_ir_serve.c:39`) so kb and server share one egress implementation rather than forking it.
- **Streaming:** honor the buffered-replay vs. true-stream distinction correctly (see the known `anthropic_http.c` stream-flag bug: the buffered-replay path must carry `stream=false` for the non-responses wire). Build the kb path stream-clean from the start and add a deterministic streaming probe to the test plan.

## §4 Server routes org calls to kb

When the resolved primary/delegate has `egress: via-kb`, the server's outbound path targets `/v1/llm/egress` on kb instead of the vendor. `egress: direct` continues to use the existing path, untouched. The switch is a single branch on the `egress` field at the point the driver/endpoint is resolved (`anthropic_http.c` primary resolution; delegate dispatch).

**The server→kb channel is always mTLS**, using the server's individual per-server cert (`src/server/kb_client_mtls.c`) — never bearer-only. mTLS provides machine identity (`cert:CN`); the user's OIDC identity (when present, and **verified by kb itself — never trusted from a server-supplied header**) rides on top for attribution per the composite-identity contract (P1 §2), which resolves the `{transport_principal, actor_principal?}` pair to one billing team fail-closed. A server without a valid per-server cert cannot reach kb egress at all — fail closed, not fall back to a shared token.

## Acceptance criteria

- A user entitled to an org model sees it in `aimee` with no key present locally; a chat through it returns a completion; **no org key is ever written to the server** (assert the local vault/agents.json never receives it). Beyond that runtime assertion, a **static CI guard** — a build/grep unit test — asserts (a) no server-side code path calls a vault-write (`vault_service_set`/equivalent) with an `org:`/`team:` principal, and (b) no server code writes an org-provider key into `agents.json`; for any `egress: via-kb` agent the server-side `api_key` field is empty end-to-end. (Named here and cross-referenced from P7 §6's fail-closed WORM defaults so the guard is a real gate, not just prose.)
- A caller in teams `{A,B}` naming team `A` is entitled, attributed (P3), and budget-checked (P4) against `A`; naming a team outside the caller's set is rejected; naming none uses the caller's `default` team.
- A user *not* entitled to that model is denied at kb, not at the server.
- A personal `egress: direct` model continues to work with the user's own key, unchanged.
- Streaming and non-streaming both work through the kb egress path (deterministic probe passes on both wires).
- Killing kb disables org models but leaves personal models working (graceful degradation, clear error — not a hang).

## Testing

- **Unit:** roster merge (org+personal, collisions, precedence); egress entitlement gate; IR backend dispatch per wire.
- **Integration:** real server↔kb split — entitled call succeeds, unentitled denied, key-never-on-server assertion, kb-down degradation, streaming probe on Anthropic + OpenAI wires; plus the scale/security claims: primary-only revocation on a keep-alive connection, atomic authorization under a concurrent entitlement mutation, and no-double-dispatch / exactly-one-audit-row across a crash between started and settle.

## Non-goals

P2b **does** write the mandatory raw `org_token_audit` attribution row for every org call (attribution is not optional for a live org call); what P3 adds on top is the **aggregation, rollup, and reporting** surfaces over those rows, and P4 adds budget enforcement. So the raw audit write rides with P2b; the metering/reporting *surfaces* are P3, and budget *enforcement* is P4. No personal-egress routing through kb. No new provider (Bedrock is P6).
