# Tier-A small-model benchmark: findings

**Date:** 2026-07-30/31 · **Branch:** `bench/tier-a-small-models`
**Question:** can a model in the hundreds-of-millions class serve the curator
Tier-A extraction stage, and how does it compare to the E4B incumbent?

Context: `docs/proposals/pending/dedicated-extraction-model-curator-tier-a.md`
asks whether Tier-A should get its own small model. Its §2 candidate list
(Qwen2.5-1.5B/3B, Gemma-2-2B, Llama-3.2-1B, Phi-3.5-mini) is a year stale. This
run tests the current sub-1B field, plus two larger reference points.

## Headline

**No slam dunk at the M-parameter scale. The honest answer is no.** Under the
production configuration, every small model commits **exactly zero facts**, not
"worse than E4B", zero. The only permissive model that commits anything is
Qwen3-1.7B, which is not an M-parameter model, and it reaches 43% of E4B.

Two distinct causes, and separating them is most of the value here:

- **A config interaction we own.** `MF_CONF_FLOOR` (0.6) discards facts emitted
  with confidence 0.0, which is what sub-1B models produce. Lift the floor and
  the same runs score 0.14–0.42. This is the same end state as the
  thinking-truncation bug in §1 of the proposal, job runs, `typed_facts` stays
  empty, reached by a different route, and in production it would have looked
  like "the small model cannot extract".
- **A genuine capability floor.** Below ~600M the models cannot do the task even
  with the floor lifted: the best 350M result is 0.179, and two models never
  produce the output shape at all.

E4B is unaffected by the first because it emits real confidences, its floored
and floor-free scores differ by 0.004.

## What was measured

The real task, not a proxy. The `memory_facts` drain is the highest-volume Tier-A
call: one note in, subject-relation-object triples out. The harness reconstructs
`MF_SYSTEM_PROMPT_TMPL` (`src/kb/kb_memory_facts.c:52`) and the 17-relation seed
ontology (`src/rel_types.c`) from the C sources and **fails the run if they
drift**. Output parsing mirrors `mf_commit_facts()` exactly: first `{` to last
`}`, require a `facts` array, drop facts with empty fields or confidence below
`MF_CONF_FLOOR`.

Gold set: 70 hand-authored notes, 64 gold triples, across ten categories
(first/third person, multi-fact, implicit, negation, transient, ambiguous, novel
predicate, infra, governance). **23 notes have a deliberately empty gold list**,
over-extraction is the expensive failure for a drain that writes into
`memory_facts`, so it is measured rather than assumed. Labelling rules and the
external-validity limits are in `data/LABELING.md`.

Every number below is regenerated from committed result files by
`harness/summarize.py`; raw per-note predictions are committed too, so any figure
traces back to the model output that produced it.

## GPU accuracy: production prompt

| model | params | licence | F1 prod | F1 no-floor | precision | recall | schema | abstain | med ms |
|---|---|---|---:|---:|---:|---:|---:|---:|---:|
| unsloth/gemma-3n-E4B-it | E4B | gemma | 0.582 | 0.578 | 0.526 | 0.641 | 0.94 | 0.05 | 1760.4 |
| Qwen/Qwen3-1.7B | 1.7B | apache-2.0 | 0.253 | 0.421 | 0.406 | 0.438 | 0.99 | 0.95 | 433.0 |
| Qwen/Qwen3-0.6B | 600M | apache-2.0 | 0.000 | 0.277 | 0.260 | 0.297 | 0.97 | 1.00 | 359.2 |
| ibm-granite/granite-4.0-350m | 350M | apache-2.0 | 0.000 | 0.179 | 0.186 | 0.172 | 0.83 | 0.85 | 301.5 |
| ibm-granite/granite-4.0-h-350m | 350M | apache-2.0 | 0.000 | 0.141 | 0.286 | 0.094 | 0.31 | 1.00 | 740.2 |
| HuggingFaceTB/SmolLM2-360M-Instruct | 360M | apache-2.0 | 0.000 | 0.000 | 0.000 | 0.000 | 0.00 | None | 259.6 |
| LiquidAI/LFM2-350M-Extract | 350M | lfm1.0 | 0.000 | 0.000 | 0.000 | 0.000 | 0.14 | 0.75 | 3384.3 |
| LiquidAI/LFM2.5-230M | 230M | lfm1.0 | 0.000 | 0.000 | 0.000 | 0.000 | 0.66 | 1.00 | 298.1 |
| unsloth/gemma-3-270m-it | 270M | gemma | 0.000 | 0.000 | 0.000 | 0.000 | 0.00 | None | 273.1 |


## GPU accuracy: confidence-literal ablation (NOT production)

| model | params | licence | F1 prod | F1 no-floor | precision | recall | schema | abstain | med ms |
|---|---|---|---:|---:|---:|---:|---:|---:|---:|
| unsloth/gemma-3n-E4B-it | E4B | gemma | 0.584 | 0.584 | 0.548 | 0.625 | 0.90 | 0.00 | 1691.8 |
| Qwen/Qwen3-1.7B | 1.7B | apache-2.0 | 0.357 | 0.378 | 0.342 | 0.422 | 1.00 | 0.83 | 437.7 |
| Qwen/Qwen3-0.6B | 600M | apache-2.0 | 0.279 | 0.286 | 0.263 | 0.312 | 0.99 | 0.09 | 468.1 |
| ibm-granite/granite-4.0-350m | 350M | apache-2.0 | 0.000 | 0.180 | 0.174 | 0.188 | 0.94 | 0.74 | 252.9 |
| LiquidAI/LFM2-350M-Extract | 350M | lfm1.0 | 0.000 | 0.000 | 0.000 | 0.000 | 0.13 | 0.33 | 3351.9 |
| unsloth/gemma-3-270m-it | 270M | gemma | 0.000 | 0.000 | 0.000 | 0.000 | 0.00 | None | 263.7 |


## CPU speed (llama.cpp, Q8_0, pinned cores)

| model | quant | pp t/s (400 tok prompt) | tg t/s (64 tok gen) | est ms/note |
|---|---|---:|---:|---:|
| HuggingFaceTB/SmolLM2-360M-Instruct-GGUF | llama 3B Q8_0 | 196.6 | 21.2 | 4304 |
| LiquidAI/LFM2-350M-Extract-GGUF | lfm2 350M Q8_0 | 269.9 | 36.2 | 2806 |
| Qwen/Qwen3-0.6B-GGUF | qwen3 0.6B Q8_0 | 136.1 | 15.2 | 6095 |
| Qwen/Qwen3-1.7B-GGUF | qwen3 1.7B Q8_0 | 53.5 | 10.4 | 12088 |
| ggml-org/gemma-3-270m-GGUF | gemma3 270M Q8_0 | 504.3 | 31.2 | 2333 |
| ibm-granite/granite-4.0-350m-GGUF | granite ?B Q8_0 | 246.3 | 29.4 | 3257 |
| unsloth/LFM2.5-230M-GGUF | lfm2 230M Q8_0 | 402.0 | 38.8 | 2233 |

## Reading the table

**`F1 prod` is what the drain would actually commit.** `F1 no-floor` is the same
run with `MF_CONF_FLOOR` lifted, the extraction ability underneath the config.
The gap between those two columns is the finding.

**The confidence floor is model-dependent, and I initially got this wrong.**
Seeing Granite first, I concluded the models were copying the literal
`"confidence":0.0` out of the prompt's own schema example. The ablation
(byte-identical prompt, example value changed to 0.9) falsifies that for Granite (it emits 0.0 regardless) but strongly confirms it for Qwen: Qwen3-0.6B goes
0.000 → 0.279, essentially reaching its own no-floor ceiling of 0.286. So a
one-character prompt change unlocks the Qwen models and does nothing for Granite.

**The unlock costs precision, but so does the incumbent.** Qwen3-0.6B's
abstention on empty-gold notes collapses from 1.00 to 0.09 under the ablation:
with confidences unlocked it commits triples on 91% of notes asserting no durable
fact. My first instinct was that this made the unlock a bad trade. E4B's own
abstention is **0.05**, which is worse. So over-extraction is what this prompt does on this task
across the board, incumbent included. It is a finding about the drain rather than about model size.

**A high abstention number in the production column is not a virtue.** Models
scoring 1.00 there (Qwen3-0.6B, granite-h, LFM2.5) achieve it by committing
nothing at all, anywhere. Read the abstention column only alongside a non-zero
F1.

## Model-by-model

**Qwen3-1.7B (Apache-2.0)**. The only model that works under the current
production config, and the accuracy leader among permissive candidates at 0.253
(0.357 with the prompt fix). It is 5× the size the brief asked for, and by far
the slowest thing measured on CPU at 12.1 s/note.

**Qwen3-0.6B (Apache-2.0)**, the most interesting M-scale candidate, and the
clearest illustration of the floor problem: 0.000 today, 0.279 with a one-literal
prompt change, against its own floor-free ceiling of 0.286. So the config, not
the model, is what stops it. Two caveats. It is the *slowest* small model on CPU
by roughly 2× (6.1 s/note vs 2.2–3.3 s for the 350M class), which is the opposite
of what its size suggests. And it reaches under half of E4B.

**Granite 4.0 350m / h-350m (Apache-2.0)**, extract correctly on simple notes
(`fp01` yields exactly `user works_for Rakuen Software`) but emit confidence 0.0
unconditionally, so nothing survives the floor and the prompt fix does not help.
The `h` hybrid variant additionally fails to terminate its JSON on 69% of notes.
Note `granite-4.0-350m` also needs `use_cache=False` under current transformers.
It selects a hybrid Mamba cache the non-hybrid checkpoint cannot satisfy.

**LFM2-350M-Extract (LFM Open v1.0)**, my pre-benchmark favourite, and it fails
this task outright. Degenerate repetition loop on 60/70 notes under production
decoding; raising the cap to production's 8192 does not fix it (10/12 still
truncate at 15.6 s/note). A 1.1 repetition penalty *does* fix the loop, schema
validity 0.75, 198 median tokens, and F1 is **still 0.0**, because it emits
capitalised noun phrases and invented relations (`Engineer`, `Database`) instead
of ontology-bound triples, and never uses the `user` subject convention. It is
built for schema-driven field extraction (invoice → named fields), not open
triple extraction against a relation ontology. Different task than its marketing
suggests, and the licence was already disqualifying.

**LFM2.5-230M (LFM Open v1.0)**, zero, same family, same mismatch.

**SmolLM2-360M (Apache-2.0)**, terminal. 0/12 schema validity in every
configuration tried, 17–22 tokens of output. It never produces the `facts`
wrapper at all.

## Licensing

You need MIT or Apache-2.0. That filter costs nothing here: the only
licence-blocked model that could have been tempting is LFM2-350M-Extract, and it
scored 0.0 on content grounds anyway. Gemma (incl. E4B) is not permissive, which
is presumably part of why you are looking to move.

`harness/verify_licences.py` re-reads each licence from the Hub and fails if it
disagrees with `models.json`, so this table is checked rather than remembered.

## What I could not verify

- **Gold set is hand-authored**, not sampled from live `memory_facts`. It measures
  relative capability on a faithful task, not absolute drain quality. Sampling
  real notes is the obvious upgrade and belongs with §4.3 shadow-mode.
- **CPU timings share a host.** The bench CT and the box this session runs in
  (`code`, CT 100) are both on the .253 i7-14700K, alongside other running CTs.
  Runs were serialised and core-pinned, but I do not control the whole machine;
  treat the CPU figures as ±20% and comparative rather than absolute.
- **E4B was fetched from the `unsloth` mirror**, the `google/` repos being gated
  and this environment having no HF token. Same weights, same Gemma licence.
- **70 notes is a small sample.** Differences under ~0.05 F1 are not meaningful.
- No fine-tuning was attempted. Every number is zero-shot.

## Recommendation

**1. Do not move Tier-A to an M-parameter model.** At ≤400M the answer is not
"worse than E4B", it is "does not work": every model at that scale commits zero
facts, and even with the floor lifted the best of them reaches 0.179 F1 against
E4B's 0.582. Two of them (SmolLM2-360M, gemma-3-270m) never emit the output shape
at all in any configuration tried. This is not a tuning gap.

**2. Fix the `MF_CONF_FLOOR` interaction regardless of model choice.** It is a
latent landmine: any future Tier-A swap to a smaller model will hit it, and the
failure is silent. The job succeeds, `typed_facts` stays empty, and it looks
like the model cannot extract. E4B masks it today because it emits real
confidences (its floored and floor-free scores are within 0.004). Options, in
order of preference: have `mf_commit_facts` treat a missing or zero confidence as
"unscored" rather than "reject", or drop the floor and gate on the `rel_types`
write gate alone, which is already the component designed to make that judgement.
Changing the schema example's literal is the cheap fix and only half-works, it
rescues Qwen, not Granite.

**3. If the goal is getting off the Gemma licence, the candidate is Qwen3-1.7B,
but only on GPU.** Apache-2.0, 0.357 F1 with the prompt fix (61% of E4B), and 4×
faster than E4B on the 5080 at 438 ms/note. That is still a real quality drop and
should go through §4.3 shadow mode on live traffic before anyone believes a
70-note number.

The CPU picture kills the cost argument. Qwen3-1.7B is **12.1 s/note** on 8
pinned cores, the slowest thing measured, 5× the 350M models. Qwen3-0.6B is
6.1 s/note, also slower than every 350M model despite being the more accurate
choice among them. Model size and CPU throughput are not tracking together here:
gemma-3-270m does 504 prompt-tok/s and Qwen3-1.7B does 53.5. So "smaller model to
save GPU" does not translate into "runs acceptably on CPU" for the two models
that actually work. If Tier-A is meant to move to CPU, none of these is a good
answer; if it stays on GPU, Qwen3-1.7B is a defensible licence-driven swap at a
measurable accuracy cost.

**4. Look at the drain's precision independently of all of this.** E4B abstains
correctly on only **5%** of the 23 factless notes, it commits triples on nearly
every note that asserts no durable fact. Qwen3-0.6B under the ablation is 9%, so
this is not a small-model regression; it is the prompt and the task as currently
specified, and it means the drain is likely writing a lot of noise into
`memory_facts` today. That is worth a look on real data regardless of which model
serves Tier-A, and it is the one finding here that costs nothing to act on.

## The path I did not benchmark, and think is the real M-scale answer

Every model here is a **decoder** asked to emit JSON. That framing is what the
prompt assumes, and it is where all the failures live: unterminated objects,
repetition loops, absent wrappers, meaningless confidence values. None of those
are extraction failures. They are failures of *serialising* an extraction.

The encoder route sidesteps the entire class. **GLiNER2** (`fastino/gliner2-base-v1`)
is 205M parameters, **Apache-2.0 for both code and weights**, CPU-native, and
scores all candidate labels in a single forward pass against a declared schema.
Our `rel_types` seed set is a fixed 17-label vocabulary, precisely the regime it
is built for. There is no JSON to malform, no confidence literal to copy, and no
repetition loop to fall into; it returns spans and scores directly, and the score
is a real model probability rather than a token the model guessed.

I did not benchmark it because it needs a different runtime than the transformers
harness the rest of this run used, and I judged a comparable-quality integration
to be more than a night's work to do honestly.

Two things make it worth a follow-up rather than a footnote. It is the only
option in this whole exercise that is genuinely M-scale, permissively licensed,
*and* architecturally suited to the task. And it fits where the stack is going
rather than where it has been: with `aimee-llm` being purged and Bekko already
served through ONNX, a 205M ONNX encoder is a smaller step than standing up a
GGUF decoder endpoint. GLiNER2 ships ONNX weights.

The honest caveat is that it changes the Tier-A contract. It does not produce
`{"facts":[...]}`, so `kb_curator_provider.c` would need a non-OpenAI call path
and `mf_commit_facts` an alternative input shape. That is real work, and it
should be costed before anyone commits to it, but it is the only route in this
report that could plausibly deliver Tier-A extraction at 205M parameters on CPU.

## Reproducing

```bash
bash bench/tier-a/harness/sweep_gpu.sh        # accuracy, needs CUDA
bash bench/tier-a/harness/sweep_cpu.sh        # speed, llama.cpp, run alone
bash bench/tier-a/harness/sweep_ablation.sh   # confidence-literal ablation
python3 bench/tier-a/harness/summarize.py     # regenerate the tables above
```
