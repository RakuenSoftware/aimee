# Runtime and Control product boundary: residual work

- **State:** PENDING — residual scope created by the 2026-08-04 proposal audit.

**Archived source:** [`product-governance-web-and-config.md`](../done/product-governance-web-and-config.md)

## Delivered baseline

Runtime-web and control-web have separate ownership descriptors and initial Go decision surfaces;
configuration and capability projections have explicit module owners.

## Remaining deliverables

- Complete the Aimee Runtime and Aimee Control product naming and packaging transition.
- Host their web surfaces as separately admitted processes with independent lifecycle/readiness.
- Prove omit/disable behavior removes routes, assets, jobs, metrics, and registration residue.
- Generate advertised effective configuration from the same authority as runtime capabilities.
- Keep cross-product traffic on the authenticated network transport rather than an in-process bus.

## Completion evidence

Runtime-only, control-only, and full deployment fixtures must prove route/config/capability truth,
independent failure behavior, and compatible upgrade from current package names.
