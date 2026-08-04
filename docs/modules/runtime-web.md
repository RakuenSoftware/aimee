# runtime-web module

## Purpose and non-goals

`runtime-web` is the optional browser interface for Aimee Runtime, the per-user interaction boundary. It
owns chat/session presentation, its dashboard, static assets, browser authentication/session handling,
the web listener, and web-only adapters. It does not own agents, memory, routing, Git, workflows,
configuration authority, or a separate dashboard lifecycle.

## Public contracts

The target lifecycle key is `runtime.web.enabled`. The module exposes one Runtime GUI whose dashboard is
inseparable from the GUI: no dashboard descriptor, enable key, standalone listener, or independently
active route set is permitted. Browser requests translate to the same Runtime contracts used by CLI,
MCP/ACP, and non-web APIs; UI convenience must not become a second execution authority.

The separately supervised module process serves one bounded Go stage at principal 23/event 9985. It
maps a server-classified RPC fault kind to the HTTP status exposed by runtime-web, preserving unknown
and unclassified faults as `502 Bad Gateway`. The server obtains that decision through its event bus
and includes the returned status in the existing error envelope. The physical `aimee-runtime-web`
provider consumes the status without importing or reimplementing the classification policy; a missing
or invalid status remains a generic `502 Bad Gateway`. Listener, TLS, authentication, sessions, route
dispatch, proxy I/O, and assets remain in the physical provider; the C `module_adapter.c` exists only
as a wire-parity fixture.

## Dependencies and consumers

- `config`: supplies effective settings and the startup lifecycle value.
- `gateway`: exposes authorized Runtime APIs without owning HTML or a fallback dashboard.
- `module-runtime`: supplies selection, lifecycle, capability, and readiness state.
- `protocols`: carries typed browser/API requests and responses.

Consumers are individual Runtime users interacting with chat, projects, agents, memory, workflows,
roundtables, Git, editor, settings, logs, and dashboard projections. Each optional page is conditional on
the corresponding active capability.

## Providers and readiness

Current providers are the Go service under `runtime-web`, the React SPA under `frontend/src`, legacy
`dashboard.c`/`dashboard_kb.c`, server dashboard routes, container entrypoint wiring, and systemd support.
Readiness must separate module selection, startup enablement, asset availability, listener/auth/session
health, Runtime transport reachability, and per-page capabilities. A running `aimee-runtime` does not
imply a running GUI.

## Configuration and activation

- `runtime_toggle.supported`: `true`; the descriptor is `enabled_by_default: true`, while the target lifecycle control is startup-evaluated and restart-class, with no automatic restart or live reload.

When `runtime.web.enabled` is false, no GUI process/listener, assets, web routes, background work, or
module metrics may start. CLI, environment, config-file, MCP/ACP, and non-web API operation remain.
Current containers use `AIMEE_WEBCHAT_ENABLED` and the `WITH_RUNTIME_WEB` image build argument,
while ordinary `make all` builds `aimee-runtime-web`. The `AIMEE_WEBCHAT_*` configuration names are
compatibility seams that remain during descriptor-driven unification.
Configuration pages expose only active settings with a real production consumer.

## Surfaces

Current surfaces include `aimee-runtime-web`, the SPA `App`, Chat, Dashboard, Projects, Agents, Settings,
Graph, Logs, Workflows, Roundtable, Pipeline, and Editor pages, plus `/api/*` web adapters. The dashboard
is a Runtime GUI route, not the legacy `aimee dashboard` standalone listener in the target contract.
Provider-specific Git OAuth controls are Git-provider settings, not OIDC governance.

## Data and migrations

Web-owned data includes the browser/session database at the configured `webchat.db`, CSRF/session state,
local TLS material, and SPA assets. Runtime DB1, workspaces, vault entries, workflow state, Git state, and
memory remain owned by core/optional capability modules. Migration must preserve active-session privacy
and avoid duplicating canonical application data in the web tier.

## Security and privacy

The web proxy's attested `ATTEST_WEBCHAT_TRUSTED` principal, Vault/login boundary, TLS, CSRF, session
cookies, origin handling, and server token are security-sensitive. The GUI cannot elevate a browser
identity, bypass core `execution-policy`, or read vault secrets for display. OAuth tokens used by Git
remain Git/vault data; they do not make the Runtime web module an OIDC provider or governance owner.

## Supported journeys

A user starts default Aimee Runtime, signs into the default-enabled `runtime-web` GUI, chats with agents, inspects the
dashboard, and uses only pages backed by ready capabilities. A headless user disables the GUI before
startup and performs the same underlying Runtime work through CLI, MCP/ACP, environment/configuration,
and non-web APIs without a web process.

## Tests and failure behavior

Current coverage spans `runtime-web` Go tests, frontend Dashboard/setup/tutorial tests, server
HTTP/dashboard tests, trusted-web principal/vault tests, server-envelope process parity and failure
handling, C/Go process parity, and container/build integrity. Future profile tests must prove
default-on behavior, independent disable/omission, dashboard co-lifecycle, no disabled residue, headless
journeys, truthful settings, and object/symbol absence. Authentication, transport, asset, or page-provider
failure must be contained and typed; it cannot start a partial authority or crash unrelated Runtime APIs.

## Operational diagnostics

Report `runtime-web` selection, startup-enabled state, listener/TLS mode, asset version, authentication and
Runtime-transport readiness, session counts, and page capability states. Exclude credentials, browser
tokens, prompts, response bodies, repository content, and vault material. Distinguish absent, disabled,
starting, ready, degraded, unavailable, and failed.

## Compatibility

`aimee-server`, `aimee-runtime-web`, `AIMEE_WEBCHAT_*`, `WITH_RUNTIME_WEB`, legacy dashboard
commands/routes, and current web package/asset names require bounded compatibility records during the
`aimee-runtime` rename.
Legacy web aliases are module-owned and unavailable when this module is disabled or omitted; core never
serves a compatibility dashboard.

## Extension and removal

New pages are capability consumers and must disappear with their inactive owner; they cannot add a
parallel scheduler, policy engine, data store, or config schema. `webchat`, `frontend/src`, legacy
dashboard code, server web routes, and packaging/service wiring are `relocate` candidates. Duplicate
routes, settings, auth helpers, and self-tested-only pages require supported-journey and production-read
evidence before consolidation or removal. The physical Go provider already lives under `runtime-web/`.
