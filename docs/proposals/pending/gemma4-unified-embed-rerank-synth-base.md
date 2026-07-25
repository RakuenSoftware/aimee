# Proposal: one Gemma-4 base for embedding, reranking, and Tier-A synth

- **State:** PENDING — design/idea only, no code in this PR. Consolidates a long
  design thread on replacing aimee's embedder + reranker + Tier-A synth model with
  a **single Gemma-4 backbone** carrying three task roles. Decisions below are
  *proposed*, not settled; the open forks are called out in §8 and every model swap
  is gated by a benchmark in §7. Nothing here flips a flag on merge — it is the
  artifact to decide against.
- **Author:** JBailes
- **Date:** 2026-07-25
- **Charter roles:** Recall (dense embedding for retrieval), Rerank (cross-encoder
  relevance scoring), Synthesize / Extract (Tier-A mechanical passes),
  Evaluate-Optimize (A/B each role against its incumbent on a fixed set),
  Gate-Promote (never a silent model swap; shadow → canary → default). Lives inside
  the existing `aimee-llm` per-role serving split and the Tier-A/Tier-B curator
  split.

## Thesis

The `aimee-llm` container already serves embedding, reranking, and synthesis from
one image (see [`unified-llm-container`](../done/unified-llm-container.md),
[`KB_LLM_BACKENDS.md`](../../KB_LLM_BACKENDS.md)) — but as **three separate models**:
a Qwen3/pplx embedder, an Ettin cross-encoder reranker, and a Gemma-4 synth model
(E4B on the CPU/Tier-A tier). That split carries three costs we keep paying:

1. **Three models resident** — three cold-fetches, three caches, three things to
   version, and on the CPU tier three sets of weights in RAM at once.
2. **The reranker is a conversion hack.** The Ettin ST score head does not survive
   GGUF conversion, so we serve the encoder headless and re-apply a Dense head in
   the gateway (`aimee_llm_rerank_head.py`), plus a CI job to pre-bake it
   ([`P2-serving-validation.md`](../../../benchmarks/results/unified-llm/P2-serving-validation.md)).
   It is the one non-llama.cpp compute in the stack and the part we like least.
3. **No shared capacity.** Embedding, reranking, and synth cannot share weights, so
   a box that runs all three pays for all three independently.

Gemma-4's architecture makes a different shape possible: **one backbone, three
roles.** E2B is a MatFormer-nested submodel of E4B, so an E2B *is* the elastic
compression of the synth model we already run; a frozen backbone plus small
task-specific LoRA adapters (the Jina-v5 pattern) lets that same backbone serve
embedding and reranking too. The result replaces all three models with **one base +
a few MB of adapters** — and, because E2B is smaller than E4B, comes out *lighter
than today's synth model alone* while absorbing the embedder and reranker into it.

This proposal is the specific realization of Option B in
[`dedicated-extraction-model-curator-tier-a`](./dedicated-extraction-model-curator-tier-a.md)
("a fine-tuned Tier-A model distilled from the incumbent"), extended so the same
fine-tuned base also carries Recall and Rerank.

## Goal

1. **One shipped model artifact** — a Gemma-4 base (proposed: E2B) plus task
   adapters — serving Recall, Rerank, and Tier-A Synthesize through the existing
   `aimee-llm` per-role backends.
2. **Tier-A synth on par with stock E4B** on the *mechanical* tier we actually run
   on CPU — verified on the curator-synth benchmark, not assumed.
3. **Embedding that matches a 4B/8B teacher on aimee's own retrieval** (the
   `embedder-sweep` LoCoMo + LongMemEval gate), CPU-usable, MRL-configurable dims.
4. **Native reranking** — delete the gateway Dense-head hack; rerank serves through
   llama.cpp with no bespoke gateway compute.
5. **A net resource win** — fewer models to fetch/cache/hold; a footprint no larger
   (target: smaller) than today's CPU tier.
6. **Never a silent swap** — each role graduates shadow → canary → default through
   the shipped calibration/bandit substrate; an operator can pin the incumbent.

## §0 What already exists (so we don't rebuild it)

- **Per-role serving.** `aimee-llm-supervisor.sh` already runs distinct embed
  (8081), rerank (8082), and synth (8083) backends behind the gateway
  (`aimee_llm_gateway.py`), each an OpenAI-compatible llama.cpp endpoint. Pointing
  all three at one base GGUF + per-role LoRA is a supervisor/config change, not new
  plumbing.
- **Tier-A/Tier-B split + `disable_thinking`.** `kb_curator_provider_for_stage`
  resolves Tier-A independently of Tier-B with no weak fallback; the shipped
  `disable_thinking` fix already makes mechanical extraction correct and cheap. A
  distilled E2B is a *model* change on the Tier-A route.
- **Benchmarks.** `benchmarks/embedder-sweep.sh` (Recall\@K / MRR on LoCoMo +
  LongMemEval), the curator-synth bench (`benchmarks/results/synth/`), and the
  reranker latency harness (`benchmarks/results/reranker/LATENCY.md`) are the three
  gates §7 reuses. The P2 serving-validation doc already recorded the Gemma/Vulkan
  serving flags and the Ettin conversion finding.
- **Calibration + bandit.** `kb_calibrate` / `kb_bandit` drive shadow → canary →
  default rollouts — the vehicle for every §7 A/B.
- **Grammar-constrained output.** `provider_client_build_openai` emits
  `response_format: json_schema`; a `--jinja` llama.cpp endpoint returns
  schema-valid JSON, so the synth role needs no prompt babysitting.
- **pgvector + MRL substrate.** DB2 holds vectors as `halfvec` (indexable to 4000
  dims); the versioned-index cutover path for a dim change already exists
  (`embedder-sweep.md`). MRL turns most future dim moves into truncate-and-reindex
  rather than re-embed.

## §1 The unification, quantified

| | today (CPU tier) | proposed |
|---|---|---|
| Synth (Tier-A) | Gemma-4 **E4B** (dense) | Gemma-4 **E2B**, distilled from E4B on Tier-A traffic |
| Embedding | separate Qwen3/pplx embedder | E2B base **+ embed LoRA + MRL head** |
| Reranking | Ettin encoder **+ gateway Dense head + CI bake** | E2B base **+ rerank LoRA**, native llama.cpp `/rerank` |
| Resident weights | 3 models | 1 base + ~few MB adapters |
| Reranker gateway compute | ~4 MB numpy Dense head | none |
| Footprint vs. today's synth alone | baseline | **≤ baseline** (E2B < E4B) |

The claim in the last row is the headline and the thing §7 must prove: absorbing
two models while getting *smaller* than the one synth model we run now.

## §2 The base (proposed: Gemma-4 E2B — gated on the CPU budget)

A prior base-selection pass compared EuroBERT (encoder, Apache-2.0, CPU-optimal,
~15 langs), ModernBERT (encoder, English+code only), Qwen3-0.6B-Base (decoder,
Apache-2.0, the proven Jina-v5 base), EmbeddingGemma-300M, and Gemma-4 E2B. **E2B is
proposed** for the unified artifact because only it makes the *three-role* story
work: it is the same lineage as the E4B synth model (so "match E4B synth" is a
distill-within-the-family), it is now Apache-2.0, and its MatFormer nesting composes
with MRL — elastic **model size** × elastic **embedding dim** from one artifact.

E2B is also the heaviest option (2.3B effective / 5B total; PLE tables must stay
Q8_0, so the footprint is above a naive Q4 estimate). Whether it clears "usable on
CPU" is **the open gate** (§8): if E2B misses the latency/RAM budget on the target
box, the fallback is Qwen3-0.6B-Base (decoder, unifies embed+synth, drops the
MatFormer elasticity) with rerank as a dedicated small encoder.

## §3 Three roles off one base

- **Synthesize (Tier-A).** Distill **E4B → E2B** on real + synthetic Tier-A traffic
  (the mechanical extract/index passes — *not* Tier-B reasoning, which the CPU tier
  never runs). Because the task is mechanical and E2B is already E4B's nested
  submodel, closing the residual quality gap on *this* distribution is tractable.
  Synth is served by the (distilled) base directly.
- **Recall (embedding).** An **embed LoRA + last-token pooling + MRL projection
  head** on the frozen distilled base. Last-token pooling (not a bidirectional
  conversion) is deliberate: it keeps the base a working generator so synth is
  preserved. Honest ceiling: this sits a notch below a dedicated bidirectional
  embedder (EmbeddingGemma went bidirectional and thereby stopped being a
  generator) — LoRA is the balance point, and §7 measures the gap.
- **Rerank.** A **rerank LoRA** (LLM-as-reranker, yes-token scoring) on the same
  base — **native llama.cpp `/rerank`, no gateway head.** The honest catch: this is
  a *decoder* cross-encoder, the efficiency profile we disliked at Qwen3-4B/8B.
  Reranking is the CPU-dominant per-query cost (top-K forward passes), so §7 must
  confirm it fits the latency budget. Mitigations: the base is already resident (no
  extra load), tight top-K + score-based early-exit, and — the MatFormer payoff —
  rerank on a **thinner nested slice** of the base than synth uses. **Fallback:** if
  unified rerank misses budget, rerank stays a dedicated small encoder, but with a
  `num_labels=1` head that serves through native `/rerank` (killing the gateway hack
  regardless). Either outcome beats today.

## §4 Prerequisite work item — PLE in llama.cpp

E2B/E4B rely on Per-Layer Embeddings; llama.cpp loads the PLE metadata but the
forward graph never injects the per-layer residual
([ggml-org/llama.cpp#22243](https://github.com/ggml-org/llama.cpp/issues/22243) —
proposal stage, no PR/branch/assignee). So the models "run" but embeddings are
silently degraded — disqualifying for an embedder until fixed. We vendor a pinned
llama.cpp (`LLAMA_TAG` in `Dockerfile.aimee-llm`), so *shipping* a patch is routine;
the cost is *implementing* it: port `get_per_layer_inputs` / `project_per_layer_inputs`
from the HF Transformers reference (the PR cited in #22243) into the ggml graph, verify
**embedding parity** against the HF reference using the P2/`embedder-sweep`
harness, and upstream it to shed the rebase burden. Localized, reference-guided,
bounded — but real ggml work, and **step zero**: it is paid once and amortized
across all three roles and both sizes.

## §5 Training sequence

1. **Synth distill:** E4B (teacher) → E2B (student) on Tier-A traffic; silver labels
   are the incumbent's high-confidence outputs (per the Tier-A extraction proposal's
   distillation bootstrap), plus a human-audited gold eval set.
2. **Freeze** the distilled base.
3. **Embed adapter:** task-targeted embedding distillation (the Jina-v5 /
   EmbeddingGemma recipe) — embedding-space distillation from a clean-licensed 4B/8B
   teacher (Apache/MIT only; **no CC-BY-NC teacher** — the NC restriction propagates
   to the student), InfoNCE on aimee-mined hard negatives, a uniformity/GOR term for
   quantization robustness, and a **Matryoshka** loss over the dim ladder (§6).
4. **Rerank adapter:** yes-token relevance scoring on labeled (query, doc) pairs.
5. **Quantization-aware** finish so the shipped Q-level holds recall (PLE tables
   pinned Q8_0 per §4).

Teacher choice is an open fork (§8): Qwen3-Embedding-8B (Apache-2.0) is the safe
clean teacher; Harrier-OSS-v1 (MIT, if confirmed) and API teachers (Voyage-4 for
code, Gemini-2) are candidates — multi-teacher ensembles are permitted since
embedding-space distillation does not require the teacher's dim to match the
student's.

## §6 Dimensions, configurable via MRL

- **One MRL ladder**, proposed `[256, 512, 1024, 2048, 2560, 4000]` — 4000 caps at
  the `halfvec` index ceiling; the vector is front-loaded so every slice earns its
  recall (measured per-slice on the sweep).
- **Operator picks the dim** via `AIMEE_EMBEDDING_DIM`; the same GGUF serves 1024 on
  a CPU box and a wider slice on a GPU box.
- **Tier moves stop being re-embeds.** Because the smaller dims are truncated
  prefixes of the larger, embed-once-at-max and dropping to a smaller tier is a
  **truncate-and-reindex** (slice stored vectors, re-normalize, rebuild the
  `halfvec(k)` index), not a re-run of the model over the corpus — collapsing the
  re-embed runbook in `KB_LLM_BACKENDS.md` into an index rebuild.
- **Not chasing width for quality.** Jina hit 8B-parity at 1024 dims; the sweep
  decides how much of the ladder earns its storage on aimee's corpus.

## §7 Evaluation & rollout (gates every role swap)

Each role A/Bs against its incumbent; none flips silently (charter Gate-Promote):

- **Synth:** curator-synth bench — E2B Tier-A precision/recall ≥ stock-E4B on the
  mechanical tasks, with a latency/footprint win. Fall back to E4B per Tier-A task
  that resists.
- **Embed:** `embedder-sweep` — Recall\@5 / MRR ≥ the 4B/8B teacher on LoCoMo +
  LongMemEval at the shipped dim and Q-level; per-slice curve committed.
- **Rerank:** latency harness — top-K rerank within the CPU budget *and* ranking
  quality ≥ the Ettin incumbent; otherwise take the §3 dedicated-encoder fallback.
- **Rollout:** shadow (log, commit neither) → canary → default via `kb_bandit`;
  operator-pinnable per role from the KB console.

## §8 Open decisions (explicitly unsettled)

1. **CPU budget + target hardware** — the embed-ms / rerank-ms / RAM ceiling on the
   real box. Gates E2B-vs-fallback (§2) and unified-vs-dedicated rerank (§3). This is
   the single most load-bearing open number.
2. **Language breadth** — E2B's 140+ langs vs. a narrower set; decides whether the
   encoder fallbacks (EuroBERT ~15 langs) are even viable alternates.
3. **Teacher(s)** for the embed distill (§5) — clean-licensed single vs. ensemble.
4. **Rerank: unified LoRA vs. dedicated encoder** — resolved by the §7 latency gate,
   not up front.

## Risks / honest tradeoffs

- **E2B may miss the CPU budget.** It is the heaviest base; PLE tables are Q8-only.
  If it does, the whole three-role-on-E2B shape gives way to the Qwen3-0.6B fallback
  and the PLE work is wasted — so §8.1 should be answered *before* the PLE port
  starts.
- **Frozen-causal base caps embedding quality** vs. a dedicated bidirectional
  embedder. Accepted to keep synth intact; §7 bounds the gap.
- **Decoder reranking is heavier per pair** than a small encoder. Mitigated by
  MatFormer slicing + top-K + early-exit; fenced by the §7 latency gate.
- **"Match E4B synth" is a claim, not a given.** MatFormer left an E2B↔E4B gap; the
  Tier-A distill closes it only on mechanical tasks, and only the bench proves it.
- **PLE depends on an unmerged llama.cpp feature we must implement.** Bounded and
  reference-guided, but it is net-new ggml work and a maintenance item until
  upstreamed.

## Acceptance criteria

- A decision recorded on §8.1 (CPU budget/hardware) and, from it, E2B confirmed or
  the Qwen3-0.6B fallback chosen.
- If E2B: the PLE port lands (parity-verified, upstream PR opened); the base serves
  all three roles off one GGUF through the existing per-role backends.
- Each role passes its §7 gate at ≥ incumbent quality with the resource win, rolled
  out shadow → canary → default and operator-pinnable.
- The gateway Dense-head hack and its CI bake are removed once rerank serves natively
  (unified or dedicated-with-`num_labels=1`).
- No change to Tier-B routing or to how facts are gated/promoted once extracted.

## Explicitly out of scope / does not re-propose

- The `aimee-llm` container, the per-role serving split, the Tier-A/Tier-B curator
  split, calibration, the bandit, the KB console — all shipped; this chooses the
  *models* and unifies their backbone.
- The `disable_thinking` fix (merged) — the synth role builds on it.
- The pgvector storage tier and the versioned-index cutover mechanism — reused, not
  changed.
- General-benchmark (MTEB/MMTEB) parity as an acceptance target — the gate is
  aimee's own retrieval on aimee's corpus, not a public leaderboard.
