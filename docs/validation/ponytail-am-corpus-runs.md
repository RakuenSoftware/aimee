# am_ corpus runs — record for the article

Every run recorded here so code, tests and cost can be compared across arms
later. One row per cell; the harness retains the patch, the changed files, a
tree-hash manifest, the hidden-test result, static quality metrics, and
token/turn/cost accounting under `/opt/bench/results/cells/<arm>__<task>__r<n>/`.

## Corpus

14 tasks, `/opt/bench/amcorpus`, on CT401/402/403 at `192.168.1.252`.
Each task is `am_` + the first 10 hex of a real aimee fix commit; the candidate
checkout is the repo at that commit's PARENT, upstream tests injected only at
grade time, the non-test diff kept as the reference patch.

## Pins (identical across every arm)

| | |
|---|---|
| codex | codex-cli 0.146.0, `gpt-5.6-sol`, reasoning medium |
| ponytail | 4.8.4 / `16f29800fd2681bdf24f3eb4ccffe38be3baec6b` |
| instructions sha256 | `da4fb09cff2f6726691ce6591cebc38c95597d79da132e49c6fa2665c4e8` |
| addon hook == instructions | true (the ablation is the addon machinery, not the text) |
| native codex subagents | disabled in every arm |

## Runs

### R1 — non-aimee baselines, n=1 (2026-08-05)

13 of 14 tasks x {baseline, ponytail-instructions, ponytail-addon}. Lanes kept
per task so a task's arms never straddle boxes (per-box cost variation is real).

baseline 7/13, ponytail-instructions 8/13, ponytail-addon 9/13.

Separating tasks: `am_270b3483d5` (addon only), `am_67e9b0449a` (both ponytail
arms, not baseline). All-fail: `am_b84c9294aa`, `am_1e7cb3da16`,
`am_12b43fa38e`, `am_842ff35656`.

`am_a7f183fd10` initially produced no cells: its hidden test is a full C build
(`PT_BUILD_TIMEOUT=2400`) and the harness graded with `PT_GRADE_TIMEOUT`
defaulting to 30s, so every arm was killed mid-grade and recorded a dead cell
rather than a result. Re-run with `PT_GRADE_TIMEOUT=2700`: all three arms
complete and PASS (wall 148-221s). The patches never hung; the grade budget was
simply smaller than the task's build. Any build-graded task needs this raised.

With that task included the non-aimee baseline is 14/14 tasks:
**baseline 8/14, ponytail-instructions 9/14, ponytail-addon 10/14.**

### R2 — aimee arm, n=1 (2026-08-05)

Aimee build under test: branch `agent/roundtable-review-bus`, commit
`318e977383`, image `aimee-server:rt17`, recorded client sha256
`b8478e75c55f9d92296723914d6f6968c517641098aff050238c163770cb7873`.
Run on CT403 only — the sole box with configured delegates.

Prior aimee cells (an older build) were archived to
`/opt/bench/results/archive-aimee-20260805/` and are NOT part of this result.

Results: pending.

## Per-cell metrics captured

`cell, arm, task, hidden_ok, compile_exit, smoke_exit, wall_s, credits,
production_added/deleted, test_added/deleted, files, tool_calls, events,
input/cached/output tokens, max_control_nesting, branch_nodes,
bare_except_handlers, mutable_default_arguments, production_files, test_files,
pre_head, model`

Collected by `.synctmp/stage/collect.sh` into `cellmetrics.jsonl`.

## Caveats to carry into the article

- n=1 everywhere. No confidence intervals; single-task flips are inside noise.
- The aimee arm is not a like-for-like prompt comparison — it has machinery the
  others do not. Gated on the same hidden tests regardless.
- Cost is provider-side: `estimated_credits` is a pure function of the codex
  token counts (uncached input, cached input, output) at the rates pinned in
  provenance. It does not depend on which box ran the cell, so cost comparisons
  are lane-matched by construction even when a task's arms ran on different
  containers. `wall_seconds` is the exception — that is local machine time and
  should not be compared across boxes.
