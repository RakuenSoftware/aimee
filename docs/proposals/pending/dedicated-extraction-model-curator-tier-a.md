# Proposal: a dedicated extraction model for the curator Tier-A

- **State:** PENDING — design/idea only, no code in this PR. Follows the shipped
  fix that disables the reasoning pass for Tier-A curator stages
  (`provider_def_t.disable_thinking`). That fix made mechanical extraction
  *correct and cheap on the model we already run*; this proposal asks the next
  question: should Tier-A run a **dedicated small model** instead of the shared
  general reasoning model at all? Presents two options — (2) a small
  non-reasoning instruct model on its own curator tier, and (3) a fine-tuned
  relation-extraction model — with a recommendation and a staged path. No flag is
  flipped by merging this; it is the artifact to decide against.
- **Author:** JBailes
- **Date:** 2026-07-08
- **Charter roles:** Extract (structured entity/fact/relationship extraction),
  Reconcile (map extracted relations onto the `rel_types` ontology),
  Evaluate-Optimize (A/B a candidate extractor against the incumbent on a fixed
  note set, shadow → canary → default via the shipped calibration/bandit
  substrate), Gate-Promote (never a silent model swap). Lives entirely inside the
  charter's Extract/Reconcile spine and the existing Tier-A/Tier-B curator split.

## Thesis

Fact extraction is the KB's highest-volume LLM call: the `memory_facts` drain and
the doc/code curator extract stages run one completion **per note and per symbol**,
continuously, offline. It is also the most *mechanical*: read a short span, emit
seed-ontology triples as JSON. It is grammar-constrained and bounded — the opposite
of a task that benefits from a large general **reasoning** model.

Today all curator stages resolve to one provider per tier, and on the split stack
Tier-A points at the same **gpu-mid reasoning model** (`aimee-synth`, Gemma) that
serves the reasoning stages. Two concrete problems follow from running extraction
on a reasoning model, both measured on the live .254 stack (2026-07-08):

1. **It broke extraction outright.** The model's chain-of-thought pass consumed the
   completion budget before the JSON, so `memory_facts` committed **0 typed facts**
   (job ran, `typed_facts` empty). Same note, reasoning disabled: complete JSON,
   `finish=stop`, **~50 completion tokens**. The shipped `disable_thinking` fix
   resolves *this* failure.
2. **Even fixed, it is the wrong tool for the volume.** A general reasoning model is
   large (VRAM-resident on the 7900 XTX), slow per call, and overqualified for
   "Jonathan Bailes → works_for → Rakuen Software." At drain scale (thousands of
   notes/symbols) that is paid latency, GPU occupancy, and — the point of the
   Tier split — capacity the *reasoning* stages (judge, synthesize) then can't use.

The Tier-A/Tier-B split already encodes the right intuition ("mechanical vs
reasoning; a weak model must never serve the reasoning stages"). This proposal
completes it: give **Tier-A its own model**, sized and shaped for extraction, and
free the big model to do only what it is for.

## Goal

1. **A dedicated Tier-A extraction model** — small, non-reasoning, grammar-capable
   — resolved by the existing `kb_curator_provider_for_stage` Tier-A path, with the
   reasoning model reserved for Tier-B.
2. **No quality regression on the constrained task** — extraction precision/recall
   on the seed ontology holds vs the incumbent, verified on a fixed note set before
   any default flip.
3. **A real throughput/cost win** — measurable drop in per-note latency and GPU
   occupancy for the drain, and reasoning-tier capacity freed.
4. **Never a silent swap** — the candidate model runs shadow → canary → default
   through the shipped calibration/bandit machinery; an operator sees the A/B and
   can pin either model.
5. **Two graduation tiers** presented, so the decision is explicit: **(2)** a small
   off-the-shelf instruct model (low effort, most of the win) and **(3)** a
   fine-tuned relation extractor (max quality/throughput, real upkeep).

## §0 What already exists (so we don't rebuild it)

- **Tier routing.** `kb_curator_provider_for_stage` (§ `kb_curator_provider.c`)
  already resolves Tier-A from `provider.*` / `LLM_ENDPOINT` and Tier-B from
  `tier_b.*`, with **no weak fallback** from B to A. A dedicated Tier-A model is a
  *config* change to `kb_curator_provider_base_url`/`_model`, not new plumbing.
- **`disable_thinking`.** Just shipped; Tier-A already sends
  `chat_template_kwargs.enable_thinking:false`. A small non-reasoning model simply
  ignores it — so the two changes compose cleanly.
- **Grammar-constrained output.** `provider_client_build_openai` already emits
  `response_format: json_schema` when a schema is passed — so a small model on a
  `--jinja` llama.cpp endpoint returns schema-valid JSON without prompt babysitting.
- **Reconciliation.** The `rel_types` seed gate + autonomous promotion (§7 of the
  sidecar proposal) already turn extracted relations into durable facts; the
  extractor only has to emit good triples.
- **Calibration + bandit.** `kb_calibrate` / `kb_bandit` already fit per-surface
  thresholds and can drive a shadow→canary→default rollout — the vehicle for the
  A/B in §4.
- **Gateway.** The gpu-mid gateway is a llama.cpp OpenAI endpoint; it can serve a
  second small GGUF, or a sibling CPU endpoint can via `LLM_ENDPOINT`.

## §1 The case, quantified

| | reasoning model (incumbent) | small non-reasoning extractor |
|---|---|---|
| Tokens/extraction (measured) | ~500 w/ thinking (empty if truncated); ~50 w/ `disable_thinking` | ~50, deterministic |
| Latency/call | high (large model) | low (1–4B) |
| GPU footprint | ~20 GB resident, competes with Tier-B | ~1–3 GB, or CPU |
| Quality on seed triples | high | high (task is easy under the ontology constraint) |
| Quality on nuanced/implicit facts | higher | *lower* — the honest tradeoff (§4 must measure it) |

The constrained-ontology task does not need the big model's headroom; the free-form
tail (implicit/multi-hop facts) is where a large model still wins, and is exactly
what §4 must A/B before trusting a swap.

## §2 Option A (recommended): a small non-reasoning instruct model on Tier-A

**What:** run a 1–4B instruct model (grammar/JSON-capable, no reasoning mode) as the
Tier-A curator provider. Candidates to evaluate (no endorsement yet): Qwen2.5-1.5B/3B
Instruct, Gemma-2-2B-it, Llama-3.2-1B/3B-Instruct, Phi-3.5-mini — all run as GGUF on
the existing llama.cpp gateway with `--jinja` for schema-constrained output.

**Deployment on the split stack:**
- **Co-resident on gpu-mid:** a 1–3B GGUF adds ~1–3 GB VRAM on the 7900 XTX
  alongside the synth model — feasible given the card's headroom; served as a
  second model id on the same gateway, or a second llama.cpp instance.
- **Or a CPU Tier-A sibling:** point `LLM_ENDPOINT` at a small CPU llama.cpp; the
  drain is offline, so CPU latency is acceptable and it leaves the GPU entirely to
  Tier-B. This matches the "zero-config CPU sibling" the tier code already
  anticipates.

**Config:** set `kb.curator.provider.base_url`/`.model` (Tier-A) to the small model;
leave `tier_b.*` on the reasoning model. Surfaced in the aimee-kb console next to the
typed-facts knobs (KB-owned).

**Effort:** low — model selection + a config route + the §4 eval. No training.

## §3 Option B (long-horizon): a fine-tuned relation extractor

**What:** a small base fine-tuned specifically for note→triples over *our* ontology.

**Training data — cheap to bootstrap:** distill from the incumbent. Run the
reasoning model (thinking on, unbudgeted) over a corpus of real + synthetic notes,
keep high-confidence triples that survive the `rel_types` gate as silver labels,
add a small human-audited gold set for eval. This is the standard "big model teaches
small model" distillation; the ontology constraint keeps the label space tiny.

**Base + method:** LoRA/QLoRA on a 1–3B base (same family as §2 so deployment is
identical), or a purpose-built extraction/NER architecture if throughput dominates.

**When it's worth it:** only if §4 shows the off-the-shelf small model (§2) leaves
meaningful recall on the table on nuanced facts *and* volume is high enough to amortize
the training/eval/upkeep. Otherwise §2 is the answer and §3 stays parked. Carries real
cost: a training pipeline, dataset versioning, drift re-training, and eval gates.

## §4 Evaluation & rollout (gates any swap)

1. **Fixed note set:** assemble ~200 notes (real memories + synthetic edge cases:
   first/third person, multi-fact, implicit, negation, ambiguous) with gold triples.
2. **Metrics:** triple precision/recall/F1 against gold; % surviving the `rel_types`
   gate; latency/call; GPU-seconds/note. Incumbent (reasoning, `disable_thinking`) is
   the baseline.
3. **Shadow:** run the candidate alongside the incumbent on the live drain, log both,
   commit neither — compare on real traffic (via the calibration substrate).
4. **Canary → default:** promote via `kb_bandit` only when precision holds and
   throughput improves; operator can pin either model from the console. Never a silent
   flip (charter Gate-Promote).

## §5 Risks / honest tradeoffs

- **Recall on nuanced facts** may drop vs the big model — §4 measures it; if it drops
  beyond a set bar, keep the reasoning model for Tier-A (the `disable_thinking` fix
  already makes that acceptable) and treat this as not-worth-it.
- **A second model on the GPU** competes for VRAM with the synth/embedding models;
  the CPU-sibling path (§2) sidesteps this entirely for an offline drain.
- **Two models to keep current** (versions, prompts) — mitigated by routing both
  through the one gateway + config, and by §3 only if truly warranted.

## Acceptance criteria

- A decision recorded: pursue §2, pursue §2→§3, or park (keep reasoning-model Tier-A).
- If §2: Tier-A resolves to the chosen small model via config; the §4 fixed-set eval
  runs and shows precision ≥ baseline with a latency/GPU win; rollout is gated
  (shadow→canary→default), operator-pinnable, KB-owned in the console.
- No change to Tier-B routing; the reasoning stages keep their model.
- `disable_thinking` remains correct for whichever model serves Tier-A.

## Explicitly out of scope / does not re-propose

- The Tier-A/Tier-B split, the curator stage machine, reconciliation, calibration,
  the bandit, the console — all shipped; this only chooses Tier-A's *model*.
- The `disable_thinking` fix (already merged) — this builds on it.
- Any change to how facts are gated/promoted once extracted.
