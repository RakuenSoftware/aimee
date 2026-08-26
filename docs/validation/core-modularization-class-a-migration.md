# Core modularization: the unmigrated module register

## What this records

Twenty-three module descriptors carry `ownership_complete: true`. Three do not. This document tracks
those three, whose module root contains nothing but `module.yaml`. No implementation has ever been
moved under `src/modules/<id>/`. It exists so that gap is recorded in one place rather than inferred
by re-measuring the tree, and so `rule=ownership-empty-domain` has somewhere to point.

The Class B descriptors that once had undeclared implementation files in their roots, governance,
learning, workspace, vault, config, git, delegates, workflows, memory, have since been declared and
latched, as has `benchmarks` (the first Class A code migration). These three empty-root descriptors
are the only remainder.

## Why an empty root cannot be latched

`validate_complete_ownership` compares the declared source and private-header sets against every
matching file under the module root. For these three, both actual sets are empty, so set equality
holds vacuously and the latch would pass on the source and private-header rules. Until slice 39 the
only thing preventing it was the separate `docs == ["docs/modules/<id>.md"]` requirement, which none
of them satisfies, one field away from a descriptor asserting completeness for a module that has not
been migrated at all.

`docs/modules/module-runtime.md` states that modules without the latch "remain migration debt and
must not feed generated build profiles." Latching an empty root would silently clear that debt for
the three modules furthest from done, and would do it for the three where the assertion is least
true. Slice 39 therefore rejects `ownership_complete: true` whenever the module-local domain is
empty, with `rule=ownership-empty-domain`.

The rule is unconditional. A module that genuinely owned no module-local C. A header-only or
pure-aggregation module, would also be rejected, and that is the intended behaviour: the right
response to such a module would be to extend the completeness domain to cover what it actually owns,
not to weaken the guard on the domain that exists. No current descriptor is a candidate, so that
design question stays open rather than being pre-answered by an opt-out.

The failure message says the module is not migrated rather than broken. These descriptors are valid;
they are early.

## The remaining three

`benchmarks` has been migrated and latched: the cohesive eval framework in the former non-descriptor
`src/modules/agent_eval/` directory (four sources + two headers) was relocated under
`src/modules/benchmarks/` and the descriptor declares and latches it. The `agent_eval` directory name,
forbidden by the canonical taxonomy, is gone; the `agent_eval_` symbol prefix is retained as the
framework's API identity.

`tools` has since been migrated (pilot #2): the posix `agent_tools*` family (three sources + the
`agent_tools_internal.h` seam) and the `agent_tools.h` contract were relocated under
`src/modules/tools/` and latched. `src/server/agent_tools.c`, which implements the
turn/snapshot/toolset session-state slice declared in `agent_tools.h`, was deliberately left in the
server as a not-module-local implementation of the contract, the same arrangement by which DB1/DB2
implement `memory`'s contract, so the `agent_tools.c` name collision between the posix and server
files never arises.

`routing` has since been migrated (pilot #3): the self-contained routing block of
`src/server/agent_config.c` (role dispatch, capability/tier selection, delegate pick, availability,
route-block reasons, and the route filters, ~503 lines) was extracted into
`src/modules/routing/routing.c` and latched. The routing contract stays in the shared
`src/headers/agent_config.h`, which the module implements while the config/auth half of
`agent_config.c` remains in the server and is called through the same header. An established pattern
(`delegate_routing.c`, a module, already calls those header predicates). The block's statics are
module-local and no config function calls the routing functions, so the split is link-clean and
`routing` has no module-private header.

`execution-policy` has since been migrated (pilot #4): the contiguous policy-decision section of
`src/server/agent_policy.c` (`policy_load`, which reads the operator `.aimee-policy.json`, and
`policy_check_tool`, the fail-closed allow/deny decision) was extracted into
`src/modules/execution-policy/execution_policy.c` and latched. Per the module boundary, schema/argument
validation (`tool_validate`) and side-effect classification (`tool_side_effect`) were deliberately left
with the server/`tools` surface, and the trace/metrics/manifest half of `agent_policy.c` stays too; all
are reached through the shared `src/headers/agent_exec.h`, which the module implements. The
enforcement points that consume the decision (guardrails' `pre_tool_check`, gateway policing) stay
where they are.

`kb-synthesis` has since been migrated (pilot #5): the KB curator family (21 sources + 16 headers) was
relocated from `src/kb/` into `src/modules/kb-synthesis/`. Unlike the earlier splits it is **KB-tier**,
the sources include KB-internal service headers (`kb.h`, `index.h`, `kb_service_*`, `kb_mdl.h`, …), so
they compile with the KB build flags into `$(OBJDIR)/kb/modules/kb-synthesis/` (the
`KB_SYNTHESIS_SRCS`/`KB_SYNTHESIS_OBJS` pair) and link only into `aimee-kb`; `-Imodules/kb-synthesis`
lets the in-KB consumers (`kb.c`, `cmd_kb.c`, the curator config/profile) resolve the curator headers.
The DB2 artifact/link storage APIs and the core `kb_curator_provider.c` adapter stay their owners' and
are consumed through their contracts. Three Class A modules remain, all three no-source.

Locations below are what each module's own canonical document names. They are a starting inventory
for a future migration slice, not an ownership assignment: no audit has confirmed that these files
are the module's, exclusively or at all. A migration slice must establish that itself.

| module | canonical document names | notes |
|---|---|---|
| `control-web` | nothing | The document describes the Control Plane GUI, dashboard, assets, listener and proxy behaviour without naming a source location. |
| `response-composition` | `src/modules/ir/include/aimee/ir/aimee_ir.h` | Only an IR contract is named; no implementation site. |
| `runtime-web` | nothing | As with `control-web`, the document specifies the surface and lifecycle key but no source location. |

Two observations worth carrying forward:

- `control-web` and `runtime-web` have canonical documents that identify no implementation location
  at all, not a path, not a bare filename. Both specify their surface and lifecycle key and stop
  there. A migration slice for either starts with locating the code, not moving it. (`workflows.md`
  is the only other module document naming neither, and `workflows` is not in this class: its module
  root holds 30 sources and 24 private headers that its descriptor does not yet declare.) These two,
  and `response-composition` (which names only the `aimee_ir.h` IR contract, no implementation site),
  are **deliberately unlatched**: their code does not exist yet. `control-web`/`runtime-web` are the
  optional GUI modules owned by `docs/proposals/pending/product-governance-web-and-config.md`, which
  will supply their implementation. The correct disposition is to keep them as valid, unlatched
  descriptors, not to write placeholder code purely to satisfy the completeness latch, and not to
  remove the descriptors (their IDs are reserved by the owning proposals). A silent flip of any of the
  three to `ownership_complete: true` without real implementation would be a regression; the descriptor
  mutation suite and the empty-domain guard (slice 39) both defend against it.
- All three remaining modules are **no-source**: `control-web`/`runtime-web`/`response-composition`
  have no located implementation, so there is nothing to migrate. The five modules whose code existed
  and could physically move (`benchmarks`, `tools`, `routing`, `execution-policy`, `kb-synthesis`) have all been migrated and latched.

## What would change this document

A migration slice that moves one of these implementations under `src/modules/<id>/` should update the
row, and the module becomes a declaration-then-latch candidate like the other nine unlatched
descriptors. When the last row is gone this document should be deleted rather than left as an empty
table.
