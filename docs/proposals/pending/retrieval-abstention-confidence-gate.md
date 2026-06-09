# Proposal: Teach recall to say "I don't know" - a calibrated abstention gate

- **State:** draft - pending review
- **Author:** JBailes
- **Date:** 2026-06-09
- **Charter roles:** Recall (gate placement on the answer path), Calibrate /
  Evaluate-Optimize (per-triple threshold tuning + abstain/false-omission A/B),
  Gate-Promote (curated-content exemption), Extract (curated/provenance signal
  the exemption keys on).
- **Scope:** `src/memory_core_search.inc` (`memory_ask_query_scoped`,
  `memory_answer_confidence`, the existing `no_answer` triggers), `src/memory_context.c`
  (`memory_retrieval_confidence` coverage/separation), `src/memory_assemble.c`
  (failure-detection wiring + LOW-marker injection), `src/headers/memory.h`
  (`memory_answer_result_t`), config plumbing (`src/headers/config.h`,
  `src/config.c`, `src/config_fields.c`, `src/config_sections.c`),
  `src/server/server_mcp.c` (no-answer rendering), `src/server/kb_client_memory.c`
  + `src/db2/kb_service_backend_memory.c` + `src/kb/kb_service_memory.c`
  (the `memory.ask` response contract), the `kb_calibrate` loop
  (`src/kb_calibrate.h`) for per-triple thresholds, unit + integration tests, an
  abstain/false-omission bench. No new service, no new model.

## Goal

Make recall **refuse to answer when the retrieved evidence does not support a
confident answer**, as a deterministic retrieval decision rather than a prompt
instruction. Today Aimee retrieves well and *reports* a confidence number, but
it only abstains on **structural** failure (nothing retrieved, no extractable
answer, no citations). When retrieval succeeds but the hits are weak, the answer
path still returns the weak **extracted** snippet and merely attaches a
low-confidence note; the context path injects a soft "## Retrieval Confidence:
LOW" marker and asks the answering LLM to abstain *for itself*. That delegates
the most important decision - "is there an answer here at all?" - to a prompt
instruction, which is exactly the failure mode this proposal removes. (The
`memory.ask` answer path is **extractive** and pays no generation cost — §R2.1;
the gate's basis is correctness, and **§A** revises *where* the decision lives.)

The change is small because the substrate is already built. The abstention
*slot*, *schema*, and *rendering* exist end-to-end; only the **decision** is
missing. This proposal wires a deterministic answerability decision into recall
(gating on the **grounding** signal — §A.2), adds the chunk-filter / answer-gate
split the literature calls for, exempts curated content, and lets the calibration
loop tune it from outcomes instead of a hand-picked constant. **§A revises the
single-path framing in the sections below per the §R / §R2 review findings.**

## §0 What already exists (so we don't rebuild it)

The expensive parts are done. Confirmed in the tree:

- **Hybrid retrieval with rank fusion** is already in place and richer than a
  two-list RRF: `memory_rrf_bonus` (`src/memory_core_search.inc:2390`, `rank_k =
  20`) fuses lexical, semantic, entity, temporal, evidence, and source rank
  signals, on top of a learned linear ranker and optional cross-encoder rerank
  (`src/kb_ranker.h`, `memory_core_search.inc:2464`). We do **not** need to add
  RRF or hybrid search.
- **A retrieval-confidence score already exists.** `memory_retrieval_confidence`
  (`src/memory_context.c:660`) computes `coverage` (fraction of significant query
  terms found across the top-K hits) and `separation` (normalized gap between the
  top-1 and third-place scores), blends them `0.6 * coverage + 0.4 * separation`,
  and sets `below_threshold` against a configurable threshold (default `0.35`).
  This is the blog's "avg_confidence", already implemented.
- **A second, answer-path confidence already exists.** `memory_answer_confidence`
  (`src/memory_core_search.inc:4675`) blends cluster score, anchor rank, support
  count, and citation presence into a 0..1 value. It is **computed and returned
  on every `memory.ask`** (lines 4868 / 4894 / 4899) - and **never used as a
  gate.** It only flows out as a number.
- **The abstention slot is wired end-to-end.** `memory_answer_result_t` already
  carries `no_answer`, `low_confidence`, and `confidence`
  (`src/headers/memory.h:108`). The MCP surface already renders it: when
  `no_answer` is set, `server_mcp.c:386` returns `No confident answer for "<q>"`.
  The RPC already serializes it (`db2/kb_service_backend_memory.c:1864`) and the
  client already parses it (`server/kb_client_memory.c:1363`, reading
  `no_answer` / `low_confidence` / `confidence` / `citation_ids`). Nothing new is
  needed in the schema or the rendering to express "I don't know."
- **`no_answer` is already triggered - but only structurally.** In
  `memory_ask_query_scoped` it fires on: zero retrieval (`:4796`), no extractable
  answer from the cluster (`:4809`), and citations-required + strip-unverified +
  zero citations (`:4879`). None of these is "we retrieved plausible-looking hits
  but they're weak." That case still returns a synthesized answer.
- **Failure detection + recovery already exists on the context path.**
  `memory_failure_detection_enabled` / `memory_failure_detection_threshold`
  (`src/headers/config.h`, default **off**, threshold `0.35`) drive a
  wider re-fetch and, if confidence stays low, inject the soft LOW marker into
  the assembled context (`memory_assemble.c:811`, marker at `:1042`). This is the
  "warn the LLM" path; it is the right *recovery* (re-fetch wider) bolted to the
  wrong *terminal action* (advise instead of abstain).
- **A calibration loop already exists.** `kb_calibrate_run`
  (`src/kb_calibrate.h`) tunes promotion thresholds per `(target_surface, kind,
  scope)` triple from audit outcomes, writing `calibration_profile` artifacts.
  The abstain gate is one more threshold this loop can own - we do not need a new
  calibration mechanism.
- **A scope/tier lattice and curator provenance already exist**, which is all the
  curated-content exemption needs to key on (no new "curated" flag required if an
  existing tier/provenance signal already distinguishes hand-authored records).

So this proposal is **a gate, a threshold split, an exemption, and a calibration
hook** - not new retrieval, not a new schema, not a new service.

## A note on architecture: gate the answer, don't instruct the model

The blog's central claim is *separation of concerns*: deciding whether evidence
supports an answer is a **retrieval** decision, made deterministically before
generation, not a behavior the answering model is asked to perform. Aimee already
agrees with this in one place and contradicts it in another:

- It **agrees** that confidence is not a relevance score - confidence is
  populated post-ranking as a calibration artifact, explicitly *not* fed back
  into ranking (`memory_core_search.inc:2014-2015`). Good.
- It **contradicts** it at the terminal step - when confidence is low it asks the
  LLM to abstain via injected prose (`memory_assemble.c:1042`) instead of
  refusing. That is the prompt-delegated abstention the blog argues against.

This proposal keeps the blend exactly as-is (we still compute the full ranked
result - no gating of *retrieval compute*) and changes only the **terminal
action** from "return + warn" to a deterministic abstain decision. **Revised
stance (see §A):** that decision belongs on a single shared seam consumed by
*both* paths — the answer path *refuses* (`no_answer`) and the context path
*withholds* weak evidence instead of injecting the soft LOW marker — because the
context path is where generation cost and the prompt-delegated anti-pattern
actually live (§R2.2). The original "gate the answer path, keep the LOW marker on
the context path" split is **superseded by §A**.

## §A Revised architecture (incorporating the §R / §R2 findings)

The review passes (§R, §R2) showed the original single-path framing above is
structurally wrong on two points and should be corrected **before**
implementation:

1. The `memory.ask` answer path is **extractive** — it pays **no generation
   cost**, and the answer is composed (`memory_pick_answer_from_cluster`) *before*
   confidence is computed — so "a deterministic gate before generation cost"
   cannot apply there (§R2.1). The generation cost lives on the **context path**
   (assembled context → main agent LLM), which is exactly the path the original
   design leaves on the prompt-delegated LOW marker (§R2.2).
2. `memory_answer_confidence` is dominated by rank position and citation presence,
   not grounding strength, so it is a weak discriminator for the
   "retrieved-but-weak" case the proposal targets (§R.2.6); the grounding signal
   (coverage/separation + citation verification) lives in a different function on
   a different path (§R.3.9).

The corrected architecture keeps the proposal's spirit — reuse the `no_answer`
contract, no new retrieval, no new service — but **moves the decision to a shared
seam and feeds it the right signal.**

### §A.1 One answerability evaluator, two terminal actions

Introduce a single, path-agnostic predicate — working name `memory_answerable()` —
computed once from the **final candidate set + query terms + thresholds**, that
returns a decision (`answerable | abstain | exempt`) plus the scores behind it. It
is a **pure function** (no I/O, no counters). Both consumers call it and own their
terminal action:

- **Answer path** (`memory_ask_query_scoped`): on `abstain` → set `no_answer = 1`,
  clear `answer`, zero citations; the **caller** increments
  `memory.answer.abstained`. Reuses the existing end-to-end contract; the
  justification is **correctness**, not cost (§R2.1).
- **Context path** (`memory_assemble`): on `abstain` → **deterministically
  withhold** the weak evidence (and/or emit one machine-unambiguous "no reliable
  memory for X" sentinel) **instead of** injecting the soft `## Retrieval
  Confidence: LOW` prose and asking the model to self-abstain. This is where the
  blog's separation-of-concerns actually bites and where generation cost is saved
  (§R2.2). The *decision* to withhold is made deterministically; what reaches the
  LLM is either trustworthy context or an explicit, non-negotiable
  "insufficient memory" signal — never weak content plus a soft plea. **This
  supersedes the original "keep the LOW marker" non-goal.**

This removes the duplication the tree carries today (coverage/separation on one
path, answer-confidence on the other, neither gating) and makes "I don't know"
**one decision with two renderings.**

### §A.2 Grounding-signal-first (which number the gate reads)

The evaluator's primary input is the **grounding** signal —
`memory_retrieval_confidence`'s coverage/separation — plus **citation
verification**, *not* `memory_answer_confidence` (cluster shape).
`memory_answer_confidence` may serve only as a secondary tiebreak. Concretely
this is the wiring §R.3 flagged as understated, now on the **critical path**, not
"optional Phase 2":

- lift `memory_retrieval_confidence` so it runs on the answer path too (it is
  context-path-only today, §R.3.9);
- make the per-candidate **retrieval score survive** to the evaluator (it is
  discarded today — `out[i] = candidates[i]` drops the ranking score, §R.3.8), and
  build the significant-query-terms array the answer path does not compute today.

This score-survival + term plumbing is the real foundational work and is promoted
to **P0** (§A.6).

### §A.3 Determinism boundary (make the gate reproducible)

A threshold decision near a boundary must be reproducible, but the cross-encoder
rerank is an external subprocess with silent fallback (§R2.4), so the *reranked
order* is not stable. Resolve this explicitly: compute the evaluator's grounding
signal from the **deterministic** hybrid/coverage signals, **not** the
subprocess-reranked order, so the abstain/answer decision is reproducible
run-to-run and host-to-host even when the cross-encoder is absent or times out.
The reranker still orders what is *shown*; it does not decide *whether to answer*.
Document this contract.

### §A.4 Side-effect boundary

The evaluator is **pure**; the **caller** records the outcome counter. This
resolves the §R2.4 contradiction ("side-effect-free" vs. a mandated counter): the
abstain decision is side-effect-free; observability is a deliberate, separate
write owned by each path.

### §A.5 Completeness is explicitly out of scope

The evaluator answers "is there *a* supported answer," not "is the answer
*complete*." Even with the newer query-shape enum and default-off aggregation
route, `memory.ask` still renders through the same single-scalar extractor
(§R3.1), and confidence gating cannot detect that failure (those queries score
*high*). Two consequences: (a) the proposal must not be sold as covering
completeness; (b) the evaluator must **not abstain** on a high-grounding
multi-record query merely because the extractor returned one value — a
completeness limitation must not masquerade as low confidence. Cardinality-aware
answer rendering is a separate, later capability.

### §A.6 Corrected phase plan (authoritative; supersedes "Phasing & ordering")

| Phase | Change | Note |
|------|--------|------|
| **P0** (new, foundational) | Extract the **pure** `memory_answerable()` evaluator; make per-candidate scores survive to the answer path; lift coverage/separation onto the answer path; compute it from the **deterministic** hybrid signal (§A.2/§A.3); evaluate the **answer anchor/cluster**, not just the top-K list (§R3.2); keep enough candidate evidence for logging/bench labels (§R4.2). | The real load-bearing refactor §R said Phase 2 hides. Nothing user-visible. |
| **P1** | Answer-path **refuse** via the evaluator (grounding-first), reusing `no_answer`; caller-counted; update string-only helper callers so abstention is not silently rendered as an empty answer (§R4.1). | Was "Phase 1"; now depends on P0. Correctness basis, not cost. |
| **P2** | Context-path **withhold** — replace the prompt-delegated LOW marker with the deterministic decision (§A.1). | Promoted from non-goal; the slice that actually saves generation and removes the anti-pattern. |
| **P3** | Curated exemption keyed on the **anchor** record's tier (L4/L5) — not "any cited record" (§R.13, §R2.7). | |
| **P4** | Per-triple calibration — **gated on first building the ask-outcome feedback signal**, which does not exist today (§R.5.14). Net-new infra, not a free hook. | |
| **Bench** | Build the abstain/false-omission harness (labeled answerable/unanswerable corpus, ask-path runner, false-omission metric, per-query confidence export, gate A/B) **before** any default-on (§R2.5). | Precedes the default-on decision. |

Config (`memory_abstain_enabled` / `memory_abstain_gate` / chunk floor) must be
**CLI-settable** — added to `config_fields.c`, which the failure-detection
precedent skips (§R.1.2) — with **effective (use-site) defaults**, since the
stored default is `memset`-zero (§R.1.3).

## Implementation contract

The decision must be concrete before any default-on discussion **(this contract
now follows the revised architecture in §A):**

- The gated signal is the **grounding** score (`memory_retrieval_confidence`
  coverage/separation + citation verification), **not** `memory_answer_confidence`,
  which is position/citation-driven and a weak discriminator (§A.2, §R.2.6).
  `memory_answer_confidence` may serve only as a secondary tiebreak; we do not
  invent a third confidence number.
- The evaluator is a **pure function** of (final candidates, query terms,
  thresholds) and computes the grounding signal from the **deterministic** hybrid
  signal, not the cross-encoder-reranked order, so the decision is reproducible
  (§A.3). The outcome counter is written by the **caller**, not the evaluator
  (§A.4). Note `memory.ask` is extractive — there is no generation cost to gate
  "before" (§R2.1); the basis is correctness.
- On abstain, the result is the **existing** shape: `no_answer = 1`, `answer`
  empty, `confidence` set to the (low) computed value, citations empty. No new
  field, no new render path - `server_mcp.c:386` already says the right thing.
- String-only helper paths (`memory_answer_query*`) and any UI/CLI text path that
  does not inspect `memory_answer_result_t.no_answer` must render an explicit
  abstention instead of treating empty `answer` as a legitimate empty result
  (§R.1.4, §R4.1).
- Abstention is **counted**: increment a `memory.answer.abstained` runtime
  counter (mirroring the existing `memory.citation.*` counters at
  `memory_core_search.inc:4831+`) so the calibration loop and bench can see the
  abstain rate without log scraping. **No silent abstention.**
- The gate is **configurable and off by default** until the bench clears it,
  following the established off → on rollout discipline used elsewhere in the
  tree.
- Curated/exempt records **bypass the gate**: a hand-authored answer is trusted
  evidence by construction and must not be refused for failing a similarity bar.

## The gaps and the proposed changes

> **Read against §A.** The per-change detail below is still useful, but the
> *architecture and ordering* are revised by §A: the gate is a shared pure
> evaluator (§A.1) keyed on the **grounding** signal (§A.2), the score-survival /
> grounding-on-the-answer-path wiring is foundational **P0** (§A.6), and the
> context-path change is a deterministic **withhold**, not the LOW marker (§A.1).
> Where the text below says "gate `memory_answer_confidence`" or "Phase 1 is
> shippable alone," prefer §A.

### Phase 1 - The answer-path gate (smallest, highest leverage)

`memory_ask_query_scoped` (`src/memory_core_search.inc:4761`) computes
`memory_answer_confidence` and returns it, but never refuses based on it.

- Add `memory_abstain_gate` (double, **default e.g. `0.40`**, range `0.0`–`1.0`;
  `0.0` disables) and `memory_abstain_enabled` (int, **default 0**) via the same
  config plumbing as the failure-detection knobs (`config.h` / `config.c` /
  `config_fields.c` / `config_sections.c`), placed beside
  `memory_failure_detection_*`.
- After confidence is computed and **before** the success returns (`:4868` /
  `:4899`), if `memory_abstain_enabled` and `confidence < memory_abstain_gate`
  and the answer is **not** curated-exempt (Phase 3): set `out->no_answer = 1`,
  clear `out->answer`, leave `out->confidence` as the computed low value, zero
  citations, increment `memory.answer.abstained`, and return `0`.
- This reuses the existing `no_answer` contract entirely. `server_mcp.c:386`
  already renders it as "No confident answer for …"; the CLI / webchat already
  understand `no_answer`. Nothing downstream changes.

Isolated to the ask path plus two config fields. The context/proactive-recall
path is untouched in Phase 1.

### Phase 2 - Two-tier thresholds (chunk filter + answer gate)

The literature (and the blog) separate two decisions that Aimee currently runs
through one `0.35` knob: which **chunks** are good enough to enter the context,
and whether the **answer** is supported at all.

- Keep the answer gate from Phase 1 (`memory_abstain_gate`).
- Add a **chunk-confidence floor** (`memory_chunk_min_confidence`, default low,
  e.g. `0.25`) applied where candidates are assembled into the answer cluster, so
  weak hits are dropped from the evidence set *before* `memory_answer_confidence`
  scores it, rather than diluting it. This is a filter on the candidate list, not
  a change to fetch budget (`memory_fetch_budget_*` stays as-is - it controls how
  many we *fetch*; this controls which fetched hits are *trusted as evidence*).
- The two thresholds are independent: a query can clear the chunk floor for
  several hits and still fall below the answer gate (weak but non-empty evidence →
  abstain), or clear the answer gate on one strong curated hit while most chunks
  are filtered out.
- When failure detection is enabled, the answer gate consumes the
  **coverage/separation** score as a floor on the cluster confidence, so a
  high-cluster-but-zero-coverage answer (fluent but off-topic) still abstains.
  This is the one place the two existing confidence functions are combined, and
  it is a `min`, not a new blend.

### Phase 3 - Curated-content exemption

The blog exempts hand-curated answers from the gate; refusing a human-authored
record because it scored below a similarity bar is strictly wrong.

- Define "exempt" from an **existing** signal - high tier and/or curator
  provenance on the anchor record - rather than a new column, if the tree already
  distinguishes hand-authored records (confirm against the tier/provenance fields
  on `memory_t` before adding anything).
- In the Phase 1 gate, skip abstention when the answer's anchor (or any cited
  record) is exempt. The answer still carries its computed confidence; it is just
  not *refused*.
- If no existing signal cleanly identifies curated records, add a minimal
  provenance flag the curator already has the information to set - but prefer
  reusing tier/provenance to adding schema.

### Phase 4 - Calibrated, per-triple thresholds (close the loop)

A single global gate is a starting point, not the endpoint. Medical-grade
domains should abstain aggressively; a casual FAQ scope should not.

- Let `kb_calibrate_run` (`src/kb_calibrate.h`) emit an **abstain-threshold**
  alongside the promotion thresholds it already calibrates per `(surface, kind,
  scope)` triple, learned from audit outcomes (was the abstain correct? was a
  synthesized answer later marked wrong?).
- The ask path reads the calibrated per-triple threshold when a
  `calibration_profile` exists for the query's scope, and falls back to the
  global `memory_abstain_gate` otherwise - mirroring how promotion gates already
  consume calibration profiles by rollout mode.
- This is the blog's "log avg_confidence, hand-label, tune per domain" loop, but
  automated through machinery that already exists, rather than a manual
  spreadsheet pass.

### Bonus (near-free) - calibrated user-facing confidence bands

The blog maps raw scores into honest user-facing bands (no chunks: 5–10%;
retrieved-but-rejected: 20–50%; answered: 70–100%) so a "62%" never reads as
"pretty sure" when it was actually an abstain. Aimee already returns `confidence`
and `no_answer`; the renderer can band the number by outcome (abstain → low band,
answered-with-citations → high band) without changing what is computed. Cheap,
and it prevents the "confidently wrong percentage" failure the gate exists to
avoid.

## Phasing & ordering

> **Superseded by §A.6.** The original ordering below underestimated the
> foundational work (it folded the score-survival + grounding-on-the-answer-path
> refactor into "Phase 2" and treated the calibration loop as a free hook). The
> **authoritative** plan is the §A.6 table: P0 (extract the pure evaluator + make
> scores survive + grounding-on-the-answer-path), P1 (answer-path refuse), P2
> (context-path withhold), P3 (anchor-keyed exemption), P4 (calibration, gated on
> a net-new feedback signal), then the bench before default-on. The table below is
> retained only to show the original framing.

| Phase | Change | Blast radius | Risk |
|------|--------|-------------|------|
| 1 | Answer-path abstain gate | `memory_core_search.inc` + 2 config fields | low |
| 2 | Two-tier thresholds (chunk filter + answer gate) | candidate assembly + 1 config field | low/medium |
| 3 | Curated-content exemption | gate predicate + (maybe) provenance signal | low |
| 4 | Per-triple calibrated thresholds | `kb_calibrate` output + ask-path read | medium |

(Original framing — now corrected by §A: Phase 1 alone is largely cosmetic
because the gated number is position/citation-driven, not grounding; the
discriminating signal arrives with the grounding wiring now promoted to P0/§A.2.)

## Testing & validation

- **Unit:** gate fires when `confidence < gate` and answer is non-curated
  (`no_answer == 1`, empty answer, citations zeroed, counter incremented); gate
  does **not** fire at/above threshold; gate **never** fires for curated/exempt
  anchors; chunk floor drops weak hits before scoring; coverage/separation `min`
  floor abstains on the fluent-but-off-topic case; the existing structural
  `no_answer` cases (`:4796` / `:4809` / `:4879`) still behave identically;
  default-off means behavior is byte-identical to today.
- **Integration:** `memory.ask` round-trip through
  `kb_client_memory_ask` → RPC → `server_mcp` rendering returns "No confident
  answer for …" on a deliberately weak query, and a normal answer with citations
  on a well-supported one; a curated record answers even when its similarity is
  low.
- **Abstain/false-omission bench (the acceptance bar):** on a fixed query set
  with hand-labeled answerability, measure **(a)** hallucination rate on
  unanswerable queries (should drop toward zero as the gate engages) against
  **(b)** false-omission rate on answerable queries (must not rise materially).
  Plot the confidence distributions for correct vs incorrect answers and pick the
  default gate where the curves separate - exactly the blog's tuning method, run
  through the existing benchmark suite. The bar for flipping
  `memory_abstain_enabled` default-on is *materially lower wrong-answer rate at
  no material increase in false omissions*. Deploy/bench runs are user-gated.

## Non-goals / risks

- **Not** changing the blend to a retrieval gate - recall compute is unchanged;
  we gate the *answer*, not what we *compute* (§0, and consistent with the
  recall-economy proposal's "blend, don't gate" stance).
- ~~**Not** removing the soft LOW marker~~ **— superseded by §A.1.** On the
  context path the soft LOW marker is *replaced* by a deterministic
  withhold / insufficient-memory decision: keeping a prompt-delegated marker on
  the generation-bearing path is the very anti-pattern this proposal exists to
  remove (§R2.2). The decision is shared across both paths — the answer path
  refuses, the context path withholds.
- **Not** addressing answer **completeness** — list/count/"all of X" queries can
  still return a single extracted scalar at high confidence and are invisible to
  the gate (§A.5, §R2.3, §R3.1); that needs cardinality-aware answer rendering, a
  separate later capability. The evaluator must not let that limitation
  masquerade as low confidence.
- **Over-abstention** is the primary risk: too high a gate refuses answerable
  queries. Mitigated by default-off rollout, the false-omission arm of the bench,
  the curated exemption, and per-triple calibration so a chatty scope is not held
  to a clinical bar.
- **Double-counting confidence:** combining `memory_answer_confidence` with
  coverage/separation must be a `min` (a floor), not a sum, or strong-on-one-axis
  answers leak through. Specified as `min` for exactly this reason.
- **Curated exemption as an escape hatch:** if "curated" is too broad, the gate
  is toothless. Keep the exemption tied to genuine hand-authored/high-tier
  provenance, and count exempt-bypass separately so the bench can see it.
- **Threshold staleness:** a hand-picked global gate drifts as the corpus grows;
  Phase 4 hands it to the calibration loop so it tracks outcomes instead of
  ossifying at `0.40`.

## §R Review findings — independent verification pass (`testing@d3eb402d`)

A verification pass re-checked every cited symbol/line/behavior against the tree
and went one level deeper into each phase. The **core premise holds and is
load-bearing-true**: `memory_answer_confidence` is computed on every `memory.ask`
and appears in **zero** conditionals — it is only assigned and serialized, never
gated (verified across `memory_core_search.inc` and the four downstream files).
The end-to-end `no_answer` round-trip is real (engine `memory_ask_query_scoped`
→ serialize `db2/kb_service_backend_memory.c:1863-1869` → kb handler
`kb/kb_service_memory.c` → client parse `server/kb_client_memory.c:1396-1422` →
render `server_mcp.c:386`); both confidence functions are deterministic; and the
`memory.answer.abstained` counter is a clean one-line add via
`memory_runtime_state_increment` (`memory_core_helpers.inc:748`). So the
substrate claims in §0 are essentially accurate. The findings below are
corrections and gaps to fold into the phases before this leaves design review.

### §R.1 Premise corrections (claims that are inaccurate as written)

1. **"Aimee currently runs two decisions through one `0.35` knob" is false — the
   answer path has *no* threshold today.** `memory_answer_confidence` is assigned
   at `memory_core_search.inc:4868/4894/4899` and never compared to anything; the
   only retrieval threshold in the tree is the single **context-path** field
   `memory_failure_detection_threshold` (`config.h:510`, used only at
   `memory_assemble.c:807` and `memory_context.c:735`). There is no second answer
   threshold, and the two `0.35` values the proposal conflates are not "one knob
   doing double duty" — there is exactly one knob, on the context path only. So
   Phase 2 is **not "splitting one threshold into two"**; it is *adding two new
   gates where zero exist on the answer path* (`memory_abstain_gate` +
   `memory_chunk_min_confidence`). Keep the literature framing, but strike the
   "currently coupled / one knob" claim — it overstates how much is already wired.

2. **The config-plumbing precedent is 2 files + a use-site default, not 4 — and
   it is *not* CLI-settable.** `memory_failure_detection_*` is declared in
   `config.h:509-510` and parsed in `config_sections.c:321-330`, but it is **not**
   in `config.c` (no stored default; it relies on `memset`→0 plus an inline
   `> 0 ? cfg : 0.35` fallback) and **not** in `config_fields.c` — so it cannot be
   set via `aimee config set` and is absent from `config get/show`. The proposal's
   "same plumbing as the failure-detection knobs" therefore yields **non-settable**
   abstain knobs, which contradicts the stated off→on rollout discipline (you
   cannot flip `memory_abstain_enabled` from the CLI). To be rollout-controllable,
   the new fields must **also** be added to `config_fields.c` (where 44 other
   `memory_*` fields already live) — a step the cited precedent skips. State this
   explicitly in Phase 1.

3. **"Default `0.40`" is an effective (use-site) default, not a stored one.**
   Following the failure-detection pattern, an absent config key zero-inits the
   field (`config.c` `memset`), so `memory_abstain_gate` would be `0.0` — which the
   proposal itself defines as *disabled*. The stated `0.40`/`0.25` defaults must be
   applied at the use site (`x > 0 ? x : 0.40`), or "default-off" holds only via
   `memory_abstain_enabled`. Pin the stored-vs-effective default in the config
   step so the table's "default 0.40" is not misread as a stored value.

4. **"The CLI / webchat already understand `no_answer`" is false for the
   human-facing surfaces.** The MCP and `--json` paths handle it
   (`server_mcp.c:386`; the JSON emitter carries the bool), but the CLI **text**
   path prints the answer unconditionally — `printf("%s\n", result.answer);`
   (`cmd_memory_embed.c:465`) — so on abstain (empty `answer`) `aimee memory ask`
   prints a **blank line**, not "No confident answer." And the webchat frontend has
   **zero** references to `no_answer` (`grep` over `frontend/src` is empty).
   Honoring the proposal's own "no silent abstention" principle therefore requires
   a one-line `no_answer` branch in `cmd_memory_embed.c` and a frontend change —
   not "nothing downstream changes." Add both to Phase 1's scope (or drop the
   "already understands" claim).

### §R.2 Phase 1 (answer-path gate)

5. **There are three confidence-computing return sites, not two — and the contract
   omits the one that matters most.** The Implementation contract and Phase 1 body
   say "before the success returns (`:4868` / `:4899`)," but there is a **third**
   at `memory_core_search.inc:4894`: the citations-`required`, zero-citation,
   non-strip path, which sets `low_confidence = 1`, computes confidence, and
   returns. These are mutually-exclusive early returns, so there is no single
   "before the success returns" point — the gate must cover all three or hoist the
   confidence computation above the citation-mode branch at `:4866`. The `:4894`
   path is precisely the *weak / unverified* case the gate most wants to catch;
   leaving it ungated would be the main miss.

6. **Phase 1 alone under-targets the weak-hit case it exists for.**
   `memory_answer_confidence` (`:4675`) is
   `0.45·cluster + 0.20·rank + 0.20·support + 0.15·citation`, and
   `memory_answer_cluster_score` (`:4555`) is built from rank *position*
   (`1/(anchor+1) + Σ0.35/(i+1)` + variety bonuses), not match strength. A single
   moderately-ranked, cited hit computes to ≈ **0.625** (the existing test
   `test_memory_ask_query_returns_structured_result` asserts `confidence > 0.6` on
   one clean hit) — comfortably **above** the proposed `0.40` gate. So a
   weak-but-top-ranked hit (the "retrieved plausible-looking but weak" target of
   the Goal) sails through Phase 1. The real weak-hit signal is
   coverage/separation — i.e. **Phase 2** — which means Phase 1 is largely
   cosmetic on its own. Re-frame "Phase 1 is the high-value slice, shippable
   alone": it wires the contract, but the discriminating power arrives with Phase
   2.

7. **`low_confidence` is already set and already injects prose — the gate must
   reconcile, not "ignore" it.** The `:4886-4891` path prepends a
   `## Retrieval Confidence: LOW … unverified and may be inaccurate` block into
   `out->answer` and sets `low_confidence = 1`. That is the *same*
   prompt/user-delegated anti-pattern the proposal removes, living on the **answer**
   path (not only the context path the Goal cites). The new gate overlaps this
   exact case; specify whether the gate supersedes it (clear the LOW-prose answer
   and abstain) or leaves it, and reconcile the two `no_answer` producers'
   disagreement on `confidence` (the structural `:4879` sets `confidence = 0.0`;
   the proposed gate keeps the low computed value), since the renderer and the
   "confidence bands" bonus must handle both.

### §R.3 Phase 2 (two-tier thresholds) — the blast radius is understated

8. **There is no surviving per-candidate *match* score to floor on.** The
   ranking score (`memory_score_parts_t`, local to `memory_rerank_matches`) is
   discarded; the answer path receives bare `memory_t` (`out[i] = candidates[i]`,
   `:4299`). The only per-candidate number that survives is `memory_t.confidence`
   — the record's **intrinsic stored** confidence, *not* its match-to-query
   strength. So a "chunk-confidence floor" on the answer path either (a) filters on
   intrinsic record confidence — a different filter than the blog's chunk-relevance
   floor — or (b) requires threading the retrieval score out of ranking into a
   parallel array / new field. Either way it is materially more than "1 config
   field + a filter on the candidate list"; the "low/medium" blast radius is
   optimistic. State which signal the floor uses.

9. **The coverage/separation `min` floor requires running a context-path function
   on the answer path, with a struct and a token-list it does not have.**
   `memory_retrieval_confidence` is called *only* from `memory_assemble.c:812/860`;
   `memory_ask_query_scoped` never calls it. It takes `context_candidate_t` (which
   carries the `.score` separation needs), not `memory_t`, and it needs the
   tokenized **significant query terms** the answer path does not build
   (it uses `normalize_key` + intent). So the "one place the two functions are
   combined — a `min`, not a new blend" hides a struct bridge, a term tokenizer,
   and net-new compute on the ask hot path.

10. **coverage is brittle on short queries; separation is discontinuous on small
    candidate sets.** Significant terms require `strlen ≥ 3` and not a stopword,
    and the stopword list includes `who/what/when/where/why/which`
    (`memory_context.c:547-552,684`); a query like "who is X" collapses to one
    significant term, making `coverage ∈ {0.0, 1.0}` — and coverage carries 0.6 of
    the blend, so a single literal-substring miss (no morphology/synonyms) trips
    the floor → false abstention. `separation` is hard-coded to `0.5` for one
    candidate, uses the *last* of two for the "third-place" gap with two, and only
    reaches the intended 3rd-place gap at ≥3 (`memory_context.c:719-730`).
    Low-cardinality answer sets are common, so the floor is noisiest exactly where
    it bites. Quantify this in the false-omission arm of the bench.

11. **The cheaper recovery is thrown away on the answer path.** The wider re-fetch
    (`memory_assemble.c:815-864`) — which the proposal calls "the right recovery" —
    is context-path only. The answer-path gate abstains *without* attempting it, so
    a weak query that a broader fetch would rescue is refused instead, inflating
    the proposal's own false-omission metric. At minimum acknowledge that
    abstain-without-refetch forgoes a known cheaper win; better, offer the same
    broaden-then-recheck step on the answer path before abstaining.

12. **Scope the chunk floor to the answer path.** Candidate assembly is shared
    between the context path (`memory_assemble.c`) and the answer path; a floor
    placed "where candidates are assembled" would silently alter proactive-recall
    context unless gated strictly to `memory.ask`. Unspecified today.

### §R.4 Phase 3 (curated exemption) — feasible, with a sharpened signal choice

13. **Implementable today with no schema change** — `memory_t` carries `tier[4]`
    and `provenance_category[32]` (`memory.h:7,18`), and the anchor index is in
    hand at the gate point (`matches[anchor]`, `:4823`). **Prefer the tier signal**
    (`memory_tier_priority(tier) >= 4`, i.e. L4/L5 = the hand-authored/directive
    tiers): `provenance_category` is a coarse 3-value field whose
    "human-authored" value `user_stated` covers *any* user utterance, which is
    exactly the "exemption too broad → gate toothless" escape-hatch risk the
    proposal lists. Also note `memory_answer_confidence`'s `rank_norm`/`support_norm`
    are degenerate on a single curated hit, so on the exempt path the *predicate*
    (not the score) does all the work — its precision is correctness-critical, and
    the separate exempt-bypass counter the proposal already suggests is the right
    guard.

### §R.5 Phase 4 (calibrated per-triple thresholds) — the blocking gap

14. **The audit-outcome signal Phase 4 trains on does not exist.** `kb_calibrate`
    reads `audit_events.applied_confidence` + `verdict IN ('accepted','rejected')`
    (`db2/calibration.c:250-266`), and `audit_events` is written exclusively by the
    **promotion / curation review** path (`db2/artifacts.c:361`, verdicts like
    `thumbs_down`/`accepted`/`rejected`). Those are "should this memory have been
    *promoted*," **not** "was the *answer* correct." The ask path
    (`memory_ask_query_scoped`) records **zero** outcome/audit events, and the only
    ask-adjacent feedback (`retrieval_attribution` via
    `kb_handle_memory_record_retrieval_outcome`) is per-row and feeds **demotion**,
    not calibration. So Phase 4 is **not** "automated through machinery that already
    exists": it needs net-new (a) ask-outcome capture keyed by the triple, (b) a
    delayed correctness-labeling flow ("answer later marked wrong" — no such
    reviewer path exists for ask answers), and (c) a semantically distinct
    abstain-quantile fit. The existing
    `db2_calibration_threshold_from_profile_json` derives a *promotion-accept*
    threshold (confidence above which a curator accepts) — the **inverse** of what
    abstain needs (confidence below which an answer is likely wrong). Re-scope Phase
    4 to include building the feedback loop, and treat it as gated on that, not on
    the existing calibrator.

15. **The triple is a 4-tuple, and the cited precedent is global-only and
    batch-time.** Calibration keys on `(target_surface, kind, scope_kind, scope_id)`
    (`db2/calibration.h:50-56`), not a 3-tuple "scope." `memory_ask_query_scoped`
    has the scope (→ `scope_kind`/`scope_id`) and can read `kind` from
    `matches[anchor].kind`, so the key is reconstructable — but the precedent it
    "mirrors," `memory_promote` (`memory_logic.c:84-117`), reads a **global** profile
    (`db2_calibration_profile_read("memory", kind, "global", "")`, `:87`) inside a
    **batch maintenance** job, never a per-query-scope read on a latency path.
    Phase 4's per-ask profile read is a DB round-trip (up to 3 fallback queries)
    that the precedent never pays — budget caching, and resolve whether the abstain
    read piggybacks on the promotion-specific `calibration_enabled` rollout enum
    (`config.h:906-909`) or needs its own switch (likely the latter, since it must
    roll out independently of promotion calibration).

### §R.6 Test / CI traps not mentioned in "Testing & validation"

16. **The config-surface CI net must be hand-edited, and its generator is stale.**
    `test_config_surface.c` (run in the aggregate `unit-tests`) asserts A≠B for the
    full parsed-field set including `memory_failure_detection_*`. New `memory:`
    fields need fixture entries (two distinct in-range values) + an assertion or
    the net silently under-covers — **and re-running `tests/gen_config_surface.py`
    is wrong**: it scans only `src/config.c`, but the memory section parses live in
    `config_sections.c`, so regeneration would drop every `memory.*` field. The new
    fields must be hand-wired into the test (or fix the generator first). Name this
    as a required step.

17. **Docs regen.** If the fields are added to `config_fields.c` (per §R.1.2),
    `docs/gen/configuration.md` is regenerated from it via
    `scripts/gen-reference-docs.py` (`make -C src docs-gen`); no CI enforces it, so
    it will not break the build, but it is the documented step and should be in the
    phase plan.

### §R.7 Minor citation corrections (symbols stable; line/scope drift)

- `memory_ask_query_scoped` is at `memory_core_search.inc:4769`; `:4761` is the
  unscoped wrapper `memory_ask_query`. (Cited as `:4761` in the Scope and Phase 1
  headings.)
- The confidence return sites are `:4868 / :4894 / :4899` (three); the
  Implementation contract lists only `4868 / 4899`. See §R.2.5.
- `kb_calibrate_run`'s implementation is `src/kb/kb_calibrate.c:111` (the Scope
  cites only `kb_calibrate.h`).
- All other §0 citations (`memory_rrf_bonus:2390`, `memory_retrieval_confidence`
  formula and `0.35`, `memory_answer_confidence:4675`, the three structural
  `no_answer` triggers, the assemble marker at `:1042`, the end-to-end contract
  lines) re-confirmed accurate against `testing@d3eb402d`.

## §R2 Second review pass — deeper behavioral findings (`testing@d3eb402d`)

A second, more intense pass read `memory_ask_query_scoped` end-to-end and traced
the actual answer-composition, recovery, generation, determinism, and bench
machinery. It surfaced findings that change the framing of the value proposition
itself, plus one correction to §R.11. These are net-new beyond §R.

### §R2.1 The "before any generation cost" framing is wrong — `memory.ask` is fully extractive (and the gate runs *after* composition)

The Goal and the Implementation contract rest on "refuse … before any generation
cost is paid" and "the gate runs **before** any answer composition cost beyond
what retrieval already paid." Both are false for this path:

- **There is no generation cost.** `memory_ask_query_scoped`
  (`memory_core_search.inc:4769`) composes the answer purely **extractively** via
  `memory_pick_answer_from_cluster` (`:4703`, called at `:4806`), which `snprintf`s
  a stored record's `summary` or a 180-char `content` snippet into `out->answer`.
  There is **no** LLM/provider/`agent_run`/generation call anywhere in the ask
  path (grep is empty). The wrong-answer risk being mitigated is a *weak extracted
  snippet*, not a hallucinated generation. The proposal's word "synthesizes" (Goal)
  is inaccurate — it is extractive selection.
- **The answer is already composed before the gate.** Composition happens at
  `:4806`; confidence is not computed until `:4868 / :4894 / :4899`. So the gate as
  proposed sits **after** answer composition, not before it. "Runs before any
  answer composition cost" cannot be satisfied by the proposed placement (and there
  is no composition cost worth gating against anyway, since it is a `snprintf`).

**Consequence:** drop the cost framing from the Goal and the Implementation
contract. The gate's only sound justification is **correctness** — do not surface
an unsupported extracted snippet as if it were an answer. Rest Phase 1 on that,
not on cost.

### §R2.2 The cost rationale is not merely absent — it is inverted

Real generation cost lives on the **context / proactive-recall path**:
`memory_assemble.c` builds the context buffer (with the soft `## Retrieval
Confidence: LOW` marker appended at `:1042`) that is fed to the **main agent LLM**
downstream — that turn is where tokens are generated. The proposal's split puts
the **deterministic hard gate on the extractive (zero-generation) answer path**
and keeps the **soft prompt-delegated marker on the generation-bearing context
path** — exactly opposite to "a deterministic gate before generation cost." If
cost avoidance is a goal at all, the deterministic suppression belongs on the
context path (suppress feeding weak context into the expensive turn); as written,
Phase 1 saves no generation. (Note the context path is *also* not harmless to
keep "as-is": `memory_assemble.c:876` records retrieval failures / can auto-create
directives — a state mutation, not just a prose marker.)

### §R2.3 Completeness blind spot: list / count / "all of X" queries pass the gate while returning a single wrong scalar

This is the failure the gate is *most* likely to face on real recall, and it is
invisible to every axis the proposal gates on:

- The intent enum has **no list / enumerate / count / definition** value — only
  `MEM_QUERY_{GENERAL,TEMPORAL,ENTITY,PROCEDURAL}` (`memory_core.c:702-705`), and
  the classifier is substring matching.
- The extractor returns **exactly one** value: `memory_pick_answer_from_event`
  emits a single field (`event_time`/`actor`/`location`/`object`), and
  `memory_pick_answer_from_cluster` stops at the **first** non-empty buffer. A
  legitimately multi-record query ("what are all my projects", "who is on the
  team", "how many times did X happen") collapses to one record's one field.
- For such a query over a well-covered corpus, `memory_answer_confidence` is
  **high** (good cluster score, high `support_norm`, citations present) **and**
  coverage/separation are high (the terms *are* well covered). So **both** the
  Phase 1 gate **and** the Phase 2 `min`-floor pass the answer through. The answer
  is wrong-because-incomplete, which is orthogonal to confidence entirely.

The proposal's claim — "refuse when the retrieved evidence does not support a
confident answer" — should be scoped to exclude completeness, or explicitly note
that incompleteness needs a separate signal (e.g. an answer-cardinality/coverage
check), because confidence gating cannot catch it.

### §R2.4 "Deterministic and side-effect-free" is false on both counts

- **Side-effects.** The answer path already performs SQLite writes:
  `memory_runtime_state_increment` (`memory_core_helpers.inc:748`) →
  `db1_runtime_state_add_int` → `INSERT OR REPLACE INTO memory_runtime_state`
  (`db1/runtime_state.c:21`), fired for the `memory.citation.*` counters at
  `:4831/4835/4874/4877/4898`. The contract's own mandated `memory.answer.abstained`
  counter is another such write. So the Implementation contract simultaneously
  claims "side-effect-free" and **specifies a side effect**. Qualify it to "no
  side-effects beyond the existing observability counters."
- **Determinism.** The cross-encoder rerank is an **external subprocess** —
  `memory_cross_encoder_scores` → `platform_exec_pipe(command, …)`
  (`memory_core_search.inc:2504`) — which **silently falls back to hybrid order**
  on missing command / parse error / timeout. The resulting order drives
  `memory_answer_anchor_index`, which sets both the extracted snippet **and**
  `rank_norm = 1/(anchor+1)` feeding `memory_answer_confidence`. So a hard `0.40`
  threshold sitting near a decision boundary can flip abstain/answer
  **non-reproducibly** across runs/hosts/load. "Given the same candidates and
  thresholds it always produces the same decision" holds only under *stable
  candidate order*, which rerank can break. (Default-off stays byte-identical; this
  caveat is for default-on and, importantly, for the bench's distribution-tuning
  step, which sits on a shifting input.)

### §R2.5 The acceptance-bar bench is net-new infrastructure, not "the existing benchmark suite"

The "Testing & validation" section says to measure wrong-answer vs false-omission
rate on a hand-labeled answerability corpus and "run through the existing
benchmark suite." The existing eval does not support this:

- The eval has an **IR track** (`ir_mrr`/`ir_ndcg_at_k`/`ir_recall_at_k` over
  `expected_ids` *relevance* labels, `agent_eval.h:58-66`) and a **QA track**
  (gold-answer accuracy via an LLM judge, `agent_eval_memory_support.c:660-745`).
- **No corpus carries answerability labels** (answerable vs unanswerable) — the
  bench's core axis. Every corpus is answerable-by-construction.
- The QA harness **never calls `memory.ask` / `memory_ask_query_scoped`** (zero
  hits in `agent_eval*`); it scores an **LLM-synthesized** answer over assembled
  context — i.e. *not the extractive path the gate modifies*.
- `hallucination_rate` is literally `1.0 - accuracy` over answerable QA
  (`agent_eval_memory_support.c:764`); there is **no false-omission / abstention
  metric**, and "correctly abstained" is not a judgment the current judge renders
  (it grades candidate-vs-gold).
- Per-query **numeric confidence is not exported** (the trace only string-matches
  the `LOW` marker), so "plot the confidence distributions" needs new
  instrumentation, and there is **no per-run gate-toggle seam** wired into the
  harness.

So the acceptance bar requires building: a labeled answerable/unanswerable corpus,
an **ask-path** harness, abstention/false-omission metrics, per-query confidence
logging, a gate A/B config seam, and an abstention-aware judge. The "Deploy/bench
runs are user-gated" line is true but moot — there is no memory-answer-abstention
bench to gate-run yet. Add bench construction as an explicit work item, ahead of
any default-on decision.

### §R2.6 Correction to §R.11 + recovery/reprompt ordering

§R.11 said the cheaper re-fetch recovery is "context-path only"; that overstated
it. The answer path **does** have a recovery: a citations **reprompt-on-miss**
re-fetch (`memory_core_search.inc:4833-4864`, `cfg.memory_citations_reprompt_on_miss`),
which runs a second (unscoped) `memory_find_facts` and replaces
`matches`/`count`/`anchor` when required-mode citations are missing. Two
implications the gate must handle:

- The reprompt **swaps the cluster underneath the gate**, so the gate and the
  abstain counter must evaluate the **final post-reprompt cluster exactly once**
  (no double-fire across the original and reprompt clusters).
- It triggers **only** on citation-miss in `required` mode, not on low confidence,
  and (default mode is `off`, below) rarely fires — so the *spirit* of §R.11
  stands: there is no **confidence-driven** broaden-and-retry. A low-confidence
  query that a wider fetch could rescue still abstains. Offering a confidence-driven
  broaden step before abstaining remains the cheaper-win opportunity.

### §R2.7 Two more concrete gaps

- **Curated exemption is broader than its own guardrail (sharpens §R.13).** Phase 3
  exempts when "the anchor **or any cited record**" is curated, and
  `memory_collect_answer_citation_ids` gathers the whole answer cluster's ids — so
  **one** curated record anywhere in the cluster exempts the entire answer, even
  when the extracted snippet (the anchor) came from a **non-curated** record. Tie
  the exemption to the **anchor record** the snippet was extracted from, not "any
  cited record," or the escape hatch is wider than the "genuine hand-authored
  provenance" intent.
- **Default `citations_mode` is `off` (`config.c:408`), so the proposed gate is the
  *only* abstention trigger in the default deployment.** With citations off, the
  common path is `:4866-4869`; the structural citation `no_answer` (`:4879`) needs
  `required` + `strip`, which the default never enables. So only zero-retrieval and
  no-extract fire today, and the gate becomes the sole quality gate — keying on a
  confidence whose `citation_norm` contributes a `0.35` floor even with zero
  citations. This raises the calibration stakes and argues the default-`off`
  posture should not be permanent for default-mode deployments.
- **180-char content truncation is a confidence-invisible quality loss.** The
  generic content fallback in `memory_pick_answer_from_cluster` hard-truncates to
  180 chars (`"%.*s", 180`) although `out->answer` is `char[1024]` and
  `memory_t.content` is `char[2048]`. A truncated/mangled extract scores the same
  confidence (which never reads the answer text) and passes the gate. Independent
  of the gate, but it bounds what the gate/bands can ever catch; consider raising
  the cap or noting the limitation.

## §R3 Follow-up review — current-tree deltas and remaining design holes (`testing@d3eb402d` + PR head)

This pass re-read the proposal after §A and checked the current retrieval tree for
places where the review text had drifted or where the new shared-evaluator plan
still leaves implementation choices underspecified.

### §R3.1 Completeness is still out of scope, but the code citation changed

§R2.3 correctly identifies the completeness blind spot, but one supporting claim
is now stale: the tree **does** have query-shape classes for list and quantitative
queries (`MEM_SHAPE_LIST`, `MEM_SHAPE_QUANTITATIVE` in `src/headers/memory.h`) and
a default-off aggregation route in `memory_find_facts_scoped`
(`memory_core_search.inc:3963-3984`). That route detects shapes like "list all X"
and can return multiple `memory_t` rows before vector readiness.

The conclusion still holds for `memory.ask`: the returned rows still flow into
`memory_pick_answer_from_cluster`, which picks one anchor/event/summary/content
fallback and writes one string to `out->answer` (`memory_core_search.inc:4703+`).
So the proposal should phrase the limitation as "the ask renderer is scalar even
when aggregation retrieval is enabled," not "there is no list/count shape support."
Implementation should add regression cases for both modes:

- aggregation disabled: list/count queries must not be treated as fixed by the
  abstention gate;
- aggregation enabled: the gate must not turn a scalar-rendering limitation into
  an abstention decision, and a later cardinality-aware renderer must own the
  user-visible fix.

### §R3.2 Grounding must be anchor/cluster-specific, not only top-K-specific

§A says the evaluator consumes the "final candidate set + query terms +
thresholds." That is not precise enough for this answer path. The extracted
answer is keyed to `memory_answer_anchor_index`, which can choose a non-top-1
candidate based on cluster shape (`memory_core_search.inc:4588-4601`), while
`memory_retrieval_confidence` measures coverage across the top-K list and
separation between list positions 0 and 2 (`memory_context.c:676-736`).

That mismatch can pass the wrong thing: the top candidates can cover the query and
create a healthy coverage/separation score while the chosen anchor is a weaker
cluster member whose snippet is not grounded by those top hits. Conversely, a
strong anchor can be penalized by unrelated top-list separation. P0 should pass
the chosen `anchor_idx` into `memory_answerable()` and compute at least one
anchor/cluster-local grounding signal:

- anchor coverage: query-term coverage over `matches[anchor]`;
- cluster coverage/support: coverage over `memory_cluster_member(...)` rows only;
- deterministic anchor rank: the pre-CE hybrid rank/score of the anchor, not the
  mutable post-CE display order.

The top-K grounding score can remain a corpus-level floor, but it should not be
the only pass/fail signal for an answer extracted from one specific anchor.

### §R3.3 Define the boundary around session-window expansion and non-ranked rows

`memory_find_facts_scoped` reranks candidates, copies the first `reranked` rows to
`out`, and then may append conversational neighbours via
`memory_expand_to_session_window` (`memory_core_search.inc:4299-4313`). Those
neighbours are useful context, but they did not participate in the query ranking
and have no retrieval score. If the proposed evaluator runs on the "final
candidate set," it can accidentally treat window-neighbour rows as evidence for
answerability, inflate `support_norm`, or even shift the anchor chosen by
`memory_answer_anchor_index`.

Pin the contract before implementation:

- run `memory_answerable()` on the ranked retrieval slice before session-window
  expansion, or
- carry a `ranked_count` / `is_context_neighbor` side band so the evaluator and
  citation support exclude unranked neighbours.

Without that boundary, enabling `memory_window_radius` can change abstain/answer
decisions for reasons unrelated to evidence quality.

### §R3.4 Context-path withhold must preserve or replace retrieval-failure learning

§A.1 correctly replaces the soft LOW prose on the context path, but the current
LOW branch is not only presentation. When confidence remains below threshold,
`memory_assemble.c` calls `memory_directive_record_retrieval_failure`
(`memory_assemble.c:866-877`), which can create an epistemic directive after
repeated failures. A deterministic withhold that simply returns no context would
silently remove that learning/clarification loop.

The context-path phase should say whether withhold keeps the retrieval-failure
recording, changes the threshold/counter key, or introduces a new
`memory.answerability.withheld` signal that the directive subsystem consumes. The
important part is that the LLM no longer receives weak evidence plus a soft plea,
but the system still learns that the memory corpus lacked reliable evidence for
the query.

## §R4 Second follow-up review — remaining surface and telemetry traps (`testing@d3eb402d` + PR head)

This pass focused on paths outside the main `memory_ask_query_scoped` body: helper
APIs, CLI/MCP surfaces, dogfood telemetry, and alternate build modes. The core
architecture still holds, but these details decide whether abstention is visible,
measurable, and shippable across all supported binaries.

### §R4.1 `memory_answer_query*` string helpers turn `no_answer` into an empty string

The proposal now correctly says structured surfaces can carry `no_answer`, but
the tree still has public string helpers:

- `memory_answer_query()` → `memory_answer_query_scoped()`
  (`memory_core_search.inc:4756-4758`);
- `memory_answer_query_scoped()` calls `memory_ask_query_scoped()` and then
  returns either a cited answer string or `safe_strdup(result.answer)`
  (`memory_core_search.inc:4903-4912`);
- in the DB2-disabled implementation, the same helpers always return
  `memory_dup_cstr("")` (`memory_core.c:333-347`).

So after the proposed gate sets `no_answer = 1` and clears `answer`, direct helper
callers observe an **empty string**, not an explicit "I don't know." Tests still
exercise these helpers (`tests/test_memory_cases_a.inc`, `tests/test_memory.c`),
and future callers can bypass the structured contract accidentally.

Phase 1 should either deprecate these helpers for answerable/abstain-sensitive
flows or make them render the same explicit abstention text as MCP. At minimum,
add regression tests for both `memory_ask_query*` and `memory_answer_query*` so
the no-answer contract cannot silently collapse on string-only paths.

### §R4.2 Abstention telemetry must retain the weak candidate ids, not only zero citations

Today dogfood logging for ask records passes `out->citation_ids` and
`out->citation_count` (`memory_core_search.inc:4765`, `cmd_memory.c:103`). The
proposed abstain action explicitly clears citations. That means the most valuable
training cases — "we retrieved weak evidence and refused" — will be logged with
no retrieved ids, indistinguishable from zero-retrieval/no-evidence abstentions
unless extra metadata is added.

That undermines two proposal goals:

- the abstain/false-omission bench needs to inspect what evidence was withheld;
- Phase 4's ask-outcome feedback needs to learn which candidates caused false
  abstentions or false answers.

The evaluator should return or side-band an evidence trace separate from rendered
citations: candidate ids, anchor id, ranked count, answerability decision/reason,
grounding score, and whether the abstention was structural vs gated. Rendered
citations can remain empty on abstain; telemetry should not be empty.

### §R4.3 DB2-disabled/stub builds need an explicit no-op contract

`memory_core.c` has an `AIMEE_DB2_DISABLED` implementation for DB-free binaries.
It already defines `memory_ask_query*`, `memory_answer_query*`,
`memory_tier_priority`, query-shape helpers, and many memory APIs as stubs. Adding
new config fields and a shared `memory_answerable()` symbol only in the DB2-backed
path risks either link failures or divergent semantics in client/webchat/test
binaries that compile with DB2 disabled.

The implementation plan should include the stub contract:

- declare the evaluator/result types in the public header only if both backed and
  DB2-disabled builds can compile them;
- provide a DB2-disabled no-op evaluator returning `abstain`/unavailable without
  touching DB state;
- keep `memory_answer_query*` string behavior explicit in the disabled path too,
  instead of another empty-string abstention.

This is small but important because the proposal is framed as "no new service /
no new schema"; it should also remain no-new-linkage-risk for DB-free clients.

### §R4.4 Per-scope calibration is not reachable from the MCP `memory_ask` surface today

The calibration plan keys thresholds by surface/kind/scope. The ask engine can
receive `scope_type` / `scope_value`, and the KB backend forwards them when
present (`db2_kb_service_memory_ask_json`, `kb_client_memory_ask`). The MCP tool
does not expose or forward scope: `tool_memory_ask` calls
`kb_client_memory_ask(jq->valuestring, NULL, NULL, limit, &result)`
(`server_mcp.c:362-366`).

If MCP is one of the main recall surfaces, Phase 4 needs a decision before
per-scope thresholds are used there:

- add optional `scope_type` / `scope_value` (or workspace/project-derived scope)
  to the MCP schema and handler;
- intentionally treat MCP as global-scope for calibration; or
- derive scope from session/workspace context elsewhere and document that source.

Without this, the "per `(surface, kind, scope)`" gate will be per-scope for CLI/RPC
callers but effectively global for MCP, which is exactly the surface most likely
to use recall inside agent turns.
