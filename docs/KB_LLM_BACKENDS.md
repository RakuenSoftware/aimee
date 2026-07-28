# KB inference backends

`aimee-kb` runs no model. It stores source and derived records, schedules work, and calls an inference
service for embedding, reranking, extraction, or synthesis.

The standard backend is `aimee-llm` on the deployment network.

In a wizard-managed deployment, starting the local LLM is also an identity transaction. The server
generates a persistent 256-bit `AIMEE_LLM_AUTH_TOKEN`, passes it to the KB and LLM only, and configures
`AIMEE_LLM_AUTH_REQUIRED=1` on the KB so none of its clients can silently downgrade to keyless access.
It also configures the KB's unified endpoint and selected role tiers. Every embed, batch, rerank, and chat request carries
that bearer. The gateway starts with its unauthenticated wildcard-bind guard enforced, so a missing
credential fails deployment instead of silently trusting every container on the bridge. This service
identity is distinct from browser login, first-user enrollment, and server-to-KB credentials.
Setup reports success only after executing an authenticated `/auth/verify` request from inside the KB
container, using the endpoint and bearer that the KB actually received.

An external-only topology does not create this local service identity and defaults
`AIMEE_LLM_AUTH_REQUIRED` to `0`; an operator may still set it to `1` together with the external
gateway's bearer. This prevents the local-LLM security rule from disabling intentionally keyless
external endpoints.

The managed contract is explicit:

| Consumer | Configuration received | Request surface |
| --- | --- | --- |
| `aimee-kb` | `AIMEE_LLM_URL=http://aimee-llm:8742`, `AIMEE_LLM_AUTH_TOKEN`, `AIMEE_LLM_MODEL` (default `aimee-synth`), and an optional pinned `AIMEE_EMBEDDING_DIM` | `/embed`, `/embed_batch`, `/rerank`, `/v1/chat/completions`, `/auth/verify` |
| `aimee-llm` | the same `AIMEE_LLM_AUTH_TOKEN`; `AIMEE_LLM_{EMBED,RERANK,SYNTH}_{MODE,TIER,URL}`; `AIMEE_LLM_SYNTH_MODEL`; optional `AIMEE_EMBEDDING_DIM`; and the runtime GPU settings | serves a local role, proxies its configured external URL, or rejects an `off` role; rejects an embedding-dimension mismatch when pinned |
| legacy KB curator sidecars | `LLM_API_KEY` aliasing the same service bearer | the unified gateway's OpenAI-compatible `/v1` surface |

The role configuration and credential form one deployment transaction. The server does not report a
successful local-LLM deploy merely because both containers started: the authenticated probe must also
succeed from the KB's own environment.

## Required operations

| Operation | Used for | Required |
| --- | --- | --- |
| embed / batch embed | dense memory, docs, code, evidence | for dense retrieval |
| rerank | refine fused top-k | optional |
| chat/synthesis | curator extraction, summaries, answer synthesis | optional by pipeline |

A stage reports degradation when its operation is unavailable. Lexical retrieval can still work
without embedding; it must not claim dense or reranked results.

## Configure

Set the inference base URL for the KB, normally through `AIMEE_EMBEDDER_URL` or the matching
descriptor-backed key. Configure one embedding model identity and one dimension for the deployment.

For a split or externally managed stack, set the same `AIMEE_LLM_AUTH_TOKEN` on `aimee-kb` and the
`aimee-llm` gateway. The managed wizard does this automatically. To adopt an existing local gateway,
set the deliberately separate `AIMEE_MANAGED_LLM_AUTH_TOKEN_OVERRIDE`; ordinary inherited
`AIMEE_LLM_AUTH_TOKEN` is ignored during managed credential creation so stale child-service state
cannot win. The override must be a 32–512 character RFC 6750 b64token or setup fails closed.

Use [generated configuration](gen/configuration.md) for current names. Container environment values
override file values where documented.

## Dimension

The model output and DB2 vector-column dimension must match. The KB records the schema dimension and
refuses startup on drift.

Changing to another model with the same width requires a controlled re-embed. Changing width also
requires rebuilding the derived vector tables. See [Retrieval stack](retrieval-stack.md).

## Custom backend

A custom service must provide:

- bounded request and response bodies;
- stable model identity and dimension;
- timeouts and cancellation;
- deterministic error classification;
- batch behavior that preserves input order;
- health and readiness separate from process liveness;
- no silent fallback to a different model.

Keep credentials and provider endpoints in the owning deployment, not KB documents or workflow
artifacts.

## Validate

Before enabling traffic:

1. probe health and model identity;
2. embed a fixed string and verify dimension;
3. run a batch and verify order/count;
4. rerank a fixed candidate set;
5. run one structured curator response through its schema;
6. stop the backend and confirm honest degradation;
7. restart it and confirm queued work resumes.

See [Inference tiers](AIMEE_KB_SYNTH_TIERS.md).
