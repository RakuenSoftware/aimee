# KB model tiers

Tiers are planning estimates for the model services used by a KB. Standard compositions run
embedding and optional model-specific synthesis in separate sidecars; either role can instead
use its configured remote endpoint. The KB application image is not a bundled model runner.

| Tier | Intended host | Typical synthesis shape |
| --- | --- | --- |
| `cpu` | CPU-only host | small extraction and synthesis model |
| `small` | about 16 GB GPU | Gemma 4 12B class |
| `mid` | about 24 GB GPU | Gemma 4 26B-A4B class, two slots |
| `large` | about 32 GB GPU | same class with more slots and context |

These are planning estimates, not readiness guarantees. Local model availability depends on the
selected sidecar and deployment profile. A remote endpoint owns its own sizing and concurrency.

Embedding width is not a tier property. It belongs to the selected embedder and the corpus vector
schema. See [Retrieval stack](retrieval-stack.md).

## Consumers and admission

Curator work and optional answer synthesis use the synthesis role of the selected KB. If that model
is also exposed for delegate work, background and interactive consumers need separate admission
limits so curation cannot take every slot.

In a fleet, admission is per KB and also subject to shared tenant budgets. Adding KB containers must
not multiply a team's hard limit.

## Tune an internal role

Concurrency, context, GPU layers, CPU offload, batch size, and model paths are deployment settings.
Start from the profile default. Change one value at a time and record:

- KB identity and model role;
- model identity and digest;
- driver and runtime version;
- resident memory;
- first-token and total latency;
- tokens per second;
- maximum stable concurrent slots;
- retrieval or structured-output quality.

An out-of-memory restart is not backpressure. Lower slots or context until the KB stays ready under
the expected mixed load.

## Keep routing explicit

Cheap lexical and index work stays with PostgreSQL and KB workers. Embedding and synthesis run in the
selected KB container or at that KB's configured remote endpoint. The server does not choose a
standalone model service and must not move a request to a different KB merely because a model is
available there.

See [KB model backends](KB_LLM_BACKENDS.md) and [KB fleet and model placement](KB_FLEET.md).
