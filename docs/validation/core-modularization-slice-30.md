# Core modularization slice 30: required protocol boundary

## Scope

This slice makes the existing MCP and ACP implementation beneath `src/modules/protocols/` a
complete, checked required-core ownership boundary. It gives externally consumed headers one
canonical `aimee/protocols/{mcp,acp}/...` include form and makes the protocols descriptor complete
for its module-local implementation.

It does not relocate `src/server/server_mcp*`, `src/cli_mcp_serve.c`, `src/server/cli_acp.c`, or MCP
persistence. Those composition and storage moves require separate caller and data-boundary slices.
MCP and ACP remain subnamespaces of one required `protocols` module.

## Header disposition

The four headers used by production translation units outside the module are public:

- `aimee/protocols/acp/acp_server.h`;
- `aimee/protocols/mcp/mcp_client.h`;
- `aimee/protocols/mcp/mcp_client_registry.h`;
- `aimee/protocols/mcp/mcp_tools.h`.

`mcp_skill_tools.h` and `mcp_tools_gateway.h` remain private composition headers. The only
out-of-module consumer of `mcp_tools_gateway.h` was a self-only registration-count assertion; that
assertion and include are removed while the gateway behavior tests remain. This avoids promoting a
private helper solely to preserve a test seam.

All production and test consumers of the public contracts now use canonical includes. Make and
CMake use only `src/modules/protocols/include`; the broad MCP and ACP implementation-directory
include roots are removed.

## Complete ownership

`src/modules/protocols/module.yaml` declares every owner-local C source and private header, all four
public headers, direct MCP/ACP tests, and `docs/modules/protocols.md`. With `ownership_complete` set,
the shared descriptor validator rejects missing, extra, stale, duplicate, or misplaced module-local
implementation entries.

The shared header-layout checker now derives retired paths and include roots from nested canonical
namespaces. That keeps the rule generic: `mcp/mcp_client.h`,
`modules/protocols/mcp/mcp_client.h`, the old physical header, and the old MCP/ACP include roots all
fail without a protocol-specific allowlist.

## Compatibility and cleanup

This document is the human-readable slice record; the canonical structured cleanup entry is slice
30 in `tests/baselines/refactor/cleanup-ledger.json`.

Default Make/CMake translation-unit membership is unchanged. The slice changes no protocol framing,
messages, errors, negotiation, CLI command, route, symbol, persistence behavior, or runtime default.
It adds no forwarding header, parallel registry, compatibility alias, or new module ID.

Cleanup removes four old physical public-header paths, two broad global include roots, noncanonical
consumer includes, and one test-only dependency on a private registration helper. The remaining
server, CLI, and persistence paths are recorded physical-ownership debt rather than claimed by the
descriptor.

## Verification

- `python3 -I -S scripts/validate_module_descriptors.py --check-schema src/modules`;
- `python3 -I -S scripts/check_module_header_layout.py`;
- `python3 -I scripts/tests/test_check_module_header_layout.py -v`;
- MCP client, SSE, registry, native-surface, gateway, ACP server, and CLI protocol tests;
- Make/CMake build and object-membership baselines;
- full lint, module-document contract, public-surface baseline, and CI.
