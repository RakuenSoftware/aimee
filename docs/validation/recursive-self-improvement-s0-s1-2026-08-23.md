# Validation: recursive self-improvement S0 + S1

Proves the S0 (endogeneity gate) and S1 (regression synthesis) slices of
[`recursive-self-improvement-closing-the-loops`](../proposals/pending/recursive-self-improvement-closing-the-loops.md).

- **Commit:** `7f49f2f88b`. "feat: grow the eval suite from live failure, and bound the recursion",
  on `worktree-recursive-self-improvement`, branched from `origin/testing` at `a2fac47caa`.
- **Environment:** `root@192.168.1.252` (pvetest), Linux 7.0.14-8-pve x86_64, 8 cores,
  31 GiB RAM, gcc (Debian 13), git 2.47.3, sqlite3, clang-format 19.1.7, ripgrep 14.1.1.
  Built and run from a scratch copy of the commit under `/root/rsi-verify`, in a
  private `HOME`/`AIMEE_HOME`; removed afterwards (see Cleanup).
- **Date:** 2026-08-23.

## Commands and results

| Command | Result |
| --- | --- |
| `make -C src -j8 all` | exit 0 |
| `make -C src -j8 unit-tests` | 729 tests; 1 failure: `unit-test-mcp-git`, **pre-existing** (see below) |
| `make -C src lint` | `lint: all 63 checks passed` |
| `make -C src docs-gen-check` | `docs-gen-check: ok` |
| `make -C src integration-tests` | `integration: 115/115 passed` |
| `build/obj/tests/unit-test-learning-eval-synthesis` | `ok` |
| `build/obj/tests/unit-test-eval-candidates` | `ok` |
| live end-to-end harness (below) | `live-e2e: all checks passed` |

### The one failure is pre-existing, and was proven so on this host

`unit-test-mcp-git` fails at `test_git_pr_auto_merge_accepts_pending_checks_without_claiming_merge`
(`tests/test_mcp_git.c:1343`, `strstr(text, "--auto")`). It passes on the
development machine and fails on this host, and it is in git PR tooling that
this change does not touch.

To be sure rather than plausible, the **base tree** (`a2fac47caa`, this change
absent) was extracted to the same host, built, and the same test run under the
same env (`AIMEE_TEST_MODULE_BIN` set as the make rule sets it). It aborts on
the identical assertion at the identical line. The failure is a property of this
host, not of this change.

## Live end-to-end run

A real `aimee-server` with the `config` and `db1` module processes attached over
the module bus, an isolated `AIMEE_HOME`, and the thin client pointed at it via
`AIMEE_API_ENDPOINT=unix:…`. Every step below went CLI → `/v1` → dispatch →
handler → DB1 module → sqlite, and back.

**The surface**

- `aimee eval candidates` on a fresh installation: empty list, and
  `admission gate: open`. An empty window reports "nothing observed", not
  "wholly endogenous", so a new installation can bootstrap.
- `--state` filter honoured; `admit` without `--suite-dir` refused; `reject`
  without `--id` refused; `reject --id 4242` reports no such candidate; an
  unknown op and a missing op are both refused.

**The loop**

1. Seeded the real `agent_jobs` ledger with two independent `failed` jobs sharing
   one failing prompt, plus a `done` job as a control.
2. `eval candidates-update scan` → read exactly the 2 failures (the successful
   job was not treated as a defect).
3. `eval candidates` → one quarantined candidate; the two failures collapsed to a
   single signature.
4. `eval candidates-update admit --suite-dir …` → `1 admitted`.
5. `regression-aa5686d408dc.json` appeared in the suite directory and parsed with
   the failing prompt, `role`, `max_turns`, **no fabricated `success_check`** (a
   failed job states what broke, not what success looks like), and
   `provenance.origin=agent_job`.
6. A second `scan` left the observation count at 2. A repeated sweep cannot
   manufacture its own reproduction.
7. `retire` with no recorded result → `0 retired`: never retired for lack of
   evidence.
8. Three recorded passes → the task file was removed from the hot suite and the
   row moved to `archived`.

**Not covered by the live run:** `eval run` refuses before loading when no agents
are configured, so it cannot serve as proof that the harness loads a synthesised
file. That claim is asserted directly instead, in
`unit-test-eval-candidates`, which calls the real `agent_eval_load_tasks()` over
the materialised suite directory and checks the parsed `eval_task_t`.

## Cleanup

The host is temporary test infrastructure and everything this run created was
removed: the scratch tree `/root/rsi-verify` (including the live server's
`AIMEE_HOME`, its sqlite database, and the server/module processes, which the
harness stops on exit via a trap), the `aimee-ka-rsi-verify` keepalive unit, and
the two packages installed for the checks (`clang-format-19`, `ripgrep`), which
were purged. Removal was verified after the fact.
