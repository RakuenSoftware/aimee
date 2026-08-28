# Security model

aimee assumes models, prompts, retrieved text, tool arguments, repositories, MCP packages, and
remote networks can all be hostile. Enforcement lives in the services and execution backends, not
in a prompt asking a model to behave.

## Release-qualified claims

The claim IDs below are defined with exact artifacts, defaults, enforcement owners, negative tests,
and limitations in [`security-claims.json`](security-claims.json). CI rejects an unqualified claim.

- **SC-001:** the shipped thin client has no DB1 or DB2 linkage.
- **SC-002:** the server has no DB2 query authority and the KB has no DB1 query authority.
- **SC-003:** network-reachable server and KB routes require an authenticated principal and declared
  capability; the KB's credential-free mode is restricted to process-local loopback and disables
  authenticated-owner mutations.
- **SC-004:** remote writes require a KB-signed identity and a live per-user grant.
- **SC-005:** registered tool paths pass schema, policy, assigned-workspace, and backend checks.
- **SC-006:** delegate containers receive neither provider nor forge credentials by default.
- **SC-007:** v2 WORM rows detect changes to chronology, attribution, ordering, and content; migrated
  v1 rows are exported as `v1-partial` and do not claim full-field coverage.
- **SC-008:** write-capable delegates use mandatory container isolation with no host fallback.

These guarantees do not make arbitrary native code safe and do not protect a host after full root
compromise without an external witness.

## Principals

Every remote or browser operation resolves to a principal:

- a local Unix-socket user;
- an enrolled thin client;
- an OIDC or PAM user;
- a browser session;
- the supervised workflow peer;
- a service identity;
- an agent acting for one of the above.

Governed tool records bind the unique session/run identifier and delegate role. Services also bind
verified issuer, subject, transport identity, and team where those identities are available. Legacy
and autonomous paths that have not yet received a verified user identity are explicitly recorded as
service actors; they must not be presented as user attribution.

## Trust boundaries

| Boundary | Enforcement |
| --- | --- |
| local client → server | filesystem ownership and Unix-socket permissions |
| remote client → server | TLS, certificate pin, bearer or mTLS identity, route capabilities |
| user → write route | signed identity token, server/team trust, per-user write grant, replay check |
| browser → web service | login, secure cookie, CSRF checks, principal propagation |
| server → KB | service authentication, TLS where configured, typed `/v1` routes |
| workflow → resource plane | direct supervised peer, kernel identity, narrow internal operations |
| agent → workspace | assigned root, worktree, path normalization, write authority |
| delegate → host | mandatory container isolation, resource limits, explicit mounts and mediated egress |
| service → provider | vault lookup, catalog, budget, rate, and egress policy |
| module → event bus | admission, private rings, subscription authorization, bounded credits |

The managed deployment mounts the host Docker socket in `aimee-server`. Anyone who controls that
server can control the Docker host. Use the split stack when that is outside the intended trust
boundary.

## Remote access

`aimee remote set` stores the supplied bearer, pins the server certificate, and on Linux enrolls a
client certificate. It does not rotate the bearer. Verify the printed fingerprint out of band.

After bootstrap, `aimee remote enroll` adds a bounded per-client bearer without invalidating
existing clients. A bearer rotation is an explicit revoke-all: it replaces the primary and clears
every additionally enrolled bearer in persisted and live state.

When `AIMEE_API_BEARER_TOKEN` supplies the primary, rotate that deployment secret and restart
instead of calling the API rotation route. A changed deployment primary revokes enrolled bearers;
an unchanged primary preserves them across an ordinary restart.

The shared bearer is read-only. Remote write authority comes from a short-lived, single-use,
KB-signed identity token whose `(server, team, subject)` has a live grant. `data` permits memory,
document, and index writes. `full` also permits agent, delegate, runner, and workspace control.

The server pins the signing authority through `AIMEE_SERVER_MGMT_JWKS_TRUST_BUNDLE` and enforces its
configured server and team IDs. Token replay, unknown keys, wrong audience/team, expired grants, and
unavailable replay storage fail closed. Grant administration is local-Unix-socket only.

`aimee.api.remote_writes` is still parsed so old configuration loads, but it no longer authorizes a
user write. A non-off value produces a warning and the `remote_writes.global_ignored` diagnostic.

Enrollment material in `remote.conf` is mode `0600`, opened without following symlinks. Do not copy
one client's certificate to another machine. Revoke the old identity and enroll the replacement.

## Event bus

The bus is an intra-daemon trust boundary, not a network perimeter. The Linux host creates anonymous
memory regions, admits a client over `SOCK_SEQPACKET`, then passes only that client's queue pair and
the shared arena.

- the control region is read-only;
- a client cannot map or enumerate another client's rings;
- observers receive only registered event kinds;
- requests and replies are correlation-bound;
- queues and arena leases are bounded;
- client reap releases held references;
- the full-stream tap is reserved for core audit and governance.

The arena assumes admitted native modules are trusted. It prevents accidental cross-client queue
access; it is not hostile-code isolation. Run untrusted extensions out of process behind a narrower
contract.

See [Event bus](EVENT_BUS.md).

## Guardrails and tool policy

Guardrails run before a tool or write reaches the backend. They cover:

- secret and production paths;
- writes outside the assigned worktree;
- planning or read-only mode;
- destructive or unsupported operations;
- direct sub-agent launchers when delegates are required;
- MCP package health and allowlists;
- tool schema and capability requirements;
- semantic risk and anti-pattern signals.

A model cannot override a hard deny in prose. A soft warning may require explicit operator or
workflow handling. Semantic guardrails produce a structured decision that is audited through the
event bus.

Path checks use canonical workspace roots. A symlink, `..`, alternate spelling, or client-local path
must not escape the assigned authority.

## Delegate isolation

Write-capable delegates use isolated worktrees. Container delegates default to:

- no network;
- no provider or git credentials;
- a narrow workspace mount;
- bounded CPU, memory, process count, and lifetime;
- an explicit base image and toolchain;
- mediated package access.

Custom images and packages expand the trusted computing base. Build them outside the agent's
control, pin what you can, scan them, and keep the Docker socket out of the container.

If the requested isolation cannot be applied or proved after start/resume, the delegate is refused
and its container is destroyed. There is no degraded host fallback.

See [Delegate sandbox](DELEGATE_SANDBOX.md).

## Credential custody

The server vault is the source of truth for provider keys, OAuth tokens, git credentials, and other
agent secrets. A turn resolves a credential only after principal, provider, agent, and policy checks.

The vault supports local root-key custody and hardened backends such as TPM 2, PKCS#11, or KMS.
Rotation and reseal operations have recovery records. Locking evicts the cached key; it does not
rewrite every stored secret.

Do not store secrets in:

- `agents.json`;
- workflow definitions or artifacts;
- prompts, memory, or project documentation;
- delegate container environments unless the policy explicitly grants that one credential;
- client-side key files.

Container environment variables are permitted only as first-boot transport.
Run `scripts/aimee-compose-vault-bootstrap.sh` before `docker compose up`; it
streams credential-shaped values into a disposable helper, seals them in the
owning server or KB Vault, and removes the helper. Long-lived server and KB
services must not declare credential-shaped environment keys, even when the
value would be supplied through Compose interpolation. The build-integrity gate
enforces this metadata boundary.

Local CLI agents may use a login that remains on the thin client when execution runs there. The
server sends commands over the authorized runner channel; it does not copy the login.

Every vault access emits an audit event with a bounded secret fingerprint, never the secret.

## MCP and package supply chain

The MCP registry records installed servers and their package source. The OSV gate checks known
vulnerabilities and can block an unhealthy package. Recheck before enabling a stale or changed
server:

```bash
aimee mcp audit
aimee mcp recheck <name>
```

MCP tool arguments, outcomes, and completion state are audited. Installing an adapter into the
server or KB does not grant it full database access; it receives the module and route contracts it
declares.

## Browser and git

Browser sessions are per user. Project selection is not authorization by itself; server routes
recheck the principal.

Git tokens and SSH keys live in the vault. SSH clones use a short-lived agent and a per-principal
`known_hosts`. First contact uses trust on first use; later connections require the recorded host
key. Verify a new host key through another channel when the repository is sensitive.

VS Code runs per user and is proxied through the authenticated browser service. It still has the
authority of that user over the selected workspace.

See [Web git security](WEBCHAT_GIT_SECURITY.md).

## Audit

The WORM store is append-only and hash-chained. Checkpoints commit the current head. A seal exports
a verifiable snapshot. Retrieval traces, provenance, fidelity, policy decisions, and action rows use
the same verification surface.

```bash
aimee audit verify
aimee audit checkpoint
aimee audit seal --help
```

The event-bus capture stream preserves accepted order and materializes large payloads. It is for
inspection and observational replay; it never runs a captured tool call.

A local ledger cannot prove integrity against an attacker who controls the host, the process, and
its keys. Use the out-of-process sealer and off-host witness/anchor for that threat. Alert on witness
lag, anchor failure, WORM verification failure, bus drops, or an uncovered enforcement path.

## Data and privacy

Core services do not enable telemetry by default. Configured providers, git hosts, package sources,
vulnerability checks, MCP servers, model fetches, and other integrations can make outbound calls;
consult the deployment egress policy before enabling them.

Memory and document ingestion can retain sensitive source text. Scope the KB, apply the retention
and erasure rules in [Data governance](DATA_GOVERNANCE.md), and avoid sending restricted evidence to
an external synthesis provider. Memory audit uses fingerprints for keys that can contain personal
data.

## Non-goals

- protecting a host after root compromise without an external trust anchor;
- treating an admitted native module as hostile code;
- making provider terms permit unattended use;
- preventing an authorized user from reading data their grant allows;
- replacing repository review, backups, or normal host hardening;
- claiming deterministic module execution from observational capture.

## Operator checklist

- change browser bootstrap credentials;
- verify certificate fingerprints before enrollment;
- configure server/team/JWKS trust before enabling remote users;
- grant users individually and review grants regularly;
- keep the Docker socket out of deployments that do not need managed launch;
- use the vault, never plaintext config, for credentials;
- keep delegate network off unless the task needs it;
- run `aimee audit verify` and monitor bus drops;
- back up DB1, DB2, workflow state, vault custody, TLS state, and audit anchors;
- test revocation and restore procedures before an incident.
