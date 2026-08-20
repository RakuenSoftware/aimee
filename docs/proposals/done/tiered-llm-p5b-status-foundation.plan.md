# P5-B management-status foundation

> **Archived proposal.** This records the design as it was agreed, not the
> system as it behaves today; parts of it have since diverged. For current
> behaviour see `docs/`, or the code.

- **State:** delivered and fail-disabled pending the P5-B composition slice.
- **Depends on:** P5-A authoritative registry and role-separated PKI.

## Delivered scope

The primary database now owns a monotonic revocation generation and an atomic
management-status lookup. The lookup binds the verified management-client leaf to
an active, unexpired `p5-kb-management` enrollment, requires team authority over an
active target server, and returns the target management fingerprint and generation
from one primary query. A dedicated `aimee_kb_status` role has EXECUTE-only access;
it cannot read the underlying tables.

The signed status object is a strict, versioned Ed25519 protocol over a
domain-separated length-prefixed transcript. Its JSON wire form accepts exactly the
specified fields, canonical decimal strings and canonical base64url without padding.
Unknown, duplicated, malformed, overflowing or trailing input fails closed. The
pure authority decision accepts peer identity only from its verified-mTLS caller and
keeps PostgreSQL lookup and custodial signing behind explicit callbacks.

The server exposes only read-only challenge and health routes. Challenges are
bounded, random, short-lived and transactionally bound to the verified peer leaf,
TLS exporter, target and purpose. Status verification consumes the nonce on every
identified verification failure and atomically compares/advances a durable local
generation high-water mark. Outstanding nonces disappear on restart while the
high-water mark survives. The management action route remains unconditional `503`.

The reverse HTTPS substrate performs one DNS resolution, filters each returned
sockaddr against denied IPv4/IPv6 ranges (including mapped IPv4), and connects
directly to the permitted result. The enrolled DNS name remains SNI, hostname and
Host. The TLS peer is pinned by issuer, normalized serial and DER fingerprint. The
persistent session rejects redirects and ambiguous HTTP response framing.

The PKI has a third, fixed management-client profile: clientAuth only, CN
`p5-kb-management`, plus a dedicated profile OID checked by the server.

## Validation completed

- Clean `-Werror` server and kb builds plus focused status, authority, endpoint,
  nonce/high-water and PKI tests.
- Focused status, authority, nonce/high-water and PKI tests under ASAN+UBSAN.
- Real PostgreSQL 17 gate on CT103 proving atomic generation increments,
  idempotent revoke, no resurrection, revoked-caller and cross-team denial, and
  EXECUTE-only authority-role isolation.
- Source line gate and diff whitespace gate.
- Adversarial review findings were applied to identity, nonce, generation, address,
  framing and failure-consumption behavior.

## Explicitly deferred

The next P5-B composition slice owns the dedicated mTLS status-authority process,
P7 custodial signing integration, automatic management-client enrollment/renewal,
the complete kb challenge→authority→health orchestrator, and the CT260↔CT262 kill
matrix. None of those paths is enabled by this foundation alone.
