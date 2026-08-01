# Choosing a synthesis model

aimee makes every KB reasoning call — extraction, indexing, entity judgement,
topic synthesis — through one endpoint, `SYNTHESIS_ENDPOINT`. This page is about what
you put behind it.

There used to be two answers to that question, a cheap model for the mechanical
stages and a capable one for the reasoning stages. Measurement did not support
the split, so there is now one synthesis role and one model behind it. If you
have read older docs that describe a Tier-A and a Tier-B model, that is the
distinction this page replaces.

## Pick one of three

| You want | Do this | What you get |
| --- | --- | --- |
| **Simplest thing that works** | Point `SYNTHESIS_ENDPOINT` at an external OpenAI-compatible endpoint | Best quality, no local GPU or RAM cost, your notes leave the machine |
| **Local, and quality matters most** | pull `aimee-kb-llm-e4b` (or `aimee-kb-nomic-llm-e4b`) | 0.82 F1 extraction (at Q8_0; ships UD-Q6_K_XL), 7.46 GB of weights, 3.3 tok/s on 8 CPU threads |
| **Local, and the box is small** | pull `aimee-kb-llm-e2b` (or `aimee-kb-nomic-llm-e2b`) | 0.69 F1 extraction, 4.61 GB of weights, 6.3 tok/s on 8 CPU threads |

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

**Only one comparison in that table is resolved by this data.** A paired
bootstrap over the same 69 notes (5,000 replicates, `harness/bootstrap_ci.py`)
gives:

| Comparison | Δ F1 | 95% CI | |
| --- | ---: | --- | --- |
| E2B − E4B | −0.1305 | [−0.2469, −0.0151] | real |
| 12B − E4B | +0.0255 | [−0.0632, +0.1133] | not resolved |
| 26B − E4B | +0.0234 | [−0.0620, +0.1135] | not resolved |

So E4B genuinely beats E2B. But **this set cannot separate E4B from either
reference model** — both intervals span zero. Earlier drafts of this page read
those gaps as "E4B is within 0.026 F1 of a 12B", which states a closeness the
data does not measure. The defensible claim is stronger and simpler: on a
69-note extraction set, a 7.5B model is not distinguishable from a 12B or a 26B
one. That is partly a statement about the models and partly about the set —
67 gold triples means one triple is worth about 0.01 F1, so a 0.02 gap is two
facts.

Neither reference model is an install candidate on a 16 GB card or a CPU-only
box, and neither is worth their latency here.

The recall column looks like E4B's weak point — 0.791 against 0.910 for the 12B.
Treat that as unmeasured rather than as a finding: the aggregate F1 difference is
not resolved, and the per-component split was not bootstrapped at all.

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

**Most differences on this page are too small for the set to resolve.** With 67
gold triples over 69 scored notes, one triple is worth about 0.01 F1. Every
comparison that matters has been run through a paired bootstrap
(`bench/tier-a/harness/bootstrap_ci.py`, 5,000 replicates, fixed seed, importing
`score.py`'s own matching so it cannot drift from the scorer). Where a delta is
reported without a CI on this page, it has not been tested and should not be
read as a ranking. Note that overlapping single-run CIs do *not* imply a
difference is insignificant — the paired test is the one to read.

**The quantisation still does not match.** Every number above was measured at
Q8_0. The bundled path ships `UD-Q6_K_XL` (Unsloth Dynamic Q6), which is a
different quantisation, so expect the shipped configuration to differ from the
table by an amount nobody has measured. It is close in size — 7.46 GB for E4B and
4.71 GB for E2B, against 7.46 and 4.61 at Q8_0 — but size is not quality, and a
dynamic quant distributes precision differently across layers rather than
uniformly.

An earlier draft named `Q4_K_M`, which is not published in these repos at all.
That would not have failed: llama.cpp's `-hf` falls back to another file when the
named quant is absent, so it would have silently served something else.

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

**The model is verified by digest, and a mirror is untrusted.** The build checks
the fetched GGUF against Hugging Face's own sha256 and fails otherwise. That
matters twice: it catches a truncated or half-written copy, which a length check
cannot see (many downloaders preallocate the full size and fill it in, so a file
can be exactly the right number of bytes and entirely wrong); and it makes
`AIMEE_MODEL_MIRROR` safe to offer, since anyone able to write to a mirror could
otherwise change which model your image runs.

**`AIMEE_MODEL_MIRROR` builds from a local copy.** Point it at a base URL that
serves the GGUF by filename and the build fetches from there instead of Hugging
Face — for an air-gapped or bandwidth-limited build site, or to rebuild the whole
matrix without pulling tens of gigabytes again. The digest check applies either
way, so a mirror cannot quietly substitute a different file.

**llama.cpp is compiled from a pinned tag, not downloaded.** Upstream publishes a
Linux binary for x64 only, so a download-based install cannot produce the arm64
images at all. `LLAMACPP_VERSION` is a git tag, which is what the build verifies — and it is the
only thing deciding which llama.cpp a user runs, so it needs deliberate bumping
for security fixes. Bumping it means re-checking three things, not just the
number: that the tag still carries gemma-4 architecture support, that
`--no-mmproj` and `LLAMA_ARG_MMPROJ_AUTO` still exist, and where the server
target lives (it moved from `examples/` to `tools/`, which changes the cmake
flags).

**The model-to-repo mapping is fixed at build time, not resolved at run time.**
`gemma-4-E2B-it` and `gemma-4-E4B-it` map to
`unsloth/gemma-4-E{2,4}B-it-UD-Q6_K_XL.gguf`, fetched by exact filename and
checked against the digest above. An earlier draft resolved a quant *tag*
(`:Q4_K_M`) at run time instead, which is worse than it sounds: that quantisation
is not published in these repos at all, and llama.cpp falls back to another file
when the named quant is absent — so it would not have failed, it would have
served something else.

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

This conclusion is resolved: the best of them, `granite-4.0-h-1b`, is −0.3070 F1
against E4B, 95% CI [−0.4461, −0.1633]. The *ordering within the table* is not —
the gaps between adjacent rows below 0.3 are a few triples wide and this set
cannot rank them. Read the table as "none of these can do the task", not as a
league.

Two further warnings that are not about size:

- **Qwen3.5-0.8B and Qwen3.5-2B are unusable here regardless of their general
  quality.** Both reason to the output cap and never emit the answer — 7,933
  completion tokens at the median against an 8,192 cap, and 0% valid JSON for
  the 0.8B. aimee bounds synthesis output at `MF_LLM_OUT_CAP` (8192), so this is
  a deployment failure, not a benchmark artefact.
- **Thinking mode helps E4B and hurts the 26B.** Paired bootstrap, same 69
  notes, 5,000 replicates:

  | Model | Δ F1 from thinking | 95% CI | |
  | --- | ---: | --- | --- |
  | E4B | +0.0840 | [+0.0094, +0.1712] | real |
  | 26B | −0.0746 | [−0.1595, −0.0048] | real |
  | E2B | +0.0812 | [−0.0337, +0.2091] | not resolved |

  Both resolved deltas sit close to their interval edge, so read them as "the
  sign is right", not as a magnitude. E2B's gain is the same size as E4B's and
  still fails to resolve, because E2B is noisier on this set. aimee no longer
  suppresses thinking, which the E4B result supports; if you point synthesis at
  a large external model, prefer it with thinking off. These on/off pairs come
  from different lanes, which is sound for accuracy — the same GGUF answers the
  same wherever it is served — but the latencies are not comparable across them.

  One number in this bullet's previous version could not be reproduced: it cited
  E2B thinking-off at 0.646, and no committed prediction file scores that. The
  four E2B lanes score 0.6099 (`ablation-conf`), 0.5793 (`gpu`), 0.5255
  (`promptfix`) and 0.6912 (`thinking`). The table above uses `ablation-conf`,
  the lane that matches E4B's cited thinking-off score. The 0.646 figure is
  unsourced and has been dropped rather than reconciled.

## Running one of these locally

**The model is part of the image.** You do not select it at runtime — you pull the
tag that carries it, exactly as you do for the embedder:

| image | embedder | synthesis |
| --- | --- | --- |
| `aimee-kb` | bekko-a25m (384) | none |
| `aimee-kb-llm-e2b` | bekko-a25m (384) | gemma-4-E2B-it |
| `aimee-kb-llm-e4b` | bekko-a25m (384) | gemma-4-E4B-it |
| `aimee-kb-nomic` | nomic-v2 (768) | none |
| `aimee-kb-nomic-llm-e2b` | nomic-v2 (768) | gemma-4-E2B-it |
| `aimee-kb-nomic-llm-e4b` | nomic-v2 (768) | gemma-4-E4B-it |

Weights are baked at UD-Q6_K_XL — 7.46 GB for E4B, 4.71 GB for E2B — from
unsloth's GGUF repos, which is where the UD quants are published.

**The container downloads nothing.** An earlier design fetched the weights on
first start and cached them on the data volume. That is a support burden: a
first-run download fails on a rate limit, behind a proxy, on a flaky link, or on
an air-gapped host, and it can silently serve the wrong file if a quant tag stops
resolving. Every one of those reaches you as "synthesis never started", long after
the deploy looked fine. `docker pull` is the one download, with the registry's
retry and resume behind it, and an image either has its model or it does not.

The cost is image size and more tags. That is the same trade the embedder already
makes, and the reason it was made there first.

Leave `SYNTHESIS_ENDPOINT` empty on these images: the container starts
llama-server against the baked file and points synthesis at loopback. Setting it
overrides that and uses the remote endpoint instead.

The embedder axis is still the one-way choice: the database records the vector
width and refuses to start if it changes, so pick `-nomic` up front if you want
768-dim vectors. The synthesis axis is not one-way — you can move between an
`-e2b` and an `-e4b` tag freely.

## Using an external model

`SYNTHESIS_ENDPOINT` takes any OpenAI-compatible endpoint, hosted or self-run:

```yaml
environment:
  SYNTHESIS_ENDPOINT: "https://your-endpoint.example/v1"
```

It defaults to empty, and that is deliberate rather than an oversight: the old
`aimee-llm` gateway container is retired, so any default would aim every
deployment at a dead host. Empty means synthesis is off, and the KB works
without it — embedding, search, recall and indexing do not go through this
endpoint.

Your notes are sent to whatever answers that URL. That is the trade the first row
of the decision table is making.

See [KB inference backends](KB_LLM_BACKENDS.md) for the provider configuration
surface.
