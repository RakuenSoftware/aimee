# Core modularization slice 18: optional execution and quality documentation

## Diff scope precondition

The slice-start commit is `185ce22539168243fedc0509aca853621f2ba243`. Allowed close paths are the
three module documents, this validation record, the documentation-status partition, and the cleanup
ledger. Production source, descriptors, build/package files, schemas, GUI, generated artifacts,
checkers, tests, and the deferred `control-web`, `governance`, and `runtime-web` documents are excluded.

## Outcome and method

This slice promotes exactly optional `workflows`, `roundtable`, and `benchmarks`. Three documentation
debts remain: `control-web`, `governance`, and `runtime-web`. Inspection covered descriptors, physical
inventory, headers/entry points, config parsing/defaults, build lists, registrations/startup, routes,
callers, stores, tests, benchmark corpora/scripts, and prior proposals. Aimee memory and index were
queried first; the configured index points at a different workspace and returned no symbol matches, so
nearby source inspection supplied the evidence.

Static references prove compilation, registration, and callers, not deployed activation. Runtime
liveness remains a hypothesis, unverified unless an explicit reachable path is cited. Candidate labels
do not confirm dead code, and discrepancies are deferred rather than fixed in this docs-only slice.

## Inventory and activation evidence

### Workflows

`src/modules/workflows` contains 45 C/header files plus `module.yaml`, including definition,
validation, engine, advancement, approval, autonomy, scheduler, providers, trigger block, roundtable
gate, and orchestration-seam implementations. The descriptor declares ten dependencies,
`enabled_by_default: false`, and no runtime toggle. `config.c:623` defaults `module_workflows` to
inherit/unset and `config_sections.c:747` parses `modules.workflows`; `trigger_scheduler.c:512` resolves
that gate before `gw_orch_workflows_run`. Yet `server.c:2302` calls `wfe_autonomy_register` and
`:2330` calls `wfe_scheduler_init` unconditionally. Current liveness is compiled, registered,
configurable at trigger intake, instantiated at server startup, reachable through trigger/workflow APIs,
and extensively tested; complete disabled build/runtime isolation is absent.

Triggers are workflow intake. `wfe_def.c` defines `trigger.watch-dir`; `wfe_validate.c:256` constrains a
trigger block to workflow start; `wfe_blocks.c:1129` executes its already-fired run-time form;
`trigger_scheduler.c:492` materializes and files runs through the workflow orchestration seam. Detection
may remain in a server adapter, but definition semantics, admission, durable run, and advancement belong
to workflows.

### Roundtable

`src/modules/roundtable` contains ensemble, chair, preset, seat-resolution, pipeline capture/chunk/eval,
review, verify, and type files plus its descriptor. The descriptor declares seven dependencies,
`enabled_by_default: false`, and no runtime toggle. Server pipeline, MCP/HTTP/preset, sweep, compute, and
workflow provider code directly consume these symbols, and `SERVER_SRCS` directly includes roundtable
objects. No `modules.roundtable` entry exists in `config_parse_modules_section`. Current liveness is
compiled, registered/reachable/configurable through feature-specific fields and presets, and tested;
descriptor-level optional activation/isolation is absent.

Workflow integration is explicit: `wfe_roundtable.c` and `wfe_live_panel.c` provide workflow block seams,
while `delegate_ensemble*`, `roundtable_chair*`, and `roundtable_verify*` retain panel semantics.
Workflows owns the awaiting work-item state; roundtable owns seat/panel/chair/findings/convergence.

### Benchmarks

`src/modules/benchmarks` contains only `module.yaml`, which declares five dependencies,
`enabled_by_default: false`, and no runtime toggle. Physical benchmark behavior is distributed across
`benchmarks/`, `bench/`, scripts, CI, `server_eval.c`, server memory-benchmark routes,
`src/modules/agent_eval/*`, platform task loaders, CLI code, DB scratch/eval support, and tests.
`AGENT_SRCS` directly includes four `agent_eval` objects, while ordinary server/CLI dispatch exposes
`eval.run` and `memory.benchmark`. No `modules.benchmarks` config field or target-module registration was
found. The target module is descriptor-only; legacy harnesses are compiled/reachable/tested but are not
optional at that boundary.

The taxonomy name is `benchmarks`, not `evals`. Legacy `agent_eval`, `eval.run`, `mem_eval_*`, and file
names are compatibility/movement debt. Offline scoring cannot own runtime verification, roundtable
consensus, workflow approvals, or live routing/policy. KB ranker benchmark gates remain learning/memory
consumers of evidence, not authority transferred to this optional module.

## Dependency reconciliation

| Module/dependency | Evidence classification | Boundary |
|---|---|---|
| workflows → audit/config/module-runtime | declared and observed | event/config/lifecycle providers; distributed registration remains |
| workflows → delegates/policy/skills/tools/workspace | declared and observed | effect providers and authority, not workflow ownership |
| workflows → ir/routing | declared and observed | canonical records and selection; routing does not own intake |
| roundtable → audit/config/delegates/ir/routing | declared and observed | evidence/config/invocation/record/provider seams |
| roundtable → response-composition | declared, boundary observed | general rendering remains core; panel composition remains roundtable |
| roundtable → module-runtime | descriptor-only at activation boundary | no effective module gate found |
| benchmarks → config/ir/memory/routing | declared; distributed legacy behavior observed | harness inputs/providers; no runtime authority |
| benchmarks → module-runtime | descriptor-only | no registration or activation field found |

No declared edge is classified stale from static evidence. Module-runtime edges for roundtable and
benchmarks are unresolved implementation gaps rather than unused descriptor text.

## Cross-module overlap and liveness matrix

| Capability | Owner | Integrator/consumer | Evidence | State / ambiguity |
|---|---|---|---|---|
| Trigger-to-run intake | workflows | server trigger adapter, routing | `trigger_scheduler.c:492`; `gw_orch_workflows.c`; `wfe_def.c:38` | implemented/reachable/tested; physical adapter distributed |
| Workflow effect authorization | execution-policy | workflows | `wfe_enforce.*`, `wfe_native_gate.*`, provider calls | implemented/tested; policy remains core |
| Delegate/skill/tool execution | delegates/skills/tools | workflows | `wfe_live_delegate.c:481`; `wfe_blocks.c` | registered/reachable/tested |
| Per-run resource authority | workspace | workflows | `wfe_replay_worktree.*`, worktree blocks | implemented/tested |
| Workflow audit/config | audit/config | workflows | descriptor plus config/action call sites | implemented; disabled isolation incomplete |
| Roundtable workflow gate | roundtable panel; workflows lifecycle | each consumes the other seam | `wfe_roundtable.c:217`; `wfe_live_panel.c:370` | implemented/tested; ownership intentionally split |
| Panel invocation | roundtable | delegates/routing providers | `delegate_ensemble.*`, seat resolve | implemented/reachable/tested |
| Final generic response rendering | response-composition | roundtable | descriptor and composed result handoff | declared/implemented boundary; no ownership transfer |
| Roundtable audit/config | audit/config | roundtable | presets/config and call paths | implemented; module activation absent |
| Benchmark cadence | benchmarks | CI/manual workflow scheduling | `.github/workflows/bench-smoke.yml`, pending cadence proposal | smoke implemented; standing cadence remains prospective |
| Roundtable verification | roundtable | benchmarks has no role | `roundtable_verify.*`, pipeline eval tests | implemented; benchmark authority absent |
| Runtime agent quality gate | runtime owners | benchmarks has no role | ordinary runtime callers do not require benchmark module | absent by contract; KB learning gates are separate |

## Overlap and cleanup findings

- `src/modules/agent_eval` is a `relocate` candidate into the benchmarks ownership boundary, with legacy
  API aliases preserved; it is live code, not dead code.
- Unconditional WFE startup registration versus gated trigger intake is an optionality contradiction,
  deferred to a workflow profile/source slice.
- Direct roundtable build/routes without `modules.roundtable` activation is an optionality contradiction,
  deferred to a roundtable profile/source slice.
- Workflow roundtable adapters and server roundtable pipelines are integrations, not duplicate owners.
- Benchmark scripts cover multiple domains and contain apparent overlapping runners; none is confirmed
  duplicated or dead without corpus/provider/result parity and CI/consumer evidence.

No `unreachable`, `superseded`, `configuration-only`, or `test-only` candidate met the deletion
threshold. Descriptor-only target ownership does not make the distributed implementation dead.

## Verification

- `python3 -I -S scripts/check_module_docs.py`
- `python3 -I scripts/tests/test_check_module_docs.py -v`
- `python3 -I -S scripts/check_module_source_ownership.py`
- `python3 -I -S scripts/check_cleanup_ledger.py`
- `python3 -I -S scripts/refactor_baselines.py`
- `make -C src lint`
- close-time changed-path and diff-stat scope checks
- technical-writer review and exact final-diff roundtable approval
- feature-branch pull-request CI
