# Proposal: unify embedding + Tier-A synth on one Gemma-4 base; a dedicated EuroBERT reranker

- **State:** PENDING — design/idea only, no code in this PR. This is a **revised**
  version: an earlier draft over-claimed a "one base, three roles" unification (rerank
  included) and asserted CPU fit "by construction"; an adversarial review found those
  claims wrong or overstated. This revision narrows to the defensible core — **embed +
  Tier-A synth on one Gemma-4 base, rerank as a *separate* dedicated encoder** — fixes the
  technical claims, and moves the speculative ladder/pruning/race material to §12 (future).
  Nothing here flips a flag on merge.
- **Author:** JBailes
- **Date:** 2026-07-25
- **Charter roles:** Recall (dense embedding), Rerank (cross-encoder scoring),
  Synthesize / Extract (Tier-A mechanical passes), Evaluate-Optimize (A/B each role vs its
  incumbent), Gate-Promote (never a silent swap; shadow → canary → default).

## Thesis

The `aimee-llm` container serves embedding, reranking, and synthesis from one image
([`unified-llm-container`](../done/unified-llm-container.md)), but as **three separate
models**: a Qwen3-Embedding-0.6B embedder, an Ettin cross-encoder reranker (served via a
**gateway-side Dense head** because its sentence-transformers score head does not survive
GGUF conversion — see
[`P2-serving-validation.md`](../../../benchmarks/results/unified-llm/P2-serving-validation.md)),
and a Gemma-4 synth model (E4B on the CPU/Tier-A tier).

Two coherent moves:

1. **Unify *embedding + Tier-A synth* onto one Gemma-4 base.** The CPU tier already runs
   E4B for synth; adding an embedding LoRA + MRL head to that same base collapses the
   embedder into the synth model. Instantiated per tier: **E2B (edge)** and **E4B (bigger
   boxes)** — same lineage, MatFormer-nested.
2. **Replace the reranker with a *dedicated* encoder that serves native `/rerank`.** The
   reranker stays a separate model (a cross-encoder is the right, fast tool), but a
   `num_labels=1` encoder head deletes the gateway Dense-head hack. This is *not* unified
   onto the Gemma base — an earlier draft claimed a "decoder yes-token" reranker could serve
   native `/rerank`; that is a category error (§7).

Net: three models become **two** (one unified embed+synth base + one reranker), the gateway
rerank hack dies, and embedding gains MRL-configurable dims.

## Goal

1. **Embed + Tier-A synth from one Gemma-4 base** (E2B edge / E4B bigger), via the existing
   per-role backends.
2. **Tier-A synth quality preserved** — E4B tier serves *actual* E4B (no change); the E2B
   tier must recover ≥ 98% of E4B's Tier-A precision/recall on the curator-synth bench.
3. **Embedding ≥ the incumbent Qwen3 embedder** on aimee's retrieval (LoCoMo +
   LongMemEval), CPU-usable, MRL-configurable dims.
4. **A dedicated reranker on native `/rerank`** — delete the gateway Dense-head hack.
5. **Never a silent swap** — each role graduates shadow → canary → default via the shipped
   calibration/bandit substrate.

## §0 What already exists (so we don't rebuild it)

- **Per-role serving.** `aimee-llm-supervisor.sh` runs embed (8081), rerank (8082), synth
  (8083) behind the gateway. Embed/rerank are `--embeddings`/`--reranking` servers; synth is
  the OpenAI `/v1/chat` server.
- **Tier-A/Tier-B split + `disable_thinking`** in `kb_curator_provider_for_stage`; a
  distilled E2B is a *model* change on the Tier-A route.
- **Benchmarks.** `benchmarks/embedder-sweep.sh` (Recall@K/MRR), the curator-synth bench
  (`benchmarks/results/synth/`), and the reranker latency harness
  (`benchmarks/results/reranker/LATENCY.md`) are the three gates §8 reuses.
- **Calibration + bandit** (`kb_calibrate` / `kb_bandit`) drive shadow → canary → default.
- **pgvector + versioned-index cutover** (`halfvec`, indexable to 4000 dims;
  `embedder-sweep.md` / `unified-llm-cutover.md`).

## §1 The unification, quantified

| | today (CPU tier) | proposed |
|---|---|---|
| Synth (Tier-A) | Gemma-4 **E4B** (`gemma-4-E4B-it-Q4_K_M`) | same base; **E2B** on edge, E4B on bigger tiers |
| Embedding | separate Qwen3-Embedding-0.6B | **same base + embed LoRA + MRL head** |
| Reranking | Ettin encoder + **gateway Dense head + CI bake** | **dedicated encoder, `num_labels=1`, native `/rerank`** |
| Resident models | 3 | **2** (unified embed+synth base; reranker) |
| Reranker gateway compute | ~4 MB numpy Dense head | none |

## §2 The base (Gemma-4 E2B edge / E4B bigger)

Gemma-4 (Apache 2.0), MatFormer-nested E-series: **E2B** (2.3B effective / 5B total) is a
nested submodel of **E4B** (4.5B effective / 8B total). Chosen because it is the *same
lineage as the E4B synth model already deployed*, so "E2B matches E4B synth" is a
distill-within-the-family, and because MatFormer + MRL compose (elastic size × elastic dim).

**Resource sizing — corrected.** An earlier draft argued "resident ≈ effective, so fit is
settled by construction." That is **wrong for a CPU tier**: PLE (Per-Layer Embeddings) saves
*accelerator* VRAM by holding per-layer tables in host memory — but on a CPU-only box host
RAM *is* the resident tier, so **resident ≈ total (~5B), not effective (~2.3B)**. E2B is
still lighter than the deployed E4B (5B total vs 8B total), so the direction holds — but:
- **not "half," and not "by construction"** — size from *total* params + KV + the Q8-pinned
  PLE tables (§4), and **measure RSS of a real E2B GGUF on the target box** before calling
  fit resolved. The supervisor already caps `--cache-ram` to avoid host OOM on a 16 GB box
  (`aimee-llm-supervisor.sh`), so RAM is contended even well above the stated edge target.
- The "2-core / 4 GB" figure is a *stated target*, not a measured fit; reconcile it against
  the real serving box (the current cpu-tier download alone is ~6.5 GB per `KB_LLM_BACKENDS`).

## §2b Stripping the base to text-only (part of the build, lossless)

aimee's use is text-only (embed + Tier-A synth over documents + code), and the Gemma-4
E-series is natively multimodal, so the build **drops the modality components**:

- The **vision encoder** (~150M ViT — the image / OCR path) and the **audio encoder**
  (USM-class), plus their projectors into the language space, are **not instantiated**.
- The **language backbone** (PLE/AltUp/LAuReL) and the text tokenizer are kept — that is what
  the §4 PLE port serves.

This is the standard Gemma-text conversion path (`convert_hf_to_gguf.py` text-only ignores the
towers), **lossless for text**, and **orthogonal to the PLE port** (the towers are separate
from the per-layer residual machinery). Savings are **modest** — a few hundred MB of encoders —
not a headline footprint reduction.

**Meaningful size reduction (getting E2B smaller) is the deferred, gated work in §12**, and it
rests on tooling that is not established: MatFormer slicing to custom sizes, Minitron
prune-and-heal (full-FT that does not fit the 16 GB training card), and vocab pruning (capped by
the multilingual requirement). Those run only after the base pipeline passes §8 — they are *not*
part of this build.

## §3 The synth role (Tier-A)

**Serving.** One base GGUF; the synth backend (8083) runs it in generation mode with
`response_format: json_schema` for the grammar-constrained extract/index passes (no prompt
babysitting — `provider_client_build_openai` already emits the schema). `disable_thinking`
stays set.

**The E4B tier is free.** A box that affords E4B runs *actual* E4B for synth — byte-identical
to today — and unification there means *only* adding the embed adapter (§3-embed). **No synth
training, no parity risk** on that tier. The synth work below is entirely about the **E2B**
edge tier.

**E2B synth = distill E4B → E2B on the Tier-A distribution.**
- *Signal:* sequence-level KD (SFT on the teacher's outputs) is the floor; add token-level KL
  where E4B logits are available. The task is **grammar-bounded JSON**, which is the regime a
  small student closes most easily — favorable.
- *Data:* replay real Tier-A traffic (the `memory_facts` drain + doc/code extract stages) as
  the primary corpus, plus synthetic edge cases (first/third person, negation, multi-fact,
  ambiguous). This is exactly the **distillation bootstrap** the Tier-A extraction proposal
  defines: run the incumbent E4B (thinking-off, unbudgeted) over the corpus, keep
  high-confidence outputs that survive the `rel_types` gate as **silver labels**, plus a
  small human-audited **gold** set for eval.
- *Gate (§8):* E2B Tier-A precision/recall **≥ 98% of stock E4B** on the curator-synth bench.
  Fallback is **per-Tier-A-stage**, not all-or-nothing: if one stage (e.g. a nuanced
  relation extract) regresses past the bar, route *that stage* to E4B and keep E2B for the
  rest. The `kb_curator_provider_for_stage` seam already supports per-stage routing.
- *Optional lift (§12 teacher escalation):* if E4B→E2B leaves a gap, distil E2B from a
  **larger** teacher (12B / 26B-A4B) instead of E4B — the student can then exceed what
  E4B-as-teacher alone yields on the hardest Tier-A stages.

## §3b The embed role (Recall)

**Serving.** Same base GGUF, the embed backend (8081) in `--embeddings` mode with **last-token
pooling** + the MRL head; a shared read-only mmap of the base with the synth instance, so the
weights are resident once.

**Training — the two-stage task-targeted recipe** (Jina-v5 / EmbeddingGemma shaped):
1. *General embedding distillation* to build the space: **embedding-space** distillation —
   project the student's pooled vector into the teacher's space via a learned linear ψ and
   minimize cosine distance (Jina's ablation: this beats score/similarity-matrix distillation,
   which plateaus). Teacher per §5's licensing gate (default Qwen3-Embedding-8B).
2. *Retrieval adapter* with a hybrid loss: **InfoNCE** over aimee-mined hard negatives +
   a **distillation-preservation** term + a **uniformity/GOR** term (cheap at full precision,
   decisive for truncation/quantization robustness) + the **MRL nested loss** over the ladder
   (§6). Adopt asymmetric **instruction prefixes** (query/doc), the Qwen3-Embedding/EmbeddingGemma
   convention.
- *Hard negatives* are mined from aimee's **real retrieval logs** (the ranker-outcome / kb_hybrid
  data) — the domain last mile that lets a small student beat a general teacher on aimee's
  distribution.

**Two honest ceilings.**
1. **Causal ≪ bidirectional for embeddings.** Last-token pooling on a frozen *causal* decoder
   is *materially* (not "a notch") weaker than a bidirectional encoder across the task range —
   which is why EmbeddingGemma/LLM2Vec convert to bidirectional. We do **not** (it would
   destroy the generator and forfeit the whole unification), so the embed role has a real
   ceiling. Consequences: the gate is **"≥ the incumbent Qwen3 embedder," not "≥ the teacher"**
   (a distilled student rarely exceeds its teacher). If teacher-parity is ever *required*, that
   forces a bidirectional, embed-only base — a **separate** model, which reopens "one base" and
   belongs in §12, not here.
2. **The MRL head is a new tensor, not a LoRA delta — prototype conversion FIRST.** LoRA adapts
   existing weights; it cannot add an output projection. The MRL projection head must be trained
   *and emitted by `convert_hf_to_gguf.py`* for the Gemma arch — the **same failure mode** as
   the ettin Dense head (P2), one role over. So the "few-MB adapters" framing is wrong for embed.
   **Gating pre-step:** before any embed training, prove `convert_hf_to_gguf.py` emits an MRL/pooling
   head and llama.cpp applies it on a stock head. If it doesn't round-trip, the honest options are
   converter arch work (net-new) or a small numpy projection in the gateway (the hack survives for
   embed even as it dies for rerank). This is on the critical path, so it runs before §5.3.

The reranker is deliberately **not** a role on this base — see §7.

## §4 Prerequisite work item — PLE in llama.cpp (re-scoped: harder than "two functions")

E2B/E4B rely on Per-Layer Embeddings, and llama.cpp does not inject the per-layer residual
in its forward graph ([#22243](https://github.com/ggml-org/llama.cpp/issues/22243) — confirm
current status; last seen unimplemented). Re-scoped honestly: in the Gemma-3n lineage PLE is
**entangled with AltUp (alternating updates) and LAuReL (learned augmented residual)** — it
is not a standalone residual add. Porting it means reproducing the interaction of three
interlocking mechanisms across ~30-35 layers (and again for the E2B slice), then holding
**embedding parity** (not just plausible generation — a subtly wrong residual silently tanks
Recall@K). This is a **multi-week ggml effort with a numeric parity gate** (cosine vs the HF
reference on a fixed corpus), vendored in the pinned `LLAMA_TAG` and decoupled from any
upstream-merge timeline. It is step zero for the E2B path and is amortized across synth +
embed on both sizes.

## §5 Training sequence (embed + synth)

1. **Synth distill (E2B tier only):** E4B (teacher) → E2B (student) on Tier-A traffic; silver
   labels are the incumbent's high-confidence outputs (per the Tier-A extraction proposal's
   bootstrap) + a human-audited gold eval set. Optionally distil both E2B and E4B from a
   **shared larger teacher** (12B/26B-A4B) to lift both — see §12.
2. **Freeze** the base.
3. **Embed adapter:** task-targeted embedding distillation — embedding-space distillation
   from a **clean-licensed** teacher, InfoNCE on aimee-mined hard negatives, a uniformity/GOR
   term, and an MRL loss over the dim ladder (§6). **Teacher licensing is a gate, not a
   footnote:** no CC-BY-NC teacher (NC propagates to the student); and any **API teacher
   (Voyage/Gemini) ToS typically forbids training a competing model** regardless of license —
   a legal sign-off item, and ensembles inherit the *most* restrictive terms. Default safe
   teacher: **Qwen3-Embedding-8B (Apache 2.0)**.
4. **InfoNCE is negatives-hungry.** Its quality depends on the effective batch of in-batch
   negatives, which a 16 GB card constrains — plan **GradCache / cross-batch memory** so the
   contrastive objective isn't hardware-bound ("the weights fit" ≠ "the objective is well
   conditioned").
5. **Quantization-aware finish**; PLE tables pinned Q8_0 (§4).

## §5b Maximizing E2B capability at fixed size (16 GB training)

Where a deployment commits to E2B (the lighter option; E4B still serves bigger boxes), the
lossless text-only strip (§2b) doesn't shrink it meaningfully — so the lever is not *smaller*
but *more capable at the same size*. Given 24/7 on one 16 GB card, in rough order of
return-for-effort:

- **Overtrain — the biggest free lever.** Small models keep improving well past the
  compute-optimal token count (train small, train long); 24/7 availability is exactly the
  resource this consumes. Cost: wall-clock only.
- **Domain-adaptive continued pre-training (DAPT).** Continue-pretrain E2B on aimee's own
  corpus (code + documents + memory text) before task distillation, raising on-domain base
  capability. Fits 16 GB.
- **Climb the LoRA → full-FT ladder as far as 16 GB allows.** LoRA caps how much the model can
  change; for capability prefer the most-trainable config that fits — fp16-base LoRA → high-rank
  LoRA → partial full-FT → **full fine-tune with an 8-bit optimizer + gradient checkpointing +
  frozen PLE tables** (only the ~2.3B transformer trains, so E2B full-FT is feasible on 16 GB).
  **Caveat:** deeper FT for one role can degrade the other (embed↔synth alignment tax) — keep
  the synth-distill → freeze → embed-adapter sequencing and re-verify *both* §8 gates after any
  deeper training.
- **Deeper distillation signal.** Match teacher *features* (hidden states / attention), not just
  outputs; teacher features are cached offline (§11), so this costs disk, not training VRAM.
- **A stronger teacher.** Teacher size is decoupled from the 16 GB card (offline cache), so a
  larger/better teacher than E4B raises the student ceiling (§12 escalation).
- **Concentrate capacity via narrowness.** The narrow aimee distribution means E2B's *effective*
  capability on your tasks exceeds what its size suggests — keep the training distribution
  tightly aimee's.

Each capability gain is bench-verified (§8), not assumed; the deeper-training levers are
constrained by role preservation, not just by 16 GB.

## §6 Dimensions via MRL — scoped precisely

- **One MRL ladder** (proposed `[256, 512, 1024, 2048, 2560, 4000]`; 4000 = `halfvec` index
  cap), front-loaded so each slice earns its recall (measured per-slice on the sweep).
- **Operator picks the dim** via `AIMEE_EMBEDDING_DIM`.
- **Re-embed-free tier moves are *narrower-only, given max-dim storage*.** Storing the full
  4000-dim vector and truncating + **renormalizing** + rebuilding the `halfvec(k)` index is
  legitimately re-embed-free — but it forfeits MRL's at-rest storage saving (you pay 4000-dim
  storage forever) and the "reindex" is an hours-scale HNSW rebuild on a large corpus, not
  free. Going *wider* than stored **is** a re-embed. State the claim as "narrower-tier moves
  only, if you stored max-dim."
- **Not chasing width for quality** — Jina reached its headline quality at 1024 dims
  (distilling a 4B teacher); the sweep decides how much of the ladder earns its storage.

## §7 The reranker (dedicated encoder, tier-aware, gated)

The reranker stays a **separate cross-encoder** — the right, fast tool — served through
**native llama.cpp `/rerank`** (`--reranking --pooling rank` + a single-linear
`cls.output.weight` head). This is the encoder-cross-encoder shape; a *decoder yes-token*
reranker does **not** serve through `/rerank` (that path is a classifier head, not an LM
logit — the earlier "unified decoder rerank" claim was a category error, and it's the exact
failure that produces near-zero scores for Qwen3-Reranker GGUFs).

**Recommended: the EuroBERT family** — Apache 2.0, genuinely multilingual (15 languages: 8
European + Chinese, Russian, Japanese, Vietnamese, Arabic, Turkish, Hindi), GQA/RoPE/RMSNorm,
8k context, beats ModernBERT on code + math, and a **210m / 610m / 2.1B** ladder that spans
tiers. Tier assignment from the measured latency wall (`reranker/LATENCY.md`: rerank cost is
K forward passes; CPU ms/cand scales ~with params):

- **CPU/edge tier → EuroBERT-210m** (candidate). Extrapolated ~185 ms/cand fits sub-1s only
  at ~top-5 (vs ettin-68m: 60 ms/cand, top-10 in 0.60s). **Gated on three checks, in order:**
  1. a **measured** ettin-68m vs EuroBERT-210m latency bake-off on the target box (GQA may
     beat the linear extrapolation — do not trust the projected number);
  2. a **top-K-depth quality check** — does 210m over a shallow top-K beat ettin-68m over a
     deep top-K on LoCoMo/LongMemEval? (a stronger reranker over fewer candidates often wins,
     but must be verified);
  3. the **native-`/rerank` smoke test** — EuroBERT's Llama-style encoder arch must convert
     and score correctly through `--pooling rank` (unverified; possibly net-new converter
     arch work; llama.cpp `/rerank` is finicky per-model, #16407).
- **GPU tiers → EuroBERT-610m** (speed is not the constraint there).
- **Fallbacks:** if the EuroBERT native-serving or CPU-latency gate fails — **re-headed
  ettin-68m** (`num_labels=1`, English-primary, fast, kills the hack) for CPU;
  **bge-reranker-v2-m3** (proven native, multilingual, 568M) for GPU.

All reranker roles are **domain-distillable** on the same pipeline (distill a strong reranker
teacher onto the chosen encoder on aimee's hard negatives).

## §8 Evaluation & rollout (gates every role swap)

- **Synth:** curator-synth bench — E2B Tier-A precision/recall **≥ 98% of stock E4B** on the
  mechanical tasks, latency/footprint win; per-task E4B fallback. (E4B tier: no gate needed.)
- **Embed:** `embedder-sweep` — Recall@5 / MRR **≥ the incumbent Qwen3 embedder** at the
  shipped dim and Q-level; per-slice curve committed. (**Not** "≥ teacher.")
- **Rerank:** the §7 three-gate ladder (measured latency, top-K-depth quality, native-serving)
  vs the ettin incumbent.
- **Rollout:** shadow → canary → default via `kb_bandit`; operator-pinnable per role.

## §9 Open decisions

1. **Language breadth** — EuroBERT's 15 vs a broader set (decides reranker fallback viability;
   also bears on the embed distribution).
2. **Embed teacher** — Qwen3-Embedding-8B (safe/clean) vs an ensemble (subject to §5 ToS/NC
   gate).
3. **MRL at-rest policy** — store max-dim (re-embed-free narrower moves, higher storage) vs
   store-at-tier-dim (cheaper storage, re-embed to change).
4. **Embed head placement** — converter-emitted MRL tensor vs a small gateway projection
   (decided by the §3 convert prototype).

## §10 Risks / honest tradeoffs

- **E2B synth-parity is the real risk** (not resources) — recover the E4B→E2B gap on
  mechanical tasks; §8-gated with E4B fallback.
- **Causal-decoder embeddings have a real ceiling** vs bidirectional; the gate is "≥
  incumbent," and the embed role may cap below a dedicated encoder embedder.
- **The MRL head and native `/rerank` both depend on GGUF-conversion paths that may need
  arch work** — the ettin-hack risk, relocated. Prototype conversion before training.
- **PLE port is multi-week ggml work** (AltUp/LAuReL entangled), with embedding parity as the
  hard part; the whole E2B path waits on it.
- **CPU fit is not "settled"** — measure RSS; the Q8-PLE pin narrows the margin vs the
  incumbent E4B-Q4.
- **EuroBERT native serving is unverified** — the reranker's clean-serving premise is a smoke
  test, not a given.

## §11 Training hardware & throughput (train on CUDA, serve on Vulkan)

One dedicated training card. **Recommended: RTX 5080 (16 GB, CUDA).** Serving stays
Vulkan/vendor-agnostic (the training card never touches production; dedicating the 5080 keeps
the 7900 XTX free to serve). 16 GB suffices for **LoRA on a frozen quantized 2–5B base**
(QLoRA fits 5–7B well under 16 GB) with CUDA's mature stack — and CUDA eliminates the
immature ggml-Vulkan-training risk. Manage 16 GB: keep the embed teacher ≤ ~14B to cache
on-card (bigger CPU-offloads, one-time); use GradCache for InfoNCE negatives (§5.4); Tier-3
full-FT (§12) is the one real pinch — spill to the 24 GB 7900 XTX or cloud. Binding constraint
is wall-clock. *(EuroBERT/ettin reranker training is small and fits trivially.)*

## §12 Future directions (explicitly out of scope for this proposal)

These were over-scoped into the earlier draft; they are real ideas but **not** part of the
decidable core, and several rest on unproven tooling:

- **Build-both-and-race / Pareto map** across E4B, E2B-distill, E2B-slice, pruned-E4B — a
  research program on one serialized card, not a "re-run."
- **Arbitrary MatFormer slicing** to custom sizes — needs bespoke MatFormer tooling, not a
  stock `transformers` op; "slice to any tier from one training run" is unproven.
- **Minitron-style prune-and-distill (trim E4B from above)** — full-parameter training, not
  the frozen-LoRA premise; the 16 GB card can't host it.
- **Cross-tier shared embedding space** ("index big, query cheap") — the earlier draft cited
  the *shared synth teacher* as the mechanism, but the embed space is produced by a *separate*
  embed adapter/teacher, so synth-teacher sharing does **not** align it. Genuine cross-tier
  space needs the embed heads of *both* tiers trained against one frozen teacher space, with
  its own gate (E2B-embedded docs retrieved by E4B queries). Unvalidated — a hypothesis.
- **Further size reduction** (beyond the lossless text-only strip in §2b) — MatFormer slicing
  to custom sizes, Minitron prune-and-heal, and vocab pruning — only after the base pipeline
  passes §8, and each gated head-to-head against the model it would replace.

## Acceptance criteria

- Embed + Tier-A synth serve from one Gemma-4 base (E2B edge / E4B bigger) through the
  existing per-role backends; the PLE port lands parity-verified.
- Each role passes its §8 gate at ≥ its incumbent (synth ≥ 98% E4B; embed ≥ Qwen3 embedder;
  rerank ≥ ettin on the three-gate ladder), rolled out shadow → canary → default.
- The dedicated reranker serves through native `/rerank`; the gateway Dense-head hack and its
  CI bake are removed.
- The MRL head's conversion path is proven (converter-emitted or a defined gateway
  projection) before embed training is scheduled.
- CPU fit is **measured** (RSS on the target box), not asserted.
- No change to Tier-B routing or to how facts are gated/promoted once extracted.

## Relationship to `dedicated-extraction-model-curator-tier-a` (both PENDING)

That proposal asks whether Tier-A should run a dedicated small model; its Option B is a
*note→triples relation extractor*. This proposal is broader (all Tier-A mechanical synth on a
Gemma base) and assumes a **different current Tier-A baseline** (CPU-tier E4B, not the
gpu-mid reasoning model that proposal describes). This **extends, does not supersede** it: if
this ships, the extraction proposal's Option B becomes a special case. Per the pending
supersession-hygiene norm, both should cross-link and reconcile their Tier-A baseline.

## Explicitly out of scope / does not re-propose

- The `aimee-llm` container, per-role serving, the Tier-A/Tier-B split, calibration, the
  bandit, the console — shipped; this chooses *models*.
- The `disable_thinking` fix (merged).
- The pgvector storage tier and versioned-index cutover — reused, not changed.
- General-benchmark (MTEB/MMTEB) parity as an acceptance target — the gate is aimee's own
  retrieval on aimee's corpus.
