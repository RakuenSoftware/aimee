# Runtime and Control product boundary: residual work

- **State:** REJECTED — mixed C, Go, build, and deployment ownership cannot be completed as one Go
  proposal; archived 2026-08-15.

**Archived source:** [`product-governance-web-and-config.md`](../done/product-governance-web-and-config.md)

## Delivered baseline

Runtime-web and control-web have separate ownership descriptors and initial Go decision surfaces;
configuration and capability projections have explicit module owners.

## Decision

Rejected under the Go-or-rejected implementation policy. The residual combines boundaries with
different owners. The independent `runtime-web` and `control-web` processes and their bounded
decision surfaces are Go-owned, while the Runtime/Control product binaries and install targets,
module adapters and bus hosts, configuration authority, compatibility names, and deployment
topology remain C, build, or deployment-owned.

Completing the umbrella as one Go change would duplicate those owners or bypass their lifecycle and
configuration contracts. The delivered Go web-process foundation remains authoritative, and this
rejection does not reject the Runtime/Control product direction. Any remaining naming, packaging,
optional-lifecycle, effective-configuration, or transport work must be split into owner-specific Go
proposals after the corresponding product, configuration, and lifecycle boundaries have Go owners.

## Remaining deliverables at rejection

- Complete the Aimee Runtime and Aimee Control product naming and packaging transition.
- Host their web surfaces as separately admitted processes with independent lifecycle/readiness.
- Prove omit/disable behavior removes routes, assets, jobs, metrics, and registration residue.
- Generate advertised effective configuration from the same authority as runtime capabilities.
- Keep cross-product traffic on the authenticated network transport rather than an in-process bus.

## Completion evidence from the archived proposal

Runtime-only, control-only, and full deployment fixtures must prove route/config/capability truth,
independent failure behavior, and compatible upgrade from current package names.
