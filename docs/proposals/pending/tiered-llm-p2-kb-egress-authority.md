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
- **Vault** — `src/server/vault_service.c` already stores creds encrypted-at-rest keyed by principal; the org keys live here on the kb side, keyed to the org principal.
- **Keyless roster + attested credential handling** — the shipped `deploy/container/agents.json` carries no keys. The old client-push `POST /v1/session/credentials` was **retired**; secrets now live in the server vault (`/v1/vault/*`) and vault writes require an attested transport (UDS or confidential TLS+bearer; never plaintext TCP — `server_cert.c:18-22`). P2 moves the credential authority up a tier: **kb** holds the org keys and *is* the egress point, so an org key never needs to reach the server's vault at all.
- **Per-agent config** — `agent_t` (`src/headers/agent_types.h:214-277`) already carries `provider`, `endpoint`, `backend`; adding `egress` is a natural field.

## §1 Org model catalog on kb

A kb-side catalog: `{model_id, display_name, provider, wire, entitled_teams[]}`. The org key for each provider lives in the kb vault, never in the catalog payload.

- `GET /v1/models/entitled` returns the caller's entitled models, resolved via P1's `kb_identity_resolve` — a user sees only models their team may use.
- Admin CRUD lives at `/v1/models/org/{add,remove,set}`, console-managed.

## §2 Server merges a blended roster

On session start (and on refresh), aimee-server calls `GET /v1/models/entitled` using the user's OIDC identity and merges the result into its agent list:

- Org models become agents with `egress: via-kb`, `provider`/`wire` from the catalog, and **no `api_key`** (the field stays empty on the server, by design).
- Personal models remain `egress: direct` with the user's key in the local vault.
- **Namespacing:** org models fall under an `org/` prefix the user cannot edit; personal models keep the user's own names. On collision, the org entry is read-only and wins on its own name; the user's own name stays theirs. Precedence is explicit.
- This extends `agent_load_config` merge logic. The mtime/identity cache fix (PR #1372) is the pattern to follow so a refreshed roster is never served stale.

## §3 kb egress endpoint

`POST /v1/llm/egress` on aimee-kb accepts a canonical request (IR or a supported wire), authenticates the caller (OIDC / cert), resolves team + entitlement (deny if the caller's team is not entitled to the requested org model), attaches the **org** vendor key from the kb vault, dispatches through the existing IR backend adapter for the model's `wire`, and streams the response back. This is the single point where P3 meters cost and P4 enforces budget/limits — one seam, not scattered.

- Reuse `aimee_ir_build_provider_body` and the backend adapters (`aimee_ir_serve.c:39`) so kb and server share one egress implementation rather than forking it.
- **Streaming:** honor the buffered-replay vs. true-stream distinction correctly (see the known `anthropic_http.c` stream-flag bug: the buffered-replay path must carry `stream=false` for the non-responses wire). Build the kb path stream-clean from the start and add a deterministic streaming probe to the test plan.

## §4 Server routes org calls to kb

When the resolved primary/delegate has `egress: via-kb`, the server's outbound path targets `/v1/llm/egress` on kb instead of the vendor. `egress: direct` continues to use the existing path, untouched. The switch is a single branch on the `egress` field at the point the driver/endpoint is resolved (`anthropic_http.c` primary resolution; delegate dispatch).

**The server→kb channel is always mTLS**, using the server's individual per-server cert (`src/server/kb_client_mtls.c`) — never bearer-only. mTLS provides machine identity (`cert:CN`); the user's OIDC identity (when present) rides on top for attribution. A server without a valid per-server cert cannot reach kb egress at all — fail closed, not fall back to a shared token.

## Acceptance criteria

- A user entitled to an org model sees it in `aimee` with no key present locally; a chat through it returns a completion; **no org key is ever written to the server** (assert the local vault/agents.json never receives it).
- A user *not* entitled to that model is denied at kb, not at the server.
- A personal `egress: direct` model continues to work with the user's own key, unchanged.
- Streaming and non-streaming both work through the kb egress path (deterministic probe passes on both wires).
- Killing kb disables org models but leaves personal models working (graceful degradation, clear error — not a hang).

## Testing

- **Unit:** roster merge (org+personal, collisions, precedence); egress entitlement gate; IR backend dispatch per wire.
- **Integration:** real server↔kb split — entitled call succeeds, unentitled denied, key-never-on-server assertion, kb-down degradation, streaming probe on Anthropic + OpenAI wires.

## Non-goals

No metering/attribution here (P3) and no budget enforcement (P4) — P2 builds the seam they attach to. No personal-egress routing through kb. No new provider (Bedrock is P6).
