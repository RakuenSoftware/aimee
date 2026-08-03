# KB model backends

Embedding and synthesis are `aimee-kb` capabilities. There is no `aimee-llm` service and no
standalone inference tier for the server to call.

## Place each role

Each KB configures the roles independently:

| Role | Internal | Remote | Off |
| --- | --- | --- | --- |
| embedding | Run the selected embedder inside the KB container. | Call the configured embedding endpoint. | Not a supported state: a KB with no embedder refuses to start. |
| synthesis | Run the selected synthesizer inside the KB container. | Call the configured synthesis endpoint. | Curator and answer-synthesis stages report degradation. |

A KB hosts the embedding role internally or remotely — never neither — and may host synthesis, or
not. Available internal models depend on the KB image and deployment profile. A remote role uses an
explicit endpoint and credential owned by that KB.

The current configuration descriptors call internal placement `local`:

- `llm_embed_backend`: `local` or `external`;
- `llm_synth_backend`: `local`, `external`, or `off`;
- `embedder_model` and `embedder_dims`: embedding identity and width;
- `embedder_url` and `llm_synth_endpoint`: remote role endpoints.

Use the [generated configuration reference](gen/configuration.md) for the exact fields exposed by
this checkout. Container environment values override file values only where that reference says
they do.

## Multiple KBs

Model placement is per KB, not global to the server. A fleet can include, for example, a KB with an
internal embedder, another with an internal synthesizer, and a KB that calls remote endpoints for
both roles. Routing must first select a KB with the correct corpus and authority, then verify that it
has the required role. It must not route to a model independently of the KB.

The current managed and split profiles deploy one KB. See [KB fleet and model placement](KB_FLEET.md)
for the target routing contract and current implementation boundary.

## Role contracts

| Operation | Used for | Required |
| --- | --- | --- |
| embed and batch embed | dense memory, documents, code, and evidence | required for dense retrieval |
| chat and synthesis | curator extraction, summaries, and answer synthesis | optional by pipeline |

Internal and remote implementations must both provide:

- bounded request and response bodies;
- stable model identity and dimension where the role produces vectors;
- timeouts and cancellation;
- deterministic error classification;
- batch behaviour that preserves input order;
- health and readiness separate from process liveness;
- no silent fallback to a different model or KB.

A stage reports degradation when its selected role is unavailable. It must not claim a dense or
synthesized result after skipping the role.

## Vector identity

The embedding output and DB2 vector-column dimension must match. The KB records the model and serving
identity, including pooling and prefixes, and refuses a mismatched vector space.

Changing width requires the guarded derived-table reset. A same-width model, pooling, or prefix
change still changes the vector space and currently needs a fresh DB2 plus source re-ingestion. See
[Change the KB embedder](runbooks/change-embedder.md).

## Credentials and egress

An internal role stays inside its KB container. A remote role crosses the KB's egress boundary, so
its endpoint, credential, budget, and allowlist belong to that KB's deployment and vault. Do not put
them in documents, workflow artifacts, or server-side model shortcuts.

## Validate a KB role

Before routing traffic to a KB:

1. probe the KB and the selected role;
2. verify the declared model identity and, for embedding, the dimension;
3. run a batch and verify order and count;
4. run one structured synthesis result through its schema when synthesis is enabled;
5. make the role unavailable and confirm honest degradation;
6. restart the KB and confirm its recorded identity still matches.

See [Inference tiers](AIMEE_KB_SYNTH_TIERS.md) for internal model sizing and
[Retrieval stack](retrieval-stack.md) for vector-space rules.
