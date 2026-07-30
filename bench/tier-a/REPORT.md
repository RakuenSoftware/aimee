# Tier-A small-model benchmark — findings

**Date:** 2026-07-30/31 · **Branch:** `bench/tier-a-small-models`
**Question:** can a model in the hundreds-of-millions class serve the curator
Tier-A extraction stage, and how does it compare to the E4B incumbent?

Context: `docs/proposals/pending/dedicated-extraction-model-curator-tier-a.md`
asks whether Tier-A should get its own small model. Its §2 candidate list
(Qwen2.5-1.5B/3B, Gemma-2-2B, Llama-3.2-1B, Phi-3.5-mini) is a year stale. This
run tests the current sub-1B field, plus two larger reference points.

## Headline

**No slam dunk at the M-parameter scale.** Under the production configuration,
six of the seven small models commit **exactly zero facts** — not "worse than
E4B", zero. The one that commits anything is Qwen3-1.7B, which is not an
M-parameter model.

The reason is mostly not extraction ability. It is a config interaction we own:
`MF_CONF_FLOOR` (0.6) discards facts that small models emit with confidence 0.0.
Lift that floor and the same runs score 0.14–0.42 F1. This is the same end state
as the thinking-truncation bug in §1 of the proposal — job runs, `typed_facts`
stays empty — reached by a different route, and it would have looked like "the
small model can't extract" in production.

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
predicate, infra, governance). **23 notes have a deliberately empty gold list** —
over-extraction is the expensive failure for a drain that writes into
`memory_facts`, so it is measured rather than assumed. Labelling rules and the
external-validity limits are in `data/LABELING.md`.

Every number below is regenerated from committed result files by
`harness/summarize.py`; raw per-note predictions are committed too, so any figure
traces back to the model output that produced it.

## Results

<!--RESULTS-->

## Reading the table

**`F1 prod` is what the drain would actually commit.** `F1 no-floor` is the same
run with `MF_CONF_FLOOR` lifted — the extraction ability underneath the config.
The gap between those two columns is the finding.

**The confidence floor is model-dependent, and I initially got this wrong.**
Seeing Granite first, I concluded the models were copying the literal
`"confidence":0.0` out of the prompt's own schema example. The ablation
(byte-identical prompt, example value changed to 0.9) falsifies that for Granite —
it emits 0.0 regardless — but strongly confirms it for Qwen: Qwen3-0.6B goes
0.000 → 0.279, essentially reaching its own no-floor ceiling of 0.286. So a
one-character prompt change unlocks the Qwen models and does nothing for Granite.

**The unlock is not free.** Qwen3-0.6B's abstention on empty-gold notes collapses
from 1.00 to 0.09 under the ablation: with confidences unlocked it commits
triples on 91% of notes that assert no durable fact. Trading "commits nothing"
for "commits noise on nine of ten factless notes" is not obviously an
improvement for a drain feeding the write gate.

## Model-by-model

**Qwen3-1.7B (Apache-2.0)** — the only model that works under the current
production config, and the accuracy leader among permissive candidates. It is
5× the size the brief asked for, and the slowest permissive option on CPU.

**Qwen3-0.6B (Apache-2.0)** — the most interesting M-scale candidate. Blocked
entirely by the confidence floor today; a one-line prompt change recovers most of
its ceiling. But it is the *slowest* small model on CPU by roughly 2×
(≈6.1 s/note vs ≈2.2–3.3 s for the others), and its precision on factless notes
is poor once unlocked.

**Granite 4.0 350m / h-350m (Apache-2.0)** — extract correctly on simple notes
(`fp01` yields exactly `user works_for Rakuen Software`) but emit confidence 0.0
unconditionally, so nothing survives the floor and the prompt fix does not help.
The `h` hybrid variant additionally fails to terminate its JSON on 69% of notes.
Note `granite-4.0-350m` also needs `use_cache=False` under current transformers —
it selects a hybrid Mamba cache the non-hybrid checkpoint cannot satisfy.

**LFM2-350M-Extract (LFM Open v1.0)** — my pre-benchmark favourite, and it fails
this task outright. Degenerate repetition loop on 60/70 notes under production
decoding; raising the cap to production's 8192 does not fix it (10/12 still
truncate at 15.6 s/note). A 1.1 repetition penalty *does* fix the loop — schema
validity 0.75, 198 median tokens — and F1 is **still 0.0**, because it emits
capitalised noun phrases and invented relations (`Engineer`, `Database`) instead
of ontology-bound triples, and never uses the `user` subject convention. It is
built for schema-driven field extraction (invoice → named fields), not open
triple extraction against a relation ontology. Different task than its marketing
suggests, and the licence was already disqualifying.

**LFM2.5-230M (LFM Open v1.0)** — zero, same family, same mismatch.

**SmolLM2-360M (Apache-2.0)** — terminal. 0/12 schema validity in every
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

<!--RECOMMENDATION-->

## The path I did not benchmark, and think is the real M-scale answer

Every model here is a **decoder** asked to emit JSON. That framing is what the
prompt assumes, and it is where all the failures live: unterminated objects,
repetition loops, absent wrappers, meaningless confidence values. None of those
are extraction failures. They are failures of *serialising* an extraction.

The encoder route sidesteps the entire class. **GLiNER2** (`fastino/gliner2-base-v1`)
is 205M parameters, **Apache-2.0 for both code and weights**, CPU-native, and
scores all candidate labels in a single forward pass against a declared schema.
Our `rel_types` seed set is a fixed 17-label vocabulary — precisely the regime it
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
should be costed before anyone commits to it — but it is the only route in this
report that could plausibly deliver Tier-A extraction at 205M parameters on CPU.

## Reproducing

```bash
bash bench/tier-a/harness/sweep_gpu.sh        # accuracy, needs CUDA
bash bench/tier-a/harness/sweep_cpu.sh        # speed, llama.cpp, run alone
bash bench/tier-a/harness/sweep_ablation.sh   # confidence-literal ablation
python3 bench/tier-a/harness/summarize.py     # regenerate the tables above
```
