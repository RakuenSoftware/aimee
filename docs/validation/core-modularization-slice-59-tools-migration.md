# Core modularization slice 59: migrate and latch `tools` (Class A #2)

## Scope

This slice migrates the required `tools` module (core capability #18, tool dispatch) from an empty
descriptor to a declared and latched module by relocating the `agent_tools*` implementation family from
`src/posix/` and its contract header from `src/headers/` into `src/modules/tools/`. It moves three
sources and two headers, repoints Make and CMake, declares the descriptor, and latches it. No
production symbol, behaviour, storage, or configuration changes; the `agent_tools_` API prefix and all
signatures are unchanged.

## Decision: the posix family is the module; the server session-state slice stays

Unlike `benchmarks` (a single cohesive directory), the tool code is distributed and carries a name
collision: `src/posix/agent_tools.c` (implementations) and `src/server/agent_tools.c` (turn/snapshot/
toolset session-state + tool-definition JSON builders) share a filename, and `agent_tools.h` declares
symbols implemented in **both**. The decomposition (self-reviewed; the roundtable was unavailable this
cycle) is:

- The `tools` module owns the dispatch/implementation surface, `agent_tools.c`,
  `agent_tools_dispatch.c`, `agent_tools_anchored.c`, plus the contract `agent_tools.h` and the
  private seam `agent_tools_internal.h`. These move under `src/modules/tools/`.
- `src/server/agent_tools.c` implements the turn/snapshot/toolset session-state slice of the same
  contract (`agent_tools_begin_turn`, `agent_tools_set_snap_id`, `agent_tools_set_active_toolset`). It
  is **left in the server** as a not-module-local implementation, the same arrangement by which `memory`
  owns its contract while DB1/DB2 implement storage (slice 56). This keeps the boundary honest and makes
  the `agent_tools.c` name collision moot, only the posix `agent_tools.c` moves.

`server/agent_tools.c` does not include `agent_tools_internal.h`; the two halves communicate only
through the public `agent_tools.h`, so the split introduces no new coupling. The `delegation_active_id`
weak symbol from `server_compute.c` is unchanged and still links standalone.

## Build repointing

The three sources move from `src/posix/` and their objects follow. `agent_tools.h` moves out of the
global `src/headers/` (auto-available via `-Iheaders`) into the module root, so consumers now find it
through `-Imodules/tools`: added to the Make `C_FLAGS` (one global flag covers all 30 includers,
including `src/windows/`) and to the six CMake `target_include_directories` lists that carry the module
include set. Make source/object lists, `tests/Rules.mk` link lists, and the CMake source references all
repoint `posix/agent_tools*` -> `modules/tools/agent_tools*`. One path-qualified include in
`test_cmd_delegate.c` (`"posix/agent_tools_internal.h"`) is rewritten to the bare header name.

## Ownership domain and tests

The module root holds exactly the three declared sources and two declared private headers, so the latch
is exact; `docs` equals `["docs/modules/tools.md"]`. `agent_tools.h` is declared a `private_header`
(module-root, flat layout) even though it is consumed cross-module, matching the `config`/`learning`/
`benchmarks` convention. `tests` declares `test_tool_output_cap.c`, whose subject
`agent_tool_output_cap_clamp()` is a header-inline function of the tools contract; the wider dispatch
surface is exercised indirectly by server/workspace/script-runner tests attributed to those modules by
subject. The test-registration baseline records the one tools row (make, not ctest).

## Verification

Run from the repository root; each must exit zero:

```sh
python3 scripts/validate_module_descriptors.py --check-schema src/modules
python3 scripts/tests/test_validate_module_descriptors.py
python3 scripts/check_module_docs.py
python3 scripts/check_module_source_ownership.py
python3 scripts/check_module_header_layout.py
python3 scripts/check_module_test_registration.py
python3 scripts/check_module_inventory.py
python3 scripts/check_cleanup_ledger.py
python3 scripts/refactor_baselines.py check
make -C src ../aimee-server      # links the relocated tool objects and every agent_tools.h consumer
```

Validated on the .253 aimee-test container: full Make build and full CMake configure/build (the
relocated sources compile at `modules/tools/`, and every `agent_tools.h` consumer, server, posix,
delegates, Windows path, tests, resolves the header through the added `-Imodules/tools`). This slice
was self-reviewed (the roundtable was unavailable this cycle: kimi quota-exhausted, MiniMax unresponsive)
under explicit operator authorization for self-review-only.

## Where the programme stands

Twenty of twenty-six descriptors are latched. Six Class A modules remain: `control-web`,
`execution-policy`, `kb-synthesis`, `response-composition`, `routing`, `runtime-web`.
