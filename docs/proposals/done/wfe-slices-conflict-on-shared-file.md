# Proposal: slices that each create the same file cannot merge after the first one lands

- **State:** DONE — stale-tip, terminal-conflict, and sibling-freeze collision fixes delivered.

> **Archived after delivery.** New slices now branch from and integrate the fetched remote
> feature tip, and content merge conflicts terminate rather than retry forever. The proposal's
> separately recommended atomic sibling-freeze collision check was subsequently delivered in
> [`wfe-sibling-freeze-collision-residual.md`](wfe-sibling-freeze-collision-residual.md).

## Symptom

In the live `build-e2e` run `wi_f96d4b18…`, both slices that reached the merge
stage stopped there with the same error:

```
slice 2   23:10:24  merge  pause  merge_pending: forge resource 400:
          {"error":"github API (pr merge, HTTP 405): Pull Request has merge conflicts"}
slice 1   23:21:10  merge  pause  (identical)
```

`gh pr list` confirms it:

```
#2013  CONFLICTING  base=aimee/feat/wi_f96d4b18…  head=aimee/wi/wi_f96d4b18….g0.2
```

## Cause

Every slice of this work item writes the same single file. The deliverable *is*
one runbook:

```
$ git diff --name-only origin/aimee/feat/<wi>...origin/aimee/wi/<wi>.…g0.2
docs/runbooks/appliance-state-recovery.md

$ git show --name-only --format="" da80f8e7      # slice 0, already merged
docs/runbooks/appliance-state-recovery.md
```

All five slice branches were cut from the feature branch at the same instant
(every `create` event carries `20:09:34`). Slice 0 merged at 21:03. Each later
slice is now based on a feature branch that has moved, and its diff touches the
file that moved.

Crucially, the slices do not *edit* a shared file — they each **create** it. A
rebase therefore cannot help. Tested in a throwaway clone:

```
$ git rebase aimee/feat/<wi>            # on slice 2's branch
CONFLICT (add/add): Merge conflict in docs/runbooks/appliance-state-recovery.md
error: could not apply bc2bb826... wfe: impl
AA docs/runbooks/appliance-state-recovery.md
```

`add/add` means two full, independently authored versions of the same document
with no common ancestor for the content. There is nothing to replay cleanly onto.
Resolving it requires deciding what the merged document should say — a content
decision, not a mechanical one.

Two gaps combine:

1. **No disjoint-ownership guard at slice time.** `test_plan_flags_overlapping_owned_files`
   shows the delegate planner has a notion of overlapping owned files, but
   nothing stopped the WFE slicer emitting five slices that all own one path.
2. **No rebase before merge.** The slice-merge path does not update a slice
   branch onto the current feature head before attempting the merge. A grep of
   `src/modules/workflows/` and `src/server/` finds no such step. This is a
   separate weakness; as shown above it would *not* have fixed this incident.

## Consequence

Both slices that have reached merge have failed there, and neither failure is a
flake or load-dependent. Whether slices 3 and 4 fail the same way is a
prediction, not an observation — see the assumptions below.

**The engine retries the unresolvable conflict indefinitely.** Measured on the
live run more than three hours after the first failure:

| slice | merge events | first | last |
|---|---|---|---|
| 1 | 7 | 23:21:10 | 02:21:27 |
| 2 | 8 | 23:10:24 | 02:21:29 |

Fifteen attempts, roughly one every 25 minutes, none of which can ever succeed:
the conflict is a property of the two trees, so it is identical on every retry.
The run neither completes nor fails — it occupies the single active-root slot
(`autonomy.concurrency: 1`, `trigger.max_concurrent: 1`) indefinitely, blocking
every other work item behind it. A merge conflict is not a transient forge error
and should not be retried as one.

Stated precisely, and no more broadly than the evidence supports: **two slices
that each independently create the same path, with content-divergent versions,
cannot be merged after the first one lands.** Three narrowings matter:

- It is narrower than "any single-file deliverable". Slices editing *distinct
  regions of a file that already exists* share a common ancestor and can merge
  normally; that is a stale-base problem (gap 2), not this one.
- It is narrower than "independently creating the same path". Two slices that
  create the same path with *identical* content merge without a conflict; git
  only reports `add/add` as a conflict when the added content diverges.
- The `add/add` result quoted above is established for the tested branch pair
  (slice 2 onto the feature head). It is evidence for the general rule, not proof
  of it.

This is separate from, and additional to, the panel-seating defect in
[wfe-panel-cannot-seat-under-self-load.md](wfe-panel-cannot-seat-under-self-load.md).
Fixing that one alone lets slices reach merge faster and fail there — as slice 1
demonstrated, passing `rt_gate` on its fourth attempt and then hitting this.

## Options

1. **Rebase the slice branch onto the feature head before merging**, re-gating if
   the rebase changed the tree. Addresses stale bases generally. Does **not**
   address add/add.
2. **Merge the feature branch into the slice branch**, then merge back. Same
   limitation as option 1 for add/add.
3. **Reject the unsafe intersection at slice time**: refuse to emit a plan in
   which more than one slice *creates* the same path. **Not implementable
   against the current schema — see below.** Prevents the diagnosed class
   without touching plans whose slices edit distinct regions of an existing file,
   but only if slice-time path data exists, and it does not.
3c. **Detect the collision at freeze time, before merge.** After `impl` each
   slice has a frozen diff, so its actual created/modified paths are known. If
   two slices in the same run create the same path with divergent content, fail
   them *there* — with the conflicting path named — instead of letting each
   reach `merge` and surface a bare forge 400. Implementable with data the
   engine already has.
3b. **Reject *all* owned_files intersections at slice time.** Stricter and
   simpler to implement, but it forbids the distinct-region-edit case this
   proposal says can merge normally. Adopting it needs a separate justification
   — e.g. that region-level disjointness cannot be verified reliably at slice
   time — which this proposal does not make.
4. **Retry with a rebase on conflict.** Cheapest, but for add/add the retry lands
   on the same unresolvable conflict.
5. **Leave as-is.** Not viable: a content-divergent create/create plan, such as
   the observed run, fails **late and opaquely** — after all implementation cost
   has been paid, at the last stage, as a bare `forge resource 400` that names
   neither the conflicting path nor the sibling it collides with. The failure is
   reported (there are explicit merge pauses), but not in a form an operator can
   act on.

## Established vs. assumed

Established:

- Both merge pauses and their identical error text, timestamped above.
- PR #2013 is `CONFLICTING`.
- Slice 0 and slice 2 change exactly the same one path.
- All five slices were created at `20:09:34`; slice 0 merged at 21:03.
- The rebase produces `CONFLICT (add/add)` — tested, quoted above.
- No rebase or branch-update step exists in the slice-merge path.
- Two of the four post-slice-0 slices (1 and 2) have failed at merge, and **both
  were independently confirmed to have the same cause**, not merely the same
  top-level error. Slice 1 (`a62163b0`, PR #2014, `CONFLICTING`) changes the same
  single path and rebases with the same result:

  ```
  $ git diff --name-only aimee/feat/<wi>...s1
  docs/runbooks/appliance-state-recovery.md
  $ git rebase aimee/feat/<wi>
  CONFLICT (add/add): Merge conflict in docs/runbooks/appliance-state-recovery.md
  error: could not apply a62163b0... wfe: impl
  ```

  So the failure is systematic rather than a property of one slice's content.

### Slice-time path data does not exist (checked)

Option 3 was originally recommended here on the assumption that the slicer knows
which paths each slice will own. **It does not.** `exec_split`
(`src/modules/workflows/wfe_blocks.c:1838`) asks the architect delegate for:

```json
{"schema_version":1,"packets":[{"packet_id":"p1","summary":"...",
  "target_blocks":["implement"],"dependencies":[],"acceptance_criteria":["..."]}]}
```

There is no path or file field in the packet schema. Confirming it from the other
direction: `owned_files` appears only in the delegate/roadmap subsystem
(`src/modules/delegates/delegate_plan.c`, `src/modules/roadmap/roadmap_decompose.c`
and neighbours) and **nowhere** under `src/modules/workflows/`. The overlap test
cited earlier is real — `src/tests/test_delegate_plan.c:116`,
`test_plan_flags_overlapping_owned_files` — but it guards the *delegate planner*,
not the WFE slicer. They are different code paths, and the WFE one has no
per-slice path data to check.

So a slice-time rejection would first require extending the packet schema to
carry intended paths and getting the architect delegate to populate them
reliably — a larger change than this proposal originally implied, and one whose
reliability depends on a model's self-declaration.

Assumed, NOT established:

- **That slices 3 and 4 will fail the same way.** They share the base and target
  file, but neither has reached merge yet.
- **Why the planner emitted five slices for a one-file deliverable.** Whether it
  has no overlap check on this path, ignored one, or deliberately split by
  document section was not determined. Option 3's exact form depends on which.
- **Relative frequency of the two gaps.** No count was taken of how often recent
  runs hit add/add-style overlap versus ordinary stale-base conflicts. The
  priority order below rests on the fact that gap 1 is *currently blocking a run*
  and gap 2 is not, rather than on measured frequency. If gap 2 turns out to be
  far more common in practice, that order should be revisited.

## Recommendation

Two separate decisions, deliberately not bundled:

**Take option 3c now. Treat option 3 as desirable but not currently
implementable.**

The earlier draft of this proposal recommended option 3 (reject at slice time).
That recommendation was wrong, and the check above is why: the WFE packet schema
carries no path data, so there is nothing to check at slice time. Recommending it
would have meant recommending a fix that cannot be written.

Option 3c catches the same defect at the first point where the engine actually
has the facts. After `impl`, each slice's frozen diff names the paths it created.
Comparing a freshly frozen slice against its already-frozen siblings is a local
computation on data already in hand, and it fires **as the second colliding slice
freezes** — before that slice reaches `merge`. The operator sees "slices 1 and 2
both create docs/runbooks/…, content differs" instead of a bare forge 400 at the
last stage.

Stated at that scope on purpose: the check is anchored to the per-slice freeze
transition, not to a run-level gate. Whether the engine has a run-level gate
between freeze and merge — which could fail the entire run in one place rather
than per-slice — was not verified, and nothing here depends on it.

It is strictly worse than option 3 in one respect: the implementation cost has
already been paid by the time it fires. It does not prevent five delegates
authoring five versions of one document; it prevents the run from ending in an
unresolvable merge, and it explains why.

Option 3 remains the better long-term shape *if* the packet schema is extended to
carry intended paths. That is a separate piece of work, and its reliability
depends on the architect delegate declaring paths accurately, which is unproven.
It should not be adopted on this proposal's evidence.

Option 3b (reject all ownership intersections) is not recommended: it would
forbid the distinct-region edits this proposal says merge fine, and it faces the
same missing-data problem as option 3.

**Optional, on its own merits — option 1.** Scoped to ordinary shared-base
staleness: a long-running slice whose base moved while it touched different
files. This is a real improvement but it is *not* a fix for this defect, and
should not be adopted as a substitute for option 3.

**Execution order, if both are taken:** option 3c's sibling-freeze check first;
rebase-before-merge applies only to slices that survive it. That ordering matters
— rebasing a slice whose frozen diff already collides with a sibling's just moves
the failure later.

Slice-time rejection is deliberately **not** part of this ordering. It is not
implementable today (see above), and reintroducing it here would bundle the
rejected option 3 back into the decision. If the packet schema is later extended
to carry intended paths, the ordering becomes: slice-time rejection, then 3c as
the backstop for paths the plan did not declare, then rebase.

Option 4 should be resisted as a primary fix: it treats the conflict as an
exception to retry, when for slices that each create the same file the conflict
is the guaranteed steady state.

## Acceptance

For option 3c, the remedy proposed here:

- When two sibling slices in a run have frozen diffs that both **create** the
  same path with **divergent** content, the later freeze is rejected and the
  root run fails at that point, naming the path and the slices involved — before
  that later slice reaches `merge`. A sibling that already merged before the
  later freeze is not rolled back; avoiding that would require the cohort-wide
  freeze/CI barrier this proposal deliberately does not adopt.
- Two sibling slices creating the same path with *identical* content are not
  failed by this rule (git merges them cleanly).
- Slices editing *distinct regions of an existing* shared file are not failed by
  this rule.
- Replaying `wi_f96d4b18…` against the fix fails **when the second slice
  freezes**, naming `docs/runbooks/appliance-state-recovery.md` and the sibling
  it collides with — instead of two PRs left `CONFLICTING`.

  Scoped deliberately to the per-slice freeze transition, which is state the
  engine already holds. Whether a *run-level* gate exists between freeze and
  merge that could fail the whole run at once was **not verified**, so no
  criterion here depends on one. If such a gate does exist, failing the run
  there would be preferable and this criterion should be revisited.
- **The check is atomic with respect to concurrent freezes.** Two sibling slices
  that freeze simultaneously must not both pass by each failing to observe the
  other. This requires serialising the compare-and-record step — a per-run freeze
  lock, a single-writer freeze queue, or equivalent — so that for any colliding
  pair exactly one slice observes the other's recorded paths and fails.
  Without this, the fix is a time-of-check/time-of-use race that the observed run
  did not happen to exercise, because its slices froze minutes apart
  (23:01:43 and 23:21:05).
- Regression coverage: a test for divergent create/create across siblings
  (fails), a test for identical create/create (passes), a test for
  distinct-region edits to an existing file (passes), and a **concurrent-freeze**
  test in which two colliding slices freeze simultaneously and exactly one is
  failed.

Independently of which option is chosen:

- **A merge conflict is treated as terminal, not transient.** A slice whose merge
  fails with a conflict does not retry the identical merge indefinitely; it fails
  with the conflicting paths named, and releases the run's active-root slot.
  (Observed: 15 retries over 3 hours, blocking every other work item behind the
  single `autonomy.concurrency: 1` slot.)

Deferred — follow-up acceptance criteria for whichever implementation is chosen,
not claims about this proposal:

- No run terminates in a state requiring a manual content-merge step, and no
  pipeline PR is left `CONFLICTING`.
- A slice that cannot merge reports the conflicting paths and the conflict type,
  rather than surfacing a bare `forge resource 400`.
- If option 1 is implemented: a slice whose base moved and whose files are
  disjoint from what landed is updated onto the feature head and merges without
  manual intervention.
