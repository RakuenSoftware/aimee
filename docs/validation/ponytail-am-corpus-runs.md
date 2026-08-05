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

**Blocker hit on the first attempt — aimee-kb wedged.** Cell 1 failed its
readiness gate with `aimee kb build` returning
`knowledge service /v1/code/build did not respond`. The KB's own log showed only
health checks plus a growing pool warning:

    db2.pool: member 0 leased 964195ms (> 300000ms ceiling)
              by kb/http/kb_tls_serve.c:463 — missed lease_end?

Two members were stuck (0 at ~964s, 1 at ~327s) with the counters climbing
monotonically, so leases taken on the mTLS serve path were never returned. The
pool is 16 connections, so two leaks did not starve it by themselves, but the
service was wedged regardless: no build request ever reached the request log.

`db2_lease_begin()` at kb_tls_serve.c:463 pairs with `db2_lease_end()` at 636
and no `return`/`break`/`continue`/`goto` sits between them, so this is not a
missing release on an error path. Either a handler blocks forever while holding
the lease, or the worker thread leaves without unwinding.

Restarting aimee-kb cleared it; `kb build` then answered in 0.5s. The failure
reproduced on relaunch, and the mechanism is now known.

**Root cause: a client timeout far shorter than the operation it invokes.**

`kb_client_code_post_json()` posts `/v1/code/build` with
`KB_CLIENT_REPAIR_TIMEOUT_MS`, which is `10 * 60 * 1000` — ten minutes
(`modules/kb_client/kb_client.c:783`). A full corpus embed of one am_ task
takes about 3.5 hours: measured here at 320 vectors/min, sustained, against a
~3000-file checkout. So:

1. readiness runs `kb build --force` against a per-cell project (no reuse);
2. the client gives up at 10 minutes and reports
   `knowledge service /v1/code/build did not respond`, failing the cell;
3. the SERVER keeps building, still holding the db2 connection it leased at
   `kb_tls_serve.c:463` — which is the >300s lease that keeps climbing;
4. the next cell orphans another one, until the service stops answering.

So the pool warning is a symptom, not the defect, and the begin/end pair is
correct — the request simply outlives the caller with no server-side
cancellation on client disconnect. Fixing it properly means one of: making the
build asynchronous (submit + poll), cancelling server-side work when the client
disconnects, or sizing the client timeout to the operation. A synchronous
3.5-hour HTTP request is not the answer.

**Consequence for this run:** `full` readiness mode cannot complete on the
current build. R2 therefore runs `PT_AIMEE_MODE=index-only`, which is what the
archived aimee cells used (`readiness_mode: index-only`, `skipped_kb_build:
true`, 2977 files indexed, ~382s/cell). That measures aimee with its code index
and NO embeddings or semantic recall. Any claim from R2 must say so; the
harness records the mode per cell precisely so the two are never conflated.

## R2 blockers — aimee defects found while trying to run the arm

None of these are the tasks failing. They are aimee failing to become ready.

**1. `full` readiness cannot complete.** `/v1/code/build` is posted with a
ten-minute client timeout (`KB_CLIENT_REPAIR_TIMEOUT_MS`,
`modules/kb_client/kb_client.c:783`) while a full corpus embed takes ~3.5h
(measured 320 vectors/min over a ~3000-file checkout). The client gives up; the
server keeps building and keeps its db2 connection. Each cell orphans another.

**2. The wedge that follows.** Orphaned builds hold leases taken at
`kb/http/kb_tls_serve.c:463` well past the 300s ceiling, counters climbing
monotonically, until the service stops answering `/v1/code/build` entirely.
`lease_begin`/`lease_end` are correctly paired -- the request outlives its
caller and nothing cancels server-side work on client disconnect.

**3. A scan can index nothing and only warn.** `aimee index scan` on a runner
cell returned `Scan complete: 1 project(s), 0 file(s) re-indexed` with

    warning: nothing was indexed — knowledge service saw no files at that path
    — it may not be able to read it (aimee-kb runs in its own container and
    does not share the server's filesystem)

The warning is good, but its stated cause is wrong here: the KB container CAN
read the path (verified by `docker exec` -- 50 entries, same as the workspace
that indexed fine). So the real cause is unreported, and readiness then fails
three assertions downstream with no reference to the warning that explains them.

**4. `status` disagrees with the transport the scan uses.** `aimee index scan`
returns `knowledge service unavailable (via local Unix socket (no remote server
configured))` while, seconds earlier and later, `aimee --json status` reports
`kb: {"status":"ok", ...}` and the container is healthy with zero pool warnings.
A green status that does not reflect the path the next command takes is worse
than no status.

**5. WITHDRAWN — this was my error, not a defect.** I claimed re-scanning an
existing project with `--force` indexes zero files. It does not: scanning the
existing project `manualprobe` with `--force` re-indexed 2978 files. The
zero-file scans I saw were the knowledge service being unavailable at that
moment (see 3 and 4), not project state. The KB route already gates its
idempotency skip on `!force`, so `--force` was never the variable. Recorded
rather than deleted because it was published as a finding.

**5a. The real pattern: a heavy scan is followed by unavailability.** A 2978-file
scan succeeds; the next scan of the same tree, seconds later, fails with
`knowledge service unavailable` — with `aimee status` reporting `kb: ok`, the
container healthy, and the KB itself still answering `/v1/health` 200 every 10s
throughout. So the KB is alive and the server's breaker is closed, yet the scan
path cannot reach it. This is what actually blocks the arm: readiness scans one
~3000-file checkout per cell, fourteen times in a row.

Note on (3): `transport_state: "closed"` in status is the circuit breaker
CLOSED, i.e. healthy. I misread it as a dead transport at first; it is not.

Reproduction for 3 and 5a: copy `/opt/bench/amcorpus/corpus/am_1e7cb3da16` to a
fresh path, `chown -R 999:999`, `aimee workspace add`, `aimee index scan
<newname> <path> --force` -> 2978 files, and `index find dstr_append` /
`index callers dstr_append` both answer correctly. `index blast-radius` returns
`{"retryable":true,"dependency":"kb","message":"blast radius lookup failed"}`
even on that good index.

## R2 status: STOPPED, zero cells produced

The aimee arm never produced a single cell. Not one task failed -- readiness
never passed. Where it ended, honestly:

**CT403's scan path is now wedged for every tree, and restart no longer clears
it.** `aimee index scan` returns `knowledge service unavailable` for the
known-good `manualprobe` tree that indexed 2978 files earlier today, on a KB
restarted minutes before, while: both containers report healthy, the server's
socket answers, `aimee --json status` reports `kb: {"status":"ok"}`, the KB
serves `/v1/health` 200 every 10s, and there are ZERO pool warnings since that
restart. Every health signal aimee exposes says fine; the operation fails.

**What is confirmed**

- A scan of a ~3000-file tree succeeds, and a later scan fails; the failure does
  not recover on its own (probed once a minute for five minutes).
- The KB container CAN read the paths it reports as unreadable: `docker exec`
  lists 50-53 entries in the exact cell directories whose scans warn
  "knowledge service saw no files at that path".
- Cell checkouts (git working trees) indexed 0 files while a plain copy of the
  same corpus indexed 2978, across fresh projects, fresh KB, matching ownership.

**What is NOT confirmed, and I am not claiming it**

- That `.git` is the cause of the zero-file scans. It is the one difference I
  isolated, but the service wedged before I could scan the same tree with `.git`
  removed as a first-post-restart scan. Twice. So it stands as the leading
  hypothesis and nothing more.
- Any root cause for the wedge itself. 64 mTLS workers and a 64-slot queue are
  not exhausted, and one stuck member out of 16 db2 connections should not
  starve the pool, so the lease warnings do not explain it either.

**Environment state:** CT403 is degraded. The non-aimee results are on 401/402/403
under `/opt/bench/results/cells` and are unaffected -- they were produced before
any of this and do not depend on the KB.

## Harness environment for the am_ corpus

Not recorded anywhere before this; reconstructed from cell artifacts.

    PT_FIXTURE=/opt/bench/amcorpus/corpus
    PT_HIDDEN=/opt/bench/amcorpus/hidden
    PT_TASKS=/opt/bench/amcorpus/arms/tasks.tsv
    PT_RESULTS=/opt/bench/results
    PT_RUNTIME=/var/lib/aimee-workspaces/bench
    PT_PONYTAIL=/opt/bench/ponytail-upstream
    PT_AIMEE=/usr/local/bin/aimee        AIMEE_HOME=/var/lib/aimee
    PT_SKIP_KB_BUILD=1                   PT_AIMEE_MODE=index-only
    PT_WS_OWNER=999:999
    PT_PROBE_SYMBOL=dstr_append          PT_PROBE_FILE=src/dstr.c
    PT_PROBE_CALLERS=anchor_format_read,ensure_codex_trusted_project_in_config,diff_format_unified
    PT_GRADE_TIMEOUT=2700                PT_RED_TIMEOUT=2700

`PT_AIMEE_MODE` only relaxes the readiness assertions; the build is skipped by
`PT_SKIP_KB_BUILD`. The probe defaults (`app/dates.py` / `end_of_month`) belong
to the synthetic battery fixture and do not exist in this corpus.

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
