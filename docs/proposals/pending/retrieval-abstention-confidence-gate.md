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
