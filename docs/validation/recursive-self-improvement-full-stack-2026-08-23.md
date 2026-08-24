# Validation — recursive self-improvement, full stack (aimee-kb + aimee-server)

The two earlier reports
([S0/S1](recursive-self-improvement-s0-s1-2026-08-23.md),
[S2–S6](recursive-self-improvement-s2-s6-2026-08-23.md)) ran with `aimee-server`
up and **`aimee-kb` down**. Everything DB2-backed was therefore covered only by
unit tests and by SQL run through `psql` by hand: S5's regret ledger, S0's gate
reading a populated ledger, and S1's correction-signal source.

This report closes that gap — and closing it found another defect.

- **Commit:** `a143590875` plus its follow-up, on
  `worktree-recursive-self-improvement`, from `origin/testing` at `a2fac47caa`.
- **Environment:** `root@192.168.1.252` (pvetest), Linux 7.0.14-8-pve x86_64,
  8 cores, 31 GiB RAM, **PostgreSQL 17.11 with pgvector 0.8.0 and pg_trgm**,
  gcc 14.2, clang-format 19.1.7, ripgrep 14.1.1. Scratch tree under
  `/root/rsi-kb`; the database `aimee_shared` did not exist beforehand and was
  created for this run.
- **Date:** 2026-08-23.

## The stack that was actually running

- **`aimee-kb`** against real PostgreSQL, with its own config module on its own
  bus, listening on `127.0.0.1:18743`. It applied the full DB2 schema itself —
  including `learning_proposal_fate`, which is how that table is confirmed to
  work in the real service rather than in a shim.
- **`aimee-server`**, with its own config and db1 modules, its own
  `AIMEE_HOME`, and `AIMEE_KB_API_URL` pointing at the KB.
- The thin client against the daemon's socket.

`approach_failures` is confirmed **absent** from DB2, as it must be after
moving to DB1.

## The defect this found

**S0's endogeneity gate was inert in the daemon.**

The gate reads the learning ledger, which is DB2. The daemon builds with DB2
compiled out, so `learning_gate_check()` took the compiled-out branch and
returned OPEN unconditionally. It was a no-op at the one place it is enforced,
`eval_synthesis_admit_pending()` — a loop whose evidence had become entirely
self-referential would still have been allowed to widen its own yardstick,
which is the single thing S0 exists to prevent.

The symptom was unmissable once both services ran: four committed proposals and
three recorded fates in Postgres, and the daemon reporting *"no settled
proposals yet."*

This is the **third defect of the same shape** in this work — a cross-tier
assumption that does not survive the real process layout — after S3's store
placement and the recall-vs-error confusion. Two of the three were found only
by standing services up.

### The fix, and two corrections to it

The gate is now answered where the ledger lives: a `learning.endogeneity` route
on the KB, reached through `kb_client`. Two things were wrong on the first
attempt and were corrected before this run:

1. The kb_client action helper **never returns NULL** on a transport failure —
   it returns an error document. The "unreachable" branch was dead code, and
   every failure would have been read as a definite answer. Detection now keys
   on the absence of a `gate` field.
2. The `eval.candidates` listing still computed the gate locally, so one
   surface reported "no settled proposals yet" while the other consulted the
   KB. One surface claiming a measurement the other does not make is worse than
   either alone. Both now ask the same question.

**Reachable and closed stops admission. Unreachable does not**: no reachable
ledger means nothing is being committed into one, so there is nothing
self-referential to guard against, and refusing would make admission depend on
the KB being up for a feature that otherwise does not need it. An unanswered
gate reports `unavailable`, never `open` — an operator must be able to tell a
measured gate from an absent one.

## What the full stack proved

With both services up, the daemon pointed at the KB, and the ledger manipulated
directly in Postgres:

| Check | Result |
| --- | --- |
| the daemon sees the KB ledger | `open (75% of 4 committed proposals exogenous)` — matching `psql` and the KB's own answer |
| a wholly self-referential ledger (25 implicit-detector commits) | `closed (0% of 25 committed proposals exogenous)` |
| a fully reproduced candidate, gate closed | `0 admitted`, and **no task file written** |
| the same candidate, gate reopened | `1 admitted` |
| S5 regret per detector, live in DB2 | attributed to the right detector, prefix-trap fate excluded |
| S1's correction-signal source, live in DB2 | selectable |
| both services after every probe | still running, no crash in either log |

That closed-gate row is the assertion that was impossible to make before: S0
enforcing itself, end to end, against a real ledger.

## Everything else, re-run on a clean build

| Command | Result |
| --- | --- |
| `make -C src -j8 all` | exit 0 |
| `make -C src -j8 unit-tests` | 734 tests; 1 failure — `unit-test-mcp-git`, pre-existing (proven earlier on this host against the base tree) |
| `make -C src lint` | `lint: all 63 checks passed` |
| `make -C src docs-gen-check` | ok |
| `make -C src integration-tests` | `integration: 115/115 passed` |
| live end-to-end (daemon only) | all checks passed |
| exploratory probing | nothing broke |
| Postgres SQL check | all statements behave |
| full stack (aimee-kb + aimee-server) | all checks passed |

Every row above was re-run on the **final commit** (`f899a9808f`) from a clean
build in a freshly stood-up environment, including a fresh `aimee_shared`
database. `integration-tests` in particular had previously only been run
*before* the three gate-fix commits; that gap is now closed.

One intermediate run reported two extra failures (`unit-test-cli-server-compat`,
`unit-test-server-compute`). Those were an artefact of copying a stale build
cache onto the host to save a rebuild: the same commit passed locally, and a
**clean** build on the host passed too. The lesson is recorded rather than
hidden — do not carry a build cache across a source change.

The daemon-only live suite also had one stale expectation of my own: it asserted
the gate reports `open` with no KB running. That is now `unavailable`, which is
the point of the change, so the check was updated to assert the honest answer
and the reason string.

## Cleanup

Removed and verified: the scratch tree `/root/rsi-kb` (both services' homes,
their databases, and every process, stopped by each harness's trap), the
`aimee_shared` PostgreSQL database and the `root` role created for it, the
`aimee-ka-rsi-kb` keepalive unit, and the three packages installed for this run
(`postgresql-17-pgvector`, `clang-format-19`, `ripgrep`), which were purged.
