# P5-C1a strict management token and durable replay barrier

> **Archived proposal.** This records the design as it was agreed, not the
> system as it behaves today; parts of it have since diverged. For current
> behaviour see `docs/`, or the code.

- **State:** DONE. Implementation and adversarial branch review converged; CT260 production and focused gates passed.
- **Parent:** `tiered-llm-p5-oidc-control-plane.md`, §3.
- **Depends on:** P5-B3c distinct management listener.
- **Followed by:** P5-C1b primary/WORM action authority and P5-C2 signed JWKS rotation.

## Boundary

Replace the permissive, double-parsed server management JWT scaffold with one strict typed
verification result and add a crash-safe server-local `jti` consume barrier. This packet does not
expose `/v1/management/action`, mint production keys or tokens, publish/fetch JWKS, dispatch an
action, write the kb WORM ledger, or change the B3c listener allowlist. The action route remains
fail-closed and the management listener continues to permit only challenge and health.

## Exact token contract

Add a bounded `server_mgmt_token_claims_t` populated by one parser/verifier pass. The compact JWT
has exactly three nonempty canonical base64url segments, no padding, no whitespace, and bounds of
1024 header bytes, 4096 payload bytes, 512 signature bytes, and 8192 wire bytes. Decode and
re-encode each segment byte-for-byte before use. Reject
embedded NUL, invalid trailing bits, overflow, duplicate JSON member names at any level, unknown
members, wrong types, non-integer numbers, and values outside their destination buffers.

The protected header contains exactly `alg`, `typ`, and `kid`: `alg` is exactly `RS256`, `typ` is
exactly `JWT`, and `kid` is a nonempty 1..64-byte ASCII token. No `crit`, `jku`, `x5u`, embedded JWK, or
algorithm/key-type fallback is accepted. Select exactly one RSA public key from the caller-supplied
authenticated JWKS by exact `kid`; duplicate kids, unknown fields relevant to key interpretation,
wrong kty/use/alg, malformed modulus/exponent, weak keys, or multiple matches fail closed. Accept
RSA moduli from 2048 through 8192 bits only and an odd exponent from 3 through `INT32_MAX`; verify
with explicit SHA-256 and RSA PKCS#1 v1.5 padding rather than relying on the JOSE header alone.
The supplied JWKS blob is a pure verifier input whose authenticity is a precondition in C1a tests;
authenticated fetch, signed generation/HWM, refresh, and rotation are owned by P5-C2.

The payload contains exactly these typed claims:

- `v`: integer `1`;
- `iss`: exact configured kb management issuer;
- `aud`: exact target `AIMEE_SERVER_ID`, as one string rather than an array;
- `sub`: at most 576 bytes and exactly the P1 `kb_identity_key` form: `owner`,
  `oidc:<percent-encoded-issuer>:<percent-encoded-subject>`, or
  `cert:<percent-encoded-issuer>:<normalized-serial>`; percent escapes are uppercase and only
  `%25`/`%3A`, components are nonempty and control-free, and a bare OIDC subject is invalid;
- `team_id`: a positive signed 64-bit tenant id bound by the kb authorization decision;
- `cap`: one 1..64-byte ASCII capability token;
- `jti`: one 16..128-byte ASCII token identifier;
- `correlation_id`: one 1..128-byte audit-safe ASCII token;
- `request_sha256`: exactly 64 lowercase hexadecimal characters;
- `peer_issuer`, `peer_serial`, and `peer_fingerprint`: exact binding to the live B3 management
  peer's at-most-511-byte control-free issuer, 1..79-byte normalized lowercase-hex serial, and
  64-lowercase-hex leaf fingerprint;
- `iat` and `exp`: exact signed 64-bit JSON integers.

Verification requires `iat <= now`, `now < exp`, `exp > iat`, and a compiled maximum lifetime of
90 seconds, with zero future-issued skew. Reject negative time, arithmetic overflow,
fractional/exponential number spellings, and any JSON integer above the exact-double boundary
`2^53-1`. A small raw JSON member lexer enforces canonical unsigned decimal spelling before cJSON
typed extraction so cJSON's double representation cannot erase lexical or precision errors. Reject an
expired token even when its `jti` row still exists. CN is not a token binding: B3 already checks the
exact profile CN, while C1a binds the individual live certificate by issuer+serial+fingerprint.
Channel binding remains owned by the same-session B3 status staple and is composed in P5-C3.

Signature verification covers the original canonical `base64url(header).base64url(payload)` bytes
and uses RSA PKCS#1 v1.5 with SHA-256 only. Return the typed claims only after signature, audience,
issuer, time, peer, digest, and shape checks all succeed. Remove the current call to the general
OIDC verifier followed by a second permissive cJSON payload parse; no consumer reparses the JWT.

## Durable local replay consume

Add a DB1-owned `db1/server_management_jti.{c,h}` module and schema table dedicated to management
token consumption, not a server TU reaching through `db1_internal.h` and not a generic idempotency
store:

`server_management_jti(jti TEXT PRIMARY KEY, issuer, kid, audience, subject, team_id,
capability, peer_issuer, peer_serial, peer_fingerprint, request_sha256, correlation_id,
issued_at, expires_at, consumed_at)`.

Schema `CHECK`s mirror every application bound: jti 16..128, issuer 1..255, kid 1..64,
audience 1..127, subject 1..576, positive team id, capability 1..64, peer issuer 1..511,
peer serial 1..79 lowercase hex, both SHA-256 fields exactly 64 lowercase hex, correlation id
1..128, and integer times with `0 <= issued_at < expires_at`. The application uses a compiled live
row ceiling of 4096; because the table can never exceed that ceiling, deletion of all expired rows
inside the transaction is itself bounded. Index `(expires_at,jti)`. A single `BEGIN IMMEDIATE`
transaction:

1. deletes rows whose `expires_at` is strictly before the current time;
2. checks a compiled live-row ceiling before insert;
3. inserts the fully verified claim tuple with the current consume time; and
4. commits before reporting success.

The unique insert is the authorization point. Constraint conflict means replay; saturation,
busy/locked DB, schema error, clock error, or commit ambiguity is unavailable/deny, never success.
The row is not deleted on downstream failure: C1a has no dispatch, and later packets must treat an
accepted token as consumed before any action attempt. Persistence across process restart is
mandatory. Expiry GC is bounded and indexed; tests use injected time and capacity rather than
sleep. The existing DB1 transaction gate serializes in-process writers; external `SQLITE_BUSY`
does not spin or sleep and fails unavailable. The promised crash boundary is process crash/restart,
not arbitrary power-loss durability: later dispatch occurs only after COMMIT returns success. If
COMMIT reports failure, no action is dispatched whether the row persisted or rolled back; a
persisted ambiguous row merely makes a retry fail closed. Do not trust in-memory state for replay
decisions.

**Non-negotiable composition invariant:** C1b/C3 must cite this packet and commit the jti consume
success before the first action side effect or dispatch byte. No later packet may merge consume and
dispatch into a best-effort or post-action step.

Expose separate pure verify and durable consume APIs so tests and later composition cannot consume
before all B3/JWKS/action checks are complete. `server_mgmt_action_authorize` is either removed or
reduced to a compatibility wrapper that cannot hard-code `remote_writes`; P5-C3 will pass the real
listener/request-context policy. `server_mgmt_endpoint_dispatch` becomes an unconditional
fail-closed stub (or is deleted with its callers) so the legacy audited-dispatch scaffold is not
merely unreachable but incapable of executing an action in this packet. No route may publish it.

## Tests and merge gates

Focused tests cover valid verification and a table-driven single mutation for every header,
payload, key, base64url, JSON, time, peer-binding, request-digest, and size invariant. Include
duplicate members, duplicate `kid`, algorithm confusion, weak/wrong key type, canonical actor
edge cases, exact integer boundaries, bad base64url trailing bits, and valid-signature/wrong-claim
tokens. Durable tests use a real temporary DB1 file and prove first consume, same-process replay,
restart replay, expiry GC, saturation, DB busy/error, and commit failure all fail as specified.
The old `kb_mgmt_token_mint` output (`cert_cn` and missing version/peer/digest/correlation/team
claims), even with a valid RS256 signature, is an explicit rejection fixture; mint replacement is
owned by P5-C1b and is not weakened into C1a.

Run production server build, existing B3 management TLS/listener/status gates, fresh ASAN/UBSAN,
strict token fuzzing with a valid seeded corpus, adversarial full-branch roundtable, all CI, and PR
merge to `testing`. Update the P5 delivery table and store an aimee memory only after merge.
