# Proposal (master plan): Tiered LLM offering — org-wide models on aimee-kb, user models on aimee-server

- **State:** proposed (pending — not started). Umbrella plan; tracks nine sub-proposals (`tiered-llm-p1`…`p9`).
- **Author:** JBailes (drafted by the engineer agent, 2026-07-17).
- **Origin:** an operator question — "several teams want to play with models from different vendors (OpenAI, Anthropic, Bedrock, …); set something sane up now rather than hand out a pile of raw vendor keys." The market answer to that ask is an internal LLM gateway (LiteLLM / TrueFoundry / Portkey / Kong / OpenRouter). aimee is already in that category — it just isn't multi-tenant yet. This plan closes the gap **without adopting a virtual-key subsystem** and **without breaking aimee-server's single-user design**, by leaning on identity aimee already has.

## Thesis

LLMs are just another resource, and they follow aimee's existing tiering invariant: anything user-specific lives on aimee-server; anything org-wide lives on aimee-kb. The KB already works this way — a user's private memory lives on their aimee-server, the organization's shared knowledge lives on aimee-kb. This plan applies the same split to model access:

- **Org-wide LLM → aimee-kb.** The organization runs aimee-kb (trusted tier). It holds the org vendor keys, offers a catalog of entitled models to users, performs the upstream call, and enforces per-team budget and cost attribution.
- **Individual LLM → aimee-server.** The user runs aimee-server (their own process). They can still configure their own models with their own keys and their own spend, exactly as today.

Because this is the KB's existing split — not a new axis — the work is mostly wiring an existing identity and an existing egress path, not green-field.

## The one fact that determines the whole architecture

The user runs aimee-server; the organization runs aimee-kb. From the org's point of view aimee-server is therefore an untrusted process — the user controls its memory, its env, its disk. Two consequences follow, and they are not optional:

1. **Org vendor keys must never reach aimee-server.** A key placed in a user-controlled process is a key handed to the user. Therefore aimee-kb — not the server — must be the **egress authority** for org models: server → kb → vendor. The server sees prompts and completions, never an org key.
2. **Identity is federated, not key-shaped.** Machines/servers always authenticate with enrollment certs (`cert:CN`); when the org configures it on kb, users additionally authenticate with OIDC (the org IdP). aimee-kb therefore always knows who is calling — which is exactly what a "virtual key" would otherwise be inventing.

**OIDC is optional — never required unless configured on aimee-kb.** It layers on additively (exactly as the existing verifier does — opt-in via `AIMEE_KB_OIDC_JWKS_FILE`, registered after the owner token). The always-present identity is the enrollment cert; when no OIDC is configured, team binding and attribution key off `cert:CN` and the existing owner-token/bearer auth, and nothing in the series stops working. A single-org box with no IdP still runs the whole plan on certs alone.

## ADR: no virtual keys

The commercial tools centre on **virtual/synthetic keys** (LiteLLM especially). A virtual key is LiteLLM's identity system — it fuses authentication, policy binding, and credential indirection into one opaque string, precisely because a bare proxy has no other notion of who is calling. aimee does have one. Decomposed:

| What a virtual key does | aimee's answer |
|---|---|
| Authenticate the caller | OIDC subject (federated, IdP-revocable, MFA) |
| Hide the raw vendor key | kb holds org keys and egresses; the key never leaves kb |
| Bind policy (team → models → budget) | Hang policy off the OIDC identity → team |
| Revoke a leaked credential | Revoke/rotate at the IdP; `cert.revoke` for machines |
| Non-human callers (CI, cron, a server) | mTLS enrollment certs (`src/kb/enroll.c`) |
| Sub-user attribution (project/env) | `scope:kind:id` selector (`src/headers/kb_scope.h`) |

**Decision:** do not build a virtual-key vault. Identity = OIDC (humans) + enrollment certs (machines); policy and cost hang off that identity. This is strictly simpler than a second, weaker identity system kept in sync with the real one — and it is the honest way to satisfy "teams never touch raw vendor keys."

## What already exists (why this is wiring, not green-field)

The four-way code survey (2026-07-17) found aimee already owns most of the hard primitives the commercial tools charge enterprise money for:

- **Cost accounting — strong.** Per-request token→USD in the `token_audit` DB1 table (`src/db1/token_audit.c`, `src/server/token_tracker.c`): multi-source pricing (static table → registry/models.dev → authoritative DB1 `model_pricing`), attributed by model/source/role/tool/session/principal/delegation with parent cost-fold. Read surface exists: `GET /v1/insights/overview`, `/v1/dashboard/*`, React `CostPanel`. **Missing only the per-team/project dimension.**
- **Credential vault — encrypted at rest**, keyed by attested principal (`src/server/vault_service.c`, `src/headers/vault_principal.h`).
- **OIDC/JWT verifier — real** (RS256/JWKS, alg-pinned), in aimee-kb and kb-console (`src/kb/auth_oidc.c`, `kb-console/auth.go`) — opt-in via env today.
- **PKI + mTLS enrollment CA** (`src/kb/enroll.c`, `src/kb/pki.c`, `src/server/kb_client_mtls.c`) — `aimee://` single-use tokens, `cert:CN` principals. Reusable for enrolling servers.
- **Canonical IR + provider drivers** (`src/headers/aimee_ir.h`, `src/server/delegate_driver.c`, `aimee-universal-gateway.md`, done) — Anthropic / OpenAI-Chat / Responses / Gemini wire adapters through a protocol-neutral pivot. This is the egress machinery kb needs.
- **Keyless roster + attested credential handling already exist** — the shipped `deploy/container/agents.json` ships no keys. The old client-push `POST /v1/session/credentials` was retired; provider secrets now live in the server vault (`/v1/vault/*`), and vault writes require an attested transport (UDS or confidential TLS+bearer — never plaintext TCP; `server_cert.c:18-22`, `server_http_identity.c:113-123`). The org-egress work reuses this attested-transport discipline rather than inventing a new one — org keys simply never leave kb in the first place.

Absent (the actual gaps, one sub-proposal each): a team/project entity; per-team/project cost attribution; per-team budgets and per-key rate limits (spend is tracked but never capped; the rate limiter is a single global bucket); OIDC on the aimee-server data plane + a kb→server control plane; and AWS Bedrock.

## Target architecture (one screen)

- **Identity.** Every aimee-server always holds its own individual kb-issued mTLS cert (a unique `cert:CN`) — server↔kb is always per-server mutual TLS, issued and revocable one server at a time, independent of OIDC. Thin-clients likewise get per-client certs (P8), so the thin-client↔server link is mutually authenticated too. Humans → OIDC (org IdP) when kb has it configured, else the existing owner-token/bearer auth. kb always resolves caller → identity → team(s) → policy, whether that identity came from a per-server cert or a JWT. The cert spine is why OIDC can be optional: there is always a strong machine identity to fall back to.
- **Roster (blended).** aimee-kb offers an entitlement catalog of org models to a server; the server merges it with the user's locally-configured personal models. Each agent carries an `egress` field: `via-kb` (org) or `direct` (personal). `aimee primary` may point at either kind; the UX is uniform.
- **Org egress.** For an `egress: via-kb` agent, the server forwards the request to aimee-kb over the authenticated channel; kb checks team policy + budget, attaches the org vendor key, calls the vendor, meters the call (attribution + cost), and streams back. No org key ever lands on the server.
- **Personal egress.** For an `egress: direct` agent, the server calls the vendor directly with the user's own key from the local vault — status quo, untouched.
- **Control plane.** Because every server already depends on kb for org egress, kb is the org-trusted tier that sits above the fleet. The same relationship, used for management, makes kb the OIDC-authenticated control plane that can enroll, list, health-check, and (policy-permitting) configure aimee-servers.

## The constraint that must survive the whole series

**Do not import LiteLLM's operational tax.** The single most-cited LiteLLM pain — and the operator's explicit worry in the origin ask — is upgrade/stability: it is mostly breaking-config + DB-migration hygiene, not load, so it bites demos and POCs too, not just production. aimee's genuine edge is the opposite: aimee-server is a single C binary on SQLite with nothing to babysit. Therefore:

- Keep aimee-server single-binary / SQLite; put the shared multi-tenant state (teams, budgets, attribution) on the kb tier, which is already the Postgres-backed scale-out tier — never on the server.
- Treat every schema migration as first-class, tested, and reversible. (The agents.json mtime-cache footgun is exactly the class of silent-migration bug that erodes a low-ops claim.)

## Security & transport invariants (binding across the whole series)

These are decided, not open. Every sub-proposal must uphold them:

1. **Org vendor keys never reach aimee-server.** They live only in the hardened kb vault (P7) and are used in place at kb egress — never returned over any API, never written to the server, never emitted to a log. (P2, P7)
2. **kb ↔ server is always mTLS, both directions,** with individual per-server certs (unique `cert:CN`, individually revocable). Fail-closed: no valid per-server cert → no org egress and no management. (P2, P5)
3. **Every aimee-server always holds its own kb-issued mTLS cert.** This machine identity is the always-present spine — it is why OIDC can be optional. (P5)
4. **OIDC is optional — required only when configured on kb.** It layers on additively for human SSO + attribution; absent it, identity is the per-server / per-client cert and the existing owner/bearer auth. Nothing in the series depends on OIDC being on. (P1, P5)
5. **Thin-client ↔ server should be mTLS too** (per-client certs); it's a default-flip, not a new build. Bearer demoted to fallback under a graceful ramp. (P8)
6. **The kb vault is hardened before it holds live keys** — external root of trust, seal/unseal, use-in-place, mlock, WORM-audited use. P7 lands with P2. (P7)
7. **Everything forwarded to kb is tagged with the originating user/server** (stamped from the authenticated mTLS identity, not caller-supplied) and scrubbed of PII/secrets. The single-user server may display PII locally (best-effort secret redaction); the multi-tenant kb never receives raw PII or secrets. (P9)
8. **Tiering invariant:** user-specific → aimee-server; org-wide → aimee-kb. Applies uniformly to models (this plan), knowledge (the KB today), cost rows (P3 keeps org rows on kb), and telemetry (P9). (all)

## Sub-proposals and sequencing

Each is its own build → live-test → unit-tests → lint → roundtable → PR → merge cycle. Ordered by dependency:

1. **P1 — Tenancy + identity** (`tiered-llm-p1-tenancy-identity.md`). Team/project entities; bind OIDC subjects and `cert:CN`s to teams; OIDC on the kb data plane. *Foundation — everything else references a team.*
2. **P2 — kb egress authority + org model catalog** (`tiered-llm-p2-kb-egress-authority.md`). The core: kb offers a catalog, server merges a blended roster, `egress: via-kb` routes server → kb → vendor with org keys held only in kb. *The heart of the plan.*
3. **P3 — Per-team/project cost attribution** (`tiered-llm-p3-cost-attribution.md`). Add the team/project dimension to `token_audit` at the kb egress point; extend the existing dashboards. *Cheapest, highest-visibility win once P1+P2 exist.*
4. **P4 — Budgets + rate limits** (`tiered-llm-p4-budgets-and-limits.md`). Turn tracked spend into enforced caps; per-team/per-key rate windows replacing the single global bucket.
5. **P5 — OIDC control plane: kb manages aimee-servers** (`tiered-llm-p5-oidc-control-plane.md`). Server registry + kb→server management channel + per-user OIDC identity propagation. *Shares identity work with P1/P4.*
6. **P6 — AWS Bedrock + vendor breadth** (`tiered-llm-p6-bedrock-and-breadth.md`). A Bedrock IR backend with SigV4/STS; STS short-lived creds are the one case that is safe even for a user-run server. *Independent; slot where a vendor-breadth pitch needs it.*
7. **P7 — Hardened kb vault** (`tiered-llm-p7-hardened-kb-vault.md`). Once P2 lands, kb holds every org vendor key for every team in one store — the highest-value secret store in the system. Harden it: envelope encryption with an external root of trust (KMS/HSM/TPM), seal/unseal, use-not-fetch (kb signs/calls internally; plaintext never crosses the API), rotation, per-team/provider key isolation, memory hygiene, WORM-audited key use. *Security-critical companion to P2 — should land with or immediately after it, before real org keys are entrusted to kb.*
8. **P8 — mTLS on the thin-client↔server link** (`tiered-llm-p8-thinclient-mtls.md`). Per-client certs so the thin-client↔server link is mutually authenticated, not bearer-only — extending the machine-identity spine to human-operated CLI clients. A default-flip (the server-side `mtls=required` machinery + enrollment CA already exist). *Independent; hardening.*
9. **P9 — Telemetry tiering** (`tiered-llm-p9-telemetry-tiering.md`). Forward all server logs/metrics to kb over mTLS, tagged with the originating user/server; expose kb telemetry to the enterprise in a standardized way (OTLP + Prometheus `/metrics`); and split the PII posture — permissive local display on the single-user server (best-effort secret redaction) vs. scrubbed, tagged forwarding to the multi-tenant kb. Also fixes the write-only IR metrics. *Reuses P1 tags and the P2 mTLS channel; feeds P3 and P5.*

**Recommended order:** P1 → P3 → P2 **+ P7** → P4 → P5 → P9 → P6; P8 anytime (independent hardening). P3 lands right after P1 as a few columns for the biggest visible payoff; P2 is the largest packet and **P7 must accompany it** — do not entrust real org keys to kb until the hardened vault is in place. P9 lands after the egress + fleet surfaces exist (it aggregates them). P5 reuses P1/P4 identity; P6 and P8 are independent.

## Non-goals

- No virtual-key subsystem (see ADR).
- No multi-tenancy inside aimee-server — it stays single-user by design; tenancy lives on kb.
- No routing of personal egress through kb — personal models stay direct.
- Not a from-scratch billing engine — P3 extends the cost accounting that exists.
