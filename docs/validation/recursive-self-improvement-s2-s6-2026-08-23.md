# Validation — recursive self-improvement S2 to S6

Proves the S2–S6 slices of
[`recursive-self-improvement-closing-the-loops`](../proposals/pending/recursive-self-improvement-closing-the-loops.md).
S0 and S1 were validated separately in
[the earlier report](recursive-self-improvement-s0-s1-2026-08-23.md).

- **Commit:** `b8c090439d` — "fix: move approach memory to DB1, where the daemon
  can actually reach it", on `worktree-recursive-self-improvement`, branched
  from `origin/testing` at `a2fac47caa`.
- **Environment:** `root@192.168.1.252` (pvetest), Linux 7.0.14-8-pve x86_64,
  8 cores, 31 GiB RAM, gcc 14.2, git 2.47.3, sqlite3, **PostgreSQL 17.11**,
  clang-format 19.1.7, ripgrep 14.1.1. Built and run from a scratch copy under
  `/root/rsi-s2s6`, in a private `HOME`/`AIMEE_HOME`; removed afterwards.
- **Date:** 2026-08-23.

## Commands and results

| Command | Result |
| --- | --- |
| `make -C src -j8 all` | exit 0 |
| `make -C src -j8 unit-tests` | 734 tests; 1 failure — `unit-test-mcp-git`, pre-existing (proven in the S0/S1 report by building the base tree on this host and reproducing the identical assertion) |
| `make -C src lint` | `lint: all 63 checks passed` |
| `make -C src docs-gen-check` | ok |
| `make -C src integration-tests` | `integration: 115/115 passed` |
| the seven new unit tests, individually | all pass |
| Postgres SQL check (below) | all statements behave |
| live end-to-end (below) | all checks passed |
| exploratory probing (below) | nothing broke |

## Postgres, not just the sqlite shim

Every DB2 query this work added had only ever run against the test shim, which
is forgiving in ways Postgres is not. A scratch database on the host's real
PostgreSQL 17.11 ran the exact statement text the C issues:

- **S5** — the fate upsert REPLACES rather than duplicating; the latest verdict
  wins; and the delimited `LIKE` that classifies regret does **not** match by
  prefix. A `reverted_by_operator` row was planted specifically to catch that,
  and is correctly excluded while `reverted` and `superseded` count.
- **S0** — committed proposals group by `(source, signal_type)` over the
  `pg_now_text()` window.
- **S1** — negative signals carrying a correction are selected correctly.

One failure here was mine, not the code's: the first run asserted 3 regret rows
where the seeded data contains 2. Got 2, which was right. The property under
test — the prefix exclusion — had passed.

S3's store is no longer DB2 (see below), so its SQL is sqlite and is covered by
`unit-test-approach-memory`.

## Live end-to-end

A real `aimee-server` with the `config` and `db1` module processes on the bus,
an isolated `AIMEE_HOME`, and the thin client pointed at it. Every step went
CLI → `/v1` → dispatch → handler → DB1 module → sqlite and back.

- **S1** (regression loop) — unchanged from the earlier report and still green.
- **S2** — a seeded ablation grid attributed correctly: `no_rescue` reported as
  "removing it cost us" (+1.000 over 3 paired tasks), `no_retry` as "no measured
  effect", and an arm run only on a task the baseline never saw as "not enough
  paired runs" rather than as a finding.
- **S3/S6** — the dead end recorded by the failure scan is recalled for the same
  goal worded differently, rendered under the `full` arm, reporting rather than
  instructing; an unrelated goal recalls nothing.
- **S4** — the backlog drain reports that no evidence probe is installed and
  closes nothing, rather than reporting a clean pass.

### Two defects the live run caught

Both were in this work, and both were found only because the run used a real
server rather than unit tests.

1. **Recall reported an error where the honest answer was "nothing known."**
   The store layer returned `-1` both for an unreachable DB2 and for a genuine
   query failure, so recall could not tell them apart. Fixed by distinguishing
   the two; a regression test closes the shim and asserts silence.

2. **The store was in the wrong tier, which made S3 inert in the shipping
   binary.** After the first fix the answer was still "nothing known" for a
   dead end just recorded. The cause was structural: `approach_failures` was in
   DB2, but both its writer (the failed-job scan) and its reader (the plan-time
   route) run in the daemon, **which builds with DB2 compiled out**. The feature
   could never work where it was used — exactly the self-contained island the
   liveness rule forbids.

   The rows belong in DB1 regardless: they are this machine's observations about
   its own failed jobs, sourced from `agent_jobs`. Moving them also forced the
   right shape, splitting the pure scoring (learning module) from the storage
   (`src/approach_store.c`), since a module may not reach a peer store directly.

   The second defect was found by **re-running the live suite after the first
   fix instead of assuming it landed.**

## Exploratory probing

Beyond the scripted path, against the same live server. Nothing crashed, the
server survived every probe, and its log records no fault.

- **Hostile input** — prompt-injection markers (`<|im_start|>`), ANSI escapes,
  SQL metacharacters (`'; DROP TABLE approach_failures;--`), a goal of only
  stopwords, a 4 KB goal, and a goal that is one newline. All answered normally;
  the table still exists afterwards.
- **Wrong types and absurd numbers** — negative, enormous, and non-numeric
  budgets; negative and 11-digit candidate ids; negative and 100000 limits; an
  unknown state filter; `--min-occurrences 0`. All refused or clamped.
- **Ordering** — attribution before any grid exists says the counterfactual was
  never run; retire before anything is admitted returns 0.
- **Degenerate grids** — a single paired task is reported as "not enough paired
  runs" rather than as evidence; a grid where *everything* failed on both arms
  blames no capability.
- **Determinism** — two identical attribution calls return identical output.

One reported "process died" was a false positive in the probe script itself: it
grepped for the word `assertion`, and the S4 message legitimately contains "by
assertion". The detector was narrowed to real crash signatures.

## Cleanup

Everything this run created was removed: the scratch tree `/root/rsi-s2s6`
(including both live servers' `AIMEE_HOME`s, their sqlite databases, and the
server/module processes, which each harness stops on exit via a trap), the
scratch PostgreSQL database (dropped by the check's own trap), the
`aimee-ka-rsi-s2s6` keepalive unit, and the two packages installed for the
checks (`clang-format-19`, `ripgrep`), which were purged. Removal was verified
afterwards.
