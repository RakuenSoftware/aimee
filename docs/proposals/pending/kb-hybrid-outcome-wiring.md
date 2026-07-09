# Proposal: kb_hybrid outcome wiring — close the learning-to-rank loop on live data

- **State:** in progress — B1 (the loop-closing plumbing) is implemented and
  integrated; B2 (a production outcome source for code-search) is the remaining
  open work. This is the follow-up prerequisite named by the done proposal
  [learning-to-rank weight fitting](../done/learning-to-rank-weight-fitting.md),
  whose fitter ships bench-only until this lands.
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

## B2 — a production outcome source (open work)

B1 makes the loop *closeable and testable*; it does not invent a signal. The
remaining question is genuinely separate: **what reports which code-search
documents were useful?** The memory surface has an established reporter (the agent
attributes recalled memories). Code-search has no equivalent yet. Candidate
sources, in rough order of fidelity:

1. Answer-attribution: when a turn cites/uses a surfaced doc in its answer, emit an
   `accepted` outcome for that doc against the query's event.
2. Explicit harness feedback (benchmark / eval harness reporting relevance).
3. Weak implicit signals (a later edit touching a surfaced file) — lowest fidelity,
   position-biased.

This is deferred, not faked: until a real source populates `ranker_outcome`, the
fitter correctly stays bench-only and `aimee kb ranker export-view` keeps reporting
the empty-view diagnostic.

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
