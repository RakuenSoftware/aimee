# Per-user remote writes: composed deployment validation residual

- **State:** PENDING — residual scope only.
- **Archived parents:** [`per-user-remote-writes-authz.md`](../done/per-user-remote-writes-authz.md)
  and [`per-user-remote-writes-authz.acceptance.md`](../done/per-user-remote-writes-authz.acceptance.md).

## Delivered foundation

OIDC and PAM login, subject/team/tier grants, KB-signed single-use write tokens, server-side
enforcement, operator grant commands, and the individual live custody/enforcement rigs are shipping.
The archived acceptance ledger maps every original §11 criterion to evidence.

## Remaining work

Prove the boundaries that the ledger currently composes from separate rigs in one production-shaped
deployment:

- run PAM login → token mint → `/v1` write as one uninterrupted test using the deployed server and KB;
- mint the enforcement token with the configured vault-custodied key rather than a locally generated
  fixture key, and verify the same `kid` through published JWKS;
- run the hardened deployment tier rather than only the development topology;
- induce a rotated-away `kid` and an unreachable JWKS endpoint in the live rig, proving bounded cache
  behavior and fail-closed recovery rather than relying only on unit tests.

## Acceptance

One CI-invocable harness creates an isolated production-shaped stack, proves all four cases above,
tears it down without shared credential or state leakage, and emits a redacted evidence summary.

