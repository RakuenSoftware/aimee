# Proposal: unify embedding + Tier-A synth on one Gemma-4 base; a dedicated EuroBERT reranker

- **State:** PENDING — design/idea only, no code in this PR. Revised after an adversarial
  review that found an earlier "one base, three roles" framing over-scoped and several serving
  claims wrong. This version keeps the defensible core (embed + Tier-A synth on one Gemma-4
  base; rerank as a separate encoder) and moves the slicing/pruning/race material to §14.
  Nothing here flips a flag on merge.
- **Author:** JBailes
- **Date:** 2026-07-25
- **Charter roles:** Recall (dense embedding), Rerank (cross-encoder scoring), Synthesize /
  Extract (Tier-A mechanical passes), Evaluate-Optimize (A/B each role vs its incumbent),
  Gate-Promote (never a silent swap; shadow → canary → default).

## Thesis

The `aimee-llm` container serves embedding, reranking, and synthesis from one image
([`unified-llm-container`](../done/unified-llm-container.md)), but as three separate models: a
Qwen3-Embedding-0.6B embedder, an Ettin cross-encoder reranker, and a Gemma-4 synth model (E4B
on the CPU/Tier-A tier). The reranker carries a gateway-side Dense head because its
sentence-transformers score head does not survive GGUF conversion
([`P2-serving-validation.md`](../../../benchmarks/results/unified-llm/P2-serving-validation.md)).

Two changes:

1. Put embedding on the same Gemma-4 base as Tier-A synth. The CPU tier already runs E4B for
   synth; an embed LoRA + MRL head on that base folds the embedder into it. Per tier: E2B on the
   edge, E4B on bigger boxes (same lineage, MatFormer-nested).
2. Replace the reranker with a dedicated encoder that serves native `/rerank`. It stays a
   separate model — a cross-encoder is the right tool — but a `num_labels=1` head removes the
   gateway Dense head. It is not folded onto the Gemma base; a decoder yes-token reranker cannot
   serve through `/rerank` (§9).

Three models become two, the gateway rerank hack goes away, and embedding gains MRL dims.

## Goal

1. Embed + Tier-A synth from one Gemma-4 base (E2B edge / E4B bigger), on the existing per-role
   backends.
2. Tier-A synth quality held. The E4B tier serves actual E4B (no change); the E2B tier recovers
   ≥ 98% of E4B's Tier-A precision/recall on the curator-synth bench.
3. Embedding ≥ the incumbent Qwen3 embedder on aimee's retrieval (LoCoMo + LongMemEval),
   CPU-usable, MRL dims.
4. A dedicated reranker on native `/rerank`; the gateway Dense head removed.
5. No silent swaps — each role goes shadow → canary → default via the calibration/bandit
   substrate.

## §0 What already exists (so we don't rebuild it)

- **Per-role serving.** `aimee-llm-supervisor.sh` runs embed (8081), rerank (8082), synth
  (8083) behind the gateway. Embed/rerank are `--embeddings`/`--reranking` servers; synth is the
  `/v1/chat` server.
- **Tier-A/Tier-B split + `disable_thinking`** in `kb_curator_provider_for_stage`. A distilled
  E2B is a model change on the Tier-A route.
- **Benchmarks.** `benchmarks/embedder-sweep.sh` (Recall@K/MRR), the curator-synth bench
  (`benchmarks/results/synth/`), and the reranker latency harness
  (`benchmarks/results/reranker/LATENCY.md`) are the three gates §10 reuses.
- **Calibration + bandit** (`kb_calibrate` / `kb_bandit`) drive shadow → canary → default.
- **pgvector + versioned-index cutover** (`halfvec`, indexable to 4000 dims;
  `embedder-sweep.md` / `unified-llm-cutover.md`).

## §1 The change, quantified

| | today (CPU tier) | proposed |
|---|---|---|
| Synth (Tier-A) | Gemma-4 E4B (`gemma-4-E4B-it-Q4_K_M`) | same base; E2B on edge, E4B on bigger tiers |
| Embedding | separate Qwen3-Embedding-0.6B | same base + embed LoRA + MRL head |
| Reranking | Ettin encoder + gateway Dense head + CI bake | dedicated encoder, `num_labels=1`, native `/rerank` |
| Resident models | 3 | 2 (embed+synth base; reranker) |
| Reranker gateway compute | ~4 MB numpy Dense head | none |

## §2 The base (Gemma-4 E2B edge / E4B bigger)

Gemma-4 (Apache 2.0), MatFormer-nested E-series: E2B (2.3B effective / 5B total) is a nested
submodel of E4B (4.5B effective / 8B total). It is the same lineage as the E4B synth model
already deployed, so "E2B matches E4B synth" is a distill within one family, and MatFormer + MRL
compose (elastic size × elastic dim).

**Resource sizing.** PLE (Per-Layer Embeddings) saves accelerator VRAM by holding per-layer
tables in host memory. On a CPU-only box host RAM is the resident tier, so resident tracks total
(~5B), not effective (~2.3B). E2B is still lighter than the deployed E4B (5B vs 8B total), but
not by half and not "by construction": size from total params + KV + the Q8-pinned PLE tables
(§5), and measure RSS of a real E2B GGUF on the target box before treating fit as resolved. The
supervisor already caps `--cache-ram` to avoid host OOM on a 16 GB box, so RAM is contended well
above the stated edge target. The "2-core / 4 GB" figure is a target, not a measured fit; the
current cpu-tier download alone is ~6.5 GB per `KB_LLM_BACKENDS`.

**Text-only build.** aimee is text-only (embed + Tier-A synth over documents and code), and the
E-series is multimodal, so the build drops the vision encoder (~150M ViT, the image/OCR path)
and the audio encoder (USM-class) with their projectors, and keeps the language backbone
(PLE/AltUp/LAuReL) and the text tokenizer. This is the standard `convert_hf_to_gguf.py`
text-only path, lossless for text, and separate from the PLE port. Savings are modest, a few
hundred MB. Real size reduction below E2B (MatFormer slicing, prune-and-heal, vocab pruning) is
deferred to §14 — the tooling is not established.

## §3 Synth role (Tier-A)

**Serving.** The synth backend (8083) runs the base in generation mode with
`response_format: json_schema` for the grammar-constrained extract/index passes;
`provider_client_build_openai` already emits the schema, and `disable_thinking` stays set.

**E4B tier.** A box that affords E4B runs actual E4B for synth, byte-identical to today.
Unification there is only the embed adapter (§4). No synth training, no parity risk. The work
below is the E2B edge tier only.

**E2B synth = distill E4B → E2B on Tier-A traffic.**
- *Signal:* sequence-level KD (SFT on teacher outputs), plus token-level KL where E4B logits are
  available. The task is grammar-bounded JSON, which a small student closes readily.
- *Data:* replay real Tier-A traffic (the `memory_facts` drain + doc/code extract stages) plus
  synthetic edge cases (first/third person, negation, multi-fact, ambiguous). This is the
  distillation bootstrap the Tier-A extraction proposal defines: run the incumbent E4B
  (thinking-off) over the corpus, keep outputs that survive the `rel_types` gate as silver
  labels, add a human-audited gold set for eval.
- *Gate (§10):* E2B Tier-A precision/recall ≥ 98% of stock E4B on the curator-synth bench.
  Fallback is per-stage: if one stage regresses past the bar, route that stage to E4B and keep
  E2B for the rest (`kb_curator_provider_for_stage` already supports per-stage routing).
- *Optional lift:* if E4B → E2B leaves a gap, distil E2B from a larger teacher (12B / 26B-A4B)
  instead of E4B (§14).

## §4 Embed role (Recall)

**Serving.** The embed backend (8081) runs the base in `--embeddings` mode with last-token
pooling and the MRL head, sharing a read-only mmap of the base with the synth instance, so the
weights are resident once.

**Training (two-stage, task-targeted; Jina-v5 / EmbeddingGemma shaped).**
1. General embedding distillation to build the space: project the student's pooled vector into
   the teacher's space via a learned linear ψ and minimise cosine distance (Jina's ablation:
   this beats score/similarity-matrix distillation). Teacher per §6 (default
   Qwen3-Embedding-8B).
2. Retrieval adapter with a hybrid loss: InfoNCE over aimee-mined hard negatives, a
   distillation-preservation term, a uniformity/GOR term (cheap at full precision, decisive for
   truncation/quantization robustness), and the MRL nested loss over the ladder (§8). Asymmetric
   query/doc instruction prefixes, the Qwen3-Embedding/EmbeddingGemma convention.

Hard negatives come from aimee's own retrieval logs (ranker-outcome / kb_hybrid data), the
domain step that lets a small student beat a general teacher on aimee's distribution.

**Two ceilings.**
1. Last-token pooling on a frozen causal decoder is weaker than a bidirectional encoder across
   the task range, which is why EmbeddingGemma/LLM2Vec convert to bidirectional. We do not, since
   that breaks the generator and forfeits the unification, so the embed role has a real ceiling.
   The gate is therefore "≥ the incumbent Qwen3 embedder," not "≥ the teacher" (a distilled
   student rarely beats its teacher). If teacher-parity is ever required, that means a
   bidirectional embed-only base, a separate model, which reopens "one base" and belongs in §14.
2. The MRL head is a new tensor, not a LoRA delta. LoRA adapts existing weights; it cannot add an
   output projection. The head must be trained and emitted by `convert_hf_to_gguf.py` for the
   Gemma arch, the same failure the ettin Dense head hits (P2). Before any embed training, prove
   the converter emits an MRL/pooling head and llama.cpp applies it on a stock head. If it does
   not round-trip, the options are converter arch work or a small numpy projection in the gateway.
   This runs before §6.3.

The reranker is not a role on this base — see §9.

## §5 Prerequisite: PLE in llama.cpp

E2B/E4B rely on Per-Layer Embeddings, and llama.cpp does not inject the per-layer residual in its
forward graph ([#22243](https://github.com/ggml-org/llama.cpp/issues/22243) — confirm current
status; last seen unimplemented). In the Gemma-3n lineage PLE is entangled with AltUp
(alternating updates) and LAuReL (learned augmented residual), so it is not a standalone residual
add. Porting it means reproducing three interlocking mechanisms across ~30-35 layers (and again
for the E2B slice), then holding embedding parity — a subtly wrong residual passes a generation
smoke test but tanks Recall@K. Budget a multi-week ggml effort with a numeric parity gate (cosine
vs the HF reference on a fixed corpus), vendored in the pinned `LLAMA_TAG` and decoupled from any
upstream merge. It is step zero for the E2B path, paid once across synth + embed on both sizes.

## §6 Training sequence

1. Synth distill (E2B tier only): E4B → E2B on Tier-A traffic; silver labels from the incumbent's
   high-confidence outputs plus a gold eval set. Optionally distil both E2B and E4B from a shared
   larger teacher (12B/26B-A4B) to lift both (§14).
2. Freeze the base.
3. Embed adapter: embedding-space distillation from a clean-licensed teacher, InfoNCE on
   aimee-mined hard negatives, a uniformity/GOR term, MRL loss over the ladder (§8). Teacher
   licensing is a gate: no CC-BY-NC teacher (NC propagates to the student), and any API teacher
   (Voyage/Gemini) ToS typically forbids training a competing model regardless of license — a
   legal sign-off item, and ensembles inherit the most restrictive terms. Default: Qwen3-Embedding-8B
   (Apache 2.0).
4. InfoNCE quality depends on the effective batch of in-batch negatives, which a 16 GB card
   constrains; use GradCache / cross-batch memory so the objective is not hardware-bound.
5. Quantization-aware finish; PLE tables pinned Q8_0 (§5).

## §7 Getting more from E2B at fixed size (16 GB training)

Where a deployment commits to E2B (E4B still serves bigger boxes), the text-only strip does not
shrink it much, so the lever is capability at fixed size, not smaller. Given 24/7 on one 16 GB
card:

- Overtrain. Small models keep improving past the compute-optimal token count; 24/7 is the
  resource this consumes. Cost: wall-clock.
- Domain-adaptive continued pre-training on aimee's own corpus (code + documents + memory text)
  before task distillation. Fits 16 GB.
- Climb the LoRA → full-FT ladder as far as 16 GB allows: fp16-base LoRA → high-rank LoRA →
  partial full-FT → full fine-tune with an 8-bit optimizer, gradient checkpointing, and frozen
  PLE tables (only the ~2.3B transformer trains, so E2B full-FT fits). Deeper FT for one role can
  degrade the other (embed↔synth), so keep the synth-distill → freeze → embed-adapter order and
  re-check both §10 gates.
- Feature-level distillation (match teacher hidden states / attention, not just outputs). Teacher
  features are cached offline (§13), so this costs disk, not VRAM.
- A larger teacher (offline cache decouples teacher size from the 16 GB card).
- The narrow aimee distribution means E2B's effective capability on these tasks exceeds its size;
  keep the training distribution to aimee's.

Each gain is bench-verified (§10), and the deeper-training levers are bounded by role
preservation, not just by 16 GB.

## §8 MRL dimensions

- One ladder (proposed `[256, 512, 1024, 2048, 2560, 4000]`; 4000 = `halfvec` index cap),
  front-loaded so each slice earns its recall (measured per-slice on the sweep).
- The operator picks the dim via `AIMEE_EMBEDDING_DIM`.
- Re-embed-free tier moves are narrower-only, and only if you stored max-dim. Truncating +
  renormalizing + rebuilding the `halfvec(k)` index avoids a re-embed, but it forfeits MRL's
  at-rest storage saving (you pay 4000-dim storage) and the reindex is an hours-scale HNSW
  rebuild on a large corpus. Going wider than stored is a re-embed.
- Width is not the quality lever — Jina reached its headline quality at 1024 dims. The sweep
  decides how much of the ladder earns its storage.

## §9 Reranker (dedicated encoder, tier-aware, gated)

The reranker stays a separate cross-encoder on native `/rerank` (`--reranking --pooling rank` + a
single-linear `cls.output.weight` head). A decoder yes-token reranker does not serve through
`/rerank` (that path is a classifier head, not an LM logit — the failure that yields near-zero
scores for Qwen3-Reranker GGUFs).

Recommended base: the EuroBERT family — Apache 2.0, 15 languages (8 European + Chinese, Russian,
Japanese, Vietnamese, Arabic, Turkish, Hindi), GQA/RoPE/RMSNorm, 8k context, stronger than
ModernBERT on code and math, and a 210m / 610m / 2.1B ladder. Tier assignment follows the
measured latency wall (`reranker/LATENCY.md`; rerank cost is K forward passes, CPU ms/cand
scales with params):

- CPU/edge → EuroBERT-210m (candidate). Extrapolated ~185 ms/cand fits sub-1s only around top-5
  (vs ettin-68m: 60 ms/cand, top-10 in 0.60s). Gated on, in order:
  1. a measured ettin-68m vs EuroBERT-210m bake-off on the target box (GQA may beat the linear
     extrapolation);
  2. a top-K-depth quality check — does 210m over a shallow top-K beat ettin-68m over a deep
     top-K on LoCoMo/LongMemEval?
  3. a native-`/rerank` smoke test — EuroBERT's Llama-style encoder arch must convert and score
     through `--pooling rank` (unverified; possibly new converter arch work; `/rerank` is finicky
     per-model, #16407).
- GPU tiers → EuroBERT-610m (speed is not the constraint there).
- Fallbacks: re-headed ettin-68m (`num_labels=1`, English-primary, fast) for CPU;
  bge-reranker-v2-m3 (proven native, multilingual, 568M) for GPU.

Any choice is domain-distillable on the same pipeline (a strong reranker teacher onto the chosen
encoder on aimee's hard negatives).

## §10 Evaluation & rollout (gates every role swap)

- Synth: curator-synth bench — E2B ≥ 98% of stock E4B on the mechanical tasks, with a
  latency/footprint win; per-stage E4B fallback. (E4B tier: no gate.)
- Embed: `embedder-sweep` — Recall@5 / MRR ≥ the incumbent Qwen3 embedder at the shipped dim and
  Q-level; per-slice curve committed. Not "≥ teacher."
- Rerank: the §9 three-gate ladder (measured latency, top-K-depth quality, native serving) vs the
  ettin incumbent.
- Rollout: shadow → canary → default via `kb_bandit`; operator-pinnable per role.

## §11 Open decisions

1. Language breadth — EuroBERT's 15 vs a broader set (bears on the reranker fallback and the
   embed distribution).
2. Embed teacher — Qwen3-Embedding-8B vs an ensemble (subject to the §6 ToS/NC gate).
3. MRL at-rest policy — store max-dim (re-embed-free narrower moves, higher storage) vs
   store-at-tier-dim (cheaper, re-embed to change).
4. Embed head placement — converter-emitted MRL tensor vs a gateway projection (decided by the §4
   convert prototype).

## §12 Risks

- E2B synth parity is the real risk, not resources: recover the E4B → E2B gap on mechanical
  tasks. §10-gated with per-stage E4B fallback.
- Causal-decoder embeddings have a ceiling vs bidirectional; the gate is "≥ incumbent," and the
  role may cap below a dedicated encoder embedder.
- The MRL head and native `/rerank` both depend on GGUF-conversion paths that may need arch work;
  prototype conversion before training.
- The PLE port is multi-week ggml work (AltUp/LAuReL), with embedding parity as the hard part; the
  E2B path waits on it.
- CPU fit is not settled; measure RSS, and the Q8-PLE pin narrows the margin vs the incumbent
  E4B-Q4.
- EuroBERT native serving is unverified; it is a smoke test, not a given.

## §13 Training hardware (train on CUDA, serve on Vulkan)

One dedicated training card: RTX 5080 (16 GB, CUDA). Serving stays Vulkan/vendor-agnostic; the
training card never touches production, and dedicating the 5080 keeps the 7900 XTX free to serve.
16 GB covers LoRA on a frozen quantized 2–5B base (QLoRA fits 5–7B under 16 GB), and CUDA avoids
the immature ggml-Vulkan training path. Keep the embed teacher ≤ ~14B to cache on-card (bigger
CPU-offloads, one-time); use GradCache for InfoNCE negatives (§6.4); Tier-3 full-FT (§14) is the
one pinch — spill to the 24 GB 7900 XTX or cloud. Binding constraint is wall-clock. EuroBERT/ettin
reranker training is small and fits easily.

## §14 Future directions (out of scope here)

Real ideas, but not the decidable core, and several rest on unproven tooling:

- Build-both-and-race across E4B, E2B-distill, E2B-slice, pruned-E4B — a research program on one
  serialized card, not a re-run.
- Arbitrary MatFormer slicing to custom sizes — needs bespoke tooling, not a stock `transformers`
  op.
- Minitron-style prune-and-distill (trim E4B from above) — full-parameter training the 16 GB card
  can't host.
- Cross-tier shared embedding space ("index big, query cheap"). A shared synth teacher does not
  align it; the embed space comes from a separate embed adapter/teacher. It needs both tiers' embed
  heads trained against one frozen teacher space, with its own gate (E2B-embedded docs retrieved
  by E4B queries). Unvalidated.
- Further size reduction (MatFormer slicing, prune-and-heal, vocab pruning) — only after the base
  pipeline passes §10, each gated head-to-head against the model it would replace.

## Acceptance criteria

- Embed + Tier-A synth serve from one Gemma-4 base (E2B edge / E4B bigger) on the existing
  per-role backends; the PLE port lands parity-verified.
- Each role passes its §10 gate at ≥ its incumbent (synth ≥ 98% E4B; embed ≥ Qwen3 embedder;
  rerank ≥ ettin on the three-gate ladder), rolled out shadow → canary → default.
- The dedicated reranker serves through native `/rerank`; the gateway Dense head and its CI bake
  are removed.
- The MRL head's conversion path is proven before embed training is scheduled.
- CPU fit is measured (RSS on the target box), not asserted.
- No change to Tier-B routing or to how facts are gated/promoted once extracted.

## Relationship to `dedicated-extraction-model-curator-tier-a` (both PENDING)

That proposal asks whether Tier-A should run a dedicated small model; its Option B is a
note→triples relation extractor. This one is broader (all Tier-A mechanical synth on a Gemma
base) and assumes a different current Tier-A baseline (CPU-tier E4B, not the gpu-mid reasoning
model that proposal describes). It extends, does not supersede: if this ships, that Option B is a
special case. Per the supersession-hygiene norm, both should cross-link and reconcile the Tier-A
baseline.

## Out of scope / does not re-propose

- The `aimee-llm` container, per-role serving, the Tier-A/Tier-B split, calibration, the bandit,
  the console — shipped; this chooses models.
- The `disable_thinking` fix (merged).
- The pgvector storage tier and versioned-index cutover — reused, not changed.
- General-benchmark (MTEB/MMTEB) parity as an acceptance target; the gate is aimee's own retrieval
  on aimee's corpus.
