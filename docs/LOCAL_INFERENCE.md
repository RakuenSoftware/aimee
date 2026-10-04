# Local inference

Aimee instances use embedding and synthesis roles. Server owns personal runtime data; KB owns
shared knowledge. Each role uses its configured model endpoints through the existing network and
credential owners. Model execution runs separately from the application in the standard deployment.

| Role | Local execution | Remote execution |
| --- | --- | --- |
| embedding | embedding sidecar | configured embedding endpoint |
| synthesis | model-specific `aimee-llm-e2b` or `aimee-llm-e4b` sidecar | OpenAI-compatible endpoint |

The native Go memory engine combines lexical, dense and graph signals without requiring a
reranking model. Curator
extraction and reasoning use one synthesis role with stage-specific budgets.

## Embedding

The default `bekko-a25m` embedder uses 384 dimensions. The standard embedding sidecar includes the weights and runs on
CPU. Pulling its image downloads those weights; ordinary startup needs no separate model download. Configure an external endpoint with `embedder_url`, `embedder_model`, and
`embedder_dims`, or their documented environment overrides.

Model identity includes dimension, pooling, prefixes, and serving identity. Changing any part after
ingest changes the vector space. Follow [Change the KB embedder](runbooks/change-embedder.md) before
switching a corpus.

The frozen-ab-v1 evaluation measured `bekko-a25m` at 0.5909 NDCG@10 and
`nomic-embed-text-v2-moe` at 0.6075. Nomic uses 768 dimensions, ran about six times slower on the
measured CPU, and added about 1.8 GB to the image. The
[selection report](validation/embedder-selection-frozen-ab-v1.md) records the setup and limits.

## Synthesis

Set `synthesis_endpoint`, `synthesis_model`, and `synthesis_api_key` for an external provider. Empty
`synthesis_endpoint` disables synthesis while embedding, search, recall, and indexing continue.

The managed local profile deploys one model-specific `aimee-llm` sidecar. The owning instance reaches it over
mTLS with a dedicated client certificate. The sidecar image fixes the model family; the instance records
the model identity it requests.

| Image | Model | Measured local role |
| --- | --- | --- |
| `aimee-llm-e2b` | gemma-4-E2B-it | smaller local option |
| `aimee-llm-e4b` | gemma-4-E4B-it | stronger measured extraction option |

See [Choosing a synthesis model](SYNTHESIS_MODELS.md) for weights, throughput, quality results, and
hardware guidance.

## Capacity and failure behavior

Local synthesis may be CPU- and memory-intensive. The measured E4B Q8_0 lane on eight CPU threads
prefilled at 19.4 tokens per second and generated at 3.28 tokens per second. Treat those figures as
component measurements and benchmark the complete workload on the target host.

Change one capacity setting at a time and record model digest, device placement, resident memory,
first-token latency, total latency, throughput, stable slots, and structured-output quality. Reduce
context or slots when an out-of-memory restart interrupts readiness under mixed load.

KB reports role health separately from process liveness. A failed role produces explicit
degradation and no dense or synthesized result.

## Configuration example

```yaml
embedder_model: bekko-a25m
embedder_dims: 384
synthesis_endpoint: https://model.example/v1
synthesis_model: your-model
```

Use the [generated configuration reference](gen/configuration.md) for exact keys and environment
overrides. See [KB model backends](KB_LLM_BACKENDS.md) for placement and trust boundaries.

## Native attention memory

The [vLLM native memory plugin](NATIVE_MEMORY_PLUGIN.md) prepares selected Aimee records into
model-specific attention banks on the inference host. Its separately staged release and GPU
requirements differ from the embedding and synthesis sidecars. It consumes Aimee records;
it is not a replacement storage or retrieval engine. Cognee instead implements the
[generic memory backend contract](modules/memory.md#memory-backend-contract).
