# Proposal: P6 — AWS Bedrock + vendor breadth

- **State:** proposed (pending — not started). Part of `tiered-llm-offering.md`.
- **Author:** JBailes (drafted by the engineer agent, 2026-07-17).
- **Depends on:** P2 (egress seam) for the org-model path; standalone otherwise.

## Thesis

The origin ask names **Bedrock** explicitly ("OpenAI, Anthropic, Bedrock, a few
others"). aimee supports Anthropic + OpenAI(Chat/Responses) + Gemini and an
OpenAI-compatible catch-all, but has **no Bedrock, Vertex, or Azure** — no
SigV4/AWS auth anywhere (`grep bedrock|vertex|azure` finds only a test string).
A "different vendors" pitch has a real hole without Bedrock. Bedrock is also the one
provider whose native auth (**STS short-lived, scoped credentials**) is safe to
hand even to a user-run aimee-server — a useful property worth capturing.

## Goal

Bedrock as a first-class egress target: an org model with `provider: bedrock` routes
through kb egress (P2) with the org's AWS credentials held in kb; SigV4-signed
requests to the Bedrock runtime, mapped through the existing IR.

## §0 What already exists

- **IR + backend adapters** — `src/headers/aimee_ir.h` abstracts wire shapes
  (`ANTHROPIC`, `OPENAI_CHAT`, `RESPONSES`, `GEMINI`); backends in
  `aimee_backend_*.c`. Bedrock hosts Anthropic (Claude) and other models behind an
  AWS envelope — so much of the *body* maps onto the existing Anthropic/IR shape;
  what's new is transport + auth, not the message model.
- **Provider driver registry** — `src/server/delegate_driver.c` (unknown provider
  falls back to the openai driver); Bedrock needs its own driver (SigV4), not the
  catch-all.
- **Pricing** — `token_tracker.c` static table + DB1 `model_pricing`; add Bedrock
  model prices so P3 attribution and P4 budgets cover Bedrock spend.

## §1 Bedrock driver + SigV4/STS auth

A `bedrock` backend/driver: AWS SigV4 request signing against
`bedrock-runtime.<region>.amazonaws.com` (`InvokeModel` /
`InvokeModelWithResponseStream`), region + model-id from the catalog entry.
Credentials resolved kb-side from the org AWS creds in the kb vault; support
**STS AssumeRole → short-lived scoped creds** as the preferred mode (the credential
the signer uses is temporary and least-privileged).

## §2 IR mapping

Map IR → Bedrock request envelope and Bedrock response/stream → IR. For
Bedrock-hosted Anthropic models this is close to the existing Anthropic backend
inside an AWS envelope; keep the mapping in a backend adapter alongside the others
so kb and server share one implementation. Handle the Bedrock event stream
(`application/vnd.amazon.eventstream`) on the streaming path — build it stream-clean
per the known buffered-replay/stream-flag caveat.

## §3 Catalog + pricing wiring

- Org Bedrock models register in the P2 catalog with `provider: bedrock`, region,
  and entitlement.
- Add Bedrock model prices so cost attribution (P3) and budgets (P4) are correct
  for Bedrock calls — no `(unattributed)` bucket for a priced model.

## §4 (optional) Vertex / Azure

Vertex (OAuth2 service-account) and Azure OpenAI (deployment-name routing) are the
same shape — a signed/authed transport over an existing IR wire. Out of scope for
this packet; noted so the driver seam is built general enough to admit them without
rework.

## Acceptance criteria

- An org Bedrock model appears in an entitled user's roster (via P2) with no AWS
  cred on the server; a chat returns a completion; streaming works
  (`InvokeModelWithResponseStream` → IR stream).
- SigV4 signing verified against a mock (or a real dev Bedrock endpoint); STS
  short-lived creds used when configured, and expiry/refresh handled.
- Bedrock spend is attributed (P3) and counts against team budget (P4) with correct
  pricing — no unpriced bucket.
- No AWS long-lived key is written to the server at any point.

## Testing

Unit: SigV4 canonical-request/signature vectors, STS assume-role + refresh, IR↔
Bedrock envelope mapping, eventstream framing → IR. Integration: end-to-end org
Bedrock call through kb egress (mock or dev endpoint), streaming + non-streaming,
attribution + budget assertions.

## Non-goals

Vertex/Azure implementation (noted, deferred). No Bedrock Agents/Knowledge-Bases
features — this is model invocation only. No personal-tier Bedrock in this packet
(org egress first; a personal Bedrock model could reuse the driver later).
