# Proposal: Streaming repetition-collapse guardrail + per-backend temperature calibration

- **State:** proposed (pending — not started)
- **Author:** JBailes
- **Charter roles:** Detect-Cluster, Constrain-Verify / Enforce, Calibrate /
  Evaluate-Optimize, Gate-Promote

## Thesis

Small reasoning models served through the gateway have a specific, reproducible
failure mode: mid-generation they fall into a **degenerate repetition collapse** —
they emit a short span ("Wait, let me reconsider…", "So the answer is", a line of
scratch arithmetic) and then re-emit it near-verbatim over and over until the
output budget is exhausted. The turn produces no answer, burns the full
`max_tokens`, and — because the gateway streams it straight through — the caller
watches a wall of repeated text before the turn finally stops. On a delegate or
roundtable panel this is worse than a plain error: it is an expensive error that
looks like progress.

The failure is not random noise; it has a well-understood shape and three
compounding causes:

1. **Over-frequent discourse tokens under uncertainty.** When a reasoning trace
   hits a hard step, the next-token distribution collapses onto a few very common
   "hedge/restart" tokens ("Wait", "Alternatively", "So"). These are
   over-represented in synthetic reasoning traces, so they win even when they
   carry no new information.
2. **Self-reinforcement through context.** Once a span appears twice, attention
   over the just-emitted repeats drives the probability of emitting it *again*
   toward 1.0. Each repeat makes the next repeat more likely — a one-way ratchet.
   Semantic repetition (same *idea*, reworded) reliably precedes verbatim
   repetition, so there is an early-warning window before the text goes fully
   periodic.
3. **Low-temperature decoding has no escape.** Stable reasoning wants low
   temperature, but low temperature is exactly what locks greedy-ish sampling
   into the reinforced loop with no way out. The loop is a property of the
   temperature *and* the ratchet, not of a "bad" prompt.

The key structural insight we adopt: **the loop is decided at one token — the
first token of the first repeat.** Everything after that position is the ratchet
executing. If we can detect the collapse early and intervene at that single
boundary, we can break the loop without disturbing the rest of the generation and
without retraining anything. That reframes an offline model-quality problem into
an **online, deterministic gateway guardrail** — which is squarely in Aimee's
lane.

This proposal adds that guardrail: a streaming detector on the served-generation
relay, a bounded decode-time intervention when a collapse is detected, and the
telemetry to calibrate each local backend's default serving temperature from
observed collapse rates.

## Goal

For any generation served through the gateway (delegate turns, roundtable panels,
ingress `/v1/messages` and `/v1/responses`, webchat), when the model enters a
repetition collapse:

1. **Detect it early** — at the semantic-repeat stage where affordable, and always
   at the unambiguous verbatim stage, before `max_tokens` is wasted.
2. **Intervene deterministically** — stop the doomed continuation and either
   (a) resume the *same* turn with escalated anti-loop sampling for the remainder
   using only standard request parameters, or (b) truncate the not-yet-emitted
   tail and finish the turn with an honest stop reason. Never let a collapsing
   turn run to `max_tokens`.
3. **Record and calibrate** — log per-backend / per-temperature collapse rates to
   the audit store, and let the server recommend (and, gated, adopt) a corrected
   default serving temperature per local backend.

Fail-open, budget-bounded, default-off, promotion-gated — the same discipline as
every other intelligence-surface pass.

## §0 What already exists (and what this reuses)

- **A provider-neutral streaming relay.** Backend SSE is parsed into a neutral
  delta model (`openai_chunk_to_deltas` → `aimee_delta_t`, `src/headers/
  aimee_ir_stream.h`) and re-rendered to the caller's stream (`anthropic_delta_
  emit`) on the live relay (`src/server/aimee_ir_serve.c`, `src/server/
  anthropic_http.c`). Every served text token already flows through one typed
  choke point (`AIMEE_DELTA_BLOCK_DELTA` with `text_delta`). **That is the tap
  point** — the detector needs no new plumbing into providers.
- **A conservative deterministic-detector precedent.** `slop_detect.c` is an
  existing "advisory, re-entrant, no global state, <1ms, false-positive-averse"
  scanner. The collapse detector follows the same contract: pure function over a
  rolling text window, no allocation on the hot path, tuned to almost never fire
  on legitimate output.
- **A guardrail orchestrator + config-flag pattern.** `guardrails_orchestrator.c`
  and the `*_enabled` config fields (`config_fields.c`) give the on/off + mode
  scaffolding. Sampling params (`temperature`, `top_p`, `repetition_penalty`,
  `max_tokens`) and `stop` sequences already thread through `provider_client.c`.
- **Calibration + promotion rails.** The shadow → canary → default gate, the
  contextual-bandit / calibration machinery, and the WORM audit store already
  exist and are the intended home for the collapse-rate metric and the
  temperature recommendation — no new statistical substrate.

The pieces are present; nothing today watches the served stream for collapse or
acts on it.

## §1 The streaming collapse detector (Detect-Cluster)

A pure state machine fed each `text_delta` on the relay, holding a bounded rolling
window (a few KB, ring-buffered — never the whole generation). It reports a
`collapse_signal` with a severity and the byte offset of the **loop-start
boundary** (the first token of the first repeat).

Detection ships in two rungs, and **v1 implements only the first**:

- **Rung 1 — verbatim periodicity (v1, high confidence).** A trailing span of
  length ≥ `M` bytes (default 60) that repeats ≥ `N` times (default 4)
  back-to-back. This is the unambiguous signal and the well-known
  repeat-detection heuristic. It is what v1 acts on.
- **Rung 2 — near-verbatim / semantic repeat (later, gated).** Normalize the
  window (collapse whitespace, casefold, strip trailing punctuation) and detect a
  span that repeats with small edit distance — the "same idea, reworded" stage
  that precedes verbatim looping, which buys earlier intervention. This layer is
  **not enabled until it clears a measured precision bar** on the checked-in
  legitimate-repeat corpus (§4); it is deliberately deferred so v1's risk is
  bounded to the unambiguous case.
- **Boundary refinement.** On a hit, walk back to the first offset where the
  periodic span begins, so the intervention targets the single deciding position.

All thresholds (`M`, `N`, edit-distance ratio, window size, per-legit-structure
allowlist) are config fields with conservative defaults. Complexity is bounded:
detection is O(window) per delta with a fixed-size window, so worst-case per-delta
cost is constant regardless of generation length — asserted by a microbenchmark in
the acceptance criteria. The detector is advisory: it emits a signal; §2 decides
what to do.

## §2 Decode-time intervention (Constrain-Verify / Enforce) — with an honest streaming model

**The timing reality.** On a live relay, deltas are already emitted to the client
as they arrive. By the time a verbatim loop satisfies "≥ N repeats," those repeats
are *already on the wire* — the gateway cannot retract bytes it has sent. Any
honest design must state what it can and cannot undo. This proposal uses a
**bounded holdback buffer** and scopes its guarantees to it:

- The relay holds back the trailing `H` bytes of the stream (default a few
  hundred bytes / ~2 sentences) before emitting — a small, bounded latency add.
  Text older than the holdback is flushed continuously, so steady-state latency is
  just the holdback fill, not the whole turn.
- Detection runs on the window *including* the held-back tail. When the loop-start
  boundary falls inside the holdback (the common case, because a loop is detected
  within a few repeats of starting), the intervention **can** cut before those
  bytes reach the client.
- When the loop-start boundary is older than the holdback (a long low-severity
  ramp), the guarantee downgrades honestly to: **stop all *future* deltas** — the
  client keeps whatever already shipped, but the turn does not run on to
  `max_tokens`. This is still strictly budget-reducing; it is not retroactive.

Given a fired signal above the acting threshold, the relay applies **one bounded**
intervention (this is the online analogue of "fix the first looping position and
leave the rest alone," realized with standard request parameters only — no logit
access, no sampler internals):

- **Mode `truncate` (v1 default, always available).** Behavior depends on where
  the loop-start boundary sits relative to the holdback:
  - **Loop-start inside the holdback (common).** The looping tail has not shipped.
    Emit the coherent prefix — the held-back text up to the last sentence boundary
    (see the deterministic segmenter in §4) that precedes the loop-start — then
    discard the rest of the holdback and close the turn.
  - **Loop-start older than the holdback (long low-severity ramp).** The coherent
    prefix has already shipped; nothing is re-emitted. Contract: flush the
    held-back bytes up to the last sentence boundary still inside the holdback if
    one exists, otherwise flush nothing further, then close the turn. This is the
    honest future-only case.
  - **No sentence boundary available in range.** Flush nothing further and close
    immediately at the current emission point.

  Stop reason is a **pinned, deterministic** mapping (never "where apt"):

  | Truncation site | Anthropic Messages | OpenAI Chat/Responses |
  | --- | --- | --- |
  | Visible answer block | `max_tokens` | `length` |
  | Thinking/reasoning block | `end_turn` | `stop` |

  Rationale: a cut visible answer is genuinely incomplete, so it maps to the
  budget/length family clients already branch on; a cut hidden reasoning block is
  a clean turn end from the client's view. **Exception:** if collapse is caught in
  a thinking block *before any visible answer block has started*, the turn yields
  no answer content, so it uses the visible-answer (length/`max_tokens`) mapping
  instead — an incomplete-turn signal is more honest to the client than
  `end_turn`-with-no-answer. The *fact* that a collapse (not a real budget hit)
  caused it is exposed only via Aimee-owned channels — an audit
  `collapse_event`, a response-metadata field, and an `x-aimee-stop-detail:
  repetition_collapse` header — so no client enum breaks.
- **Mode `resample` (later rung, backends with mid-turn continuation).**
  Re-request the remainder from the coherent prefix using **only standard request
  parameters** (sampling parameters + stop sequences — never logit access): raise
  `temperature` toward the calibrated post-fix optimum (§3), raise
  `repetition_penalty` / presence-penalty and apply a `min_p` floor where the
  backend's sampling API exposes them (all three are standard, provider-neutral
  sampling *parameters*, not logit manipulation), and set the detected loop-start
  span as a `stop` / `stop_sequences` value so the continuation cannot re-enter the
  same span. Backends that expose none of the penalties still get temperature +
  stop sequence. **Budget contract:** resample-issued tokens count against the
  caller's *original* `max_tokens` and billing — the guardrail spends the caller's
  remaining budget, never additional tokens beyond it. **Continuation mechanism:**
  where a backend exposes a native continuation/prefix API, resume from it; where
  it does not, the coherent prefix is re-sent as context and that prefix
  re-processing is **input-token cost the caller bears** (documented up front so
  there is no silent inflation) — a further reason resample stays a deferred,
  gated rung behind `truncate`. Escalation is **bounded**: at most `K` re-requests
  (default 1–2) and a hard cap on extra tokens (itself inside the remaining
  budget), then fall through to `truncate`.
- **Mode `observe` (shadow).** Detect + record only; forward the stream
  byte-for-byte unchanged (the holdback still applies so latency is measured too).

Guarantees: fail-open (any detector/holdback/intervention error → flush the
holdback and forward unchanged, log it), idempotent per turn, and — beyond the
bounded `K`/extra-token cap for `resample` — strictly output-budget *reducing*.

## §3 Per-backend temperature calibration (Calibrate / Evaluate-Optimize)

Every served turn records a compact `collapse_event` to the WORM audit store:
backend/model id, requested temperature, prompt class, context length, whether a
collapse fired, severity, which mode acted, and the **counterfactual tokens-saved
baseline**. Defined precisely against the holdback so already-shipped bytes are
never counted as "saved": tokens-saved = (looping tokens still in the held-back
tail at detection) + (tokens the model went on to emit *after* the detection point
until the collapsed generation actually ended). In `observe` mode the second term
is directly observed (we let the collapse run, so its full tail is visible); in
acting modes it is the tail we prevented. Tokens emitted before the holdback are
excluded. Because the un-acted generation is fully visible in `observe` mode, this
counterfactual is measurable there without any intervention. In acting modes the
prevented tail is by definition not observed, so realized savings report the
first term exactly (the dropped held-back tail) and *estimate* the second term
from the per-backend / per-prompt-class observe-mode distribution — the estimate
is labeled as such and only the observe-mode measurement feeds the temperature
fit, keeping the calibration loop grounded in observed data.

This operationalizes the most portable empirical finding about this failure:
**local reasoning checkpoints collapse mostly at low temperature, and once
collapse is removed the best serving temperature shifts** — a model tuned to run
cold may serve better slightly warmer once the loop is no longer the dominant
failure. Two payoffs:

- **Observability.** A per-backend collapse-rate + tokens-wasted panel — "which
  local checkpoint loops, at which temperature, how often, how much it costs."
  Purely additive.
- **Recommended default temperature.** Over enough events, fit collapse-rate vs.
  temperature per backend, **stratified by prompt class and context length** (so a
  recommendation is not confounded by traffic mix), and recommend a serving
  temperature that minimizes collapse without over-warming. Surfaced as a
  recommendation; adoption is **gated** (shadow → canary → default) exactly like
  other calibrated defaults, never auto-flipped silently.

## §4 Safety, precision, and honest gates

- **False positives are the whole risk.** Legitimate output *does* repeat:
  markdown tables, enumerated lists, ASCII art, repeated code boilerplate, JSON
  arrays. The detector treats these as first-class allowlist cases
  (structural-repeat suppression: repeats separated by list/table/code delimiters,
  or inside a fenced block, do not count). A checked-in corpus of legitimate
  repeats is the precision fixture; v1 ships `observe` first and must measure the
  false-positive rate on real traffic before any mode acts.
- **A ground-truth labeling pipeline (resolves the "observe can't measure
  precision" gap).** Detections are labeled real-collapse vs. false-positive two
  ways: (1) the checked-in corpus gives deterministic labels for regression, and
  (2) a sample of *live* `observe`-mode detections is scored by an LLM-judge with
  spot human review, yielding a precision estimate with a confidence interval.
  `observe` therefore produces both a **precision** number (from labeling) and a
  **counterfactual tokens-saved** number (from §3) — the two quantities the gate
  needs — without acting on traffic.
- **Thinking vs. answer blocks.** Collapse in a hidden reasoning/thinking block is
  the common case and the safest to intervene on; collapse in the visible answer
  is rarer and higher-stakes. The acting threshold is configurable per block kind
  (the delta model already distinguishes text vs. thinking).
- **Promotion discipline (numeric).** Each rung must clear a measured bar from the
  audit store before the next turns on, all default-off until cleared:
  - **Shadow (`observe`, verbatim only):** detector precision ≥ 0.98 on the
    checked-in corpus **and** ≥ 0.95 (lower CI bound) on sampled live detections;
    measured counterfactual tokens-saved > 0 with a per-delta p99 latency within
    the microbenchmark budget.
  - **Canary (`truncate`, verbatim only):** measured over a fixed window capped at
    **500 acted turns or 7 days, whichever is reached first**, and requiring a
    minimum of **200 acted turns** so a low-traffic backend cannot clear on thin
    data. Three deterministic metrics: (a) the shadow gate's detection precision
    ≥ 0.95 (lower CI bound) carried forward onto canary traffic; (b)
    *coherent-prefix preservation* ≥ 0.99, where "coherent prefix" is defined
    deterministically — the emitted prefix ends at a boundary matched by the fixed
    sentence-boundary rule (terminal `.!?` / newline / list-or-fence delimiter, the
    same segmenter §2 uses) and contains no span from the detected loop; and (c)
    **zero** confirmed false truncations of legitimate output (a truncation whose
    detection the labeling pipeline marks a false positive) over the window.
  - **Default (`truncate` verbatim):** the canary metrics sustained across a
    second window of the same size.
  - **Semantic rung (`observe`→act) and `resample`:** each repeats the same
    shadow→canary→default ladder on its own, gated on the semantic layer's
    measured precision meeting the same ≥ 0.98 / ≥ 0.95 bars first.
- **Scope honesty (explicit non-goal / deferred).** The deepest fix is at
  *training* time — generating single-position preference data at the loop
  boundary and fine-tuning a small adapter so the model stops choosing the
  loop-start token at all. That belongs to whoever owns the local checkpoints'
  training pipeline, **not to this C gateway repo**, and is out of scope here.
  This proposal is the inference-time, model-agnostic guardrail that helps *every*
  backend today, including third-party ones we will never retrain. The exact
  `collapse_event` boundaries this guardrail records are the natural seed for such
  an offline generator later — a separate proposal.

## Non-goals

- Not changing any provider's sampler internals and not requiring token-logit
  access — every intervention uses standard request fields (`temperature`,
  `repetition_penalty`/presence-penalty and `min_p` where exposed, and
  `stop`/`stop_sequences`; all are provider-neutral sampling *parameters*, not
  logit manipulation) over the streamed text the gateway already sees.
- Not retracting bytes already sent to the client beyond the bounded holdback
  window (§2 is explicit about this limit).
- Not inventing a non-standard `stop_reason` enum value on the wire — the client
  sees a standard stop reason; the collapse detail rides Aimee-owned metadata.
- Not retraining, fine-tuning, or shipping model weights (see §4 scope honesty).
- Not auto-adopting a new default serving temperature without the same promotion
  gate as every other calibrated default.
- Not a general output-quality filter — this targets one specific, well-defined
  failure (degenerate repetition collapse), not "slop," verbosity, or correctness.

## Acceptance criteria

1. A pure, re-entrant verbatim-collapse detector (`repetition_collapse_*`) with
   unit tests over fixtures: verbatim loops (fires), and legitimate repeats —
   tables, lists, code, JSON — (does **not** fire). Checked-in
   legitimate-repeat corpus with a measured precision ≥ 0.98. Semantic rung is
   scaffolded but disabled.
2. A per-delta latency microbenchmark asserting worst-case (full window) cost is
   within a fixed budget (target p99 well under 1ms) and independent of total
   generation length.
3. The detector + bounded holdback buffer tapped into the live IR-delta relay
   behind a default-off config flag, with an `observe` mode that records
   `collapse_event`s (including the counterfactual tokens-saved baseline) to the
   audit store and forwards the stream byte-for-byte unchanged — proven by a relay
   test asserting output equality in `observe` mode.
4. `truncate` implemented: handles both the inside-holdback and older-than-holdback
   cases per §2, emits the coherent prefix, closes with the **pinned** stop-reason
   mapping (§2 table) plus an Aimee-owned collapse detail (audit field + response
   metadata + `x-aimee-stop-detail` header); fail-open; tested on a synthetic
   looping backend to show served tokens strictly drop and a coherent prefix is
   preserved. `resample` implemented behind its own flag using only standard
   sampling params (temperature + penalties/`min_p`-where-exposed + `stop`
   sequence), spending only the caller's remaining `max_tokens` budget, bounded by
   `K` and an extra-token cap.
5. Config fields (thresholds, mode, holdback size, per-block-kind acting
   threshold, `K`/extra-token bounds) parse through config and appear in generated
   configuration docs.
6. A per-backend collapse-rate + tokens-saved metric readable from the audit
   surface, stratified by prompt class/context length, and a documented (not yet
   default-on) recommended-temperature calibration derived from it.
7. Promotion gate documented with the numeric shadow → canary → default thresholds
   in §4, plus the ground-truth labeling pipeline (checked-in corpus + sampled
   live LLM-judge/human review) that produces the precision numbers.
