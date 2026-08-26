# Core modularization: Class A decomposition analysis

## What this establishes

`docs/validation/core-modularization-class-a-migration.md` records eight module descriptors whose
root holds only `module.yaml`, lists a *starting inventory* of files each module's canonical document
names, and states plainly that the inventory is "not an ownership assignment: no audit has confirmed
that these files are the module's, exclusively or at all. A migration slice must establish that
itself."

This document performs that establishment for the three the migration register calls out as
"decomposition questions" (`tools`, `routing`, and `execution-policy`) by reading each named file
and classifying it as **module-core** (the module's own implementation, a migration candidate) or
**consumer/foreign** (code that *uses* the module, or that belongs to another module, and must stay
put). It moves no code. Its output is a recommended migration boundary per module and the specific
decision each migration still needs, so a future migration slice starts from an evidence-based
boundary rather than the raw inventory.

Method: for each named path, examine the file header and its top-level definitions, then classify by
what the code *is*, not by what its name suggests or which module document happened to cite it.

## Shared finding: the config files are consumer surface, not module-core

`src/modules/config/config_fields.c` and `src/modules/config/config_sections.c` are named in the
inventory for **both** `tools` and `execution-policy`. They are neither module's implementation: they
are `config` module sources that define the configuration fields and sections those modules read.
They are the config *surface* a module consumes, not the module. No Class A migration should move
them; they already have a latched-candidate owner in `config`. This removes two of the cited paths
from both the `tools` and `execution-policy` domains before either migration begins.

## `tools` (five dependents)

| path | lines | classification | evidence |
|---|---|---|---|
| `src/posix/agent_tools_dispatch.c` | 2230 | module-core | "the tool-call dispatcher … routes by name and applies the shared guardrails/snapshot/slop hooks" |
| `src/posix/agent_tools.c` | n/a | module-core | the dispatcher's own header: "each tool's implementation lives in `agent_tools.c`" |
| `src/posix/agent_tools_anchored.c` | n/a | module-core | anchored-edit tool implementations, sibling of the dispatcher |
| `src/modules/config/config_fields.c`, `config_sections.c` | n/a | consumer (config) | tool-related config fields; owned by `config` (see shared finding) |

**Boundary:** the module is the `agent_tools*` family under `src/posix/`. The boundary is the
cleanest of the three. No file needs splitting.

**The obstacle is extraction, not identification.** The `agent_tools*` files are compiled inside the
core posix agent-runtime bundle in both build systems (Make `CORE_SRCS` and CMake list them beside
`agent_runtime.c`, `agent_bridge.c`, `sandbox.c`, `workspace_provider.c`), they include headers from
`delegates`, `workspace`, `economizer`, and `sandbox`, and `agent_tools_dispatch.c` binds
`delegation_active_id` as a weak symbol resolved by `server_compute.c` at link time. Relocating them
under `src/modules/tools/` reshapes the hot-path build layout and would either pull their tightly
coupled runtime siblings along or introduce new cross-module include edges from `tools` back into
`src/posix`.

**Decision the migration needs:** whether tool *dispatch* should physically live under
`src/modules/tools/` at all, or whether it is properly agent-runtime code that the `tools` descriptor
groups aspirationally. If the former, the migration is a bounded (if coupling-heavy) extraction of the
`agent_tools*` family; if the latter, `tools` is a header/contract module and its completeness domain
should be defined around what it actually owns rather than by moving runtime code.

## `routing` (six dependents)

| path | lines | classification | evidence |
|---|---|---|---|
| `src/server/agent_config.c` | 2376 | **mixed**: routing functions embedded in a config file | header: "config loading/saving, agent routing, role checking, auth resolution"; defines `agent_route_with_caps`, `agent_routing_block_reason` among config/auth code |
| `src/headers/agent_config.h` | 189 | **mixed** contract | declares the `agent_route_*` and `delegate_pick_for_role` routing surface alongside the config contract |
| `src/modules/delegates/delegate_routing.c` | 432 | module-core **or** delegates | "shared delegate route override helpers": `delegate_filter_route_capabilities`, `delegate_route_by_provider`, `delegate_apply_route_overrides`: routing logic that currently lives in the `delegates` module |

**Boundary:** there is no `routing.c` to move. The routing surface is (a) a *subset* of
`agent_config.c`, the `agent_route_*` functions interleaved with config load/save, role checking, and
auth resolution, and (b) `delegate_routing.c`, which is delegate-specific routing sited in the
`delegates` module.

**Decision the migration needs:** two, before any code moves. First, whether to split
`agent_config.c` (extract the `agent_route_*` functions and their header declarations into a `routing`
module) or to leave routing as a server-config concern. Second, whether `delegate_routing.c` belongs
to `routing` or stays with `delegates`. It is delegate route-override logic, so the honest default is
that it is `delegates`' and `routing` owns only the general `agent_route_*` surface. This is a
file-split plus a boundary call, not a file move.

## `execution-policy` (ten dependents)

| path | lines | classification | evidence |
|---|---|---|---|
| `src/server/agent_policy.c` | 1197 | **mixed**: policy embedded with unrelated concerns | header: "validation, policy, trace, metrics, env, manifest, contract"; defines `tool_validate`, `tool_suggest`, `tool_side_effect` among trace/metric/manifest code |
| `src/modules/guardrails/guardrails_action_audit.c` | 146 | consumer (guardrails) | "the per-action governed-action audit (P2 / S2)"; `pre_tool_check` is "a thin wrapper around the verdict logic (`pre_tool_check_inner` in `guardrails_orchestrator.c`)": guardrails enforcement that *applies* policy |
| `src/modules/config/config_fields.c`, `config_sections.c` | n/a | consumer (config) | policy config fields; owned by `config` (see shared finding) |

**Boundary:** the most distributed of the three, matching the register's note that "the policy surface
is distributed rather than sited." The only genuinely policy-owned code is a *subset* of
`agent_policy.c` (the `tool_validate`/`tool_suggest`/`tool_side_effect` validation surface),
interleaved with trace, metrics, env, and manifest handling that is server-runtime concern.
`guardrails_action_audit.c` is guardrails' enforcement point, not policy; the config files are
config's.

**Decision the migration needs:** whether to split `agent_policy.c` to extract the tool-validation
policy surface into `execution-policy`, and where the policy / guardrails-enforcement / config-surface
lines fall across three already-latched-candidate modules. With ten dependents and no cohesive site,
this is the highest-risk of the three and the least suited to being done ahead of an explicit
architecture decision.

## Tractability ranking and recommendation

1. **`tools`**: cleanest boundary (the `agent_tools*` family is the module); blocked only by an
   extraction-vs-keep decision and build coupling. The one candidate whose migration could be a
   bounded slice once the dispatch-location question is answered.
2. **`routing`**: requires splitting `agent_config.c` plus a routing-vs-delegates boundary call.
3. **`execution-policy`**: requires splitting `agent_policy.c` and drawing lines across three modules;
   highest dependent count; do last, if at all, as a deliberate decomposition.

`control-web` and `runtime-web` remain un-analysable here: their canonical documents name no source
location, so their establishment step is *locating* the code, not classifying it, and is out of scope
for this file. `benchmarks`, `kb-synthesis`, and `response-composition` are single-site or
contract-only rows the register already characterizes and are not decomposition questions.

## What this does not do

No production code, build membership, descriptor, or public symbol changes here. This is analysis.
The recommended boundaries are inputs to a future migration slice and an operator decision, not
ownership assignments. When a migration slice acts on one of these modules it should cite the relevant
section, record what it actually moved, and update both this file and the migration register.
