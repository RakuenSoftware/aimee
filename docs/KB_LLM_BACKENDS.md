# KB inference backends

`aimee-kb` stores source and derived records and schedules work. It serves its own embedder and calls
out for synthesis.

**Embedding is in-container.** The weights ship inside the KB image, so there is no inference
container and no first-boot download. Select the model in the wizard's Deploy topology step, or set
`embedding_model`. Until one is selected the KB serves a builtin lexical embedder and logs that it is
doing so. Retrieval works, but it is not dense.

**Which model to put behind the synthesis endpoint — the measured
candidates, their numbers and their caveats — is
[Choosing a synthesis model](SYNTHESIS_MODELS.md).**

To embed against something else, set `AIMEE_EMBEDDER_URL`. That takes precedence, and the bundled
model stays unloaded.

**Synthesis is external-only.** `AIMEE_LLM_URL` has no default, so a deployment that wants synthesis
supplies its own OpenAI-compatible endpoint. Unset, synthesis stages report degradation rather than
inventing a result.

The `aimee-llm` container that previously served both roles is retired.

| Consumer | Configuration received | Request surface |
| --- | --- | --- |
| `aimee-kb` | `AIMEE_LLM_URL` (synth only, no default), `AIMEE_LLM_AUTH_TOKEN`, `AIMEE_LLM_MODEL` (default `aimee-synth`). Embedding is in-container; `AIMEE_EMBEDDER_URL` overrides it with an external endpoint | `/v1/chat/completions` on the synth endpoint |
| legacy KB curator sidecars | `LLM_API_KEY` aliasing the same service bearer | the endpoint's OpenAI-compatible `/v1` surface |

`AIMEE_LLM_AUTH_REQUIRED` defaults to `0`. Set it to `1` with the endpoint's bearer to stop clients
downgrading to keyless requests.

## Required operations

| Operation | Used for | Required |
| --- | --- | --- |
| embed / batch embed | dense memory, docs, code, evidence | for dense retrieval |
| chat/synthesis | curator extraction, summaries, answer synthesis | optional by pipeline |

A stage reports degradation when its operation is unavailable. Lexical retrieval can still work
without embedding; it must not claim dense results.

## Configure

Configure one embedding model identity and one dimension for the deployment. Leave
`AIMEE_EMBEDDER_URL` unset to use the bundled model; set it to embed against your own endpoint.

For synthesis, set `AIMEE_LLM_URL` and, where the endpoint authenticates, `AIMEE_LLM_AUTH_TOKEN`.

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
4. run one structured curator response through its schema;
5. stop the backend and confirm honest degradation;
6. restart it and confirm queued work resumes.

Step 5 is the one people skip. A backend that looks healthy under load and lies
about it when it is down costs more than one that never started.

Use [generated configuration](gen/configuration.md) for current names. Container
environment values override file values where documented.

See [Inference tiers](AIMEE_KB_SYNTH_TIERS.md) and
[Choosing a synthesis model](SYNTHESIS_MODELS.md).
