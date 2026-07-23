# protocols module

## Purpose and non-goals

`protocols` is required core and owns supported external framing and adapter contracts, including MCP and
ACP as first-class core interfaces. It parses, validates, negotiates, and serializes protocol messages so
the rest of Aimee uses canonical IR and tool contracts. It does not own the tools themselves, route model
work, translate provider semantics, or make MCP/ACP optional extensions.

## Public contracts

MCP and ACP are subnamespaces of this one required module. Consumers include public headers only
through the module include root:

| Canonical header | Purpose |
| --- | --- |
| `aimee/protocols/acp/acp_server.h` | Inbound ACP framing, handshake, prompt parsing, updates, and stdio server contract |
| `aimee/protocols/mcp/mcp_client.h` | MCP transports, JSON-RPC framing, and client-session contract |
| `aimee/protocols/mcp/mcp_client_registry.h` | Configured remote-client registry, discovery, and namespaced dispatch |
| `aimee/protocols/mcp/mcp_tools.h` | Native MCP catalog, presentation profiles, discovery tools, and family demultiplexing |

Headers used only to compose module-local tool families remain beside their implementation and are
declared private. Production code outside `src/modules/protocols/` may not include those headers or
use a bare/source-tree spelling for a public header. There is one canonical header for each public
contract; the retired paths have no forwarding copies.

Canonical module source includes the MCP client, registry, tool-table/profile, gateway bridge, and
skill-tool code plus server-direction ACP in `src/modules/protocols/acp/acp_server.c`. Significant MCP server
dispatch and ACP client/CLI integration remain under root and `src/server`, including `server_mcp_*`,
`cli_mcp_serve`, and `cli_acp`; those are physical-ownership debt to inventory rather than evidence of a
second protocol module.

`src/modules/protocols/module.yaml` is the complete checked inventory for module-local sources and
private headers. Its `ownership_complete` latch means an undeclared owner-local translation unit or
private header, or a stale declaration, fails descriptor validation. Public headers, direct tests,
and this document remain explicit declared contracts.

## Dependencies and consumers

- `config`: supplies enabled endpoints, transports, limits, credentials references, and protocol policy.
- `ir`: supplies canonical request/response and streaming structures behind protocol adapters.
- `module-runtime`: supplies required lifecycle and extension contracts for always-present adapters.
- `translation`: maps canonical semantics to and from external client/provider shapes.

Consumers include gateway, tools, delegates, skills, CLI/editor integrations, server RPC, workflows, and
external MCP servers/clients and ACP editors/agents. Vault and execution policy protect credentials and
actions but do not replace protocol parsing or capability negotiation.

## Providers and readiness

MCP `stdio`/SSE client transports, the Aimee MCP server/proxy, inbound ACP server, and outbound ACP client
are distinct adapters beneath one required module. Each reports handshake, transport, capability, and
tool readiness independently. Required-core acceptance keeps Aimee's inbound MCP server/proxy and ACP
server present with handshake, transport, and capability diagnostics. Only remote MCP clients and outbound
ACP clients may remain unconfigured; that absence is normal and not a core readiness failure.

## Configuration and activation

- `runtime_toggle.supported`: `false`; protocol adapters are core while individual external endpoints and transports remain configurable.

`mcp_clients`, transport commands/URLs, timeouts, tool profiles, OAuth references, and ACP integration
settings tune concrete adapters. Config and web surfaces must appear only when their consumer exists and
must distinguish disabling one external client from removing MCP/ACP support. Protocol changes require
version/capability negotiation rather than an undocumented boolean fork.

## Surfaces

Surfaces include JSON-RPC framing, MCP `initialize`, `tools/list`, and `tools/call`, Aimee's stdio proxy and
server dispatch, ACP initialize/session/prompt/update messages, tool-call stream notifications, and editor
integration commands such as `aimee mcp-serve` and `aimee acp-serve`. Native tools reuse the MCP catalog
but remain owned by `tools`.

## Data and migrations

Protocol sessions, `mcp_clients` registry state, OAuth references, capability caches, and correlation
IDs may persist through config or owning server stores; wire requests remain transient. Migrations must
preserve client names, transport semantics, tool identifiers/schemas, ACP session identity, negotiated
versions, and credential references without copying live secrets into general protocol state.

## Security and privacy

All JSON-RPC `tools/call` methods, paths, tool names/arguments, external commands, URLs, and server responses are
untrusted. Registry startup applies OSV/package gates where relevant; OAuth tokens belong in vault-backed
storage; tool calls require execution-policy authorization. Protocol logs must redact credentials and
private content while preserving method, correlation, transport, and bounded error details.

## Supported journeys

Aimee can serve its tool catalog to an `MCP` client, connect to configured external MCP servers and merge
their tools under controlled profiles, accept an ACP editor session and stream typed updates, or drive an
outbound ACP agent. Each adapter converts at the IR/tool boundary so gateway, routing, and delegates use
the same core journeys rather than protocol-specific execution loops.

## Tests and failure behavior

`test_mcp_client.c`, registry/SSE/integration tests, `test_mcp_native_surface.c`,
`test_cli_mcp_serve.c`, `test_acp_server.c`, `test_cli_acp.c`, gateway-tool tests, and native-dispatch tests
cover framing and bridges. Malformed JSON, handshake mismatch, unavailable transport, unsafe package, or
unknown tool fails with a protocol error and must not dispatch a partial or unauthorized action.

Run `python3 -I -S scripts/validate_module_descriptors.py --check-schema src/modules` for descriptor
set equality and `python3 -I -S scripts/check_module_header_layout.py` for canonical include/path
enforcement. `python3 -I scripts/tests/test_check_module_header_layout.py -v` plants retired nested
MCP/ACP include roots and spellings to prove the boundary check fails closed.

## Operational diagnostics

Use per-adapter readiness, negotiated version/capabilities, registry/client state, `transport` lifecycle,
tool catalog counts, JSON-RPC method/correlation IDs, and bounded protocol errors. Diagnostics should
distinguish parse, handshake, transport, profile, authorization, tool, and downstream execution failures
without exposing OAuth tokens, command environments, tool secrets, or private message bodies.

## Compatibility

MCP/ACP method names, JSON-RPC IDs, capability negotiation, tool schemas/names, error envelopes, stream
ordering, and session semantics are compatibility contracts. Moving root/server MCP and ACP code into
`src/modules/protocols` requires full caller/build inventories and wire fixtures. A simplified ACP dialect
must not be presented as full ACP, and partial support must remain explicit in capability negotiation.
This header-boundary change preserves the existing wire behavior, CLI names, routes, exported symbols,
and Make/CMake object membership; it changes only header ownership and include spelling.

## Extension and removal

Add protocols as `adapter` implementations around IR, tools, policy, and gateway rather than cloning routing or execution.
Consolidate MCP tables and ACP client/server framing only after verifying direction-specific semantics;
same acronym does not imply duplicate code. MCP and ACP are required core adapters and cannot be extracted
as optional plugins, though individual remote endpoints can remain unconfigured.
