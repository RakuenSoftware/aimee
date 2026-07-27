# KB inference backends

`aimee-kb` runs no model. It stores source and derived records, schedules work, and calls an inference
service for embedding, reranking, extraction, or synthesis.

The standard backend is `aimee-llm` on the deployment network.

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
