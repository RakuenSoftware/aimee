# Proposal: kb_hybrid outcome wiring — close the learning-to-rank loop on live data

- **State:** in progress — B1 (the loop-closing plumbing) is integrated. B2's
  first outcome source — a dogfood-autolabel → retrieval-outcome bridge — is
  implemented for the memory surface (closing the long-dormant demotion loop) and
  is ranker-ready; the ranker (`kb_search`) capture hook and higher-fidelity
  signals are the remaining open work. Follow-up prerequisite named by the done
  proposal [learning-to-rank weight fitting](../done/learning-to-rank-weight-fitting.md).
- **Author:** JBailes
- **Date:** 2026-07-09
- **Charter roles:** Calibrate (feed the fitter real observed outcomes, not just
  fixtures), Evaluate-Optimize (the fitter's benchmark gate still decides
  promotion), Gate-Promote (unchanged — a fitted model only lands on lift).

## Thesis

The learning-to-rank fitter (option A) infers, fits, benchmark-gates, and refits —
but its §1 training-view join is empty on the shipped substrate, so it can only
ever fit fixtures. The gap: the ranker's features are written on
`feature_rows.subject_kind='kb_document'` (the kb_hybrid code-search surface,
`src/kb/kb.c`), while the only outcome labels that exist are attributed to
`memory` row ids on the memory-recall surface. Disjoint id spaces; no shared
grouping key. The fitter cannot learn from live traffic until the kb_hybrid
surface itself reports which surfaced documents were useful.

## What was found (the design pivot)

The memory surface does **not** capture outcomes inline in its recall hot path.
A dedicated, caller-driven pair of endpoints does it: `evidence.emit_retrieval_event`
mints a `retrieval_event` over the surfaced ids, and `memory.record_retrieval_outcome`
records per-id verdicts as artifacts. That means the kb_hybrid loop can be closed
the same way — **endpoint-driven, with zero `src/kb/kb.c` retrieval hot-path
change** — rather than the schema-migration + hot-path rewrite the original
option-B framing assumed. This is strictly safer and reversible.

## B1 — the loop-closing plumbing (implemented)

- A dedicated `ranker_outcome` artifact kind for kb_hybrid outcomes
  (`kb_ranker_outcome_write` in `src/kb/kb_ranker_fit.c`), keyed by kb_document
  doc_id and tied to a `retrieval_event_id`. It is deliberately **not**
  `retrieval_attribution`: keeping a separate kind means the memory demotion
  scorer (which reads `retrieval_attribution`) is untouched, and a memory row id
  that happens to equal a doc_id can never collide into the ranker's training view.
- `kb_ranker_emit_event` mints a kb_hybrid `retrieval_event` capturing the
  surfaced doc_ids (audit + the grouping key that ties a query's outcomes together).
- Two caller-facing KB-service methods mirroring the memory evidence pair:
  `ranker.emit_event` and `ranker.record_outcome`
  (`src/kb/kb_service_agent.c`, dispatched from `src/kb/kb_service.c`).
- The fitter's training view (`kb_ranker_training_view`) now reads `ranker_outcome`
  instead of `retrieval_attribution` — which both enables this loop and removes the
  latent id-collision described above. Nothing else about the fitter changes: the
  same benchmark gate and refusal rails apply.

The result: once outcomes are reported for candidates that have feature rows, the
existing fitter fits, benchmark-gates, and (on lift) promotes — on live data.
Verified end-to-end by `test_closed_loop_capture` in `src/tests/test_ranker_fit.c`
(emit event → record outcomes → training view groups by event and joins feature
rows → non-empty, correctly-labelled batch).

## B2 — the first outcome source: a dogfood-autolabel bridge (implemented)

Production evidence (`.254`) reframed the problem: there are **408 `retrieval_event`
artifacts and 0 attributions** — the memory demotion loop was never wired to an
outcome source either, despite the endpoint existing. And there are **185
`kb_document` feature rows** already waiting. So the missing piece is universal,
not ranker-specific: nothing ever converts the *signal already computed each turn*
into an outcome.

That signal exists. `dogfood_classify_next_turn()` already labels the next user
turn **continuation** or **repair** after a retrieval. This proposal adds
`retrieval_outcome_bridge` (`src/server/retrieval_outcome_bridge.c`): the emitter
notes the prior turn's surfaced rows + event id (`ingress_preinject.c`), and the
next turn's classification writes the outcome — `accepted` on continuation,
`corrected` on repair. Wired for **memory** now (closing the dormant demotion loop
from the 408 existing events, via `retrieval_attribution`) and **ranker-ready**
(the same bridge writes `ranker_outcome` for a "ranker" surface note). Default-off
behind `learning_implicit_retrieval_outcome`; observation-only. Unit-tested end to
end in `test_retrieval_outcome_bridge.c`.

### Honest limit — this is a weak, positively-biased label

`dogfood_classify_next_turn` returns **CONTINUATION for any substantive
non-correction follow-up** — i.e. "the user kept talking and didn't immediately
correct me," a loose proxy for "the rows were useful." So the label skews heavily
positive (the exact position/outcome bias the LTR proposal's Risks section names).
It is a legitimate *first* source — it turns a dead loop into a live trickle of
real weak labels — but the fitter must not over-trust it, and the benchmark gate
remains the backstop. Higher-fidelity sources are the real goal.

### Fitter side, landed: pairwise objective + IPW weight

The fitter (`scripts/rank-fit.py`) now supports a **pairwise (RankNet) objective**
(`intelligence.ranking.fit.objective = pairwise`) alongside pointwise. This matters
directly for the weak-label problem: pairwise optimises *within-query ordering*, so
a feature that is **constant across a query's candidates earns ~0 weight** — which
is exactly why a turn-level, all-`accepted` label carries no learnable signal, and
why the *per-document contrast* below is the thing that unlocks it. The
per-candidate `weight` field now flows through to the fitter, so an **IPW /
propensity or confidence weight** on an outcome is honoured unchanged.

### Remaining open work — the real fidelity lever

1. **Per-document answer↔doc overlap attribution.** The turn verdict is applied to
   *all* surfaced rows identically, giving a ranker no within-query contrast (and
   the existing "citation" machinery is circular — `memory_collect_answer_citation_ids`
   returns the top-ranked match's cluster, computed at *retrieval* time, so it just
   reinforces the current order). The non-circular fix: at attribution time, the
   prior turn's assistant answer is already in the message history — score each
   surfaced doc's snippet against it and emit **per-doc** `accepted` (used) vs
   `contradicted` (surfaced-but-unused). This is what turns a flat verdict into the
   contrastive labels the pairwise objective needs.
2. **Ranker (`kb_search`) capture hook.** Note `ranker` outcomes when the agent uses
   `kb_search` in a turn (scoped to in-turn tool use — `kb.search` is also CLI/API
   callable outside a turn; `kb_search`'s response must expose `doc_id`).
3. **IPW propensity logging.** Log the surfacing propensity (reuse the bandit's
   `db2_bandit_decision_insert` pattern) and populate the outcome `weight` with
   `1/propensity`; the fitter already consumes it.
4. **Explicit harness/eval feedback** — highest fidelity, for benchmark runs.

## Non-goals

- No change to `src/kb/kb.c` retrieval scoring or ordering.
- No schema migration (outcomes are artifacts, like every other evidence row).
- No new inference behaviour — the fitter and its gate are unchanged.

## Tests

- `test_closed_loop_capture` — emit + record + training-view join over live-shaped data.
- The existing fitter suite (`src/tests/test_ranker_fit.c`) now exercises the
  `ranker_outcome` path throughout (refusals, gate commit/hold, round-trip).

## Provenance

Follow-up to `docs/proposals/done/learning-to-rank-weight-fitting.md` (option A).
Mirrors the memory evidence pattern in `src/kb/kb_service_memory.c` and
`src/kb/kb_service_agent.c`.
