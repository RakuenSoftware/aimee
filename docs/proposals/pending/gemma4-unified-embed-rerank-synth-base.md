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

**Resource fit is not a concern:** the CPU tier already deploys the *larger* E4B
for Tier-A synth, so E2B — roughly half the active compute (2.3B effective vs. E4B's
~4B) — is a strict **reduction**. PLE keeps resident memory near the *effective*
size (its whole reason for existing, and why the E-series runs on edge hardware), so
sizing from the 5B *total* overstates it; the deployed E4B is the existence proof
that E2B fits the target 2-core / 4 GB box with headroom. The single-forward embed
and rerank passes are lighter still than the E4B *generation* already run there. The
open risk is not fit but **synth-quality parity** (§7) — recovering the E4B→E2B
nested-submodel gap; if that gap proves unrecoverable on the mechanical tier, the
fallback is a dedicated small embedder + encoder reranker with Tier-A staying on a
capable node.

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

1. **Synth distill:** distill Tier-A synth into *both* E4B and E2B from a **shared,
   larger teacher** (e.g. the 12B / 26B-A4B already run on the bigger tiers, or a
   larger external model) — not only E4B→E2B — so both students are pulled toward one
   target, which also aligns their output geometry for the cross-tier shared space
   (§10). Silver labels are the teacher's high-confidence outputs (the Tier-A
   extraction proposal's distillation bootstrap) plus a human-audited gold eval set.
   Plain E4B→E2B remains a valid cheaper fallback.
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

1. **CPU budget** — target 2-core / 4 GB. **Resolved for the base:** the tier already
   runs the larger E4B, so E2B (less resource-intensive) is within budget by
   construction; the base is confirmed E2B. The residual CPU question is only
   unified-vs-dedicated rerank latency (§3), not whether the base fits.
2. **Language breadth** — E2B's 140+ langs vs. a narrower set; decides whether the
   encoder fallbacks (EuroBERT ~15 langs) are even viable alternates.
3. **Teacher(s)** for the embed distill (§5) — clean-licensed single vs. ensemble.
4. **Rerank: unified LoRA vs. dedicated encoder** — resolved by the §7 latency gate,
   not up front.

## §9 Model extraction & size reduction (validation phase)

The narrow use case — multilingual document + code ingestion (embed, rerank, Tier-A
extract), no image/audio, no general chat — permits cutting E2B down to only what
serves ingestion. This is **not required for fit** (E2B is already lighter than the
deployed E4B); it is a **latency / throughput headroom** lever, run as a **gated
experiment *after* the base pipeline is proven** — not a front-loaded decision.

**Sequencing — an incremental, gated ladder, tested between each step.** Prove the
base unified pipeline first (PLE port §4 → E4B→E2B synth distill + embed/rerank
adapters §5 → all §7 gates green on stock text-only E2B). Then apply the tiers in
order, re-running the three gates (embedder-sweep, curator-synth, rerank latency)
*after each* so every cut has its own pass/fail checkpoint before the next begins.
Tiers 1→2 are the incremental path (each well-defined, each independently validated).
Tier 3, *if feasible at all*, is not judged against the raw gates but **head-to-head
against the surviving Tier-2 model** — adopted only if it is materially smaller/faster
at equal gate quality, enough to pay for its bespoke-serving cost.

- **Tier 1 — drop the modality towers (applied during the base build; lossless).**
  Convert text-only: the vision (SigLIP-class — this is the "OCR" path) and audio
  (USM-class) encoders are never instantiated. Hundreds of MB–~1 GB off, zero
  text-quality cost, standard Gemma-text serving. The baseline cut, re-confirmed in
  validation.
- **Tier 2 — extract a smaller MatFormer slice (low risk).** Pull the smallest nested
  submodel that still passes the gates. It is a Google-trained coherent submodel and
  stays a known Gemma shape, so no extra serving burden.
- **Tier 3 — prune-and-distill to a custom size (Minitron-style; highest payoff,
  highest serving cost).** Structured-prune depth / FFN-width / heads (and vocab only
  for scripts never ingested — multilingual caps this; PLE tables shrink with any
  vocab cut), then heal by distilling E4B → pruned student on aimee's distribution.
  The narrow heal distribution is precisely what permits aggressive pruning. Cost: a
  bespoke architecture the vendored llama.cpp must carry, compounding the §4 PLE work
  — evaluated head-to-head against the Tier-2 slice (not just against the raw gates):
  it wins only if it is materially smaller/faster at equal quality; otherwise Tier 2
  stands and Tier 3 is parked.

Every cut is a search for the smallest model that still passes §7, not a fixed
target; none flips a default (charter Gate-Promote).

## §10 Generalization across the tier ladder (E4B and beyond)

The design is **backbone-agnostic** — it is "a Gemma-4 MatFormer base + synth +
embed/rerank adapters + MRL," not anything specific to E2B. Since E2B is a MatFormer
submodel *inside* E4B, the same pattern applies up the ladder as **one program**, not
a per-tier rebuild:

- **Train against E4B, slice down.** With nested / MatFormer-aware objectives, training
  the unified stack on E4B can yield the E2B tier by *slicing* — E4B for larger boxes,
  E2B-slice for the edge, from one effort.
- **The E4B tier carries no synth-parity risk.** The §7 open risk (recovering the
  E4B→E2B nested-submodel gap) is specific to the *shrunk* tier. An E4B-unified base
  runs *actual* E4B for synth and only adds adapters — so E4B is the **lower-risk**
  instantiation and the natural place to **prove the three-role mechanics** (adapters,
  MRL head, native `/rerank`, the §4 PLE port) *before* attempting the E2B compression.
- **Cross-tier shared embedding space (the prize, if trained for).** If the MRL heads
  across slices are aligned to one teacher space, tiers share an embedding space — a KB
  embeds on the edge (E2B) and queries/reranks on a larger tier *without re-embedding*
  (the "index big, query cheap" property). This is a deliberate space-alignment
  objective, not automatic.

Caveats: **slice-after-training is not free** — preserving nestedness requires
MatFormer-aware nested training *or* a frozen backbone with per-slice adapters (naive
full fine-tune breaks the nesting); **role heads likely need per-tier validation** (the
backbone nests, the heads may not transfer for free); and the pattern is **cleanest
within the MatFormer E-series (E2B↔E4B)** — the dense-12B / MoE-26B synth models that
serve Tier-B reasoning have no MatFormer nesting and are a separate unification question,
out of scope here.

**Build both and race them.** The comparison is *not* "pick a champion" — E4B and E2B
have different quality-per-resource profiles, so they most likely each win a *different*
tier. The evaluation output is a **tier → model Pareto map**: on the 2-core / 4 GB edge,
E2B may win by *fitting interactively at all*; on a box that affords it, E4B's higher
embed/synth quality may be worth the cost. The marginal cost of building both is low —
the expensive machinery (PLE port, teacher, distillation data, bench harness, adapter
recipe) is paid once, so the second backbone is a re-run, not a rebuild. The sharpest form
fixes a **cost target** (the edge tier's latency/RAM) and races contestants that
approach it *from both directions*:

- **Distill up from below** — E2B (a MatFormer slice or a dedicated E4B→E2B distill),
  or a further-downsized sub-E2B trained toward E4B quality on aimee's narrow
  distribution.
- **Trim down from above** — E4B structurally pruned to ~E2B size and speed
  (Minitron-style, §9 Tier 3), healed by distillation.

E4B itself is the quality-ceiling reference. The winner is simply the best quality that
still holds at the cost target on the §7 benches — which settles §9's Tier-2 (slice vs
distill) *and* Tier-3 (is a trimmed-E4B better than any E2B?) inside one head-to-head.
The honest asymmetry the race resolves: **trimming from above tends to retain more
quality per parameter** (it starts from E4B's good weights) but yields a **bespoke
serving shape**; **distilling up a fixed slice keeps a known shape** but is
**capacity-bounded** — quality-per-param vs serving simplicity, decided empirically.

## §11 Training hardware & throughput

Target training rig: one **24 GB Radeon RX 7900 XTX** (RDNA3) with ~157 GB system RAM,
available 24/7 (the `.254` box in
[`P2-serving-validation`](../../../benchmarks/results/unified-llm/P2-serving-validation.md)).
Sufficient, because two things decouple from the 24 GB VRAM:

- **Student side is LoRA/QLoRA on a frozen, quantized base** — the memory-light case
  (QLoRA fit 33B on 24 GB); adapters + MRL head + optimizer states are small. A
  Minitron prune-heal of a ~2–4 B student also fits with gradient checkpointing + an
  8-bit optimizer.
- **Teacher targets are cached offline**, decoupling teacher size from training VRAM:
  run the (larger) teacher in inference passes — quantized on-card, or CPU-offloaded
  into the 157 GB system RAM — over the corpus *once* to cache embeddings / soft labels,
  then train students against the cache. A 24–32 B teacher runs on-card quantized; up to
  ~70 B is reachable via CPU offload, slowly. The teacher and the training never
  co-reside.

The binding constraint is therefore **wall-clock, not memory**: one GPU running the
multi-model race (E2B-slice, E2B-distill, E4B-distill, pruned-E4B) sequentially is a
weeks-scale program — which is what 24/7 availability is for.

**Caveat to validate early:** the AMD/ROCm training path on RDNA3 (PyTorch-ROCm plus a
working 4-bit/LoRA stack — bitsandbytes / flash-attention are less turnkey than on CUDA).
The memory budget fits; the toolchain needs a smoke test before the full program is
scheduled on this card.

## Risks / honest tradeoffs

- **Synth-quality parity is the real risk, not resources.** E2B is *less*
  resource-intensive than the E4B the tier already runs, so fit is settled. The open
  risk is recovering the E4B→E2B nested-submodel quality gap on the Tier-A mechanical
  tasks (§7 gate); if it proves unrecoverable, keep Tier-A on a capable node and ship
  the embed/rerank roles as dedicated small models.
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
- The size-reduction validation phase (§9) runs only *after* the base pipeline passes
  §7: Tier 1 (tower-drop) applied during the build, Tiers 2–3 adopted only where the
  benches hold.
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
