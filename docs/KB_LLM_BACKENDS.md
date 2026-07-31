# The KB runs one model and calls one endpoint

`aimee-kb` serves the bundled embedder from inside its own container. Everything
else it needs from a model, it asks an OpenAI-compatible endpoint for over HTTP.
There is one such endpoint, `AIMEE_LLM_URL`, and it is synthesis only.

The `aimee-llm` gateway that used to sit between the KB and its models is
retired. `AIMEE_LLM_URL` defaults empty rather than pointing at it, because a
default aimed at a container nobody deploys any more fails at first use instead
of at configuration time. Two roles, and how each is served, are in
[Local inference](LOCAL_INFERENCE.md).

## Configuration the KB actually receives

| Variable | Role | Default |
| --- | --- | --- |
| `AIMEE_EMBEDDER_URL` | embedding, external | empty: the in-container model serves |
| `EMBEDDER_MODEL` | embedder identity | empty: bundled `bekko-a25m` |
| `AIMEE_EMBEDDING_DIM` | vector width | empty: 384, the bundled model's output |
| `AIMEE_LLM_URL` | synthesis | empty: synthesis stages stay idle |
| `AIMEE_LLM_MODEL` | synthesis model name | `aimee-synth` |
| `AIMEE_LLM_AUTH_TOKEN` | bearer for the synthesis endpoint | none |
| `AIMEE_LLM_AUTH_REQUIRED` | refuse a keyless synthesis request | `0` |

`aimee config deploy-env` emits these, and the container has to receive them. A
wizard that selects an external embedder but whose choice never reaches the KB
leaves the builtin serving forever, silently, which is why the emission is
tested rather than assumed.

With `AIMEE_LLM_AUTH_REQUIRED=1` and no token resolvable from the Vault,
`kb_curator_provider_for_stage` returns no provider and the stage stays idle. It
does not fall back to a keyless request. An external-only topology defaults the
flag to `0` so an intentionally keyless endpoint is not broken by a rule written
for a local one; an operator can still set it to `1` alongside the external
endpoint's bearer.

To adopt an existing local gateway, set `AIMEE_MANAGED_LLM_AUTH_TOKEN_OVERRIDE`.
Inherited `AIMEE_LLM_AUTH_TOKEN` is deliberately ignored during managed
credential creation, so stale child-service state cannot win. The override must
be a 32 to 512 character RFC 6750 b64token or setup fails closed.

## Two operations, and only one of them is required

| Operation | Used for | Required |
| --- | --- | --- |
| embed / batch embed | dense memory, docs, code, evidence | for dense retrieval |
| chat / synthesis | curator extraction, summaries, answer synthesis | optional per pipeline |

A stage reports degradation when its operation is unavailable. Lexical retrieval
still works without embedding. It must not claim dense results while doing so.

## Changing the embedder means re-embedding

The model output width and the DB2 vector-column width must match. The KB records
the schema dimension and refuses startup on drift.

Another model at the same width still needs a controlled re-embed, because the
vectors mean something different. A different width also needs the derived vector
tables rebuilt. See [Retrieval stack](retrieval-stack.md).

## What a custom backend has to provide

- bounded request and response bodies;
- stable model identity and dimension;
- timeouts and cancellation;
- deterministic error classification;
- batch behaviour that preserves input order;
- health and readiness separate from process liveness;
- no silent fallback to a different model.

The last one is the expensive failure. A backend that quietly serves a different
model produces vectors that are wrong in a way no dimension guard catches.

Keep credentials and provider endpoints in the owning deployment, not in KB
documents or workflow artifacts.

## Validate before enabling traffic

1. probe health and model identity;
2. embed a fixed string and verify the dimension;
3. run a batch and verify order and count;
4. run one structured curator response through its schema;
5. stop the backend and confirm honest degradation;
6. restart it and confirm queued work resumes.

Step 5 is the one people skip. A backend that looks healthy under load and lies
about it when it is down costs more than one that never started.

Use [generated configuration](gen/configuration.md) for current names. Container
environment values override file values where documented.

See [Local inference](LOCAL_INFERENCE.md).
