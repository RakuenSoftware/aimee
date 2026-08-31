# Runtime and Control product boundary: Go residual

- **State:** PENDING. Corrective Go-owned residual restored 2026-08-16 after rejection #2690.

**Archived source:** [`product-governance-web-and-config.md`](../done/product-governance-web-and-config.md)

## Correction

PR #2690 rejected this residual because its Go web processes crossed C product hosts and
configuration plus build and deployment packaging. That mixed placement is migration evidence, not a
reason to discard the product boundary. The remaining decisions have existing Go owners:
`runtime-web` and `control-web` own their processes and web lifecycles, and the Go config store is the
target authority for effective settings. C hosts and adapters, container/service definitions, install
targets, and compatibility aliases transport or package those decisions; they do not own them.

The rejection's mixed-owner concern is therefore resolved by assigning every remaining policy
decision to `runtime-web`, `control-web`, or `server-go/internal/config`; retained C, build, packaging,
and deployment surfaces are mechanical consumers whose parity is acceptance evidence, not independent
ownership.

This correction restores the proposal lifecycle only. None of the runtime acceptance below is claimed
as implemented.

## Delivered baseline and current gaps

The physical `runtime-web/` and `control-web/` programs are Go executables. Their separately supervised
stages under `server-go/modules/runtime-web` and `server-go/modules/control-web` already own bounded Go
status/authorization decisions, while C module adapters are parity fixtures. The Go config store under
`server-go/internal/config` serves a live validated API seam, but its current editable projection does
not yet cover the complete product surface.

The current processes and packaging do not satisfy this residual merely by existing. Static GUI field
lists, environment-specific enablement, an enabled process that idles without becoming ready, legacy
product names, and packaging that can imply availability independently are migration inputs. Each must
converge on the Go decisions below or be removed.

## Go ownership

### Runtime and Control web processes

The physical Go `runtime-web` process owns the Runtime browser listener, authentication and sessions,
routes, assets, web-only jobs and metrics, and its readiness. The physical Go `control-web` process owns
the equivalent Control Plane surface. Their server-go module stages may supply bounded policy decisions,
but do not create a second listener, dashboard, configuration authority, or lifecycle.

Each web process publishes one startup state: `disabled`, `starting`, `ready`, `degraded`, or `failed`,
bound to its product, effective-config version, asset version, and upstream transport state. Process
existence is not readiness. A C host or deployment probe may relay that state but cannot promote it.

### Effective settings and capability truth

The Go config store is the target authority for effective Runtime and Control settings. It validates
and snapshots persisted/default input, then combines that snapshot with admitted Go module/process
state once. Both the advertised configuration catalog and runtime capability advertisement project the
same effective result.

Every projected field carries its owner, activation condition, reload class, secrecy class, and proven
production consumer. A CLI, API, GUI, profile generator, or package script may filter or render the
result for its audience, but cannot invent a field, default, capability, active state, or readiness
claim. Missing, invalid, stale, or mismatched input fails closed and leaves the last valid version
explicit; it cannot advertise a configured-but-inactive surface as effective.

The broad current C configuration contract remains a compatibility input until callers migrate. It may
parse and forward legacy values, but cannot independently decide effective web settings or capabilities.

## Lifecycle, disablement, and omission

`runtime.web.enabled` and `control.web.enabled` are startup-evaluated, restart-class controls owned by
their Go processes and effective config. They do not trigger an automatic live restart.

- When disabled, supervision does not launch the process. If a disabled binary is launched
  accidentally during migration, it must not register, bind, load or serve assets/routes, start jobs,
  emit active-module metrics, or report ready. An idle compatibility container is not an active web
  process and cannot satisfy readiness.
- When omitted from a build/profile, the selected executable, assets, service/image layer, module grant
  and registration, routes, metrics, and linked symbols are absent. Omission cannot substitute a stub
  provider or core-hosted HTML fallback.
- Runtime and Control remain fully operable through their supported headless CLI, configuration, and
  non-web API journeys when either GUI is disabled or omitted.
- Failure, restart, disablement, or omission of one web process does not change the other product's
  lifecycle or the readiness of unrelated core capabilities.

Legacy route aliases remain owned by the corresponding Go web process and disappear with it. External
disabled and unknown routes retain the same bounded response shape; any internal `capability_absent`
reason is audit evidence, not a fallback route.

## Product and transport boundary

Aimee Runtime remains the per-user interaction boundary and Aimee Control remains the multi-tenant
management/governance boundary described by the archived proposal. Each product hosts its own local
event bus; module admission on one bus does not make that module callable through the other.

Cross-product calls use the existing authenticated, versioned network transport. They preserve product,
principal/tenant, request, protocol-version, deadline, and correlation identity. No in-process call or
event-bus shortcut may bypass network authentication, merge readiness domains, or turn an unavailable
remote product into a local fallback. Timeout, authentication failure, version mismatch, and remote
unavailability remain distinct typed outcomes.

## Naming, packaging, and upgrades

Canonical product identity is `aimee-runtime` and `aimee-control`. The bounded `aimee-server` and
`aimee-kb` compatibility names remain migration aliases only. Go owners publish canonical process,
product, version, capability, and readiness identity; Docker, Compose, systemd, package, and install
logic consumes that identity mechanically.

Every retained alias has a documented owner, old/new mapping, supported version window, and removal
gate. Packaging cannot parse a second enablement policy, infer readiness from a PID/container, or expose
an omitted capability. New source, route, config, image, or service references use canonical names.

Upgrade evidence covers the current legacy-name release, the bounded dual-name transition, and the
canonical-only release. Both mixed-version directions must either interoperate through a declared
network protocol version or refuse with an actionable version error. Rollback restores the prior
package/config state without reviving disabled web residue or losing the last valid effective settings.

## Acceptance

- Runtime-only, Control-only, and full deployment fixtures report exact product identity, route,
  config, capability, process, and readiness state from the Go owners.
- Each web process can be independently disabled at cold start. No listener, registration, route,
  asset, job, active metric, or ready result remains, while all documented headless journeys pass.
- Build-profile omission proves absence of the executable, assets, service/image layer, grant,
  registration, routes, metrics, and linked symbols; core cannot render or proxy a fallback dashboard.
- Killing, failing, restarting, disabling, or omitting one Go web process leaves the other product and
  unrelated capabilities available and truthfully reported.
- CLI schema, API schema, both GUIs, capability advertisements, and profile snapshots are exact
  projections of one effective Go config/process snapshot. Tests reject static-only, unowned, unread,
  secret, stale, wrong-activation, and configured-but-inactive fields.
- Changing a restart-class web setting persists a new validated version without claiming live effect;
  the next cold start applies it before any listener or registration, or fails without falling back to
  the old advertised state.
- Cross-product integration captures authenticated network traffic and proves there is no cross-product
  event-bus path. Authentication, deadline, protocol-version, and remote-readiness failures stay typed
  and do not start a local substitute.
- Package/install tests prove canonical names, bounded old-name aliases, no new legacy references, and
  compatible upgrade and rollback in both mixed-version directions.
- Retained C/config/build/deployment adapters pass parity tests showing they transport or package the Go
  decision without changing lifecycle, config, capability, identity, or readiness.

### Trace to archived acceptance

The archived proposal required Runtime-only, Control-only, and full deployment fixtures to prove
route/config/capability truth, independent failure behavior, and compatible upgrade from current
package names. The acceptance bullets above preserve and refine that evidence requirement; they do not
replace it.

## Non-goals

- This corrective PR does not implement the Go migration or mark the residual done.
- The web processes do not become owners of core Runtime/Control capabilities, secrets, policy, shared
  data, or product-local event buses.
- Packaging and compatibility aliases do not become a second config, lifecycle, capability, or
  readiness authority.
- Runtime and Control are not collapsed into one process, bus, failure domain, or authentication
  boundary.
