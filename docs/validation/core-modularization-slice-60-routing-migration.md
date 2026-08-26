# Core modularization slice 60: migrate and latch `routing` (Class A #3)

## Scope

This slice migrates the required `routing` module by extracting the self-contained routing block from
`src/server/agent_config.c` into `src/modules/routing/routing.c` and latching the descriptor. It is a
file *split*, not a move: ~503 lines (roughly the `agent_supports_role` static through
`agent_route_with_caps`) move to the module; the config/auth half of `agent_config.c` stays. No
production symbol, behaviour, storage, or configuration changes; every routing symbol keeps its name.

## Why the split is clean (self-reviewed)

The routing functions are contiguous in `agent_config.c` (former lines ~1383–1885), and the extraction
is link-safe by construction:

- **The block's statics are module-local.** `agent_supports_role`, `agent_command_on_path`,
  `agent_pick_random`, `delegate_role_rand`, `agent_satisfies_required_caps`,
  `agent_route_with_caps_inner`, and the `g_route_health_filter`/`g_route_policy_filter` pointers are
  each used only within the block, verified none is referenced before the block start or after its
  end. They move with it and stay `static` in `routing.c`.
- **No config→routing coupling.** No config/auth function calls the routing functions, so nothing in
  the remaining `agent_config.c` needs a routing symbol.
- **Routing→config calls go through the shared header.** The block calls three functions that stay in
  the server (`agent_load_config`, `agent_has_role`, `agent_has_resolvable_credentials`), declared in
  `src/headers/agent_config.h`. A module calling those is an established pattern. The delegates
  module's `delegate_routing.c` already calls `agent_has_role`/`agent_is_exec_role` from the same
  header. Public block functions that config might call resolve the same way.

Because the routing contract remains in the shared `agent_config.h` and the module implements it while
the server implements the config/auth half, `routing.c` has **no module-private header**
(`private_headers` is empty; the module root holds only `routing.c`, so the latch is exact). This is
the `memory` arrangement (contract owned centrally, implementation split) applied to routing.
`delegate_routing.c` stays in the delegates module; `router_advise.c` stays workflow-owned.

## Build

`routing.c` is added to every source/object list that carries `server/agent_config.c` /
`agent_config.o` (Make `AGENT_SRCS` and `BENCH_OBJS`, CMake `aimee-agent` sources, and the four
`tests/Rules.mk` link lists), so it is compiled and linked into exactly the binaries that already link
the config half. No new `-I` root is needed, `routing.c` includes `agent_config.h` from the global
`src/headers/` dir. The refactor baseline is refrozen (the removed `src/headers` surface is unchanged;
`routing.c` is a module source, not a tracked public header).

## Ownership domain and tests

The module root holds exactly `routing.c`; `sources = ["src/modules/routing/routing.c"]`,
`private_headers = []`, `docs = ["docs/modules/routing.md"]`. The latch is exact. `tests` declares
`test_agent_caps.c`, whose subject is capability-based routing (`agent_route_with_caps` /
`agent_satisfies_required_caps`). The test-registration baseline records the one routing row.

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
make -C src ../aimee ../aimee-server   # both link routing.o with the config half
```

Validated on the .253 aimee-test container: full Make build and full CMake configure/build, plus the
`test_agent_caps` routing test. Self-reviewed under explicit operator authorization for
self-review-only (the roundtable is unavailable this cycle). The linker enforces the partition. A
wrong cut would surface as a missing or duplicate symbol, which the .253 build would fail on.

## Where the programme stands

Twenty-one of twenty-six descriptors are latched. Five Class A modules remain: `control-web`,
`execution-policy`, `kb-synthesis`, `response-composition`, `runtime-web`. `execution-policy` is the
highest-risk remaining migration (policy distributed across server/guardrails/config); the three
web/IR modules have no located source.
