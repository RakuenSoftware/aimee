# KB model tiers

Tiers size model roles that run inside an `aimee-kb` container. They do not name separate inference
services. A KB can instead use a remote endpoint for either role.

| Tier | Intended host | Typical synthesis shape |
| --- | --- | --- |
| `cpu` | CPU-only host | small extraction and synthesis model |
| `small` | about 16 GB GPU | Gemma 4 12B class |
| `mid` | about 24 GB GPU | Gemma 4 26B-A4B class, two slots |
| `large` | about 32 GB GPU | same class with more slots and context |

These are planning estimates, not readiness guarantees. Internal model availability depends on the
KB image and deployment profile. A remote endpoint owns its own sizing and concurrency.

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
