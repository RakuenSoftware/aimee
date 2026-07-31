# Inference tiers

Synthesis runs against an external OpenAI-compatible endpoint; the knowledge base embeds
in-container and needs no inference service. The tiers below describe the synth model
set and concurrency at runtime.

| Tier | Intended host | Synthesis shape |
| --- | --- | --- |
| `cpu` | CPU-only host | small local extraction/synthesis |
| `small` | about 16 GB GPU | Gemma 4 12B class |
| `mid` | about 24 GB GPU | Gemma 4 26B-A4B class, two slots |
| `large` | about 32 GB GPU | same class with more slots and context |

These size the endpoint you run. Embedding width is not a tier property: it is the dimension of the
embedder the KB serves, chosen in the wizard's Deploy topology step. See
[Retrieval stack](retrieval-stack.md).

Hardware estimates include model residency, not every driver or concurrent workload. Validate on the
real host before promising a slot count.

## Consumers

The KB uses the synth endpoint for curator work. The server may also register that model as a free
delegate. These consumers have separate admission limits so background KB work cannot take every
interactive slot.

## Tune

Concurrency, context, GPU layers, CPU expert offload, batch size, and model paths are deployment
settings. Start from the tier default. Change one value at a time and record:

- model identity and digest;
- driver/runtime version;
- resident memory;
- first-token and total latency;
- tokens per second;
- maximum stable concurrent slots;
- retrieval and structured-output quality.

An out-of-memory restart is not backpressure. Lower slots or context until the service stays ready
under the expected mixed load.

## CPU and GPU separation

Cheap lexical/index work stays in PostgreSQL and KB workers. Embedding runs in the KB process;
synthesis goes to the external endpoint. GPU tiers use the larger synth model; the retrieval pipeline
itself does not change.

See [KB inference backends](KB_LLM_BACKENDS.md) and [Retrieval stack](retrieval-stack.md).
