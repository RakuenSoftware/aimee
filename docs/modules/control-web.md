# control-web module

## Purpose and non-goals

`control-web` is the optional browser interface for Aimee Control Plane. It owns the management GUI,
its dashboard, static assets, browser session shell, web listener, and web-only proxy behavior. It does
not own shared memory, the core audit ledger, governance policy, OIDC semantics, configuration authority,
or a separate dashboard module.

## Public contracts

The target lifecycle key is `control.web.enabled`. The module exposes one authenticated Control Plane
GUI whose dashboard is inseparable from that GUI: there is no dashboard descriptor, enable key,
standalone listener, or independently active route set. Browser actions consume the same typed Control
Plane APIs used by headless clients and cannot create a second configuration or authorization contract.

The separately supervised module process serves one bounded Go stage at principal 24/event 10241. It
authorizes console-admin and fleet proxy method/path pairs, preserving exact methods, segment counts,
`{id}` wildcard semantics, path bounds, and the fleet trailing-slash rejection. The physical
`control-web` provider imports that exact Go policy package, so live proxy behavior and the event-bus
boundary cannot drift. HTTPS, authentication, sessions, CSRF, credential selection, proxy I/O, and
assets remain in the physical provider. The C `module_adapter.c` is a wire-parity fixture. The KB
authorizes console-admin requests only by calling the separately supervised control-web module through
its event bus; an unavailable or invalid module response fails closed with HTTP 503, with no local
authorization fallback.

## Dependencies and consumers

- `config`: supplies the effective, activation-filtered configuration catalog and startup lifecycle value.
- `gateway`: exposes authorized Control Plane APIs without making HTML a core fallback.
- `module-runtime`: supplies selection, lifecycle, capability, and readiness state.
- `protocols`: carries typed browser/API requests and responses.

Consumers are Control Plane operators using the dashboard, accounts, governance, fleet, shared-memory,
and provider-administration pages. Optional pages consume their owning module only when that module is
selected and active.

## Providers and readiness

The current physical providers are `control-web` for the Go HTTPS/session proxy and shared route policy,
plus `frontend/src/console` for the React SPA. Readiness must distinguish module selection, startup enablement,
asset availability, listener state, Control Plane API reachability, credential validity, and optional-page
capabilities. A healthy `aimee-control` API does not imply that the GUI is running.

## Configuration and activation

- `runtime_toggle.supported`: `true`; the descriptor is `enabled_by_default: true`, but the target web control is evaluated at startup and changing it does not reload or restart a running process.

When `control.web.enabled` is false, the module must not load/register, bind, serve assets or routes,
start jobs, or emit module metrics. CLI, environment variables, configuration files, and non-web APIs
remain operable. The current `compose.yaml` ships `aimee-control-web` enabled and lets an operator set
`AIMEE_CONTROL_WEB_ENABLED=0`; the process then idles without a listener. Effective GUI configuration
must advertise only active settings with a proven production read; disabled or absent
module/provider settings are hidden.

## Surfaces

Current surfaces include the `control-web` HTTPS listener and proxy, `console.html`, `ConsoleApp`,
`ConsoleDashboard`, Accounts, and Governance pages. Dashboard routes are part of the same console shell.
OIDC issuer-profile controls appear only when `governance` is selected and active. The GUI may manage
workflows or other capabilities but cannot register their schedulers.

## Data and migrations

Current web-owned state is the `sessions.db` browser-session store, local TLS material, SPA assets, and
break-glass presence state under `KB_CONSOLE_HOME`. Canonical Control Plane data, OIDC profiles, audit
events, policies, and shared memory remain owned by their API modules. Migration must preserve session
confidentiality while avoiding copies of canonical configuration or secrets in browser/web stores.

## Security and privacy

The console-admin credential is read through `KB_CONSOLE_CRED_FILE` and must remain mode `0600`; inline
`KB_CONSOLE_CRED` is rejected. Non-loopback binding, TLS verification bypass, browser tokens, CSRF,
sessions, CSP origins, proxy allowlists, and break-glass access are security boundaries. Client secrets
belong in `vault`, never rendered configuration or browser state. Disabled and unknown web routes must be
externally indistinguishable; internal audit may retain `capability_absent`.

## Supported journeys

An operator starts the default-enabled `control-web` GUI, authenticates, views its dashboard, and administers
available Control Plane capabilities through typed APIs. If governance is active, the same GUI configures
a provider-neutral issuer profile. An operator can instead disable the entire GUI at cold start and
complete equivalent supported administration through headless surfaces.

## Tests and failure behavior

Current coverage includes `control-web` auth, ACL, session, TLS, proxy, rate-limit, console,
malformed-wire, C/Go event-bus parity, the KB provider seam, and fail-closed missing-module behavior;
`test_kb_http_routes` covers dashboard and OIDC configuration APIs. Future profile tests must prove
default-on and independent disable/omission, dashboard co-lifecycle, no disabled residue, headless
operation, truthful fields, and Make/CMake absence. Missing assets, invalid credentials, unreachable
Control APIs, or half-configured OIDC must fail closed without starting a misleading partial console.

## Operational diagnostics

Report `control-web` selection, startup-enabled state, listener/address, TLS mode, asset readiness,
Control API probe, authentication mode, and active page capabilities. Never log bearer values, OIDC
tokens, session identifiers, client secrets, or raw policy/config payloads. Diagnostics must distinguish
absent, disabled, starting, ready, degraded, unavailable, and failed states.

## Compatibility

`aimee-kb`, `aimee-control-web`, `CONTROL_WEB_*`, `/v1/console/*`, and existing console asset locations
are product/config/package surfaces requiring bounded compatibility records during the
`aimee-control` rename. A web-route alias exists only while this module is selected and enabled. The
dashboard remains inseparable throughout migration; compatibility cannot resurrect it in core.

## Extension and removal

New pages declare their owning capability and consume the generated effective catalog; they do not add
parallel config schemas, auth, data stores, schedulers, or dashboards. The provider lives under
`control-web/`, the SPA under `frontend/src/console`, and its image in `Dockerfile.control-web`.
Duplicate routes, settings, and assets require caller, production-read, profile, and journey evidence
before consolidation or deletion.
