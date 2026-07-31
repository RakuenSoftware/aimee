# Local inference is two roles, not four

aimee runs an embedder and a synthesis model. That is the whole taxonomy.

| Role | What it does | Served by |
| --- | --- | --- |
| **Embedding** | vector search over memories, documents and code | baked into `aimee-kb`, or an external endpoint |
| **Synthesis** | every LLM call the KB makes: extraction, indexing, entity judgement, topic synthesis | the optional `llm` compose service, or an external OpenAI-compatible endpoint |

Earlier versions of this document described four: embedding, reranking, a cheap
Tier-A synthesis model and a capable Tier-B one. Three of those distinctions did
not survive measurement, and the sections below say what replaced each.

## There is no reranking model

A cross-encoder rerank stage was measured across 20 configurations and two
embedders. The best result was +0.0032 NDCG@10 and most were negative, so the
model came out of the stack. See [Retrieval stack](retrieval-stack.md) and the
[reranker report](validation/reranker-and-pipeline-2026-07-29.md).

What stands in its place is `kb_ranker`, a linear in-process ranker over lexical,
dense and recency features. It fits weights from interaction data and falls back
to reciprocal-rank fusion when no model is committed. It runs no neural network
and needs no endpoint, so reranking is now a ranking stage rather than a thing
you have to deploy.

## There is no cheap model for the mechanical stages

Tier-A was pitched as extraction and indexing: mechanical work a small model
could do, so a deployment could run the volume stages locally and pay for a
frontier model only on judgement. The curator still routes stages internally.
They now resolve to one model.

On a 69-note extraction set, gemma-4-E2B scores 0.646 strict F1 and E4B 0.828.
The larger models are being re-measured, so I am not quoting their numbers here
yet. What the two I have show is a curve still climbing at E4B, on the task that
was supposed to be the flat one. A model good enough for extraction is good
enough for judgement, and a model too weak for judgement is too weak for
extraction.

The evidence and its defects are in `bench/tier-a/MEASUREMENT_LOG.md`. Read the
"not fixed" section before leaning on any single figure: the gold set has one
author, n is 69, and the scorer's normalisation rules were fitted against this
data and are worth 6 to 13% of F1 on their own.

**Unsettled:** the ladder above E4B is mid-rerun. An earlier sweep sent
`disable_thinking` on the Tier-A stages, which cost E4B 0.09 F1 by itself (0.738
with thinking suppressed, 0.828 without). That flag is gone from
`kb_curator_provider.c` and every model is being re-run without it. Until that
finishes I have one post-fix data point per size and will not extrapolate a
ladder from it.

## Embedding ships bundled at 384 dimensions

`bekko-a25m` sits inside the `aimee-kb` image with its weights baked in. A fresh
install embeds on first boot with no inference service, no GPU, no download and
no network. It scores 0.5909 NDCG@10 on the frozen-ab-v1 set.

Above 384 dimensions, run your own. Point `AIMEE_EMBEDDER_URL` at a GPU-served
endpoint and supply `EMBEDDER_MODEL` and `AIMEE_EMBEDDING_DIM`, because the KB
sizes its vector columns from the width and cannot derive it from an endpoint it
does not serve. The measurement winner is not the bundled one:
`nomic-embed-text-v2-moe` scored 0.6075, but it is 768-dim, about 6x slower on
CPU, needs its card's prefixes to reach that number, and cost 1.8GB of image.
[The selection report](validation/embedder-selection-frozen-ab-v1.md) has both.

Changing the embedder after ingest invalidates every stored vector. DB2 records
the dimension it built its columns with and refuses startup on drift, which is
deliberate: a silently empty vector search is worse than a hard start failure.

## Synthesis is one URL, and you pick what answers it

`AIMEE_LLM_URL` points at an OpenAI-compatible endpoint. It defaults empty, and
that is not an oversight. The old `aimee-llm` gateway container is retired, so
any default would aim every deployment at a dead host.

Two ways to fill it:

- **Local.** Bring up the `curator-llm` compose profile. It runs upstream
  `llama.cpp:server` against `ggml-org/gemma-4-E4B-it-GGUF:Q4_K_M`, fetched once
  on first boot into a volume. `CURATOR_LLM_HF_REPO` overrides the model.
- **External.** Any OpenAI-compatible endpoint, hosted or your own.

The curator calls synthesis only from the background drain, so a model still
warming up never blocks ingest.

### Local synthesis on CPU works, and it is slow

Derived from `llama-bench` on 8 threads (`bench/tier-a/results/cpu/`), E4B at
Q8_0 prefills at 19.4 tok/s and generates at 3.28 tok/s. The extraction prompt
runs 279 prompt tokens and 25 completion tokens at the median, which puts a note
at roughly 22 seconds, or about 160 notes an hour. That is a calculation from
component throughput, not an end-to-end timing.

Extraction runs on every memory continuously, so a busy knowledge base will feel
it. Topic synthesis is far rarer and cheaper to host locally. If synthesis points
at an external endpoint, none of this applies.

Expect the local option to cost real memory as well as real CPU. E4B is not a
small resident.

## Tuning is per-deployment, one variable at a time

Concurrency, context, GPU layers, CPU expert offload and batch size are
deployment settings, not product tiers. Change one and record:

- model identity and digest;
- driver and runtime version;
- resident memory;
- first-token and total latency;
- tokens per second;
- maximum stable concurrent slots;
- retrieval and structured-output quality.

An out-of-memory restart is not backpressure. Lower slots or context until the
service stays ready under the expected mixed load.

A mixture-of-experts model fits a smaller card than its parameter count
suggests: route the expert tensors to CPU with `-ot ".ffn_.*_exps.=CPU"` and keep
attention resident. That is a memory strategy rather than a quality one. On the
extraction set, dense and MoE models of comparable size scored within noise of
each other, and MoE's gain was throughput.

## Consumers share the endpoint, with separate limits

The KB uses synthesis for curator and retrieval work. The server may also
register the same model as a free delegate. These have separate admission limits
so background KB work cannot take every interactive slot.

## What to set

```yaml
environment:
  AIMEE_EMBEDDER_URL: ""      # empty: bundled bekko-a25m, 384-dim
  AIMEE_LLM_URL: ""           # empty: no synthesis; fill it or run curator-llm
```

Decide the embedder before you ingest anything, because changing it later means a
full re-embed. Decide synthesis whenever you like, because it is a URL.

See [KB inference backends](KB_LLM_BACKENDS.md) and
[Retrieval stack](retrieval-stack.md).
