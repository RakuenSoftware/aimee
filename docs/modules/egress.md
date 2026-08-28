# Egress module

`egress` is the sole HTTP/SSE transport for network calls initiated by Go
process modules. Callers attach a request-only bus identity distinct from their
serving identity and submit the exact method, destination, purpose,
credential-presence bit, request digest, limits, and bounded bytes over the bus.
Only this process opens the external connection.

The policy binds each decision to the bus-attested caller. Forge and review
artifact identities are restricted to fixed GitHub origins; remote MCP
destinations must resolve entirely to public addresses; and the memory identity
may reach a configured local embedder. DNS decisions are bound to the actual
dial address, redirects require a new governed request, response bodies are
bounded, and response content never enters WORM.

All authorize, unary HTTP, and stream lifecycle/frame stages are ledger-class.
The bus governance tap records caller, stage, trace, byte counts, status, intent,
and outcome while deliberately excluding credential-bearing requests and
external response content from the raw capture stream and immutable evidence.

Memory embedding, forge API, review-artifact retrieval, and MCP SSE carry their
request/response or streaming frames through this process. The static egress
check rejects direct network primitives outside declared network owners, and
the runtime launcher denies Internet sockets to ordinary process modules.

Forge credentials cross the Git process only as 30-second X25519/AES-GCM
envelopes. The authenticated scope fixes the egress key generation, Git caller
identity, `api.github.com`, operation, and owner/repository; the egress process
adds the bearer only after all of those checks pass. An egress restart revokes
outstanding envelopes. MCP instances carry `mcp:<egress-principal-ref>` rather
than an environment name. Egress deterministically maps that identity to
`AIMEE_MCP_<egress-principal-ref>_TOKEN` and asks the server/KB Vault helper over
a pipe. The helper accepts only the installed egress executable as its parent,
and both helper and egress disable dumpability before plaintext is present.

The runtime network-owner inventory is intentionally explicit:

| Process owner | Network capability | Constraint |
| --- | --- | --- |
| `egress` | outbound HTTP/SSE for Go process modules | caller/purpose/destination policy, pinned DNS, bounded bytes, seven ledger stages |
| `postgres` | PostgreSQL client transport | store-only process contract and DSN hardening |
| `sandbox` / `aimee-delegate-egress` | isolated tool/delegate proxy transport | separate destination policy and sandbox boundary |
| observability/browser/control listeners | inbound service sockets | declared core/listener ownership; not an ordinary outbound caller |
| C server and KB planes | provider, peer/KB/database, forge/git and management protocols | every remaining primitive is pinned by `core-network-contracts.json` to an owner, purpose, destination/credential constraint, audit disposition and review boundary; exact call counts are lint-ratcheted, while convergence into this egress service remains proposal work |

This distinction matters: the enforced claim is complete for ordinary Go
process modules. Trusted core protocols are formally constrained and cannot grow
silently, but are not represented as having traversed the Go egress transport.
