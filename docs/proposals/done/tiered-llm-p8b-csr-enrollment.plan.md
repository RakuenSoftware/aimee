# P8b — thin-client CSR enrollment

> **Archived proposal.** This records the design as it was agreed, not the
> system as it behaves today; parts of it have since diverged. For current
> behaviour see `docs/`, or the code.

- **State:** DONE — delivered scope archived 2026-07-26.

> **Archived complete (2026-07-26).** The audit found the scoped deliverables shipped,
> superseded by the current implementation, or fully represented by completed child slices.

## Scope

Add a server-owned enrollment endpoint that accepts a client-generated CSR, verifies
proof-of-possession, ignores the CSR subject, and issues a `clientAuth`-only leaf under
the existing server enrollment CA. The private key never crosses the API.

## Invariants

- Only the existing attested operator transports may authorize signing.
- The server chooses and sanitizes the certificate CN; CSR subject and extensions are
  never copied.
- CSR signature verification is mandatory before signing.
- The issued serial/CN/expiry is inserted into the durable `pki_certs` roster before
  success is returned.
- Existing `cert.issue` remains compatibility-only; the new client enrollment path must
  use CSR signing.

## Validation

- Unit: valid CSR signs; forged or malformed CSR fails; requested CSR subject cannot
  override the server-selected CN; the leaf has `clientAuth` and not `serverAuth`.
- Route: unauthorized transports fail; attested operator receives cert and serial but no
  private key.
- Integration: generate two independent client keys, sign both CSRs, complete two mTLS
  handshakes, revoke one serial, and prove the other remains valid.
