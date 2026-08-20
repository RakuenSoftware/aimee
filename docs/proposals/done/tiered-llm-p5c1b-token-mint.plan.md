# P5-C1b exact management-token mint core

> **Archived proposal.** This records the design as it was agreed, not the
> system as it behaves today; parts of it have since diverged. For current
> behaviour see `docs/`, or the code.

- **State:** DONE — implementation, adversarial branch review, local/ASAN/fuzz gates, and exact-head CT260 validation passed.
- **Parent:** `tiered-llm-p5-oidc-control-plane.md`, §3.
- **Depends on:** P5-C1a strict verifier and durable server replay consume.
- **Followed by:** P5-C1c primary/WORM action journal, P5-C2 custody-backed signing and signed
  non-rollback JWKS lifecycle, then P5-C3 end-to-end action composition.

## Boundary

Replace the obsolete raw-PEM/cJSON kb token helper with a bounded typed builder for the exact C1a
management JWT contract. The builder accepts one already-authorized immutable claim tuple and one
narrow RS256 signing callback. It validates and canonically serializes the tuple, asks the callback
to sign only the resulting protected signing input, and returns a compact token in caller-owned
bounded storage.

This packet deliberately does not generate or persist `jti`, correlation id, time, `kid`, actor,
team, target, peer identity, or request digest. P5-C1c owns their primary-authoritative admission
and immutable replay tuple; P5-C2 binds the admitted `kid` to a production custody key and supplies
the signing callback. Keeping those values explicit makes exact retry deterministic and prevents a
later database authority from fighting hidden mint-side randomness or clock reads. No production
API accepts or returns a private PEM or arbitrary key bytes.

There is no PostgreSQL/schema change, custody/HWM implementation, key provisioning, JWKS
publication/fetch, network dispatch, listener allowlist change, or route enablement here.
`/v1/management/action` remains unconditional `503`.

## Typed API and ownership

Replace `kb_mgmt_token_mint(private_key_pem, ...)` with fixed-capacity public types in
`headers/kb_mgmt_token.h`:

- `kb_mgmt_token_claims_t` contains exactly `issuer`, `audience`, `subject`, positive `team_id`, a
  closed `kb_mgmt_token_capability_t`, `jti`, `correlation_id`, `request_sha256`, `peer_issuer`,
  `peer_serial`, `peer_fingerprint`, `kid`, `issued_at`, and `expires_at`.
- The only capability in C1b is `KB_MGMT_TOKEN_CAP_REMOTE_WRITES`, serialized exactly as
  `remote_writes`. Unknown enum values fail; there is no free-form authorization string.
- `kb_mgmt_token_sign_fn(ctx, signing_input, input_len, signature, signature_cap,
  signature_len)` receives bounded non-secret bytes and must return one raw RSA signature. It is a
  dependency seam, not a generic production arbitrary-key service. The builder supplies a
  1024-byte maximum signature buffer, accepts 256..1024 bytes, and zeroizes it on every exit.
- `kb_mgmt_token_build(claims, signer, signer_ctx, jwt_out, jwt_cap, jwt_len)` returns a typed
  `OK`, `INVALID`, `SIGN_UNAVAILABLE`, or `OUTPUT_TOO_SMALL` result and clears output/length on
  every failure. It performs exactly one signer call only after all claims and output bounds have
  passed. No heap-owned token or private material crosses the API.

The caller owns generation and authority. C1c will derive `subject` and `team_id` from the verified
tenant transaction, restrict capability, generate `jti`/correlation id with the OS CSPRNG, compute
the canonical action-envelope digest, read bounded authoritative time, recheck the active target
registry and active outbound management certificate identity in one primary transaction, and
persist all fields including `kid`/times before returning this tuple. C1b never silently repairs,
normalizes, or substitutes a field.

## Exact serialization and signing

Emit header members in fixed order as exactly `alg`, `typ`, `kid`, with values `RS256`, `JWT`, and
the admitted key id. Emit payload members in fixed order as exactly the C1a contract: `v=1`, `iss`,
`aud`, `sub`, `team_id`, `cap`, `jti`, `correlation_id`, `request_sha256`, `peer_issuer`,
`peer_serial`, `peer_fingerprint`, `iat`, `exp`. Integers use canonical unsigned decimal spelling;
reject negative values, values above `2^53-1`, nonpositive team, `iat >= exp`, and lifetime above
90 seconds. The builder has no clock and therefore does not decide current validity.

Validate every string against the corresponding C1a grammar and capacity before serialization:
canonical P1 identity key, ASCII token ids/kid, exact lowercase-hex fields and serial, control-free
issuer, exact audience and field lengths. Use one local canonical JSON string encoder that escapes
quote, backslash, and every permitted ASCII control spelling consistently; because C1a rejects
control characters in the decoded governed fields, controls fail before encoding. Reject embedded
NUL by using fixed arrays plus explicit bounded lengths (or by proving one NUL terminator within
the capacity and no ambiguous suffix); never truncate. Do not depend on cJSON number formatting or
object construction for signing bytes.

Base64url encoding is canonical, unpadded, checked for size overflow, and writes no partial result.
The signing input is exactly `base64url(header) + '.' + base64url(payload)`. The callback returns
the raw RS256 signature; append its canonical base64url segment only after checking the callback
result and length. Identical tuple plus identical RSA key produces byte-identical JWT output.

Remove the legacy callable PEM API entirely. Tests may implement an OpenSSL PEM fixture callback,
but it stays test-local and is not exposed by kb production headers or objects.

## C1a compatibility correction

C1a documents and validates RSA public keys through 8192 bits but its current decoded-signature
buffer is 512 bytes, making valid 8192-bit (and any over-4096-bit) signatures impossible. Expand
the verifier's decoded signature bound to 1024 bytes, retaining the 8192-byte total wire bound, and
add valid 8192-bit plus over-bound signature regressions. This is contract alignment, not algorithm
expansion: only RS256 with RSA PKCS#1 v1.5 remains accepted.

## Tests and gates

Replace the legacy mint tests with focused table-driven coverage for every field boundary and
grammar, exact header/payload member set and order, JSON escaping, canonical integers/base64url,
capacity arithmetic, signer failure/invalid length, output clearing, exactly-once callback, and
deterministic retry. Mutation tests prove every changed governed input changes or rejects the token.

Generate 2048-, 4096-, and 8192-bit RSA fixtures through test-only callbacks and round-trip every
valid token through `server_mgmt_token_verify` with matching strict JWKS and bindings. Explicitly
prove the obsolete `cert_cn`/missing-claims shape cannot be built and remains rejected. Add a fuzz
target for typed tuple/bounds/serialization, seeded with a valid round-trip tuple; fuzz code may use
a deterministic fake signer while unit tests exercise real RS256.

Run production server and kb builds, focused mint/verifier tests, existing C1a replay and B3
management TLS/status regressions, fresh ASAN/UBSAN, the valid-seed fuzz gate, and an adversarial
full-branch roundtable. Validate the exact merged candidate on CT260 before PR merge to `testing`.
