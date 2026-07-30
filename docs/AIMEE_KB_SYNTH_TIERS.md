# Inference tiers

One `aimee-llm` image serves embedding and synthesis. `AIMEE_LLM_TIER` selects the model
set and concurrency at runtime.

| Tier | Intended host | Embedding width | Synthesis shape |
| --- | --- | ---: | --- |
| `cpu` | CPU-only host | 1024 | small local extraction/synthesis |
| `small` | about 16 GB GPU | 2560 | Gemma 4 12B class |
| `mid` | about 24 GB GPU | 2560 | Gemma 4 26B-A4B class, two slots |
| `large` | about 32 GB GPU | 2560 | same class with more slots and context |

Hardware estimates include model residency, not every driver or concurrent workload. Validate on the
real host before promising a slot count.

## Deploy

```yaml
environment:
  AIMEE_LLM_TIER: cpu   # cpu | small | mid | large
```

The normal image downloads models into a persistent volume on first boot. The pre-baked CPU image is
for offline installs. Moving between GPU tiers keeps the 2560-wide embedding schema. Moving between
CPU and GPU widths requires the DB2 re-embed procedure.

## Consumers

The KB uses the gateway for curator and retrieval work. The server may also register the local
synthesis model as a free delegate. These consumers have separate admission limits so background KB
work cannot take every interactive slot.

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

Cheap lexical/index work stays in PostgreSQL and KB workers. Model work goes to `aimee-llm`. GPU
tiers use the larger synth model; the retrieval pipeline itself does not change.

See [KB inference backends](KB_LLM_BACKENDS.md) and [Retrieval stack](retrieval-stack.md).
