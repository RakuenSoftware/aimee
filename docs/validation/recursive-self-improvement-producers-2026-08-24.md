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
| four scratch harnesses on a real stack (see the note below) | all passed |

## One assertion corrected, not worked around

The daemon-only harness still asserted that `learning resolve` reports "No evidence probe
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

The reverse direction holds up too, though the first pass reached that verdict
from naming rather than evidence. Two providers the KB registers are owned by
files `aimee-server` also builds -- `kb_route_acl.c` and
`kb_curator_grounding.c` -- and both were settled the same way as the third
candidate above: `control-web authorization unavailable` and `sidecar temp path
too long to quote safely` appear in `aimee-kb` and are **absent from
`aimee-server`**. The rest are `aimee_db2_*` host contracts, compiled out of the
server by `-DAIMEE_DB2_DISABLED`.

**The learning classifier was the only live instance.**

## What now stops it recurring

The fix had no regression protection: `check-module-placement.py` guards the
neighbouring gap -- a stage no placed module serves -- but not this one, where
the module is placed and present and nothing registers the provider. No test
could have caught it either, because every test registers its own provider and
so can never observe that production does not.

`scripts/check_provider_registration.py` closes that, and runs in `make lint`
(now 64 checks). For each provider some daemon registers through its
module-stage adapter, any daemon that *builds* the file owning that function
pointer must register it too, or record why the code cannot run there. The five
unreachable entries each carry binary-level evidence rather than an argument
from source, and a stale entry fails the check so it cannot outlive its reason.

It was proved against the bug it was written for: deleting the registration line
reproduces the original defect and the check reports it.

## The full environment

The harness that found the classifier gap started two modules. That is part of
why the gap hid: a module that is *granted but never attached* fails exactly
like a module that was never placed. The full-environment harness attaches
**every** module
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
| full environment: all modules, broad exercise, provider sweep | all checks passed |
| producers and gate, both services | all checks passed |
| daemon alone | all checks passed |
| exploratory | nothing broke |
| raw SQL on real Postgres | all statements behave |

## Correction: where this evidence now lives

Every run above was driven by harnesses in a gitignored `src/build/`, named in
an earlier draft of this report as `full_env.sh`, `kb_stack.sh`, `live_e2e.sh`,
`explore.sh` and `pg_check.sh`. **None of those files exist.** The directory was
cleaned during the session and they were lost, so this report cited evidence a
reader could neither run nor find — which is worth stating plainly rather than
quietly renaming.

What replaced them is committed, and covers what they covered:

| Suite | Subject |
| --- | --- |
| [`tests/e2e/module-liveness-pg-e2e.sh`](../../tests/e2e/module-liveness-pg-e2e.sh) | every granted module attaches; no provider is silently null; signal capture and supersession |
| [`tests/e2e/learning-loops-pg-e2e.sh`](../../tests/e2e/learning-loops-pg-e2e.sh) | S0 gate closes on a self-referential ledger and reopens; a closed gate admits nothing and writes nothing; S4 probe; S5 supersession and operator verdict; S6 arm |

Both were run on a real stack (`root@192.168.1.252`, both services, PostgreSQL
17, every granted module attached): **13 passed / 0 failed** and **20 passed / 0
failed** respectively. `module-liveness` is additionally proved against the bug
— deleting the KB registration turns it red on four assertions.

The two areas an earlier draft of this section listed as *not* reconstructed --
the malformed-argument sweep across the learning CLI, and the direct-SQL checks
of the fate ledger -- are now sections 6 and 7 of `learning-loops-pg-e2e.sh`.
Listing them as follow-up was the wrong call; they are covered.

Section 7 also pinned a contract that had been printing as a surprise:
`--budget 0` is not "do nothing". `curiosity_resolve_pass` treats any budget
`<= 0` as unset and substitutes its default, so an operator asking for none
still gets a full pass. That is deliberate, and the suite now asserts it rather
than letting the output read as a command ignoring its argument.

## Limits of this run

- The S4 drain is now exercised in **both** directions. A gap whose subject the
  memory graph covers is closed; the uncovered one beside it stays open, and the
  pass reports `resolved 1 of 2`. The earlier version only ever saw the decline
  path, which a probe hardwired to answer "no evidence" would have satisfied
  just as well -- indistinguishable from the inert version this slice shipped
  with.
- In this 2026-08-24 run the S6 arm came back as the default (`full`). The
  [2026-08-25 six-loop run](learning-loop-evidence-2026-08-25.md) closes that
  limit on a real stack: reward pressure selects and records non-default
  `brief`. That follow-up also makes the two-way S4 proof and all six loop rows
  reproducible through one Make target.
