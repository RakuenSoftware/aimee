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
confident answer**, deterministically, on the answer path, before any generation
cost is paid. Today Aimee retrieves well and *reports* a confidence number, but
it only abstains on **structural** failure (nothing retrieved, no extractable
answer, no citations). When retrieval succeeds but the hits are weak, the answer
path still synthesizes and merely attaches a low-confidence note; the context
path injects a soft "## Retrieval Confidence: LOW" marker and asks the answering
LLM to abstain *for itself*. That delegates the most important decision - "is
there an answer here at all?" - to a prompt instruction, which is exactly the
failure mode this proposal removes.

The change is small because the substrate is already built. The abstention
*slot*, *schema*, and *rendering* exist end-to-end; only the **trigger** is
missing. This proposal wires the already-computed confidence into a deterministic
gate, splits one threshold into the two the literature calls for (a chunk filter
and an answer gate), exempts curated content, and lets the existing calibration
loop tune the gate from outcomes instead of a hand-picked constant.

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
result - no gating of *retrieval compute*) and only changes the **terminal
action on the answer path** from "synthesize + warn" to "abstain" when calibrated
confidence is below the gate. The soft LOW marker stays as the *context-path*
behavior for proactive recall, where there is no single answer to refuse; the
hard gate is added on the *answer path* (`memory.ask`), where there is.

## Implementation contract

The gate must be concrete before any default-on discussion:

- The gated confidence is **`memory_answer_confidence`'s existing 0..1 output**,
  optionally floored by `memory_retrieval_confidence.score` (coverage/separation)
  when failure detection is enabled. We do not invent a third confidence number.
- The gate is **deterministic and side-effect-free**: given the same candidates
  and thresholds it always produces the same abstain/answer decision, and it runs
  **before** any answer composition cost beyond what retrieval already paid.
- On abstain, the result is the **existing** shape: `no_answer = 1`, `answer`
  empty, `confidence` set to the (low) computed value, citations empty. No new
  field, no new render path - `server_mcp.c:386` already says the right thing.
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

| Phase | Change | Blast radius | Risk |
|------|--------|-------------|------|
| 1 | Answer-path abstain gate | `memory_core_search.inc` + 2 config fields | low |
| 2 | Two-tier thresholds (chunk filter + answer gate) | candidate assembly + 1 config field | low/medium |
| 3 | Curated-content exemption | gate predicate + (maybe) provenance signal | low |
| 4 | Per-triple calibrated thresholds | `kb_calibrate` output + ask-path read | medium |

Phase 1 is the high-value slice and is shippable alone: it turns the
already-computed-but-ignored confidence into an actual refusal, reusing the
end-to-end `no_answer` contract. 2 sharpens it, 3 removes the obvious false
refusal, and 4 makes the threshold learn instead of being guessed.

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
- **Not** removing the soft LOW marker - it stays as the proactive-recall /
  context-path behavior, where there is no single answer to refuse. The hard gate
  is added on the answer path only.
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
