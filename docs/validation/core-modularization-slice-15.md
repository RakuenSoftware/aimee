# Core modularization slice 15: core execution-path documentation

## Outcome

This slice promotes six required execution descriptors from documentation debt: `ir`, `translation`,
`routing`, `gateway`, `protocols`, and `delegates`. Together they describe the supported path from an
external request through canonical representation, conversion, target selection, execution, and response.
MCP and ACP are explicitly required protocol adapters, and gateway/delegates are explicitly core rather
than optional modules.

## Deep-dive findings

- `ir`, `translation`, `routing`, and `gateway` currently have descriptor-only module directories. Their
  implementations are spread across root, `src/server`, `src/gateway`, headers, and adjacent modules.
- IR is the canonical typed seam. Translation must converge legacy/provider builders without confusing
  byte-preserving passthrough with dead code. Routing owns agent/provider selection, not every file named
  router; HTTP dispatch, workflow routing, and channel delivery retain separate owners.
- Gateway's future physical boundary must separate the universal policy/IR/routing journey from optional
  delivery platforms while keeping gateway itself required core.
- Protocols already owns substantial MCP client/tool and ACP server code, while MCP server dispatch and
  ACP client/CLI code remain distributed. Same-protocol direction-specific code is not automatically
  duplicate. MCP and ACP cannot be extracted as optional plugins.
- Delegates already owns drivers, backends, routing seams, and run phases, while the main worker/API paths
  remain under server/root. Backend registration and self-tests do not prove a live production consumer;
  later source slices must trace actual command/filesystem binding before retaining or deleting a path.

No production code is removed in this slice. The retention criteria in `delegates.md` and the other module
documents apply to future source-movement slices, where liveness audits must distinguish compatibility/
parity branches from code supported only by its own tests.

## Cleanup and scope

This is documentation and baseline accounting only. It changes no production source, descriptor,
dependency, build graph, route, configuration behavior, database, GUI, or runtime profile. The status
baseline moves exactly `delegates`, `gateway`, `ir`, `protocols`, `routing`, and `translation` from debt to
substantive. The thirteen remaining debt entries are `audit`, `benchmarks`, `config`, `control-web`,
`execution-policy`, `git`, `governance`, `roundtable`, `runtime-web`, `tools`, `vault`, `workflows`, and
`workspace`.

Across these six descriptors, `runtime_toggle.supported: false` declares that no hot runtime enable switch
is supported and records the required-core target. It does not claim this documentation slice has already
consolidated physical source or replaced existing build/profile enforcement.

## Verification

- `python3 -I -S scripts/check_module_docs.py`
- `python3 -I scripts/tests/test_check_module_docs.py -v`
- `python3 -I -S scripts/check_module_source_ownership.py`
- `python3 -I -S scripts/check_cleanup_ledger.py`
- `python3 -I -S scripts/refactor_baselines.py`
- `make -C src lint`
- feature-branch pull-request CI
