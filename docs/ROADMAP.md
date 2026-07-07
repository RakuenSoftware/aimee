# Roadmap: storage, retrieval, curator, operational closeouts

This document captures the current ordered plan for the storage-layer
maturation and the retrieval/curator arc built on top of it, *in what order*
to land the work.

Last revised: 2026-04-29.

## Single thread

aimee is walking a deliberate line: **one owner per storage tier, one
typed API per owner, and no backend knowledge outside the owning
directory.** DB1 owns local user/session state for the local
`aimee-server`; DB2 owns durable project/workspace/global knowledge plus
the dense vector indexes derived from it (pgvector extension, in-process).
DB2 can be local or shared depending on the `aimee-kb` deployment, so
promotion into DB2 must match the configured KB scope. With that boundary
treated as the baseline, the deep-curator
work takes the next natural step: lift curator + knowledge into
`aimee-kb` behind a versioned public HTTP API. Dynamic-alpha fusion and
ingest-lab are retrieval-quality polish on that stack. Three
operational closeouts need calendar time and run in parallel, with the
**working profile** earmarked as the proving ground for the
retroactive-review pattern the curator depends on.

## Critical path: in flight

### Storage-boundary enforcement

The DB1 sqlite / DB2 postgres storage split landed; the vector tier,
originally a separate Qdrant sidecar, was folded into DB2 as a pgvector
extension (#1575) so vectors live in the same Postgres instance as the
rest of DB2 knowledge. The active work is no longer subsystem migration
planning; it is boundary enforcement and cleanup. Each storage tier
already has an owning source directory (`src/db1/`, `src/db2/`), and the
current audit removes stale public surface, docs, comments, route names,
config names, and lifecycle calls that would let old ownership
assumptions re-enter.

- **DB1:** keep user-local/session-local state behind `src/db1/`.
- **DB2:** keep project, workspace, and global knowledge, including
  the dense vector indexes (pgvector tables) derived from it, behind
  `src/db2/`; `aimee-kb` owns DB2 lifecycle outside shutdown/startup
  unwind. Vector transport (`src/db2/pgvec_*.c`) sits inside DB2 since
  pgvector is a Postgres extension running in-process; there is no
  separate sidecar.
- **Guards:** `scripts/check_tier_deps.sh` is the executable contract
  for the split and should grow whenever the audit removes another
  stale surface.

Greenfield rule: once a boundary is clean, do not add compatibility
branches, migration-era aliases, or backend-named public surfaces. They
count as debt to delete, not steady-state architecture.

## Parallel to Boundary Enforcement

These land alongside storage-boundary enforcement and do **not** depend
on its completion.

### Phase 0 of deep-curator: embedder-sidecar retrieval lift

Carve-out from the full curator work. Ships the MiniLM sidecar and
`aimee kb repair` against the current KB pipeline. Target: raise
`kb_search` mean P@5 from 0.268 → ≥ 0.45 on the 44-query POC set.
Reversible in one config flip + repair. This is the single biggest
near-term retrieval win.

### Operational closeouts: calendar-bound, scheduled

Each of these now has a named owner, a week-by-week schedule, and a
**close-or-promote deadline** so they stop accumulating in `pending/`
indefinitely.

- **Working profile:** Baseline 2026-04-22, first-field enablement 2026-05-20, dogfood
  decision 2026-06-20, close-by 2026-06-30. **Also the proving ground
  for the retroactive-review pattern** before deep-curator generalises
  it.
- **Dogfood auto-label:** Flag flips start 2026-04-22, first month-end artefact 2026-05-07,
  derived PR 2026-05-21, close-by 2026-06-07.
- **Learning-signals router phase 2 fixtures:** Labelled corpus 2026-05-22, first detectors land, simulation traces
  by 2026-06-22, close-by 2026-07-15.

### Ingest-lab (read-only tooling surface)

The validation-surface / strategy-comparison half is pure tooling that
does not write to DB2; it can ship against the current KB pipeline. The
DB2-facing parts (facet writes, canonical text promotion) wait on an
explicit DB2 ingestion contract.

## Gated on Storage-Boundary Enforcement

### aimee-kb service + public API: platform phases

Platform half of the former deep-curator work. Owns process split,
`/v1/` API, OpenAPI, SDKs, install-today profile picker, corpus
staging. Phased:

1. Service scaffold + OpenAPI v1 skeleton.
2. Install-today profile picker.
3. Ingest normalization + `POST /v1/docs`.
4. Corpus staging + release gating.
5. Retrieval endpoints (served against artifacts from the extraction
   work below).
6. Reflection HTTP surface. Gated on the working-profile operational
   cycle above (proving ground for the retroactive-review pattern).
7. Distributed-mode validation + v1 API stability tag.

### Deep-curator doc + code extraction: neural phases

Neural half of the former deep-curator work. Runs inside the
aimee-kb service above. Phased:

1. Curator infra + schema + embedder sidecar inside aimee-kb.
2. `extract_doc` + N-attempt reducer. Publishes `doc_summary` +
   `claim`; feeds `POST /v1/search`.
3. `resolve_entities` + canonical entity graph + mention links.
4. `cognify_code_unit` structural stage. Feeds `/v1/code/*`.
5. `cognify_code_unit` semantic stage + `implements` links. Feeds
   `/v1/implements`.
6. `detect_contradictions`. Feeds `/v1/contradictions`.
7. `synthesize_topic`. Feeds `/v1/synthesize`.

### Dynamic-alpha KB fusion

**Conditional.** Runs only if the Stage-A offline benchmark on aimee's
own corpus beats the retrieval-quality bar set by deep-curator Phases
0 + 2 + 4. Not a default flip; gated on evidence.

## Independent feature stream

### Conversation branching

Moved from `accepted/` to `pending/`. Dependencies (slash-commands,
worktree) have both landed, so it is technically unblocked, but it has
been accepted-and-dormant; the move back to pending reflects that
status honestly and leaves the decision to pick it up to whoever has
the cycles.

## Cross-cutting items not yet tracked

The following are real gaps that are not tracked anywhere yet. Each deserves an
owner when someone is ready to pick it up.

- **Developer onboarding:** as external services accumulate (DB2 with
  pgvector, Python embedder sidecar, LLM endpoint for curator), the
  "fresh-Debian to green-test-suite" story stops fitting in anyone's
  head. A `setup.sh` that handles all of it is worth a week of effort
  and has no technical blocker.
- **Standing LoCoMo / LongMemEval cadence:** too many acceptance
  criteria cite "within ±0.005 of baseline," but the benchmarks don't
  run on a cadence. Either put them on nightly/weekly CI (with a
  retention policy for the artefacts) or stop citing parity
  criteria that nobody measures.
- **Proposal-supersession hygiene rule:** the PR that supersedes a
  proposal should move the old one (to `rejected/`, `deferred/`, or
  wherever it belongs) **in the same commit**. Drift like the
  pluggable-db proposal sitting in `pending/` after being deleted by
  its successor is how `pending/` becomes stale signal. Worth a short
  `CONTRIBUTING.md` addition alongside the PR template.
- **First-class operator audit surface:** the data is all there
  (`operator_id` on every shareable row, `content_hash`, timestamps),
  but a CLI verb rendering per-operator / per-scope activity in a
  legible way does not exist. Flagged as a known gap in the
  DB1/DB2 storage-boundary work.
- **Distributed-mode auth design:** the deep-curator token lifecycle
  is deliberately minimum-viable (opaque bearer, config-file seeded,
  per-token audit). OIDC, short-lived tokens with refresh, per-user
  login flows, and token-management endpoints are all deferred until
  a real multi-tenant deployment motivates them. When that lands, it
  deserves its own tracking item.

## What this roadmap does not cover

The broader backlog, roughly a hundred tracked items across agent runtime,
delegation, memory-quality pillars, UX, reliability, and platform work, flows
on its own cadence and is intentionally outside this roadmap's scope.
