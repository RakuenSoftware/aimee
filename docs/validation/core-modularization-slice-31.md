# Core modularization slice 31: complete IR ownership

## Scope

This slice marks the required `ir` descriptor `ownership_complete: true`. It changes ownership
metadata, validation coverage, documentation, and cleanup accounting only. Production source, public
symbols, build inputs, runtime behavior, and wire behavior do not change.

The descriptor owns exactly:

- `src/modules/ir/aimee_ir.c` and `src/modules/ir/aimee_ir_metrics.c`;
- `src/modules/ir/include/aimee/ir/aimee_ir.h`, `aimee_ir_metrics.h`, and `panel_result.h`;
- `src/tests/test_aimee_ir.c`, `src/tests/test_aimee_ir_metrics.c`, and
  `src/tests/test_panel_ir_contract.c`; and
- `docs/modules/ir.md`.

Translation remains an independently reviewable follow-up. IR depends only on `module-runtime`, while
translation depends on IR, so completing IR first follows the descriptor graph without requiring an
atomic cross-module change.

## Production liveness and ownership

`aimee_ir.c` is a shipping input at `src/Makefile:440`. Its focused Make contract is
`src/tests/Rules.mk:1656`, its direct CMake test is `src/tests/CMakeLists.txt:160`, and it is exercised by
the cross-boundary CMake tests at lines 162-166. Production callers include the translation frontends and
backends, `modules/memory/gw_stage_memory.c`, `server/aimee_ir_serve.c`, `server/aimee_ir_shadow.c`,
`modules/delegates/aimee_ir_rescue.c`, `posix/agent_ir_parse.c`, and `posix/agent_runtime_tmux.c`. The
source owns provider-neutral request/response lifecycle, semantic accessors, equality, and transform
sequencing, and depends only on cJSON and libc.

`aimee_ir_metrics.c` is also a shipping input at `src/Makefile:440`. Its focused Make contract is
`src/tests/Rules.mk:1668`, its direct CMake test is `src/tests/CMakeLists.txt:161`, and it participates in
the CMake shadow/serve tests at lines 164-165. Production writers are
`modules/delegates/aimee_ir_rescue.c`, `server/aimee_ir_serve.c`, `server/aimee_ir_shadow.c`, and
`server/anthropic_http.c`. `server/server_state.c` reads and exports the names, per-wire counts, and totals
to dashboard metrics. These counters describe provider-neutral IR parse, build, parity, and stage state,
so IR is their canonical owner.

The public types are independently live. Translation, memory, server, and POSIX runtime consume
`aimee_ir.h`; the production writers and dashboard reader consume `aimee_ir_metrics.h`; required delegates,
optional roundtable, workflows, and server pipeline/compute consume `panel_result.h`. IR owns the latter's
provider-neutral result layout only, while delegates and optional providers own execution.

Neither production source is self-tested-only, and no duplicate canonical IR implementation was found.
The Aimee code index still reported the pre-move `src/server` and `src/headers` paths during this audit, so
the current-worktree build and caller evidence above is authoritative. The cited Make/CMake line references
were rechecked in that worktree. An exact symbol search across `src` found only direct-test call sites for
`aimee_ir_response_reasoning`.

## DRY and dead-code findings

Three symbol-level cleanup candidates remain explicit:

- `aimee_ir_request_equal` is a cross-protocol test-support export;
- `aimee_ir_metrics_reset` is a test-only reset seam; and
- `aimee_ir_response_reasoning` currently has only direct-test callers.

They do not make either source file dead: both files have shipping consumers. This slice does not delete
or privatize the symbols because that would change public and test contracts. A later focused API-cleanup
slice must decide compatibility, move any retained test helper into test support, and prove downstream
callers before changing them.

## Regression controls

Production descriptor mutation tests now remove `aimee_ir.c`, plant an unlisted C source, plant an
unlisted private header, and remove the IR canonical document. Each mutation must fail with the stable
`ownership-complete` rule. The existing header-layout gate continues to enforce declared canonical public
headers and reject flat shadows or broad source include roots.

## Verification

- descriptor validation and its complete mutation suite;
- module header-layout, source-ownership, docs, cleanup-ledger, and refactor-baseline checks plus focused
  mutation suites;
- full Make build and lint;
- build and execute `unit-test-aimee-ir`, `unit-test-aimee-ir-metrics`, and
  `unit-test-panel-ir-contract`; and
- technical-writer review, exact-final-diff roundtable approval, and all required pull-request checks.
