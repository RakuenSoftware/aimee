# P5-B2a workload identity and instance-custody provider

> **Archived proposal.** This records the design as it was agreed, not the
> system as it behaves today; parts of it have since diverged. For current
> behaviour see `docs/`, or the code.

- **State:** completed; roundtable-converged and validated locally plus CT260.
- **Depends on:** P5-A management-client PKI profile; P5-B1c online authority;
  P7 custody/seal primitives.
- **Followed by:** B2b primary instance enrollment/renewal state and B2c local
  certificate orchestrator/bundle lifecycle. B3 remains the first health exchange.

## Boundary and decision

This slice adds the missing workload-identity/custody provider contract and no
certificate issuance or management route. The canonical application interface is
one versioned root-owned executable helper, not a direct SPIFFE-only API. SPIFFE,
cloud KMS/IAM, TPM attestation, and PKCS11 attestation are helper backends. This
keeps the C state machine portable while requiring every enabled backend to prove
the same two facts: a current unique workload identity and possession of the exact
instance custody anchor used by wrap/unwrap. `vault_unseal()==0`, a KMS decrypt,
a TPM-local blob, a PKCS11 PIN, uid/hostname, or a root-owned helper path alone is
never identity evidence.

Only the KMS/SPIFFE integration profile is enabled in B2a. TPM2 stays disabled for
B2 until a helper supplies a verified EK/AK/PCR-bound identity proof using the same
TPM anchor. PKCS11 stays disabled until a helper supplies a verified proof bound to
the token serial, slot and custody key; a PIN/label is insufficient. Compile-time
and runtime gates must both refuse these incomplete modes.

## Public C contract

Add `kb_workload_provider.{h,c}` with no global ambient DB/vault state:

```c
typedef enum {
  KB_WORKLOAD_PROVIDER_NONE = 0,
  KB_WORKLOAD_PROVIDER_KMS_SPIFFE_V1 = 1,
  KB_WORKLOAD_PROVIDER_TPM2_V1 = 2,
  KB_WORKLOAD_PROVIDER_PKCS11_V1 = 3
} kb_workload_provider_kind_t;

typedef enum {
  KB_WORKLOAD_OK = 0,
  KB_WORKLOAD_DISABLED = 1,
  KB_WORKLOAD_UNAVAILABLE = 2,
  KB_WORKLOAD_INTEGRITY = 3,
  KB_WORKLOAD_INVALID = 4
} kb_workload_result_t;

typedef struct {
  kb_workload_provider_kind_t kind;
  const char *helper_path;
  const char *jwks_path;
  const char *proof_spki_path;
  const char *expected_issuer;
  const char *expected_audience;
  uint32_t max_token_age_seconds;
  uint32_t helper_timeout_ms;
} kb_workload_provider_config_t;

typedef struct {
  char issuer[601];
  char subject[601];
  uint64_t issued_at;
  uint64_t expires_at;
  unsigned char proof_anchor_id[32];
  unsigned char custody_anchor_id[32];
  unsigned char token_hash[32];
} kb_workload_identity_t;

kb_workload_result_t kb_workload_provider_open(const kb_workload_provider_config_t *,
                                               kb_workload_provider_t **);
kb_workload_result_t kb_workload_attest(kb_workload_provider_t *,
                                        const unsigned char challenge[32],
                                        const unsigned char binding[32],
                                        kb_workload_identity_t *);
kb_workload_result_t kb_workload_wrap(kb_workload_provider_t *,
                                      const unsigned char challenge[32],
                                      const unsigned char binding[32], const void *plain,
                                      size_t plain_len, kb_workload_identity_t *,
                                      unsigned char *cipher, size_t cap, size_t *len);
kb_workload_result_t kb_workload_unwrap(kb_workload_provider_t *,
                                        const unsigned char challenge[32],
                                        const unsigned char binding[32], const void *cipher,
                                        size_t cipher_len, kb_workload_identity_t *,
                                        unsigned char *plain, size_t cap, size_t *len);
void kb_workload_provider_close(kb_workload_provider_t *);
```

Constructors take copied, bounded configuration: exact enum kind; absolute helper
path; pinned JWT issuer/audience; root-owned JWKS file; root-owned P-256 proof public
key; maximum token age no greater than five minutes; and maximum helper runtime no
greater than five seconds. Outputs are zeroed before work and on every failure.
Callers own no helper allocations. Identity equality is exact issuer+subject plus
both anchor ids. A changed identity or anchor is a terminal integrity result, never
renewal. The opaque provider is constructor-allocated and owns one checked helper
fd plus copied public configuration; `close` releases and cleanses all of it.
Wrap callers provide at least 32768 output bytes and unwrap callers at least 16384,
so capacity failure is rejected before the custody operation. Input and output data
may overlap; identity and length objects may not overlap data or one another. The
caller quiesces all operations before `close`.

The JWT validator first bounds the compact token, then reuses
`aws_webidentity_validate` only behind a distinctly named wrapper. Because that
validator neither exposes `nbf` nor rejects duplicate claims and its output fields
are smaller than this contract, the wrapper does not trust its output copy: only
after the base signature/issuer/audience/time check succeeds, it decodes the
already-verified payload into a fresh bounded parser tree, counts and rejects
duplicate security claims, and extracts them without truncation. It additionally
requires exactly one bounded nonempty printable `sub`,
canonical `iss`, exact audience, `iat <= now+2`, `exp > now-2`, `exp-iat <= 300`,
rejects a present `nbf` later than now+2, and requires a remaining lifetime of at
least 30 seconds. Issuer and subject are each 1..600 ASCII bytes. It hashes then
cleanses the token and every parser copy before return. Numeric dates must be finite
exact nonnegative integers representable as `uint64_t`; arrays or fractional values
fail.
JWKS replacement is atomic and root-owned; each operation reads a fresh bounded
snapshot through a checked directory descriptor, and an unknown kid causes one
checked reopen/reload for that call. This ensures removal of a retired key takes
effect without waiting for a new `kid`. "No
identity cache" means no successful token, claims, helper proof, or operation result
is reused. No network JWKS fetch, discovery, federation, bearer fallback, or cached
proof past expiry exists.

## Helper trust and file boundary

The configured helper and every ancestor must be root-owned, regular/directory as
appropriate, non-symlink, and not group/world writable. Resolve every absolute-path
component with `openat(O_NOFOLLOW)` from an opened `/`, verify each descriptor with
`fstat`, and execute the checked leaf descriptor with Linux
`execveat(AT_EMPTY_PATH)` rather than reopening a path. B2a accepts a native ELF
helper only, avoiding the CLOEXEC interpreter-script ambiguity. Refuse setuid/setgid
files. Unsupported platforms return DISABLED. The parent creates CLOEXEC pipes,
forks with a precomputed fd bound, and the child receives only stdin/stdout; stderr
and every unrelated fd are closed. Before descriptor exec the child sets
`PR_SET_NO_NEW_PRIVS`; the environment is an explicit empty array. A five-second
absolute deadline covers request write, response read, and normal child exit. On
expiry the parent kills the helper process group and exactly reaps the direct child;
the final kernel reap can extend past the deadline only for an uninterruptible task,
avoiding zombies at the cost of that explicit kernel-state exception. Calls on one
provider are mutex-serialized so a provider cannot create an unbounded concurrent
child set.
Short/extra output, signal exit, timeout, partial framing, or diagnostics are generic
unavailable and all request/response buffers are cleansed.

The helper is a trusted platform adapter, but its assertions are not trusted
without independent cryptographic verification: C verifies the workload JWT under
the pinned JWKS and verifies a response-binding ECDSA P-256/SHA-256 signature under
the pinned proof SPKI. Its SHA-256 is `proof_anchor_id`; a distinct
`custody_anchor_id` is SHA-256 of the provider wrapping-key resource identifier and
is included in every proof. Deployment must policy-bind that proof key and
wrap/unwrap permission to the same platform principal. The C boundary proves the
JWT, the pinned-helper proof, and the exact transcript; it cannot prove cloud IAM
policy, possession of an X.509-SVID not present on this wire, or that a remote
custody operation occurred. Those remain explicit trust obligations of the pinned
helper/backend. A backend unable to enforce them returns disabled; B2a makes no
direct X.509-SVID/JWT equivalence claim.

## Binary wire protocol v1

No JSON, shell, argv secret, native-width integer, or unbounded read is used. Each
direction is one frame: 8-byte magic `AIMEEWI1`, u8 operation, u8 status,
u16 reserved zero, u32 big-endian payload length, then an operation-specific series
of u32 big-endian length-prefixed byte strings. Total frame is at most 65536 bytes;
all fixed fields require their exact lengths and no trailing bytes are allowed.

- Request status and reserved are zero. Response status is exactly 0=OK,
  1=DISABLED, 2=UNAVAILABLE, or 3=INTEGRITY; non-OK responses contain no fields
  because their payload length must be zero. Unknown status is fatal. Operation
  must echo the request operation on every response. The reserved u16 and every
  u32 are big-endian; the eight magic bytes are compared as bytes.
- `ATTEST=1` request fields: challenge(32), binding(32). Response fields:
  token(1..16384), proof_anchor_id(32), custody_anchor_id(32), proof(8..80).
- `WRAP=2` request fields: challenge(32), binding(32), plaintext(1..16384).
  Response fields: token, proof_anchor_id, custody_anchor_id, proof,
  ciphertext(1..32768).
- `UNWRAP=3` request fields: challenge(32), binding(32), ciphertext(1..32768).
  Response fields: token, proof_anchor_id, custody_anchor_id, proof,
  plaintext(1..16384).

The verified proof is DER ECDSA from the constructor-pinned prime256v1 SPKI, with
strict minimal DER and high-S signatures rejected rather than normalized. Its
domain-separated transcript is an unambiguous length-prefixed binary encoding of:
`"aimee.workload.provider.v1"`, operation, challenge, binding, SHA-256(token),
proof_anchor_id, custody_anchor_id, SHA-256(request data field or empty), and
SHA-256(response data field or empty). For ATTEST, both data hashes are exactly
SHA-256 of the zero-length byte string. The domain literal and every following field
are u32-length-prefixed. Thus a proof cannot move between attest/wrap/unwrap, calls,
bindings, tokens, either anchor, ciphertexts, or plaintexts. Challenge is OS-random
for every call and is
never accepted twice by the test helper; production freshness additionally relies
on verified JWT time bounds and platform backend policy.

`binding` is computed by the later caller from its versioned bundle/issuance
transcript. B2a treats it as opaque but always signs it. B2c must include workload
issuer/subject hash, both anchor ids, certificate issuer/serial/fingerprint, SPKI
digest, lineage generation, bundle nonce, storage identifier, and provider-specific
anchor locator in that length-prefixed transcript.

## Provider matrix

- `KMS_SPIFFE_V1`: enabled only when the helper returns a JWT-SVID whose signature,
  issuer, audience, subject and time pass, and a binding proof whose pinned SPKI hash
  is the proof anchor id and covers the separate custody anchor id. Wrap/unwrap must
  be performed by the helper under that same platform principal with workload hash
  in the provider encryption context. This same-principal relation is an explicit
  backend/deployment policy invariant; C independently verifies the token and
  pinned-helper transcript signature but cannot audit cloud IAM policy or prove the
  custody operation independently of that helper.
  Shared exported/fleet KEKs are forbidden. A mock uses a dedicated P-256 signer and
  per-subject AEAD keys; CT integration uses SPIRE-issued identity plus the same
  helper proof key/custody policy.
- `TPM2_V1`: constructor returns DISABLED in B2a even when existing TPM custody
  unseals. Enabling later requires verified AK/EK chain, nonce quote, allowed PCR
  policy, both anchor ids bound to the same TPM object/NV lineage, and wrap/unwrap in
  that TPM domain.
- `PKCS11_V1`: constructor returns DISABLED in B2a. Enabling later requires a signed
  external workload attestation bound to `CK_TOKEN_INFO.serialNumber`, slot id and
  non-extractable wrapping/proof key. PIN, module, slot, label, or successful login
  alone never qualify.

## Failure and refresh rules

Provider absence/disabled is a normal fail-disabled B2 state. Malformed proof,
identity/anchor change, impossible time, proof mismatch, or unwrap
binding mismatch is integrity failure and requires operator repair; no overwrite,
fallback, generic enrollment, or identity migration occurs. Dependency outage or
timeout is unavailable and retried with bounded exponential backoff by B2c. A
currently loaded future management credential is outside B2a; this provider grants
no grace.

Attestation is fresh per operation and B2a has no identity cache. Wrap/unwrap always
returns and verifies a proof in that same operation.
Any different issuer/subject/anchor from persisted state disables P5 management.

## Implementation and validation

1. Implement the strict framing/transcript/parser and pure proof/JWT validation.
2. Implement checked-descriptor helper execution, deadlines, fd/env isolation, and
   enabled/disabled provider constructors.
3. Add a native test-helper executable implementing two distinct subjects and
   per-subject AEAD wrap. Its translation unit lives under `tests/`, has a dedicated
   test target, and is absent from `KB_SRCS`, install/package targets, and every
   production binary closure.
4. Unit/fuzz/ASAN/UBSAN/leak tests cover every length/overflow/truncation/extra-byte,
   DER malleability, wrong token claims/key/audience/subject/time, proof replay,
   cross-op/binding/token/proof-anchor/custody-anchor/data substitution, helper timeout/crash/output,
   planted fd/env inheritance, path/symlink/owner/mode replacement, two-subject
   cross-unwrap denial, output/JWT parser-copy clearing and cleansing. Windows and
   every platform without checked-descriptor exec are explicit DISABLED stubs.
5. CT260 uses a root-owned mock platform helper with real OpenSSL/JWT signatures to
   prove distinct identities, wrap/unwrap isolation, restart, helper outage, stale
   identity and either-anchor replacement fail closed. Old ciphertext rejection is
   a B2c lineage-binding test, not a B2a provider claim. If direct
   SPIRE workload issuance can be configured without touching production CTs, add a
   throwaway agent/backend gate; it is not allowed to weaken or bypass the portable
   helper contract.

No schema, CA sign, management certificate, registry dial, status request, server
challenge, health route, action, or console surface is added in B2a.
