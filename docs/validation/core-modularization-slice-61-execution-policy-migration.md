# Core modularization slice 61: migrate and latch `execution-policy` (Class A #4)

## Scope

This slice migrates the required `execution-policy` module by extracting the tool-action policy
**decision** from `src/server/agent_policy.c` into the current Go owner at
`server-go/modules/execution-policy/execution_policy.go`
and latching the descriptor. It moves the contiguous policy section — `policy_load` (reads the operator
policy `.aimee-policy.json`) and `policy_check_tool` (the fail-closed allow/deny decision), plus the
`g_policy_json` static — and nothing else. No production symbol, behaviour, storage, or configuration
changes.

## The boundary is the decision, not validation (self-reviewed against the module doc)

`docs/modules/execution-policy.md` sets the boundary explicitly: the module owns "the decision contract
and denial reasons, not the action implementation," and "schema and argument validation such as
`tool_validate` stays with `tools`." An initial broad extraction that pulled the whole policy region
(including `tool_validate`, `tool_suggest`, `normalize_args`, `validate_against_schema`, and the
tool-prompt helpers) was **rejected on review as doc-violating** and redone as decision-only:

- **Moved:** `policy_load`, `policy_check_tool`, and their `g_policy_json` static — the contiguous
  `/* --- Policy checking --- */` section (former lines 391–591).
- **Stayed in `agent_policy.c`:** schema/argument validation (`tool_validate`, `normalize_args`,
  `validate_against_schema`, `tool_suggest`, `edit_distance`, the tool-prompt embed/collect helpers and
  their `g_arg_aliases`/`g_tool_names` statics), side-effect classification (`tool_side_effect`), and
  the execution-trace/metrics/env/manifest half.

The extraction is link-clean by construction: `g_policy_json` is used only inside the moved section,
the moved functions do not call any staying function, and nothing staying calls `policy_load`/
`policy_check_tool` (their callers are external, via `agent_exec.h`). The decision contract stays in the
shared `src/headers/agent_exec.h`, which the module implements while the server implements the rest —
the `memory`/DB2 arrangement — so `execution_policy.c` has no module-private header (`private_headers`
empty; the module root holds only `execution_policy.c`, latch exact).

Enforcement points that *consume* the decision — guardrails' `pre_tool_check`, gateway request policing
— stay where they are. Consolidating them onto this single decision engine is future work, not this
slice.

## Build

`execution_policy.c` is added to every source/object list carrying `server/agent_policy.c` /
`agent_policy.o` (Make `AGENT_SRCS`, CMake, the two `tests/Rules.mk` link lists). `agent_policy.o`
keeps its specific rule (it still includes the generated `tool_prompts_data.h` for the tool-prompt
helpers that stayed); `execution_policy.o` uses the generic rule and does not include
`tool_prompts_data.h`.

## Ownership domain and tests

Module root holds exactly `execution_policy.c`; `sources = ["src/modules/execution-policy/execution_policy.c"]`,
`private_headers = []`, `docs = ["docs/modules/execution-policy.md"]` — latch exact. `tests` is empty:
the policy decision has no dedicated test whose subject is `execution_policy.c` (the intercept
classifier test `test_agent_policy_intercept.c` exercises `policy_check_tool` but its subject is the
server-side `agent_policy_intercept.c`), matching the `benchmarks` precedent.

## Verification

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
make -C src ../aimee ../aimee-server
```

Validated on the .253 aimee-test container: full Make build and full CMake configure/build. Self-reviewed
under explicit operator authorization for self-review-only (the roundtable is unavailable this cycle);
`nm` on `execution_policy.o` confirms it defines `policy_check_tool` and not `tool_validate` or the trace
functions, and the linker enforces the partition.

## Where the programme stands

Twenty-two of twenty-six descriptors are latched. Four Class A modules remain: `control-web`,
`kb-synthesis`, `response-composition`, `runtime-web`. `kb-synthesis` is a large KB-coupled extraction;
the three web/IR modules have no located source.
