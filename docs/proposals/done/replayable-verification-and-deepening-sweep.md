# Replayable-evidence verification + architecture-deepening sweep

- **State:** roundtable-reviewed in two rounds — R1 (3 lenses: architect /
  security / contrarian → 2 approve-with-changes, 1 rework, all converging),
  R2 (focused consult on the rubric split + the `typed_facts` modeling → both
  sound-with-tweak). Hardened per both rounds; the three formerly-open items are
  resolved (Rubrics / Settled decisions / Areas & caps). Awaiting user
  proposal-gate.
- **Scope:** intelligence-surface (roundtable review discipline) + autonomous-dev
  producer (a new sweep that files work items). Two parts, one producer→consumer
  loop.
- **Author:** JBailes, 2026-06-20.
- **Origin:** a recurring tax — roundtable panelists raise BLOCKERs that turn out
  false on inspection, so every finding has to be re-verified by hand. The cause
  is that review items carry a prose claim and a `sources` string but **no
  reproducible evidence the verifier can replay**. Separately, the codebase's
  remaining bloat is duplication (test dup, files at the 2000-line cap) that DRY
  would fix — there is no producer that systematically files those refactors.

## Problem

Two gaps, related by a shared discipline (a claim is only as good as the command
that reproduces it).

**1. Roundtable verification is claim-based, not evidence-replayed.**
`roundtable_review_item_t` (`src/headers/delegate_ensemble.h`) holds
`severity / category / location / summary / recommendation / sources` — a
panelist's *assertion*. There is no field for the exact command/query that
produced a count or a call-site list, and no pass that re-runs it. The aggregator
synthesizes across panelists but cannot mechanically check any single claim. So a
plausible-but-wrong finding survives to the artifact, and a human re-greps it
later. Strength ("blocker", "high") is asserted by the panelist, never earned
against reproduced evidence.

**2. No producer for systematic deepening/DRY work.**
The autonomous-dev path (`wfe` engine, work items, `/v1/dev/submit`,
`build.yaml`) executes proposals it is handed, but nothing *generates* the
backlog of "this duplicated logic across N call sites should become one module."
That class of work — the bulk of the remaining bloat — is found ad hoc, if at
all, and there is no guard against re-filing something already shipped or already
recorded as a settled decision in `typed_facts`.

## Goal

- Every roundtable review item carries **replayable evidence**, and an
  independent pass **re-runs it** before the item reaches the artifact;
  unreproducible items are dropped, not downgraded. Strength is assigned from
  reproduced evidence, never from the panelist's assertion.
- A new **deepening sweep** scans the codebase (whole or a chosen subsystem) for
  duplication-across-call-sites, verifies each candidate with the same
  replay discipline, and files survivors as **vertical-slice work items** into
  the existing dev queue — **delta-aware** (excludes already-filed proposals and
  settled decisions in `typed_facts`), resumable, analysis-only.

## Phasing (ship A before B)

**Part A ships first, standalone.** It has demonstrated value — it removes the
false-blocker tax on every roundtable review today — and a small surface. **Part
B is a follow-up** that builds on a battle-tested Part A: it is a new producer
for a category of work (duplication refactors) that is valuable but not urgent,
and materially more complex (scope resolution, exclusion maps, cross-area dedup,
worktree management). Coupling them would gate A's proven win behind B's
speculative infrastructure. So: A first, exercise its evidence vocabulary on real
runs, then B once that vocabulary is stable.

## Security model (the one trust boundary)

Every panelist and sub-agent is an **untrusted model**: its text — claims,
"evidence commands", work-item bodies — is adversarial input. Two rules make the
rest safe and are non-negotiable in both parts:

1. **Replay is structured-query-only — never a shell.** The verifier never
   executes a panelist-authored command line. Evidence is expressed as one of a
   closed set of **structured queries** over the existing read-only surfaces
   (`find_symbol`, `lsp_references`, `ast_grep_search`, `search_graph`) plus a
   path glob constrained to the area root. An item whose evidence cannot be
   expressed in that vocabulary is **dropped at parse time**, not retried. This
   removes the shell-injection / data-exfiltration / network-egress surface
   wholesale — there is no arbitrary `argv`, no `make`, no `curl`.
2. **The verifier model never sees raw query output.** A non-model replay layer
   reduces each query's output to a **fixed-shape record** —
   `{ count, identity_key = sha256(sorted("file:line")[:N]), … }` — and the
   verifier reasons only over that record. Raw output is logged for human audit,
   not fed to the model. This closes indirect prompt injection through planted
   text in grep'd files (`// IGNORE PREVIOUS INSTRUCTIONS …`) and makes the
   dedup/identity key deterministic, not model-derived.

## Design

### Part A — replayable-evidence verification in the roundtable

Add to `roundtable_review_item_t` a **structured-evidence** field. Because the
struct is fixed-width and embedded in `items[ROUNDTABLE_MAX_REVIEW_ITEMS]`
(`src/headers/delegate_ensemble.h`), the evidence does **not** go inline: store
it in a **side buffer with a length field and a hard 4 KiB cap**, referenced from
the item. An item whose evidence exceeds the cap is dropped as unverified
*before* the verifier sees it (no truncation — a truncated query is a different
query). The struct's existing copy/compare/serialize patterns are audited as part
of this ABI change. The evidence is a **structured query** (per the security
model), not a shell line: query kind ∈ {symbol lookup, reference count, pattern
match, graph query}, its parameters, the area-root glob, and the **expected
count/record**.

Mark each review item's content as **factual** (counts, locations,
presence/absence — replayable) or **interpretive** (what the count *means* — not
replayable). Only the factual portion is subject to replay; an interpretive claim
rides on a verified factual base or is flagged as opinion. Replay confirms "14
call sites exist", never "those 14 indicate bad error handling".

After the panel fans out and **before** synthesis, run a **verification pass** —
a fresh evaluator that sees **only the claims and the reduced-output records, not
the panelists' reasoning and not raw query output** (security model rule 2):

1. **Replay** each item's structured query through the read-only surfaces; the
   replay layer returns the fixed-shape record. Record matches the expected
   (exact, or off-by-a-few correctable in place) → keep. Cannot reproduce → drop;
   add to a **clearly-marked rejected appendix** (panelist claim + query +
   reduced record + verifier "why" + stable item ID) — visible to the human, not
   silently buried.
2. **Re-derive severity** from the reproduced record against the **fixed,
   version-pinned Part-A rubric** (see Rubrics below). The verifier applies the
   rubric mechanically, never its own taste: it **downgrades** when the claim
   exceeds what the reproduced facts support, and **promotes** when a reproduced
   fact meets a higher tier's criterion — but it **never escalates on
   interpretation alone** (no "this smells like a blocker"). The verifier's
   verdict is itself a **structured schema**, not free prose.
3. **De-dupe** items sharing the deterministic `identity_key` from their reduced
   record (not a model-derived key) into one.

The artifact then carries only reproduced findings + a short rejected list. This
reuses the existing panel/persona machinery (`delegate_roundtable_run`,
`panel_persona_name`, `ROUNDTABLE_REVIEW`); the new piece is the evidence field
+ the replay pass. Default the replay pass on for review mode; gate behind a
config flag so it can be disabled.

Run panel and verifier as **short, separate delegate calls**, not one long call:
a single long call that drops loses the whole run, whereas a dropped short call
costs only its own slice and is retryable. (This mirrors the failure class behind
prior delegate drop/deadlock issues.)

### Part B — the deepening sweep (a new analysis-only producer)

A command/skill (`aimee sweep <path-or-subsystem>`, default whole codebase) that:

1. **Resolves scope → areas** from a **configured source-glob allowlist** (e.g.
   `src/**`, `tests/**`) — never trusted to the sub-agent, so an area can't be
   pointed at `secrets/`, `deploy/`, or a home dir. Concrete partitioning rule:
   top-level source subdirectories, max ~50 files/area, split by `#include`
   cluster when a dir exceeds the cap; tiny leftovers fold into the nearest
   neighbour. One unit per iteration so each has a tractable, self-contained
   scope.
2. **Builds an exclusion map** from (a) existing `docs/proposals/` +
   open/filed work items and (b) **settled decisions in `typed_facts`** (the
   `architecture_settled` relation — see Settled decisions), using a
   **deterministic match key** — the `(primary_file_path, symbol/function-name
   prefix)` of the proposed extraction site vs. the recorded decision subject —
   not fuzzy title matching. The exclusion lookup is itself a **replayable
   structured query** (consistent with the rest of the design), reading
   active-only decisions via the existing `db2_typed_fact_by_relation`
   (which filters `active = 1`). The map is **re-read (and its content hash
   captured) before each area** so items filed mid-sweep are seen by later areas;
   the hash is embedded in the area commit so resume can detect drift. A
   candidate matching the map is excluded, not re-filed. "Zero new candidates" is
   a valid, honest result.
3. **Fans out one short sub-agent per area** (one-retry-with-narrower-brief on
   failure; mark an area "needs manual look" rather than stalling the whole
   sweep). The **deepening signal is mechanical, not a thought-experiment**: the
   sub-agent extracts the proposed seam's dependency edges (callers, callees,
   shared state) from `search_graph` / `lsp_references` and a candidate qualifies
   under the **Part-B "rule of three" rubric** (see Rubrics) — the "deletion
   test" expressed as a replayable graph query, so the verifier can re-derive it
   rather than re-interpret prose. A candidate whose extraction region carries an
   **open Part-A-class blocker is held, not filed**, until that blocker clears —
   the sweep never files an "extract this" ticket over known-broken code (a
   gating precondition, not a merged score: the two rubrics stay separate). Each
   candidate returns title, strength, the **structured evidence query + expected
   record**, the proposed module/seam, the edge-count verdict, and the
   deterministic exclusion-key check.
4. **Verifies every candidate** with the Part-A replay discipline (a fresh
   verifier per area that re-runs the structured query and **re-derives the
   edge-count verdict from the reduced record**, not the proposer's summary).
   Cross-area de-dupe merges candidates sharing
   `sha256(reduced_record)` — computed by the verifier, not either sub-agent —
   into **one** work item.
5. **Files survivors as vertical-slice work items** into the dev queue, through a
   **strict filing schema enforced by a non-model gate**: paths
   (files-to-repoint, test locations) must resolve under the repo root and match
   the area glob, no symlinks/hardlinks escaping the area root, no shell-like
   syntax in any free-form field; filed items are **quarantined until a human
   signs the body** (the existing gate covers implementation, which is downstream
   of this filing injection point). Each item is independently grabbable = the
   new module + **every call site repointed** + tests at the new interface + the
   old copies deleted (no "part 1 of 3"). Acceptance criteria are checkable;
   tests assert behaviour *through* the new interface, not past it; the test
   runner the wfe later uses has no network and no env secrets.

**Guardrails (baked in):**

- **Analysis-only.** The sweep writes work items + a per-area summary; it makes
  **zero source edits**.
- **Off the remote default branch in a dedicated worktree**, never local refs (a
  stale base re-finds already-shipped refactors) and never the default branch.
  Staging is done by a **fixed (non-model) committer** against an explicit
  per-area path allowlist snapshotted at sweep start; it refuses any path off the
  allowlist or any symlink/hardlink resolving outside the area root. Never
  `git add -A`. No push. The branch uses a `sweep/*` prefix so no CI auto-builds
  it.
- **Bounded cost.** A per-sweep cap (max areas, max total delegate calls) and a
  per-area filed-item cap, with a hard abort and per-area cumulative-cost logging
  — a guard against a runaway sweep given the known delegate drop/deadlock class.
- **Resumable, never restart-from-zero.** Per-area commit + an area checklist are
  the checkpoint; an interrupted sweep resumes from the first unchecked area.
  Re-check the remote tip on resume; if it moved significantly, **re-derive the
  exclusion map and area boundaries from the new tip** and replay only unchecked
  areas against it (don't file against a dead base), marking prior areas
  potentially-stale for the next delta.
- **Delta-aware.** Re-running a swept area excludes prior candidates and writes
  only genuinely-new ones.

Implementing the filed work items is the **existing** autonomous-dev path
(one ticket per iteration, project gates, vertical-slice as definition-of-done) —
this proposal adds the producer and the verification discipline, not a new
executor.

## The loop, once both parts land

Part A is the verification engine; Part B is its first heavy consumer (shipped
after A per Phasing). With both in place the deepening backlog is verified by the
same replay rule, closing the loop: sweep → verified work item → implement →
record settled decision in `typed_facts` → next sweep excludes it.

## Out of scope

- No new file-based issue store — work items go through the existing dev queue.
- No auto-implement: the sweep is analysis-only; implementation stays behind the
  human/roundtable gates already in place.
- No new model/provider plumbing — reuses the existing panel/persona/delegate
  machinery.

## Resolved by the roundtable (were open questions)

- **Evidence field** → side buffer, length-prefixed, 4 KiB hard cap, dropped if
  exceeded; ABI change audits the struct's copy/compare/serialize. (Part A)
- **Replay execution model** → structured-query-only over the read-only surfaces;
  no shell, ever. The verifier sees reduced records, not raw output. (Security
  model)
- **Deletion test** → a mechanical dependency-edge count via `search_graph` /
  `lsp_references`, not a model thought-experiment. (Part B step 3)
- **Exclusion/dedup keys** → deterministic (`(file, symbol-prefix)` for
  exclusion; `sha256(reduced_record)` for dedup), never model-derived. (Part B
  steps 2, 4)

## Rubrics (two, deliberately separate)

Part A (defect impact) and Part B (extraction leverage) measure different things;
folding them into one score (`severity × leverage`) loses the dimension you need
to act on, so they stay separate. They meet in exactly one place — the gating
rule in Part B step 3 (don't file an extraction over an open blocker) — and that
is a precondition, not a merged number.

**Part A — severity, anchored on reproduced facts:**
- **blocker** — a correctness / security / build failure whose factual trigger
  the replay *reproduced*.
- **concern** — degrades but doesn't break. An **interpretive-only** item (no
  reproduced trigger) **caps here** — it can never be a blocker.
- **nit** — style / preference.

The verifier moves an item *by rubric* in either direction from the reproduced
record (down when over-claimed, up when a reproduced fact meets a higher tier),
but **never escalates on interpretation alone**.

**Part B — the "rule of three" (structural leverage):**
- **strong** — ≥3 independent callers whose only shared state is the proposed
  interface.
- **worth-exploring** — 2 callers, **or** ≥3 callers but with extra shared state
  (a leaky seam).

The count (default 3) is a **named, configurable** threshold ("rule of three"),
not a buried magic number — tunable per language/component.

## Settled decisions in `typed_facts`

A "settled decision" the sweep must not re-propose is **one new ontology
relation**, reusing the existing assert / supersede / recall machinery — no new
table, no new accessor:

- Relation `architecture_settled`, `head_kind = code_site`, `tail_kind = SCALAR`
  (added to the seed `TF_ONTOLOGY[]` in `src/db2/typed_facts.c`).
- **subject** = canonical site `file:symbol-prefix` (prefer the **symbol-
  qualified** identity over a bare path, so a moved file doesn't silently strand
  the decision); **object** = the verdict/pointer (`extracted@<commit>` /
  `rejected:pass-through`); **source** = the proposal/commit ref; confidence as
  usual.
- **Supersede-on-contradiction** gives free decision history (rejected → later
  extracted) at no cost; the exclusion reads **active-only** via
  `db2_typed_fact_by_relation` (already `active = 1`), so a superseded decision
  never wrongly blocks re-evaluation.
- **Multi-site decisions** group by the shared `source` ref (one row per site —
  the single-row model is sufficient for v1; scope-globs and expiry are noted
  limits, not built).
- **Staleness check (cheap):** if a decision's `source` commit is no longer
  reachable from the current tip, surface it for re-review rather than silently
  excluding.

## Areas & caps (defaults + calibration, not open)

- **Area partition:** within the allowlisted source roots → top-level
  subdirectories; ≤50 files/area; a dir over the cap splits by `#include`-cluster
  (connected components of the include graph) until each ≤50; leftovers (<~8
  files) fold into the nearest neighbour by include-distance.
- **Cost caps (starting):** ≤40 areas/sweep; ≤3 delegate calls/area (proposer +
  one retry + verifier) ⇒ ≤~120 calls; ≤10 filed items/area; **hard abort** at
  1.5× the projected call budget or a wall-clock ceiling; per-area cumulative
  cost logged.
- **Calibration:** the first whole-codebase run is a **dry run** (file nothing)
  that records the area-size distribution and candidate yield; the file-cap and
  call budget are tuned from that distribution. So this is *defaulted with a
  calibration procedure*, not an unknown.

## Genuinely deferred (out of v1, by choice)

- Scope-glob / pattern-level settled decisions and time-based expiry (the
  single-row, site-pinned model covers v1; `source`-reachability handles
  staleness).
- A content-hash site identity that survives arbitrary code movement (symbol-
  qualified subject is the v1 mitigation).
