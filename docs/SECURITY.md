# Security Model

## Overview

This document describes the security model for `aimee`, including its trust boundaries, principals, attack surfaces, capability model, enforcement path, token handling, and known non-goals.

aimee runs primarily as same-user, local-machine operation. Its strongest protections apply to local clients connecting to the local `aimee-server` over a Unix domain socket, where the server validates the caller's operating-system identity with `SO_PEERCRED`.

The model distinguishes clearly between what is protected and what is not:

- Protected:
  - Separation between same-UID and different-UID local processes on the host
  - Separation between authenticated and unauthenticated clients
  - Restriction of delegated operations to explicitly granted capabilities
  - Mediation of sensitive operations through server-side guardrails
- Not fully protected:
  - Compromise by processes already running as the same local user
  - Full confidentiality or integrity of content sent to external delegate providers
  - Browser access as a hardened Internet-facing security boundary
  - Administrative isolation equivalent to a multi-tenant remote service

## Trust Boundaries

```mermaid
flowchart TB
  subgraph TZ[Trusted Zone: same-user local machine]
    PA[Primary Agent\nClaude / Gemini / Codex]
    CLI[aimee thin client]
    SRV[aimee-server]
    DB[DB1 local store]
    DA[Delegate agents\nsub-agents via HTTP]

    PA <--> |Unix socket\nSO_PEERCRED + auth token| SRV
    CLI --> |hooks via stdin/stdout| PA
    SRV --> |fork/exec| DA
    SRV --> DB
  end

  subgraph STZ[Semi-Trusted Zone]
    WEB[aimee-server webchat]
    BROWSER[Browser]
    WEB <--> |HTTPS + PAM\nself-signed TLS| BROWSER
  end

  subgraph UTZ[Untrusted Zone]
    HTTP[Delegate agent HTTP client]
    PROVIDERS[OpenAI / Anthropic / Gemini]
    HTTP --> |HTTPS\nAPI key / OAuth| PROVIDERS
  end
```

Boundary summary:

- Trusted zone:
  - Local same-user components on the same machine
  - Trust is based largely on OS-level same-UID identity and local process assumptions
- Semi-trusted zone:
  - Browser-facing webchat path protected by HTTPS and PAM
  - Useful for authenticated operation, but not treated as equivalent to the local Unix-socket boundary
- Untrusted zone:
  - External model providers and any network path used to reach them
  - Requests may be authenticated, but remote systems are outside the local trust domain

## Principals

| Principal | Identity | Trust Level |
|-----------|----------|-------------|
| Local user | `SO_PEERCRED` UID match | Full (same-user) |
| Authenticated client | Capability token | Operational (no admin) |
| Unauthenticated same-UID | `SO_PEERCRED` only | Read-only |
| Different-UID local process | Different OS identity | Untrusted |
| Browser user | PAM-authenticated session | Semi-trusted |
| External delegate provider | API key or OAuth to remote service | Untrusted |

Principal distinctions:

- Local user:
  - A process connecting over the Unix socket with a matching UID is the most trusted operational principal in the system.
- Authenticated client:
  - A holder of a valid capability token can perform only the actions explicitly granted by that token.
  - This principal is operational, not administrative.
- Unauthenticated same-UID:
  - Same-user identity alone may permit limited read-only behavior, but does not imply full access.
- Different-UID local process:
  - A different local OS user is not trusted merely for being on the same machine.
- Browser user:
  - PAM-backed browser access is authenticated, but the browser path is treated as less trusted than the Unix-socket path.
- External delegate provider:
  - External AI providers are required for some delegated operations but are outside the trusted computing base.

## Attack Surfaces

The primary attack surfaces are:

1. Unix socket interface
   - Local IPC endpoint to `aimee-server`
   - Protected primarily by filesystem permissions, local host access, and `SO_PEERCRED`
   - Main concern: unauthorized local access, confused-deputy behavior, or misuse by same-user processes

2. Capability tokens
   - Used to authorize operational actions
   - Main concern: token theft, overbroad grants, replay within the valid lifetime, or incorrect scope enforcement

3. Browser webchat
   - HTTPS endpoint with PAM-backed authentication and self-signed TLS
   - Main concern: session misuse, local browser trust assumptions, and weaker guarantees than the Unix-socket channel

4. Delegate execution path
   - Server fork/exec of delegate agents and sub-agent orchestration
   - Main concern: privilege misuse, unsafe parameter forwarding, or unintended expansion of allowed actions

5. Delegate HTTP client to external providers
   - Outbound HTTPS to OpenAI, Anthropic, Gemini, or similar providers using API keys or OAuth
   - Main concern: disclosure of prompts, metadata, or outputs to third parties; dependency on remote provider security and policy

6. DB1-backed local state
   - Local persistence used by the server
   - Main concern: tampering by same-user processes, accidental over-retention, or unauthorized reads on a compromised host account

7. aimee-server `/v1` HTTP listener (optional, off by default)
   - A loopback-only TCP endpoint (`http://127.0.0.1:<port>/v1`) turned on with `aimee api enable`; it exposes the OpenAI-compatible surface to local editors
   - Bearer-authenticated, per-bearer rate-limited, and gated by token scope; a scoped token is denied with `403` when it requests an action outside its grant
   - Main concern: bearer theft by a same-host process, overbroad token scope, or exposing the port past loopback through a misconfigured proxy

8. aimee-kb (DB2) transport
   - The knowledge store is reached over a same-host Unix socket, or over TLS to a shared host
   - The Unix-socket path leans on the local OS boundary; the remote path requires TLS (mutual TLS where configured) plus a bearer or scope-bound token, and denies cross-scope access with `403`
   - Main concern: weak transport configuration on the remote path, token theft, or scope confusion between projects and workspaces. See [PUBLIC_API.md](PUBLIC_API.md) for the full TLS and token model

These protections are scoped narrowly. They resist different-UID local misuse better than same-UID local compromise. If an attacker already controls the same local user account, many protections reduce to best-effort mediation rather than strong isolation.

## Capability Model

Capabilities are the core authorization mechanism for operational actions. A client may identify as the same local user, but access to non-read-only actions still depends on possession of a valid capability token and successful guardrail checks.

```mermaid
flowchart LR
  P[Principal] --> I[Identity established]
  I --> C{Capability token present and valid?}
  C -- No --> RO[Same-UID read-only access only]
  C -- Yes --> S[Scoped capabilities]
  S --> A1[Allowed operational action]
  S --> A2[Allowed delegated action]
  S --> D[Denied if action outside scope]
```

Capability properties preserved by this model:

- Capabilities grant operations explicitly, not implicitly.
- Holding a token does not make a client administrative.
- Same-user identity and token possession are distinct signals.
- Read-only behavior may be available to unauthenticated same-UID callers, but broader actions require a token.
- Requests outside the token scope must be denied even if the caller is local and authenticated.

What the capability model protects:

- Accidental or unauthorized use of operational APIs without an explicit grant
- Lateral use of the server as a confused deputy for actions not covered by a token
- Expansion from authenticated status to unrestricted access

What it does not protect:

- Misuse by a same-user attacker who can steal or invoke a valid token
- Actions that are intentionally within the granted token scope
- Content confidentiality once data is intentionally sent to an external delegate provider

## Guardrail Enforcement

Sensitive operations are mediated server-side. The enforcement chain combines transport identity, token validation, and action-level checks before work is executed.

```mermaid
flowchart TD
  REQ[Client request] --> ID[Establish caller identity\nSO_PEERCRED or web auth]
  ID --> TOK[Validate capability token]
  TOK --> SCOPE[Check requested action against scope]
  SCOPE --> RULES[Apply server-side guardrails]
  RULES --> DECISION{Allowed?}
  DECISION -- Yes --> EXEC[Execute local action or delegate]
  DECISION -- No --> DENY[Deny request]
```

Enforcement expectations:

- Identity is established first.
- Capability validation happens before operational execution.
- Scope checks are performed before local actions or delegation.
- Guardrails are enforced on the server side, not delegated to clients.
- Denial is the expected outcome when identity, token state, or requested capability does not satisfy policy.

This model protects against clients claiming authority they do not have. It does not protect against arbitrary actions by a fully compromised same-user environment.

## Token Lifecycle

Capability tokens are lifecycle-managed security artifacts on the active authorization boundary.

Lifecycle stages:

1. Issuance
   - A token is created with explicit operational scope.
   - The token represents a bounded authorization grant, not blanket trust.

2. Presentation
   - The client presents the token when requesting protected actions.
   - Presence of a token supplements, but does not replace, transport or session identity.

3. Validation
   - The server verifies that the token is recognized, valid, and suitable for the requested operation.
   - Invalid, missing, or insufficiently scoped tokens must result in denial.

4. Use
   - Actions are limited to the token's granted capabilities.
   - Delegated work is still subject to server-side checks.

5. Expiry or revocation
   - A token should cease to authorize actions once expired or no longer accepted by the server.
   - Expired or revoked tokens must not continue to permit operational access.

Security implications:

- Token secrecy matters because possession enables the granted operations.
- Token scope matters because overbroad tokens enlarge the blast radius of theft or misuse.
- Server-side validation matters because local identity alone is insufficient for broader operations.

## Agent Credential Custody (thin client)

Third-party agent/delegate API keys (and Codex/OAuth tokens) are sealed in the
server's **credential vault** — encrypted at rest — and are the server's single,
permanent credential store. The legacy client-held model (client keyring + a
RAM-only per-session push) was retired.

- **Storage of record is the server vault.** Keys are sealed under the server
  principal, encrypted at rest; `aimee agent add … --key K` writes `K` into the
  vault and **refuses plaintext storage**. The server's `agents.json` holds
  definitions (endpoint, model, roles) only — never the key. Codex/OAuth tokens
  are vaulted the same way (a legacy plaintext token is migrated and scrubbed on
  first use).
- **Autonomous unseal, no interactive unlock.** Each data-encryption key is
  wrapped twice — once for an interactive principal and once under a server
  master key (`.vault/.server-master.key`) — so the server can decrypt a
  credential on its own to run a turn without a human unlocking the vault.
  Credentials are resolved per turn from the vault (the turn's attested
  principal, falling back to the server principal).
- **No client custody, no RAM keyring.** The client-held keyring
  (`~/.config/aimee/agent-keys.json`) and the per-session push (`POST
  /v1/session/credentials`) are gone; `aimee agent key import` migrates any
  leftover client keys into the vault. Agents are configured **once on the
  server** and shared across every client.
- **Trade-off.** The server is now a durable secret store, so the protection
  boundary is the server host and especially `.vault/.server-master.key` (the
  root of the autonomous-unseal capability) — restrict its file mode and host
  access and threat-model the server host accordingly.
- **Transport.** `agent add --key` sends the key over the same authenticated
  `/v1` channel as other requests; on a plaintext-HTTP LAN deployment it is only
  as confidential as that network. Use TLS / a trusted network for the server
  endpoint.

See [THIN_CLIENT.md](THIN_CLIENT.md) for operational details.

## Local-CLI agent execution stays on the client

A `--provider claude` agent runs the standard `claude` CLI in a tmux session,
which executes where the binary and login live — the **client** — even when it
is driven through a remote `aimee-server`. On a detached workspace the tmux
session driver marshals its tmux commands over the runner reverse channel and the
client runs them locally, with `claude` authenticating via the client's own login
(`~/.claude`). (`claude -p` print mode is not used.)

- No Claude credential is transmitted to or stored on the server; the server only
  relays the prompt and reads back the captured session output.
- The CLI runs against the client's working tree, under the client user's
  identity — the server gains no new ability to execute binaries it does not have.
- This keeps the server from being a place where third-party agent logins
  accumulate (a `claude` CLI subscription login is not an API key and is not
  vaulted; it stays on the client; see [DELEGATES.md](DELEGATES.md)). On a
  plaintext-HTTP
  LAN deployment, the prompt relayed to the server is only as confidential as
  that network — use TLS / a trusted network for the server endpoint.
- Claude run via the `claude` CLI login (not an API key) is **primary-only by
  default** — see [DELEGATES.md](DELEGATES.md#claude-via-the-cli-is-primary-only-by-default)
  for the account-risk rationale and the `claude_cli_delegate_enabled` opt-in.

## Explicit Non-Goals

This security model does not aim to provide:

- Protection against a hostile process already running as the same local user with the ability to inspect local process state, files, or tokens
- Internet-hardened exposure for the browser interface comparable to a public SaaS perimeter
- End-to-end confidentiality from the local system to external AI providers once prompts or outputs are intentionally sent over delegate HTTP APIs
- Strong multi-tenant isolation between mutually untrusted users on the same host beyond the documented local-user and token boundaries
- Administrative privilege separation equivalent to a dedicated sandbox, VM, or MAC-enforced isolation layer

These non-goals are intentional design constraints.

## Audit History

This document covers:

- Trusted, semi-trusted, and untrusted zones
- The principal classes and their trust levels
- The Unix socket, browser, delegate, token, and DB1 local-state attack surfaces
- Capability-scoped operational authorization
- Server-side guardrail enforcement before execution
- Token-based authorization as distinct from local transport identity

No separate historical audit record exists yet. This section is the baseline for future audit updates.
