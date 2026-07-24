# Core modularization: the unmigrated module register

## What this records

Twenty-one module descriptors carry `ownership_complete: true`. Five do not. This document tracks
those five, whose module root contains nothing but `module.yaml` — no implementation has ever been
moved under `src/modules/<id>/`. It exists so that gap is recorded in one place rather than inferred
by re-measuring the tree, and so `rule=ownership-empty-domain` has somewhere to point.

The Class B descriptors that once had undeclared implementation files in their roots — governance,
learning, workspace, vault, config, git, delegates, workflows, memory — have since been declared and
latched, as has `benchmarks` (the first Class A code migration). These five empty-root descriptors
are the only remainder.

## Why an empty root cannot be latched

`validate_complete_ownership` compares the declared source and private-header sets against every
matching file under the module root. For these five, both actual sets are empty, so set equality
holds vacuously and the latch would pass on the source and private-header rules. Until slice 39 the
only thing preventing it was the separate `docs == ["docs/modules/<id>.md"]` requirement, which none
of them satisfies — one field away from a descriptor asserting completeness for a module that has not
been migrated at all.

`docs/modules/module-runtime.md` states that modules without the latch "remain migration debt and
must not feed generated build profiles." Latching an empty root would silently clear that debt for
the five modules furthest from done, and would do it for the five where the assertion is least
true. Slice 39 therefore rejects `ownership_complete: true` whenever the module-local domain is
empty, with `rule=ownership-empty-domain`.

The rule is unconditional. A module that genuinely owned no module-local C — a header-only or
pure-aggregation module — would also be rejected, and that is the intended behaviour: the right
response to such a module would be to extend the completeness domain to cover what it actually owns,
not to weaken the guard on the domain that exists. No current descriptor is a candidate, so that
design question stays open rather than being pre-answered by an opt-out.

The failure message says the module is not migrated rather than broken. These descriptors are valid;
they are early.

## The remaining five

`benchmarks` has been migrated and latched: the cohesive eval framework in the former non-descriptor
`src/modules/agent_eval/` directory (four sources + two headers) was relocated under
`src/modules/benchmarks/` and the descriptor declares and latches it. The `agent_eval` directory name,
forbidden by the canonical taxonomy, is gone; the `agent_eval_` symbol prefix is retained as the
framework's API identity.

`tools` has since been migrated (pilot #2): the posix `agent_tools*` family (three sources + the
`agent_tools_internal.h` seam) and the `agent_tools.h` contract were relocated under
`src/modules/tools/` and latched. `src/server/agent_tools.c`, which implements the
turn/snapshot/toolset session-state slice declared in `agent_tools.h`, was deliberately left in the
server as a not-module-local implementation of the contract — the same arrangement by which DB1/DB2
implement `memory`'s contract — so the `agent_tools.c` name collision between the posix and server
files never arises.

`routing` has since been migrated (pilot #3): the self-contained routing block of
`src/server/agent_config.c` (role dispatch, capability/tier selection, delegate pick, availability,
route-block reasons, and the route filters, ~503 lines) was extracted into
`src/modules/routing/routing.c` and latched. The routing contract stays in the shared
`src/headers/agent_config.h`, which the module implements while the config/auth half of
`agent_config.c` remains in the server and is called through the same header — an established pattern
(`delegate_routing.c`, a module, already calls those header predicates). The block's statics are
module-local and no config function calls the routing functions, so the split is link-clean and
`routing` has no module-private header. Five Class A modules remain.

Locations below are what each module's own canonical document names. They are a starting inventory
for a future migration slice, not an ownership assignment: no audit has confirmed that these files
are the module's, exclusively or at all. A migration slice must establish that itself.

| module | canonical document names | notes |
|---|---|---|
| `control-web` | nothing | The document describes the Control Plane GUI, dashboard, assets, listener and proxy behaviour without naming a source location. |
| `execution-policy` | `src/server/agent_policy.c`, `src/modules/guardrails/guardrails_action_audit.c`, `src/modules/config/config_fields.c`, `src/modules/config/config_sections.c` | Named locations span server, guardrails and config; the policy surface is distributed rather than sited. |
| `kb-synthesis` | `src/kb/` (`kb_curator_synthesize.h`), `src/db2` | Lives with the KB and its PostgreSQL store. |
| `response-composition` | `src/modules/ir/include/aimee/ir/aimee_ir.h` | Only an IR contract is named; no implementation site. |
| `runtime-web` | nothing | As with `control-web`, the document specifies the surface and lifecycle key but no source location. |

Two observations worth carrying forward:

- `control-web` and `runtime-web` have canonical documents that identify no implementation location
  at all — not a path, not a bare filename. Both specify their surface and lifecycle key and stop
  there. A migration slice for either starts with locating the code, not moving it. (`workflows.md`
  is the only other module document naming neither, and `workflows` is not in this class: its module
  root holds 30 sources and 24 private headers that its descriptor does not yet declare.)
- `execution-policy` is named across two or more directories and is declared as a dependency by ten
  other descriptors. Its migration is a decomposition question — which of the named files is the module
  and which is a consumer — not a file move.
  `docs/validation/core-modularization-class-a-decomposition.md` performs that establishment from the
  source. In short: the `config_fields.c`/`config_sections.c` paths cited for `execution-policy` are
  `config` surface, not module-core; `execution-policy` needs `agent_policy.c` split across three
  modules and is the highest-risk remaining migration. (The `tools` and `routing` distributed cases —
  a posix/server session-state split with a name collision, and a routing/config split of
  `agent_config.c` — were resolved in their migrations by keeping the non-module half in place and
  reaching it through the shared header.)

## What would change this document

A migration slice that moves one of these implementations under `src/modules/<id>/` should update the
row, and the module becomes a declaration-then-latch candidate like the other nine unlatched
descriptors. When the last row is gone this document should be deleted rather than left as an empty
table.
