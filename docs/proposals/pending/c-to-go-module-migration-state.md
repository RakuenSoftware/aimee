# C to Go module migration: what is left, and why

- **State:** NOTES, 2026-08-06. Not a proposal; a record of measurement so the
  next pass does not repeat the search.
- **Context:** the modularization suite's endpoint is that module logic lives in
  Go, runs as its own process, and is reached over the event bus. The C core
  keeps the bus and what is needed to keep things running.

## Where the migration actually stands

Measured per module (C lines still in `src/modules/<id>`, Go lines in
`server-go/modules/<id>`, stages served):

| | C | Go | stages |
|---|---|---|---|
| Effectively migrated | | | |
| `runtime-web` | 38 | 96 | 1 |
| `control-web` | 131 | 204 | 1 |
| `response-composition` | 101 | 121 | 1 |
| `governance` | 185 | 148 | 1 |
| Partially | | | |
| `sandbox` | 764 | 472 | 2 |
| `roundtable` | 4,723 | 2,056 | 2 |
| Barely started | | | |
| `memory` | 22,173 | 58 | 5 |
| `delegates` | 12,883 | 919 | 1 |
| `git` | 12,000 | 110 | 3 |
| `workflows` | 10,507 | 365 | 3 |
| `kb-synthesis` | 6,231 | 217 | 1 |
| `workspace` | 6,036 | 81 | 1 |
| `tools` | 5,114 | 87 | 1 |

Across the 17 process modules: ~90,000 lines of C against ~5,500 of Go.

## The two migrations done here, as the pattern

`git-ci-grade` (event 7427) and `workflow-gate-decision` (event 9219). Both
followed the same shape, which is worth repeating:

1. Port the rule to Go with its tests ported **one-for-one from the C suite**, so
   the migration is pinned to the behaviour it replaces.
2. Declare the stage in `process-contracts.json`, the module descriptor, the
   dispatch table, and the registry-vs-contracts test.
3. Cut C over **in the same commit**. Never leave the rule in two languages: the
   roundtable migration's own comment records what that cost last time, when a
   C-side guard had no effect on reviews because Go held the real settings.
4. Keep in C anything that is substrate (a hash), a hot-path comparison, or
   needed before the request can even be built (`wfe_gate_effective_quorum`).

## Why the next one is not obvious

A candidate needs five properties. Scoring 1,353 exported functions: 94 pass
markers plus returns-a-value plus plain-args; 54 of those are not called in a
loop. But sampling the top of that list, every remaining substantial candidate
fails on something the scan cannot see:

| Candidate | Lines | Why not |
|---|---|---|
| `delegate_xml_fallback.c` | 1,041 | Highest value (parses untrusted model output in C) but only **31 assertions** cover it. Too thin to port faithfully. |
| `wfe_def_validate` | 436 | Consults the custom-block **registry**: runtime state, not arguments. |
| `rtp_chunk_plan` | 135 | Its FNV-1a hash is a **shared primitive** used in four other places; moving it puts the hash in two languages. |
| `memory_effective_importance` | 185 | Pure and returns a value, but called **per row** in listing loops. Needs a batch contract first. |
| `tool_egress_for` | 163 | A static declaration table consulted per tool invocation. A bus round trip is a regression for pure data. |
| `kb_curator_judge_same_entity` | 193 | Spawns a shell sidecar. |
| `rt_resolve_seat_model` | 180 | Takes `agent_config_t`: the agent registry. |
| `learning_implicit_*` | 127 | Emits through the router; no return value. |
| `wfe_autonomous_selectable` | 11 | Genuinely pure, but a stage costs more than it saves. |

So the remaining work is **state-relocation decisions**, not a search. The
question for each is "what state moves with it", and that is a design call.

## Dead code: what the instruments cannot tell you

Five methods were tried for "which C sources reach a shipped binary". Four were
wrong, each differently, and two produced confident nonsense:

| Method | Failure |
|---|---|
| `nm` on the binaries | They link `-s`. Every symbol reads absent, live ones included. |
| `make -n` | On an up-to-date target it prints "is up to date" and emits no link line. The object set silently fell 1214 -> 44 and reported `vault_service.c` as unshipped. |
| Single object prefix | Sources compile to `build/obj/<s>.o`, `build/obj/server/<s>.o`, `build/obj/kb/<s>.o` or `build/obj/posix/<s>.o`. |
| `-Wl,-Map` link map | **LTO**. `-flto` replaces per-object attribution with `ccXXX.ltrans*.o`, so the map cannot attribute anything. |
| `make -n -B` + all prefixes | Best available, but blind to sources compiled straight to a binary with no object at all (`aimee_forwarder.c`), and to the CLI's `/v1` route dispatch. |

Two rules came out of this:

- **Assert before reporting.** Every scan must check that a list of known-live
  files appears, and refuse to emit results otherwise. The two nonsense runs both
  happened in scripts where that assertion had been dropped.
- **Referenced is not reachable.** `cmd_guardrails` is named in `cmd_table.c` and
  `delegate_read_prompt_stdin` is called from `cmd_agent_delegate.c`, which looks
  like proof of life. Neither caller is linked, and `./aimee guardrails` answers
  "Unknown command". Being called by dead code does not make a file live.

What was actually deleted: four sources plus one header, confirmed by removing
them and watching the build, lint and full test suites still pass.

## Open, and needing a decision rather than a search

- **The 16 `module_adapter.c` files.** Unshipped (every module process is Go:
  `17 processes, 17 Go, 0 C`) and they duplicate Go policy with nothing keeping
  the two in sync -- `learning_signal_policy.c` restates `learning.Handle` rule
  for rule. But **9 test targets link them as in-process module doubles**
  (`test_kb_route_acl.c` calls `aimee_control_web_module_handler` directly), so
  removing them is a test-infrastructure refactor, not a deletion.
- **The CLI `cmd_*.c` surface.** `./aimee review` answers "command 'review' has
  no /v1 route", suggesting the CLI is a thin client dispatching over `/v1` while
  local command implementations may be legacy. Whether those files are dead is an
  architectural question about dispatch, not a tooling one.
- **`memory`** is the largest single body of unmigrated C (22,173 lines) and the
  one whose migration would need the kb-over-bus decision first.
