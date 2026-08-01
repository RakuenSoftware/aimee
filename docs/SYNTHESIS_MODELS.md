# Choosing a synthesis model

aimee makes every KB reasoning call — extraction, indexing, entity judgement,
topic synthesis — through one endpoint, `AIMEE_LLM_URL`. This page is about what
you put behind it.

There used to be two answers to that question, a cheap model for the mechanical
stages and a capable one for the reasoning stages. Measurement did not support
the split, so there is now one synthesis role and one model behind it. If you
have read older docs that describe a Tier-A and a Tier-B model, that is the
distinction this page replaces.

## Pick one of three

| You want | Do this | What you get |
| --- | --- | --- |
| **Simplest thing that works** | Point `AIMEE_LLM_URL` at an external OpenAI-compatible endpoint | Best quality, no local GPU or RAM cost, your notes leave the machine |
| **Local, and quality matters most** | an `aimee-kb` image variant with llama.cpp bundled, `gemma-4-E4B-it` (default) | 0.82 F1 extraction, 7.46 GB of weights, 3.3 tok/s on 8 CPU threads |
| **Local, and the box is small** | the same variant, configured for `gemma-4-E2B-it` | 0.69 F1 extraction, 4.61 GB of weights, 6.3 tok/s on 8 CPU threads |

Those weight sizes and throughputs are Q8_0, which is what was measured. The
shipped default is Q4_K_M, which is roughly half the size and faster —
see [the caveats](#caveats-you-should-read-before-leaning-on-any-of-this).
Resident memory is larger than the weights by the KV cache for your configured
context, which is a deployment setting rather than a property of the model.

E4B is the default because it is the better model. E2B exists on this page
because it is roughly half the resident memory and about twice the CPU speed,
and on a small box that is the difference between synthesis running and
synthesis not running.

Anything smaller than these two, we measured and do not recommend. See
[What not to install](#what-not-to-install).

## The numbers

Two tasks were measured. **Extraction** is the high-volume one — it runs on every
memory continuously — and it is scored as strict F1 over triples against a
69-note gold set. **Summarisation** is rare and expensive, scored over 6 topics
for coverage of the source and faithfulness to it.

### Extraction, 69 notes

All four rows come from the same lane with thinking on, so they can be read
against each other:

| Model | F1 | Precision | Recall | Valid JSON | Median latency |
| --- | ---: | ---: | ---: | ---: | ---: |
| gemma-4-E4B-it | **0.8217** | 0.855 | 0.791 | 100% | 309 ms |
| gemma-4-E2B-it | **0.6912** | 0.681 | 0.702 | 94.2% | 2,650 ms |
| *gemma-4-12B-it (reference)* | *0.8472* | *0.792* | *0.910* | *95.7%* | *10,381 ms* |
| *gemma-4-26B-A4B-it (reference)* | *0.8451* | *0.800* | *0.896* | *95.7%* | *44,714 ms* |

The two reference rows show what you give up by staying local and small, and the
answer is less than you might expect: E4B is within 0.026 F1 of a 12B model and
within 0.023 of a 26B one. Neither reference model is an install candidate on a
16 GB card or a CPU-only box, and neither is worth their latency here.

Where E4B does lose is recall — 0.791 against 0.910 for the 12B. It finds fewer
of the facts that are there. It does not invent more.

E4B's low latency is not straightforwardly a speed win: it abstains on 91% of
the notes that have nothing to extract and emits 25 tokens at the median, where
E2B emits 420. E4B is faster because it says less, and it is also more precise
and slightly better at recall while doing so.

### Summarisation, 6 topics

| Model | Format | Coverage | Faithfulness | Invented claims | Median latency |
| --- | ---: | ---: | ---: | ---: | ---: |
| gemma-4-E4B-it | 1.00 | 0.90 | 0.9744 | 1 of 39 | 6,350 ms |
| gemma-4-E2B-it | 1.00 | 1.00 | 0.9268 | 3 of 41 | 3,390 ms |

E2B covers more of the source and invents more while doing it. E4B is the
conservative one on both tasks. Six topics is a small denominator — treat this
table as "both are usable, E4B fabricates less", not as a ranking.

### CPU throughput

`llama-bench`, Q8_0, 8 threads, i7-14700K:

| Model | Parameters | Prompt tok/s | Generation tok/s |
| --- | ---: | ---: | ---: |
| gemma-4-E2B-it | 4.65B | 38.6 | 6.29 |
| gemma-4-E4B-it | 7.52B | 19.4 | 3.28 |

The extraction prompt runs 279 prompt tokens and 25 completion tokens at the
median. That puts a note at roughly 22 seconds on E4B and 11 on E2B, or about
160 and 320 notes an hour. Those are calculations from component throughput, not
end-to-end timings.

## Caveats you should read before leaning on any of this

**The quantisation does not match.** Every number above was measured at Q8_0.
The shipped default is `Q4_K_M`, which is roughly half the size and
faster, and which we did not measure for quality. Expect the shipped
configuration to be somewhat worse than the table says, by an amount we have not
quantified.

**The gold set has one author and n is 69.** One person wrote and labelled the
extraction set. There is no second annotator and no inter-rater agreement
number, so systematic bias in the labels is not detectable from the data.

**The scorer is worth 6 to 13% of F1 on its own.** Its normalisation and alias
rules were fitted against this same data. A different but equally defensible
scorer moves every row in the table.

**Extraction is scored on 69 of 70 notes.** One note is flagged `excluded` in the
gold set and dropped from both numerator and denominator.

**Summarisation has 6 topics.** That is enough to catch a model that cannot do
the task at all, and not enough to separate 0.95 from 0.97.

**E4B is not a small resident.** Local synthesis costs real memory as well as
real CPU, and extraction runs continuously. A busy knowledge base will feel it.

The full measurement record, including the defects found in the harness while
producing these numbers, is in `bench/tier-a/MEASUREMENT_LOG.md`.

## What not to install

We measured the range below 1B parameters specifically to find out whether a
cheap model could carry the mechanical stages. It cannot. Extraction F1, same
69-note set:

| Model | F1 | What happens |
| --- | ---: | --- |
| granite-4.0-h-1b | 0.5147 | Best sub-1B result, still half of E4B |
| Qwen3-0.6B | 0.3056 | |
| granite-4.0-h-350m | 0.2045 | Emits valid JSON on only 30% of notes |
| granite-4.0-350m | 0.1985 | |
| LFM2.5-230M | 0.1061 | |
| LFM2-350M-Extract | 0.0144 | Runs to the token cap on most notes |
| SmolLM2-360M-Instruct | 0.0000 | Omits the required output wrapper entirely |
| gemma-3-270m-it | 0.0000 | Never terminates; hits the cap on every note |

Two further warnings that are not about size:

- **Qwen3.5-0.8B and Qwen3.5-2B are unusable here regardless of their general
  quality.** Both reason to the output cap and never emit the answer — 7,933
  completion tokens at the median against an 8,192 cap, and 0% valid JSON for
  the 0.8B. aimee bounds synthesis output at `MF_LLM_OUT_CAP` (8192), so this is
  a deployment failure, not a benchmark artefact.
- **Thinking mode helps small models and hurts large ones.** E4B gains 0.084 F1
  with thinking on (0.738 → 0.822) and E2B gains 0.045 (0.646 → 0.691). The 26B
  model loses 0.075 (0.920 → 0.845). aimee no longer suppresses thinking, which
  is the right default for the models on this page; if you point synthesis at a
  large external model, prefer it with thinking off. These on/off pairs come
  from different lanes, which is sound for accuracy — the same GGUF answers the
  same wherever it is served — but the latencies are not comparable across them.

## Using an external model

`AIMEE_LLM_URL` takes any OpenAI-compatible endpoint, hosted or self-run:

```yaml
environment:
  AIMEE_LLM_URL: "https://your-endpoint.example/v1"
```

It defaults to empty, and that is deliberate rather than an oversight: the old
`aimee-llm` gateway container is retired, so any default would aim every
deployment at a dead host. Empty means synthesis is off, and the KB works
without it — embedding, search, recall and indexing do not go through this
endpoint.

Your notes are sent to whatever answers that URL. That is the trade the first row
of the decision table is making.

See [Local inference](LOCAL_INFERENCE.md) for how the endpoint fits the rest of
the stack, and [KB inference backends](KB_LLM_BACKENDS.md) for the provider
configuration surface.
