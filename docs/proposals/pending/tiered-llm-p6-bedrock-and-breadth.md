# Proposal: P6 — AWS Bedrock + vendor breadth

- **State:** proposed (pending — not started). Part of `tiered-llm-offering.md`.
- **Author:** JBailes (drafted by the engineer agent, 2026-07-17).
- **Depends on:** P2 (egress seam) for the org path, and **P3/P4** for the Bedrock cost-attribution + budget acceptance criteria (Bedrock prices must wire into both); standalone otherwise.

## Thesis

The origin ask names **Bedrock** explicitly ("OpenAI, Anthropic, Bedrock, a few others"). aimee supports Anthropic, OpenAI (Chat/Responses), Gemini, and an OpenAI-compatible catch-all, but has **no Bedrock, Vertex, or Azure** — and no SigV4 or AWS auth anywhere (`grep bedrock|vertex|azure` returns only a test string). A "different vendors" pitch has a real gap without Bedrock. Bedrock's native auth is also a defence-in-depth win: kb can resolve the org creds to **STS short-lived, scoped credentials** (AssumeRole) before signing, so the credential the kb signer holds is temporary and least-privileged. Like every org credential it stays on kb and is never handed to the server (invariant #1); short-lived STS narrows the blast radius even at the kb tier. (A user's *own* personal AWS creds, by contrast, may live on their server like any personal-tier key.)

## Goal

Bedrock as a first-class egress target: an org model with `provider: bedrock` routes through kb egress (P2) with the org's AWS credentials held in kb; SigV4-signed requests to the Bedrock runtime, mapped through the existing IR.

## §0 What already exists

- **IR + backend adapters** — `src/headers/aimee_ir.h` abstracts wire shapes (`ANTHROPIC`, `OPENAI_CHAT`, `RESPONSES`, `GEMINI`); backends live in `aimee_backend_*.c`. Bedrock is **not one wire format**: `InvokeModel` takes a **model-family-specific** request/response body (Anthropic Messages, Amazon Titan/Nova, Meta Llama, Mistral, Cohere each differ), so only **Claude-on-Bedrock** maps cleanly onto the existing Anthropic IR shape — other families do **not**. Bedrock's **Converse/ConverseStream** API, by contrast, exposes **one normalized message+tool schema across all families**, which is the natural IR mapping target. So the message-model work is real for non-Claude models, not zero; the design (§2) makes Converse the primary path and gates native per-family `InvokeModel` adapters behind an explicit allowlist.
- **Provider driver registry** — `src/server/delegate_driver.c`. Unknown providers fall back to the openai driver; Bedrock needs its own driver (SigV4) rather than the catch-all.
- **Pricing** — `token_tracker.c` static table plus DB1 `model_pricing`. Add Bedrock model prices so P3 attribution and P4 budgets cover Bedrock spend.

## §1 Bedrock driver + SigV4/STS auth

A `bedrock` backend and driver performs AWS SigV4 request signing against `bedrock-runtime.<region>.amazonaws.com` (`InvokeModel` and `InvokeModelWithResponseStream`), taking region and model-id from the catalog entry. Credentials are resolved kb-side from the org AWS creds in the kb vault. **STS AssumeRole → short-lived scoped creds** is the preferred mode: the credential the signer uses is temporary and least-privileged. The base credential used to `AssumeRole` — a long-lived org IAM key, or (preferred) a federated web-identity/OIDC role — is itself an org secret held in the **P7 kb vault** and used in place, never on the server; STS then narrows it to a short-lived scoped session per call. With web-identity federation no long-lived AWS key is stored at all. **Two modes, kept strictly distinct — each maps to a *specific* AWS STS operation, never conflated:**

**(a) workload-identity federation** (preferred) — kb obtains a **short-lived OIDC token** from a **named** workload-identity mechanism and calls **`AssumeRoleWithWebIdentity`** (not `AssumeRole`) with it, so **no long-lived AWS key exists**. The three token sources are *not* interchangeable; each needs its own AWS trust integration:
- **cloud IAM** (EKS IRSA / Pod Identity, GKE Workload Identity) — the platform projects a signed OIDC token into the workload; AWS trusts the cluster's registered **IAM OIDC identity provider**, and the role trust policy is `Principal:{Federated:<oidc-provider-arn>}`, `Action:sts:AssumeRoleWithWebIdentity`, `Condition:StringEquals{<issuer>:sub,<issuer>:aud}`.
- **SPIFFE** — the SVID is presented as a **JWT-SVID** to the same `AssumeRoleWithWebIdentity` call; AWS must have an IAM OIDC provider registered for the SPIFFE **trust-domain issuer**, and the trust policy pins `sub` (the SPIFFE ID) and `aud`.
- **generic OIDC web-identity** — same call; an IAM OIDC provider for the token's `iss`, trust policy pinning `sub`/`aud`.

kb **validates the token's `iss`/`aud` against the configured expectations before use**, and **refreshes it from its source on expiry — the web-identity token is never persisted as a vault secret** (it is a short-lived bearer minted on demand). Revoking the workload identity stops *new* `AssumeRoleWithWebIdentity` immediately; an already-minted token or already-issued STS session stays valid until its own (short) expiry.

**(b) long-lived IAM access key** (fallback) — a real IAM access key held in the P7 kb vault and **used in place to SigV4-sign an `AssumeRole` call**; here the role trust policy names that **IAM principal** (`Principal:{AWS:<iam-principal-arn>}`, `Action:sts:AssumeRole`), *not* a federated identity. This IAM key is an org secret like any other: a **per-team/per-provider-isolated DEK** under P7 §9 (its own AAD-bound slot), rotated on the P7 cadence with the old key revoked at AWS/IAM, never on the server — compromise of one team's key never exposes another's.

Both modes carry a per-tenant **`ExternalId`** on the assume call (confused-deputy protection). The distinction is binding: mode (a) → `AssumeRoleWithWebIdentity` + `Federated` trust principal; mode (b) → `AssumeRole` + `AWS` (IAM) trust principal.

**Least-privilege session policy is derived per Bedrock target type** (a single
`bedrock:InvokeModel*` on "the model/region" is **not** a sufficient general rule,
because inference profiles route across multiple foundation-model ARNs and regions).
The session policy grants the invoke actions matching the wire (§2) — `bedrock:InvokeModel`
/ `bedrock:InvokeModelWithResponseStream` for native adapters, `bedrock:Converse` /
`bedrock:ConverseStream` for the Converse path — over a **resource-ARN set resolved from
the catalog entry's target type**:
- **foundation model** → `arn:<partition>:bedrock:<region>::foundation-model/<model-id>`.
- **provisioned / custom model** → `arn:<partition>:bedrock:<region>:<account>:provisioned-model/<id>` (or `custom-model/<id>`).
- **application inference profile** → the `application-inference-profile/<id>` ARN **plus** the underlying **foundation-model ARNs in every region the profile routes to** (invoking a profile is authorized against both the profile and its target models).
- **cross-region (system-defined) inference profile** → the `inference-profile/<id>` ARN **plus** the foundation-model ARNs in **each destination region** the profile fans out to.

All ARNs, the **AWS partition** (`aws` / `aws-us-gov` / `aws-cn`), and the destination
**region set** are resolved from **primary-authoritative catalog configuration**, never a
client-supplied string. If an exact least-privilege policy for the target type cannot be
constructed (unknown target type, unresolved profile routing, missing region set), egress
**fails closed** — kb does not fall back to a broad `Resource:*` or `InvokeModel*` grant.
The RoleArn is **team-bound** with a per-tenant **`ExternalId`** (confused-deputy
protection on cross-account assume). A concrete **maximum STS TTL (≤15 min)** is fixed. The STS session cache is
**instance-local and explicitly non-authoritative**, and its **cache key is tenant- and
policy-complete** — it includes **org/team, provider key-slot + credential generation,
RoleArn, ExternalId, AWS partition + region(s), the model-or-inference-profile target,
a normalized session-policy hash, and (mode (a)) the workload-identity subject** — so a
session minted for one team/model/region/role/policy can never be served to another. On
**every request** kb re-authorizes on the **primary** and **rejects** any cached entry
whose role, slot/credential generation, entitlement, session-policy hash, or revocation
generation no longer matches (a rotation or entitlement revocation bumps the generation
and invalidates the entry). Negative tests prove a cached session cannot cross **teams,
models, regions, roles, credential rotations, or entitlement revocations**. (STS acquisition is **not** atomic with the P4 reservation — the
reservation commits first, then AssumeRole runs, per the two-transaction model; there is
no "single write".) **Every Bedrock signing** (not only the AssumeRole/derivation) is
WORM-audited on the default-on P7 §6 path, **fail-closed** (if the audit append cannot
commit, the sign is refused); the audit row records team/model/region/request_id and
**never** the session creds or SigV4 secret headers — a test asserts AWS creds and
`Authorization`/`X-Amz-*` secrets never appear in logs/traces/errors/persistence. **Any
key-holding kb** (mode (b), or any deployment storing org creds) must be **sealed under a
non-`file` anchor** and **fail closed at boot** on a `file` root (invariant #6); an
acceptance test asserts **cross-team DEK independence** enforced by the P7 §9
per-`(team,provider)` key-slot schema (each its own AAD-bound DEK) — one team's
AWS DEK cannot decrypt another's). Honesty on mode (a): revoking the workload identity
stops *new* AssumeRole, but an already-minted web-identity token (and any already-issued
STS session) stays valid until its own expiry — so TTLs are kept short; revocation is
not instantaneous for in-flight credentials. **Ordering (not a false atomic):** AssumeRole
is external, so it is not literally atomic with the P4 reservation — the reservation is the
primary write that commits *first* (T1), then AssumeRole and SigV4 signing happen after,
like the vendor call in the two-transaction model. **Every Bedrock signing** (not only the
AssumeRole/derivation) is on the default-on P7 §6 WORM key-use path, so no later per-call
signing bypasses the vault audit. **The egress target — region, model, and (for a
cross-region inference profile) its fixed destination-region set — is taken from the
catalog entry**; a request cannot select an arbitrary region or model. (A cross-region
inference profile legitimately spans several regions, but *which* regions is a fixed
property of the catalogued target, not a client choice.) An acceptance test asserts
**cross-team AWS DEK independence**.

## §2 IR mapping

**Wire families and adapters.** Bedrock `InvokeModel` bodies are **model-family-specific**,
so the IR mapping is defined **per family**, not once:
- **Converse / ConverseStream (primary path)** — Bedrock's unified message+tool API maps
  directly onto the IR for every family it supports (Claude, Amazon Nova/Titan, Llama,
  Mistral, Cohere, …). This is the default adapter and the reason vendor breadth is
  tractable without one bespoke serializer per model family.
- **Native `InvokeModel` adapters (allowlisted)** — where a model has no Converse support,
  or a needed feature/parameter is only reachable via the native body, a **per-family native
  adapter** handles it. For Bedrock-hosted **Anthropic** models the native body is close to
  the existing Anthropic backend inside an AWS envelope.

The catalog entry carries **`bedrock_api` (`converse` | `invoke`)** and **`model_family`**;
a model whose `(bedrock_api, model_family)` pair has **no registered adapter is rejected at
catalog validation and before any reservation or egress** — kb never signs a request it
cannot faithfully serialize and parse, and never silently falls through to the openai
catch-all. Keep every adapter alongside the other backends so kb and server share one
implementation. Handle the Bedrock event stream (`application/vnd.amazon.eventstream`) on the streaming path, built stream-clean per the known buffered-replay/stream-flag caveat. Because this is hand-parsed binary framing in C, the decoder **validates the per-message CRC, bounds frame/header allocation (reject oversized), parses headers strictly, and handles AWS error/exception frames explicitly** — no unbounded reads, no trust of attacker-influenced length fields (memory-safety discipline, fuzz-tested). Framing/decode happens at **kb egress** (kb is the egress authority); kb relays tokens **in-flight and does not buffer/persist** prompt or completion content (invariant #7 governs telemetry, not the in-flight egress stream — see P2).

## §3 Catalog + pricing wiring

- Org Bedrock models register in the P2 catalog with `provider: bedrock`, **`bedrock_api`**
  (`converse` | `invoke`), **`model_family`**, **target type** (`foundation-model` |
  `provisioned` | `custom` | `application-inference-profile` | `cross-region-inference-profile`),
  AWS **partition**, region (or, for a cross-region profile, the **destination-region set** and
  underlying **foundation-model ARNs** it routes to), and entitlement — all
  **primary-authoritative**, so both the §2 adapter selection and the §1 least-privilege
  policy derivation read from one verified source, never a client string.
- Add Bedrock prices to the **DB2 `org_model_pricing`** table (P3), keyed by region + model/version + inference profile (Bedrock pricing varies on all three), versioned; a call pins an **immutable** `org_model_pricing` version (the *current-version pointer* is read authoritative/primary; the pinned version row never changes, so it is safely cacheable/replica-readable — pricing is slow-changing, unlike authorization/spend which must stay on the primary), so P3 attribution and P4 budgets are correct for Bedrock — no `(unattributed)` bucket for a priced model. Vertex/Azure (§4) reuse the driver seam but each still needs its own auth work (OAuth2 SA / deployment routing) — the seam admits them without rework, but they are not free.

## §4 (optional) Vertex / Azure

Vertex (OAuth2 service-account) and Azure OpenAI (deployment-name routing) are the same shape: a signed/authed transport over an existing IR wire. They are out of scope for this packet; the driver seam is built general enough to admit them without rework.

## Acceptance criteria

- An org Bedrock model appears in an entitled user's roster (via P2) with no AWS cred on the server; a chat returns a completion; streaming works (Converse `ConverseStream` or native `InvokeModelWithResponseStream` → IR stream).
- A **non-Claude family** (e.g. Llama or Nova) works via the **Converse** adapter; a model whose `(bedrock_api, model_family)` pair has **no registered adapter is rejected at catalog validation / before reservation**, never routed to the openai catch-all.
- **Federation is mode-correct:** mode (a) calls **`AssumeRoleWithWebIdentity`** with a validated (`iss`/`aud`) workload-identity token that is **never persisted as a vault secret**; mode (b) SigV4-signs **`AssumeRole`** with a vault-held IAM key. A test asserts the web-identity token is refreshed from source and never written to the vault.
- **STS cache isolation:** negative tests prove a cached STS session cannot be reused across **teams, models, regions, roles, credential rotations, or entitlement revocations**; a rotation/revocation bumps the generation and invalidates the cache entry.
- **Inference-profile least privilege:** invoking an application- or cross-region inference profile is authorized by a session policy covering the **profile ARN + every destination-region foundation-model ARN**; when the exact policy cannot be derived, egress **fails closed** (no `Resource:*` fallback). Each target type is tested against IAM-deny and cross-region cases.
- SigV4 signing is verified against a mock or a real dev Bedrock endpoint. STS short-lived creds (≤15 min) are used when configured, and expiry and refresh are handled.
- Bedrock spend is attributed (P3) and counts against the team budget (P4) with correct pricing (region + model/version + inference profile) — no unpriced bucket.
- No AWS long-lived key is written to the server at any point.

## Testing

Unit: SigV4 canonical-request and signature vectors; `AssumeRoleWithWebIdentity` (mode a) and `AssumeRole` (mode b) paths incl. token `iss`/`aud` validation and refresh-without-persist; IR↔Converse mapping across families and IR↔native-`InvokeModel` for the allowlisted families; unsupported `(bedrock_api, model_family)` rejection; per-target-type session-policy derivation (foundation / provisioned / custom / application- / cross-region-inference-profile) incl. fail-closed when underivable; STS cache-key isolation (negative cross-team/model/region/role/rotation/revocation); eventstream framing → IR. Integration: end-to-end org Bedrock call through kb egress (mock or dev endpoint), Converse and native, streaming and non-streaming, a cross-region inference-profile invocation under least-privilege policy, attribution and budget assertions.

## Non-goals

Vertex and Azure implementation (noted, deferred). No Bedrock Agents or Knowledge Bases features — this is model invocation only. No personal-tier Bedrock in this packet (org egress first; a personal Bedrock model could reuse the driver later).
