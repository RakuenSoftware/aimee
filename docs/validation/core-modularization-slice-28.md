# Core modularization slice 28: roundtable runtime surfaces

## Scope

This slice makes the optional, default-off `roundtable` module absent from live server and MCP
discovery and invocation. It extracts activation from the ensemble engine into the owner-local
`src/modules/roundtable/roundtable_activation.c`, which is the single source of truth for startup
activation and roundtable-owned server-operation and MCP-tool names.

It does not remove CLI commands, static reference documentation, workflow block schemas, or selected
Make/CMake objects. Those surfaces either explain how to enable the module or belong to later physical
profile and GUI/config projection slices.

## Startup contract

`server_init` calls `roundtable_runtime_configure` once from the loaded configuration. A config-load
failure and a `NULL` configuration both leave the module disabled. `module.yaml` declares
`runtime_toggle.supported: false`, and this slice adds no administrative hot-toggle path, so changes to
`modules.roundtable` or `AIMEE_MODULE_ROUNDTABLE` take effect on the next server start. Direct engine
and CLI callers continue to resolve their supplied configuration with `roundtable_module_enabled`;
execution guards from slice 27 remain defense in depth.

## Registration boundaries

- `server_dispatch` checks `roundtable_operation_available` before capability lookup and returns the
  existing `UNKNOWN_METHOD` shape while disabled.
- `handle_server_info` applies the same predicate to `server_dispatch_table`, so disabled operations
  do not appear in `methods`.
- `route_match` is the HTTP availability choke point. `v1_route_caps_lookup`, `v1_route_dispatch`,
  `v1_route_is_local_only`, `v1_route_available`, and therefore `server_http_route_allowed` all funnel
  through its owner predicate; separate route consumers cannot disagree about whether a disabled
  roundtable row exists. The aggregate and roundtable route rows bind `delegate.aggregate` and
  `delegate.roundtable` respectively in their `op` fields, so the owner predicate cannot be bypassed
  by an unclassified matching row.
- `server_http_submit_op_run` repeats the operation preflight before allocating a run because internal
  callers bypass `v1_route_dispatch`. Current callers include MCP `ensemble_review` in
  `server_mcp.c` and authoring-pipeline submit/retry paths in `server_pipeline.c`.
- `mcp_build_full_served_list` calls the server-composition filter, so `tools/list`, `find_tools`, and
  `describe_tool` see the same active catalog. `handle_mcp_call` checks both collapsed family names and
  post-demux aliases before guardrails, pipeline mutation, or async submission.

The owner tables classify `delegate.aggregate`, `delegate.roundtable`, and `pipeline.*` operations;
they classify MCP `ensemble_review`, collapsed `pipeline`, and direct `pipeline_*` aliases. Non-owned
operations and tools pass through unchanged.

## Disabled and enabled behavior

Disabled HTTP operation routes return 404 and internal submission returns 404 without a run ID.
Disabled raw operations and MCP tools use unknown-method/tool semantics. CLI aliases remain visible and
retain the actionable activation diagnostic from slice 27. Enabling the module at server startup
restores the existing method, route, capability, async-run, and MCP catalog behavior.

## Verification

- `test_delegate_ensemble` covers startup reset, exact/prefix ownership, non-owned pass-through, and
  enabled/disabled operation and tool predicates;
- `test_server_dispatch` covers disabled handler non-invocation, `UNKNOWN_METHOD`, filtered
  `server.info`, and enabled compatibility;
- `test_server_http` covers disabled route 404, zero capability/authorization availability, internal
  preflight without a run ID, and enabled route/capability compatibility;
- `test_mcp_client_registry` covers disabled removal and enabled retention of `ensemble_review` and the
  collapsed `pipeline` family while retaining core `delegate`;
- the existing slice-27 tests retain config/environment precedence, zero provider work, and permanent
  workflow failure coverage.
