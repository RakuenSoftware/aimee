# Validation — recursive self-improvement, the producing halves

The [full-stack report](recursive-self-improvement-full-stack-2026-08-23.md)
proved the S0 gate against a real ledger with both services up. It did not
prove that anything ever *writes* the rows the other slices read. Three of them
shipped with the consuming half built and the producing half absent, and this
report covers the run that fixed that and proved each producer fires.

- **Commit:** `ec8ac446ae` (the fix) and `9a26a9e6f2` (this record) on
  `agent/fact-authority-lifecycle`.
- **Environment:** `root@192.168.1.252` (pvetest), Linux 7.0.14-4-pve x86_64,
  PostgreSQL 17 with pgvector and pg_trgm, gcc 14.2, go 1.24.4. Scratch tree
  under `/root/rsi-fix`, then a second and fuller one under `/root/rsi-env`;
  the database `aimee_shared` did not exist beforehand and was created for
  these runs. Everything created was removed afterwards.
- **Date:** 2026-08-24.

## The defect this run found

Signal capture is served by `aimee-kb` — that is where the learning tables
live. The router it calls needs a *signal classifier* to decide which sinks a
signal reaches, and only `aimee-server` ever registered one. In the KB the
pointer was null, so every signal was refused:

```
WARN  learning: signal classification unavailable; refusing signal type=mark_rule
POST /v1/actions/learning.propose_signal -> 200
      {"status":"error","message":"failed to record learning signal"}
```

The route answered 200 and wrote nothing. Signal ingest through the KB had
never worked, which is why S5's supersession producer — which lives on the
router's commit path — could not fire in a real deployment no matter how
correct its own code was.

This is the same shape as the gap `scripts/check-module-placement.py` was
written for, and it took the same fix: place the `learning` module in the KB as
well as the server, add the adapter, register it. The check now covers this
stage too (77 placed stages, 0 known gaps), so the gap cannot reopen silently.

## What the stack was

- `aimee-kb` on real PostgreSQL, listening on `127.0.0.1:18743`, with its
  **config and learning modules attached to its own bus**. The Go modules share
  one host binary and take their identity from `argv[0]`, so `learning` is
  deployed by copying that host under its own name next to its grant.
- `aimee-server` with its own config and db1 modules and `AIMEE_KB_API_URL`
  pointing at the KB.
- The thin client against the daemon's socket.

## The producers, each proved by its own effect

| Slice | What had to happen | Observed |
| --- | --- | --- |
| S5 supersession | a second commit to a target marks the earlier one superseded, unasked | `propose_signal -> {"status":"ok","dispatch":{"signal_id":1,"proposal_ids":[1],"committed_ids":[1]}}`, and proposal 8001's fate became `superseded` |
| S5 operator verdict | a verdict the router cannot infer is recorded through the daemon | `aimee learning fate 8001 contradicted` → fate `contradicted` |
| S6 arm selection | the service answers with an arm it declares | `{"decision_point":"plan_advisory","arm":"full","default_arm":"full"}` |
| S4 backlog drain | a real probe runs and reports a real pass | `resolved 0 of 5 considered (budget 5)` — five gaps genuinely uncovered, so leaving them open is the correct answer, and the "no probe installed" refusal is gone |

The S5 rows were read back with `psql` against `learning_proposal_fate`, not
from the process that wrote them.

## Suites run on this commit

| Suite | Result |
| --- | --- |
| `make -j8 all` | clean, `-Werror` |
| `make unit-tests` | all pass |
| `make lint` | 63/63, including `check-module-placement` |
| `make docs-gen-check` | ok |
| `make integration-tests` | pass |
| `kb_stack.sh` (both services, real Postgres) | all checks passed |
| `live_e2e.sh` (daemon alone) | all checks passed |
| `explore.sh` (exploratory) | nothing broke |
| `pg_check.sh` (raw SQL on real Postgres) | all statements behave |

## One assertion corrected, not worked around

`live_e2e.sh` still asserted that `learning resolve` reports "No evidence probe
is installed". That was true when S4 ran in the daemon; S4 now runs in the KB,
and this suite runs the daemon alone, so the honest answer is that the
knowledge service is unreachable. The check now asserts exactly that, and fails
if the command ever claims a real pass with no service running.

## The sweep for siblings of the same defect

One null provider found by accident is a reason to look for the others, not a
reason to assume it was alone. The daemon registers 37 providers and the KB 15;
36 are server-only. For each, the question is whether the file that owns the
static pointer is also built into `aimee-kb` -- if it is, the same silent-null
failure is possible there.

Three candidates came out of that comparison, and each was resolved with
evidence rather than reasoning:

| Candidate | Verdict |
| --- | --- |
| `ws_scope_register_ref_validator` | `workspace_scope.c` is linked into the KB, but its only consumers (`git_project.c`, the webuser files) are not. Unreachable. It also fails **closed** -- a null validator rejects every ref -- so it could not have been a silent accept. |
| `wfe_advance_register_decision_provider` | `wfe_advance.c` is linked, but `wfe_engine.c` and `wfe_advance_exec.c` are not in the KB build. Unreachable. |
| `agent_tools_register_classifier` | The strongest candidate: `posix/agent_runtime.c` *is* in the KB build and calls `dispatch_tool_call_ctx`. Settled at the binary: strings unique to that path (`error: spill store unavailable`, `git tools are not available on this surface`) are present in `aimee-server` and **absent from `aimee-kb`** -- the linker's `--gc-sections` dropped the whole path because nothing in the KB reaches it. |

The reverse direction is clean too: every provider only the KB registers is an
`aimee_db2_*` host contract, compiled out of the server by
`-DAIMEE_DB2_DISABLED`, or a `kb_`-prefixed provider with no server consumer.

**The learning classifier was the only live instance.**

## The full environment

The harness that found the classifier gap started two modules. That is part of
why the gap hid: a module that is *granted but never attached* fails exactly
like a module that was never placed. `full_env.sh` attaches **every** module
each daemon is granted and has a binary for -- 7 on the KB (config, learning,
memory, postgres, kb-synthesis, control-web, benchmarks; `db2` is granted but
deliberately not started in the KB image) and 17 on the server -- reports any it
could not start instead of skipping it quietly, drives the product across its
surfaces, and then sweeps both logs for the signature of this defect class:

```
classification unavailable | provider not registered/unavailable/missing |
no provider | not registered | stage <x> unserved | verdict=TRANSPORT |
module call failed | grant rejected/refused/invalid
```

Result: **no hits in either log**, all modules still attached, both services
alive, nothing crashed, and signal capture answering
`{"status":"ok","dispatch":{...,"committed_ids":[...]}}` with the supersession
recorded unasked.

Two things that run found, both mine rather than the product's:

- The KB reported `db2_ok:false` and published the blocker *"store unavailable:
  the KB database schema is not ready, so nothing can be stored or retrieved"*
  while it was visibly storing and retrieving. Cause: the `postgres` module
  reaches DB2 by `AIMEE_DB2_URL`, not by the libpq defaults the KB itself uses,
  and the harness had not set it -- an under-configured environment, not a
  defect. With it set, `db2_ok:true`, `db2_kb_tables_ok:true`.
- Two probes named commands that do not exist (`learning list`,
  `approaches`). A probe that answers "unknown command" proves nothing about
  the system, so the harness now fails on that answer instead of printing it.

The remaining blocker in this environment is `no embedder configured`, which is
the correct answer here -- nothing embeds. The run asserts that it is the *only*
blocker rather than asserting overall `status: ok`, because calling this
environment healthy would be the dishonest reading.

## Suites re-run against the full environment

| Suite | Result |
| --- | --- |
| `full_env.sh` (all modules, broad exercise, provider sweep) | all checks passed |
| `kb_stack.sh` (producers, gate, both services) | all checks passed |
| `live_e2e.sh` (daemon alone) | all checks passed |
| `explore.sh` (exploratory) | nothing broke |
| `pg_check.sh` (raw SQL on real Postgres) | all statements behave |

## Limits of this run

- The S4 pass resolved nothing, because the seeded gaps are genuinely
  uncovered. That proves the probe runs and reports honestly; it does not
  exercise the path where a gap is closed by found evidence.
- The S6 arm came back as the default (`full`). The sampler answered, which is
  what was inert before; arm *selection under reward pressure* is covered by
  unit tests, not by this run.
