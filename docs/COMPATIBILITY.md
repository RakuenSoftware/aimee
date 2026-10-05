# Compatibility

## Platforms

| Component | Linux | macOS | Windows |
| --- | --- | --- | --- |
| thin client, MCP, ACP | supported | supported universal binary | supported x86-64 binary |
| native TLS | OpenSSL | Secure Transport | Schannel |
| server, KB, workflow plane | supported | development use | use Docker or WSL2 |
| event-bus host | supported | not in v0 | not in v0 |
| delegate container backend | supported | through Docker | through Docker/WSL2 |

Debian 13 x86-64 is the main development and deployment target. Ubuntu 22.04+, Proxmox VE 8+, and
other current glibc distributions use the same server build. The native macOS and Windows products
are thin clients; POSIX process, PAM, `memfd`, and service-manager features remain server-side.

Linux client enrollment can create an mTLS certificate automatically. macOS and Windows use their
native TLS stacks but currently need an explicitly provisioned client certificate when mTLS is
required.

## Native-memory model plugins

The separate 0.3.2 candidate targets Linux x86-64, CPython 3.12, glibc 2.39+ and exactly vLLM
0.30.0. Its measured CUDA profiles are Gemma4 12B Q4_K_M, Gemma4 26B A4B Q3_K_M and Qwen3.8
27B FP8 with CPU matrix offload. The application platform table above does not establish plugin
compatibility. The signed candidate and source-export repair are prepared; final image qualification
and public installation remain pending. See
[Native memory](NATIVE_MEMORY_PLUGIN.md) and its [release preparation](releases/native-memory-v0.3.2/README.md).

## Coding tools

| Tool | Hooks | MCP | Other path |
| --- | --- | --- | --- |
| Claude Code | session, pre-tool, post-tool | yes | Anthropic Messages ingress |
| Codex CLI | session and tool hooks where supported | local plugin | OpenAI Responses ingress |
| VS Code | no native hook contract | yes | ACP and OpenAI-compatible model endpoint |
| GitHub Copilot | supported hook subset | yes | none |
| Claude Desktop | none | yes | none |
| OpenCode and similar front ends | none | optional | OpenAI Chat Completions ingress |

Client setup updates detected registrations without duplicating them. Set
`AIMEE_NO_CLIENT_INTEGRATIONS=1` to opt out.

Hook coverage is limited by the client. MCP and server-side tool policy still apply when a client
does not expose a pre-tool hook.

## Provider protocols

aimee has native translations for:

- OpenAI Chat Completions and Responses;
- Anthropic Messages;
- Gemini;
- Mistral;
- AWS Bedrock Converse/EventStream;
- local OpenAI-compatible endpoints, including Ollama-style servers;
- Codex OAuth and supported local provider CLIs.

All routes use the canonical IR, but a model still needs the capability requested by the task:
tools, images, reasoning, streaming, or a sufficient context window. Use `aimee model show` and
`aimee provider test` instead of assuming protocol compatibility means model compatibility.

Local CLI agents need their executable, login, and terminal runtime on the machine that executes
them. A remote workspace can run a CLI agent through the thin-client runner so the login stays on
the client.

## MCP and ACP

`aimee mcp-serve` uses JSON-RPC 2.0 over stdio and supports tool, resource, and prompt discovery.
HTTP/SSE MCP is not the primary client transport. The optional in-daemon MCP adapter is a module for
running registered integrations; it does not change the thin-client stdio contract.

ACP uses the local stdio bridge. Exact methods and tools depend on the installed build; inspect the
client with `aimee help --all` and the protocol handshake.

## Build requirements

Server builds need a C11 toolchain, GNU Make, SQLite with FTS5, libcurl, OpenSSL, pthreads, PAM on
Linux, and the remaining dependencies listed in the Makefile. Go builds cover supervised modules,
workflow and browser services; use the version declared in `server-go/go.mod`. Native application
hosts do not own libpq pools. PostgreSQL 18 and pgvector run in the separate store service;
pgvectorscale is optional where available. Some retained development fixtures still use libpq.

The Makefile is canonical for Linux development. CMake carries portable thin-client and test builds.

## Stability

| Surface | Contract |
| --- | --- |
| core CLI commands | deprecate before removal |
| named `/v1` routes | additive within a major; breaking changes use a new prefix |
| config descriptors | unknown keys rejected; documented migrations for removed keys |
| event-bus wire | frozen vectors and version negotiation |
| workflow definitions | canonical version hash and immutable run snapshot |
| proposals and validation reports | historical records, not compatibility promises |

## Known limits

- The event-bus v0 host uses Linux `memfd` and `SCM_RIGHTS`.
- Observational capture does not provide deterministic module execution replay.
- Native Windows does not host the full server stack.
- Automatic mTLS CSR enrollment is Linux-only today.
- Claude Desktop is MCP-only and has no coding-tool hooks.
- A local provider's claimed OpenAI compatibility may omit tools, usage, or streaming details.
- External PostgreSQL, KMS, PKCS#11, and witness deployments remain operator-owned integrations.
