# Proposal: P6 — AWS Bedrock + vendor breadth

- **State:** proposed (pending — not started). Part of `tiered-llm-offering.md`.
- **Author:** JBailes (drafted by the engineer agent, 2026-07-17).
- **Depends on:** P2 (egress seam) for the org-model path; standalone otherwise.

## Thesis

The origin ask names **Bedrock** explicitly ("OpenAI, Anthropic, Bedrock, a few others"). aimee supports Anthropic, OpenAI (Chat/Responses), Gemini, and an OpenAI-compatible catch-all, but has **no Bedrock, Vertex, or Azure** — and no SigV4 or AWS auth anywhere (`grep bedrock|vertex|azure` returns only a test string). A "different vendors" pitch has a real gap without Bedrock. Bedrock is also the one provider whose native auth (**STS short-lived, scoped credentials**) is safe to hand even to a user-run aimee-server — a property worth capturing.

## Goal

Bedrock as a first-class egress target: an org model with `provider: bedrock` routes through kb egress (P2) with the org's AWS credentials held in kb; SigV4-signed requests to the Bedrock runtime, mapped through the existing IR.

## §0 What already exists

- **IR + backend adapters** — `src/headers/aimee_ir.h` abstracts wire shapes (`ANTHROPIC`, `OPENAI_CHAT`, `RESPONSES`, `GEMINI`); backends live in `aimee_backend_*.c`. Bedrock hosts Anthropic (Claude) and other models behind an AWS envelope, so most of the *body* maps onto the existing Anthropic/IR shape. The new work is transport and auth, not the message model.
- **Provider driver registry** — `src/server/delegate_driver.c`. Unknown providers fall back to the openai driver; Bedrock needs its own driver (SigV4) rather than the catch-all.
- **Pricing** — `token_tracker.c` static table plus DB1 `model_pricing`. Add Bedrock model prices so P3 attribution and P4 budgets cover Bedrock spend.

## §1 Bedrock driver + SigV4/STS auth

A `bedrock` backend and driver performs AWS SigV4 request signing against `bedrock-runtime.<region>.amazonaws.com` (`InvokeModel` and `InvokeModelWithResponseStream`), taking region and model-id from the catalog entry. Credentials are resolved kb-side from the org AWS creds in the kb vault. **STS AssumeRole → short-lived scoped creds** is the preferred mode: the credential the signer uses is temporary and least-privileged.

## §2 IR mapping

Map IR to the Bedrock request envelope and Bedrock responses/streams back to IR. For Bedrock-hosted Anthropic models this is close to the existing Anthropic backend inside an AWS envelope; keep the mapping in a backend adapter alongside the others so kb and server share one implementation. Handle the Bedrock event stream (`application/vnd.amazon.eventstream`) on the streaming path, built stream-clean per the known buffered-replay/stream-flag caveat.

## §3 Catalog + pricing wiring

- Org Bedrock models register in the P2 catalog with `provider: bedrock`, region, and entitlement.
- Add Bedrock model prices so cost attribution (P3) and budgets (P4) are correct for Bedrock calls — no `(unattributed)` bucket for a priced model.

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
