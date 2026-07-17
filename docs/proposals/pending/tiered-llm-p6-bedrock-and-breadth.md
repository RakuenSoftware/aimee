# Proposal: P6 — AWS Bedrock + vendor breadth

- **State:** proposed (pending — not started). Part of `tiered-llm-offering.md`.
- **Author:** JBailes (drafted by the engineer agent, 2026-07-17).
- **Depends on:** P2 (egress seam) for the org path, and **P3/P4** for the Bedrock cost-attribution + budget acceptance criteria (Bedrock prices must wire into both); standalone otherwise.

## Thesis

The origin ask names **Bedrock** explicitly ("OpenAI, Anthropic, Bedrock, a few others"). aimee supports Anthropic, OpenAI (Chat/Responses), Gemini, and an OpenAI-compatible catch-all, but has **no Bedrock, Vertex, or Azure** — and no SigV4 or AWS auth anywhere (`grep bedrock|vertex|azure` returns only a test string). A "different vendors" pitch has a real gap without Bedrock. Bedrock's native auth is also a defence-in-depth win: kb can resolve the org creds to **STS short-lived, scoped credentials** (AssumeRole) before signing, so the credential the kb signer holds is temporary and least-privileged. Like every org credential it stays on kb and is never handed to the server (invariant #1); short-lived STS narrows the blast radius even at the kb tier. (A user's *own* personal AWS creds, by contrast, may live on their server like any personal-tier key.)

## Goal

Bedrock as a first-class egress target: an org model with `provider: bedrock` routes through kb egress (P2) with the org's AWS credentials held in kb; SigV4-signed requests to the Bedrock runtime, mapped through the existing IR.

## §0 What already exists

- **IR + backend adapters** — `src/headers/aimee_ir.h` abstracts wire shapes (`ANTHROPIC`, `OPENAI_CHAT`, `RESPONSES`, `GEMINI`); backends live in `aimee_backend_*.c`. Bedrock hosts Anthropic (Claude) and other models behind an AWS envelope, so most of the *body* maps onto the existing Anthropic/IR shape. The new work is transport and auth, not the message model.
- **Provider driver registry** — `src/server/delegate_driver.c`. Unknown providers fall back to the openai driver; Bedrock needs its own driver (SigV4) rather than the catch-all.
- **Pricing** — `token_tracker.c` static table plus DB1 `model_pricing`. Add Bedrock model prices so P3 attribution and P4 budgets cover Bedrock spend.

## §1 Bedrock driver + SigV4/STS auth

A `bedrock` backend and driver performs AWS SigV4 request signing against `bedrock-runtime.<region>.amazonaws.com` (`InvokeModel` and `InvokeModelWithResponseStream`), taking region and model-id from the catalog entry. Credentials are resolved kb-side from the org AWS creds in the kb vault. **STS AssumeRole → short-lived scoped creds** is the preferred mode: the credential the signer uses is temporary and least-privileged. The base credential used to `AssumeRole` — a long-lived org IAM key, or (preferred) a federated web-identity/OIDC role — is itself an org secret held in the **P7 kb vault** and used in place, never on the server; STS then narrows it to a short-lived scoped session per call. With web-identity federation no long-lived AWS key is stored at all. Two modes, kept distinct: **(a) workload-identity federation** (preferred) — kb's platform identity (SPIFFE / cloud IAM / OIDC web-identity) is named directly in the role's `AssumeRole` trust policy, so **no long-lived AWS key exists**; revoking the workload identity stops *new* `AssumeRole` immediately (an already-issued STS session stays valid until its short expiry — STS TTLs are kept short to bound that window). **(b) long-lived IAM access key** (fallback) — the key is held in the P7 kb vault and used in place to call `AssumeRole`; here the trust policy names that IAM principal (not the workload identity). This IAM key is an org secret like any other: a **per-team/per-provider-isolated DEK** under P7 §9 (its own AAD-bound slot), rotated on the P7 cadence with the old key revoked at AWS/IAM, never on the server — compromise of one team's key never exposes another's. The trust policy names the platform identity only in mode (a).

**STS lifecycle at stateless scale.** "Scoped" means a concrete **least-privilege
session policy** (only `bedrock:InvokeModel*` on the entitled model/region) plus a
**team-bound RoleArn** with a per-tenant **`ExternalId`** (confused-deputy protection on
cross-account AssumeRole), not merely a shorter lifetime — and the session's
scope/RoleArn/region are built from the **primary-verified catalog entry**, never a
client-supplied string. A concrete **maximum STS TTL (≤15 min)** is fixed; a cached
session is re-checked per request against **primary-backed team membership, entitlement,
and revocation**. (STS acquisition is **not** atomic with the P4 reservation — the
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
signing bypasses the vault audit. A cached STS session is re-validated **per request against
primary-backed team membership, entitlement, *and* revocation** — not just cert/credential
revocation. **Region is fixed by the catalog entry** (provider+region+model); a request
cannot select an arbitrary region. An acceptance test asserts **cross-team AWS DEK
independence**.

## §2 IR mapping

Map IR to the Bedrock request envelope and Bedrock responses/streams back to IR. For Bedrock-hosted Anthropic models this is close to the existing Anthropic backend inside an AWS envelope; keep the mapping in a backend adapter alongside the others so kb and server share one implementation. Handle the Bedrock event stream (`application/vnd.amazon.eventstream`) on the streaming path, built stream-clean per the known buffered-replay/stream-flag caveat. Because this is hand-parsed binary framing in C, the decoder **validates the per-message CRC, bounds frame/header allocation (reject oversized), parses headers strictly, and handles AWS error/exception frames explicitly** — no unbounded reads, no trust of attacker-influenced length fields (memory-safety discipline, fuzz-tested). Framing/decode happens at **kb egress** (kb is the egress authority); kb relays tokens **in-flight and does not buffer/persist** prompt or completion content (invariant #7 governs telemetry, not the in-flight egress stream — see P2).

## §3 Catalog + pricing wiring

- Org Bedrock models register in the P2 catalog with `provider: bedrock`, region, and entitlement.
- Add Bedrock prices to the **DB2 `org_model_pricing`** table (P3), keyed by region + model/version + inference profile (Bedrock pricing varies on all three), versioned; a call pins an **immutable** `org_model_pricing` version (the *current-version pointer* is read authoritative/primary; the pinned version row never changes, so it is safely cacheable/replica-readable — pricing is slow-changing, unlike authorization/spend which must stay on the primary), so P3 attribution and P4 budgets are correct for Bedrock — no `(unattributed)` bucket for a priced model. Vertex/Azure (§4) reuse the driver seam but each still needs its own auth work (OAuth2 SA / deployment routing) — the seam admits them without rework, but they are not free.

## §4 (optional) Vertex / Azure

Vertex (OAuth2 service-account) and Azure OpenAI (deployment-name routing) are the same shape: a signed/authed transport over an existing IR wire. They are out of scope for this packet; the driver seam is built general enough to admit them without rework.

## Acceptance criteria

- An org Bedrock model appears in an entitled user's roster (via P2) with no AWS cred on the server; a chat returns a completion; streaming works (`InvokeModelWithResponseStream` → IR stream).
- SigV4 signing is verified against a mock or a real dev Bedrock endpoint. STS short-lived creds are used when configured, and expiry and refresh are handled.
- Bedrock spend is attributed (P3) and counts against the team budget (P4) with correct pricing — no unpriced bucket.
- No AWS long-lived key is written to the server at any point.

## Testing

Unit: SigV4 canonical-request and signature vectors; STS assume-role and refresh; IR↔Bedrock envelope mapping; eventstream framing → IR. Integration: end-to-end org Bedrock call through kb egress (mock or dev endpoint), streaming and non-streaming, attribution and budget assertions.

## Non-goals

Vertex and Azure implementation (noted, deferred). No Bedrock Agents or Knowledge Bases features — this is model invocation only. No personal-tier Bedrock in this packet (org egress first; a personal Bedrock model could reuse the driver later).
