# P5-C2a management-token and JWKS trust-root bootstrap

> **Archived proposal.** This records the design as it was agreed, not the
> system as it behaves today; parts of it have since diverged. For current
> behaviour see `docs/`, or the code.

- **State:** complete.
- **Parent:** `tiered-llm-p5-oidc-control-plane.md`, §3.
- **Depends on:** P7 KMS signed-HWM custody and P5-C1b's fixed RS256 token contract.
- **Followed by:** P5-C2b immutable signed-JWKS publication, P5-C2c authenticated fetch/durable server cache, P5-C2d token authority, then P5-C3 action composition.

## Boundary and rulings

Offline-provision the two independent trust roots required by P5-C2:

1. a fixed RSA-3072 management-token signing key; and
2. a distinct Ed25519 JWKS-manifest signing root.

Persist only custody-encrypted private material plus exact public bindings. Produce a recoverable public deployment bundle containing the token `kid`/JWK and the manifest verification pin. Bind, but do not advance, the separately configured signed publication-HWM identity that C2b will use to reserve JWKS generations.

This packet has no JWKS generation/document/signature, online signer, token mint, key-use admission, listener, socket, HTTP route, server cache, rotation, retirement, revocation, overlap, console action, or network dispatch. `/v1/management/action` remains unconditional `503`. Ordinary KB and server targets link none of the new provisioner/private-custody objects. All new private-use paths are reachable only from the offline provisioner.

The existing status-authority key is never reused. A rotating or compromised token key therefore cannot authorize its own JWKS replacement. The manifest public pin is installed through owner-controlled deployment/enrollment configuration and is never learned from a later JWKS response.

Use fixed RSA-3072, exponent 65537, two-prime PKCS#8 for the token key. This fits the existing 4096-byte protected-use ceiling while the C1a verifier retains its general 2048..8192-bit interoperability. The manifest root is exactly one 32-byte Ed25519 seed. Both use distinct domain-separated AAD, key slots, custody identities, signed HWMs, envelope digests, bootstrap ids, and WORM actions.

## Fixed topology and public bindings

Add owner-only platform registries for exactly:

- token slot `org:p5-token / management / rs256`: immutable configured custody id, bootstrap id, enabled phase, current version, canonical public RSA modulus/exponent, canonical JWK digest, wire `kid`, v1/v2 envelope digests, initial seal epoch, and verified HWM2 attestation digest;
- manifest slot `org:p5-jwks-manifest / management / ed25519`: the corresponding immutable binding with exact 32-byte public key and wire id; and
- publication-HWM root: immutable helper ancestry, provider identity/custody id, verifier/signature domain, and digest of a verified signed initial HWM value of exactly 1. This value is only a pristine predecessor; C2b must CAS 1→2 before finalizing the first published generation, so no two generation-1 payloads can share an unadvanced HWM.

The production token `kid` avoids self-reference: it is `p5-token-v1-<first-32-lowercase-hex-of-SHA-256(domain || uint32be(modulus_len) || modulus || uint32be(exponent_len) || exponent)>`, where domain is fixed ASCII `aimee.p5.token.public.v1\n`, modulus and exponent are unsigned minimal big-endian integers, and exponent is exactly 65537. One shared length-aware canonical public-key codec then defines the exact six-member JWK in fixed order `kty,kid,use,alg,n,e`, values `RSA,<derived-kid>,sig,RS256,<canonical-unpadded-base64url-modulus>,AQAB`, and rejects leading-zero modulus encodings, padding, noncanonical trailing bits, unknown members, and over-bound output. The provisioner and later publisher/consumer reuse this codec; no component independently spells kid/JWK bytes.

The manifest wire id is `p5-jwks-root-v1-<first-32-lowercase-hex-of-SHA-256(raw-public-key)>`. The public deployment bundle uses a non-recursive construction. First encode one canonical bounded payload object in fixed order with members `format_version=1`, token `kid`, nested canonical token JWK, manifest wire id, canonical base64url manifest public key, and publication-HWM public identity digest. Compute lowercase-hex SHA-256 over those exact payload bytes. Then emit the final canonical object with the same members in the same order followed by `bundle_sha256`; the digest member is excluded from its own preimage. It contains no timestamp, host, run id, private/ciphertext/provider bytes, or mutable endpoint.

## Crash-resumable dual-root state machine

Add one offline owner-only orchestrator and narrow database adapter patterned on P5-B1b. It opens an explicit hardened migration/owner session, asserts exact role/session/search-path/row-security/public-schema state, acquires the global maintenance barrier and fixed root advisory locks in token-then-manifest order, and inspects the complete topology before generating anything.

Each root independently follows:

1. `EMPTY`: require no registry, slot/current/secret/rotation history and a fresh verified provider signed HWM of exactly 1. Generate in an mlock/MADV_DONTDUMP/MADV_WIPEONFORK arena, derive and validate public bindings, encrypt the same secret independently as inert v1 and candidate v2 under domain-separated canonical AAD, and stage immutable rows plus one fixed 1→2 activation record.
2. `STAGED`: decrypt only v2 inside the protected arena, rederive every public/envelope binding, and reject any mismatch. CAS the exact configured provider HWM 1→2. A definite compare failure is accepted only when a fresh verified signed read proves exact 2; ambiguity/outage/forgery/rollback is fatal or retryable without database repair.
3. `CAS_DONE`: under the same global/root lock order, require exact staged bindings and fresh signed HWM2, advance current to v2, enable the registry, record the attestation digest, and append one secret-free WORM activation event atomically.
4. `FINAL`: revalidate the entire persisted binding, decrypt v2 and rederive the public key/kid/digests, freshly verify provider HWM2, and return converged without mutation or secret output. Changed enabled state is integrity failure.

The outer orchestrator treats the cross-product explicitly. Exact `token=FINAL, manifest=EMPTY|STAGED|CAS_DONE` continues only the manifest state; the symmetric state continues only token; both final proceed to publication-root binding; either invalid state stops. Kill before or after either stage, CAS, final database commit, or between roots converges without regeneration, duplicate rotations, or duplicate WORM rows.

After both roots are exact-final, require the configured publication-HWM identity to have pristine verified signed value 1 and no existing publication registry/generation/candidate/use state. Persist its immutable identity/domain/helper and attestation digest in a final owner transaction with a WORM bootstrap event. A rerun revalidates the same live signed HWM1 and exact binding. Any pre-advanced HWM, changed identity/domain/helper, existing publication state, or signed-HWM ambiguity fails closed.

No function heuristically repairs or overwrites immutable state. PostgreSQL rollback after a provider CAS leaves a recoverable exact `CAS_DONE` state; missing or changed staged evidence after CAS is integrity failure, never regeneration.

## Secret handling and output recovery

Generate and parse RSA/Ed25519 keys only inside a root-owned mlocked, nondumpable, wipe-on-fork arena with cancellation disabled across secret ownership. Set `PR_SET_DUMPABLE=0`, `RLIMIT_CORE=0`, reject memory-protection failure, cleanse every PKCS#8/seed/DER/BN/KEK/DEK/AAD/plaintext/envelope temporary non-elidably, and prohibit secret-bearing child environments, argv, stdout/stderr, logs, audit detail, core files, or generic errors.

Envelope AAD is a versioned binary transcript with a fixed domain and unsigned big-endian length prefix before each exact principal, agent, cred, and version encoding; it never relies on delimiter concatenation. Token-root and manifest-root AAD domains are distinct and frozen as independent fixtures. Envelope digests length-frame format, AAD, wrapped DEK, nonce, ciphertext, and tag. Recovery validates stored ciphertext size before decrypt: RSA PKCS#8 must be nonempty and at most 4096 bytes; Ed25519 plaintext is exactly 32 bytes. Recovered RSA must be exactly 3072 bits, exponent 65537, two-prime, private-key-check valid, and yield the persisted public JWK/digest/kid.

Fresh successful completion emits the canonical public deployment bundle only after both roots and the publication-HWM binding commit. A crash after commit but before stdout cannot lose the trust pin permanently: an explicit owner-only `--export-public` mode accepts only the fully FINAL exact topology, freshly revalidates both provider HWM2 values and publication HWM1, decrypts/rederives both public bindings in the protected arena, and re-emits the identical public bundle marked only by process exit status, not by an extra JSON field. It performs no database mutation or WORM append. Partial topology, sealed state, unavailable provider, or any mismatch emits nothing.

## PostgreSQL boundary and target isolation

All registries, staged envelopes, rotations, and publication-root bindings are platform-scoped and owner-only. Hardened functions use definition-time `pg_catalog,pg_temp`, fully qualified objects, fixed input shapes, advisory locks, immutable transition checks, and exact SQLSTATE classes. Revoke all table/sequence/function access from PUBLIC, runtime, status roles, token roles, and ordinary migration logins except the explicit provisioner role/function set. Revoke PUBLIC schema CREATE and test temp shadowing. Owner UPDATE/DELETE/TRUNCATE of finalized roots and activation/WORM records is denied.

Add SQLite shape-only representations with no false authority functions. Register a separate `aimee-kb-token-roots-provision` target and its explicit object closure. Plant tests prove ordinary `aimee-kb`, `aimee-served`, the status authority/provisioner, and existing containers/services do not link, install, call, or expose the token-root provisioner, private custody, or export path. No workflow file changes.

## Tests and gates

Focused tests cover deterministic canonical JWK/kid/manifest id/bundle bytes, RSA-3072 generation and PKCS#8 recovery, wrong size/exponent/prime count/key check, Ed25519 recovery, distinct AAD domains, every bound and output-clearing path, protected cleanup/cancellation, exact-final export, and rejection of partial/changed topology. Cross-component fixtures freeze the canonical JWK and bundle digests for later C2 packets. Existing C1a 2048/4096/8192 verifier fixtures remain green; production provision rejects non-3072 keys.

Real PostgreSQL 17 plus signed-HWM validation on CT103/CT260 covers fresh dual-root bootstrap; every token/manifest phase cross-product; 32-way concurrent same-operation convergence; kills before/after each stage, CAS, final commit, between roots, and after final commit before output; exact export recovery; signed-HWM compare ambiguity resolved only by verified reread; forged/stale/rollback/pre-advanced/outage states; changed helper/domain/custody binding; seal and maintenance races; audit rollback; WORM immutability; ACL/role/temp-shadow/public-schema denial; and no duplicate secret, rotation, current, registry, or audit rows.

Run production server/KB/status builds for non-regression, the new provisioner build, lint/schema sync/alter order/SQLite shape/target isolation, focused release and fresh ASAN/UBSAN/leak gates, exact-head CT260 KMS validation, adversarial full-branch roundtable, GitHub CI, then merge to `testing`. Move this plan to done, update delivery status, and store memory only after merge.
