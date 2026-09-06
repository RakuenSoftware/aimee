# Providers GUI validation

Provider management acceptance passed on 2026-09-06 in disposable **CT 104** on
`192.168.1.253`, at `192.168.0.169:8443`. The originally requested `.252` was
unreachable; the subsequent authorization to use `.253` was used. No VM was
needed. CT 104 was stopped and destroyed after testing, including its storage
and Proxmox configuration. Existing guests were left alone.

## Real deployment

The guest ran Debian 13, PostgreSQL 17, the feature branch's native server,
required Go modules, the dedicated config module, runtime-web, and the built
SPA. Browser login used PAM with a disposable test account. Provider requests
used real Vault storage and the Go egress module. Only the external model vendor
was replaced by `scripts/validation/providers/fixture.py`.

This validates the provider/model management surface and its dependencies.
The guest did not have KB, workflow runners, Docker delegation, or external
vendor subscriptions configured. Global readiness correctly reported the absent
KB; this is not full-application, live-vendor, or workflow-execution acceptance.

## Results

The checked-in [acceptance report](providers/acceptance.json) contains the browser
results, deployed SHA-256 values, and credential-free vendor event log. Deployed
server, web, and providers binaries were compared with the feature worktree's
artifacts.

- Nine exercise checks passed: real login; two same-URL accounts without models;
  duplicate/cancel behavior; discovery and completion probes; limits and prices
  on Models; endpoint edits; blank-key preservation; key rotation; delete
  confirmation; and hard refresh. Some checks combine related assertions.
- Four post-restart checks passed: login, persisted connections/model identities
  and credentials, cascade deletion limited to the selected provider, retention
  after deleting the last model, and deletion of an empty provider.
- Five exploratory checks passed: cross-origin rejection, visible discovery
  failure with successful manual model entry, incompatible protocol edits,
  failed-edit credential preservation, and keyless reuse after deletion.
- Killing the providers process left the server alive. Both roster APIs returned
  errors, and Models displayed an unavailable message instead of an empty
  successful result. Restarting the real services restored the saved state.
- Account labels in vendor events proved A/B isolation and use of A's rotated
  credential after restart. The recreated keyless account received HTTP 401.
- The roster was mode 0600 and contained no literal test credentials. The latest
  raw audit capture also contained no literal test credentials.
- Desktop and 390-pixel screenshots were visually inspected. Provider controls
  remained usable and wrapped within the narrow content area.

Server PID changed from 6290 to 6723, module launcher from 6291 to 6724, and web
from 6292 to 6725. The provider process kill and service restart were real host
operations, not substituted browser responses.

## Local checks

- `server-go`: full `go test ./...`; race-enabled providers and egress suites.
- Independently exported providers module: race-enabled tests and executable build.
- `runtime-web`: full Go suite, including actor forwarding and cross-origin checks.
- Frontend: 196 tests across 22 files; production build.
- Native server build; native agent/configuration, API-key, model-provider,
  model-registry, and management-transport suites using the real Go fixture peer.
  PostgreSQL-only cases in the broader native agent suite were explicitly skipped;
  the browser deployment used real PostgreSQL.
- Descriptor, process contract, package/source ownership, bus boundary, egress
  boundary, provider registration/seams, module placement, export generator, and
  repository lock checks passed.

Reproduce with `scripts/validation/providers/README.md`, `browser.cjs`, and
`exploratory.cjs`. Local screenshots and raw reports were retained under
`/tmp/aimee-provider-e2e-artifacts`; command logs use the
`/tmp/aimee-provider-final-*` prefix. Earlier UI smoke runs are not counted as
real-stack acceptance.
