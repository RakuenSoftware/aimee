# P5-B2c management certificate and local bundle lifecycle

- **State:** draft implementation plan.
- **Depends on:** P5-B2a workload identity/custody provider; P5-B2b primary
  management-instance lineage; P5-A management certificate profile; P7-custodied
  kb CA loading.
- **Followed by:** P5-B3 live management challenge/status/health exchange.

## Boundary

B2c is a KB-only, no-route lifecycle orchestrator. It generates a per-instance
private key and CSR, signs through the existing custodied CA path, verifies the
complete public certificate profile, wraps the local bundle through the exact B2a
workload/custody identity, durably stages it, commits its public metadata through
B2b, and recovers or renews it after restart. It adds no listener, status staple,
server dial, health exchange, operator action, registry mutation, OIDC surface, or
new CA/storage authority.

No private key, PEM/DER bundle, helper JWT, proof, plaintext wrapping key, or
workload token enters PostgreSQL, WORM output, logs, argv, environment, or a
plaintext file. PostgreSQL remains authoritative for identity lineage and active
public metadata. The root-owned local ciphertext is only the material needed by
that one instance to present the active leaf later in B3.

## Lifecycle contract

Add `src/kb/kb_management_cert_lifecycle.{h,c}` to the KB-only source closure.
The constructor copies bounded configuration: a pre-opened B2a provider, the
operator-granted 128-bit installation id, custodied CA directory, and a root-owned
absolute bundle directory. It opens that directory component-by-component without
following symlinks, rejects non-root ownership or group/world write, holds an
exclusive nonblocking lifecycle lock, and never reopens it by path. Unsupported or
incomplete provider modes remain fail-disabled.

The public API returns only typed status plus public active metadata:

```c
typedef enum {
  KB_MANAGEMENT_CERT_OK = 0,
  KB_MANAGEMENT_CERT_DISABLED,
  KB_MANAGEMENT_CERT_UNAVAILABLE,
  KB_MANAGEMENT_CERT_DENIED,
  KB_MANAGEMENT_CERT_CONFLICT,
  KB_MANAGEMENT_CERT_INTEGRITY,
  KB_MANAGEMENT_CERT_INVALID
} kb_management_cert_result_t;

kb_management_cert_result_t kb_management_cert_lifecycle_open(...);
kb_management_cert_result_t kb_management_cert_reconcile(...);
kb_management_cert_result_t kb_management_cert_load_active(...);
void kb_management_cert_lifecycle_close(...);
```

The secret output type has fixed `KB_PKI_KEY_PEM_MAX`, `KB_PKI_CERT_PEM_MAX` and
`KB_PKI_CERT_PEM_MAX` key/leaf/CA buffers plus exact populated lengths; a required
`kb_management_cert_bundle_clear()` helper cleanses it. All outputs are zeroed
before work and on failure. Secret buffers are locked where supported, cleansed
before release, and never returned from `reconcile`. `load_active` unwraps only the
B2b-current generation into that caller-owned bounded secret bundle for the later
B3 TLS context. There is no cached plaintext bundle and no API for provider tokens,
proofs, ciphertext internals or candidate files. One lifecycle object serializes
operations, and close requires callers to quiesce it. The constructor also holds
`flock(LOCK_EX|LOCK_NB)` on the already checked bundle-directory descriptor for its
entire lifetime, so upgrade overlap cannot create a second process-local authority.
The provider is borrowed, must outlive the lifecycle object, and both objects must
be quiesced before either close. B2c obtains the provider kind from a read-only B2a
accessor on the opaque provider; it never trusts a parallel caller-supplied enum in
a custody transcript.

## State machine and ordering

Every reconcile begins with a fresh B2a attestation and constructs the exact B2b
binding from verified issuer, subject, proof anchor and custody anchor. A change in
any component is terminal integrity, never implicit replacement or re-enrollment.
Operation ids, bundle nonces and storage ids are independent OS-CSPRNG values. An
initial issue also chooses a fresh 128-bit authority id before begin and persists it
inside the wrapped key-intent header/binding; renewal reuses the exact authority id
from the active B2b snapshot. A replacement installation receives a fresh authority
id rather than inheriting the replaced instance's enrollment authority.

For initial issuance or renewal:

1. Read the primary B2b snapshot. B2b deliberately maps both absent and inactive
   rows to `DENIED`, so B2c does not infer absence from that result. When there is
   no local current manifest or intent, it may durably stage a fresh initial intent
   and call the fixed initial facade; B2b remains authoritative and denies inactive,
   revoked, replaced, or otherwise unauthorized rows. For an
   active row, begin renewal only inside B2b's inclusive 1200-second window and
   bind the exact current enrollment tuple and next generation. Outside that window
   return the still-active public snapshot without signing or writing.
2. Choose the operation id before key generation. Generate exactly an RSA-2048
   private key and CSR in memory. Canonical SHA-256 digests cover OpenSSL's strict
   `i2d_X509_REQ` DER and the CSR public key's `i2d_PUBKEY` DER. Before calling B2b,
   encode the private-key DER and CSR DER in a distinct
   `aimee.p5.management-key-intent.v1` plaintext, wrap it through B2a with a separate
   fresh challenge and a binding over installation, lineage, generation, operation
   id, authority id, B2b binding digest, both CSR digests, provider kind, fresh nonce and storage
   id, and durably stage it as a distinct immutable `intent.<operation-id>` record.
   Its strict outer header carries those exact public fields, the custody-binding
   digest, ciphertext length and ciphertext; the wrapped plaintext is the key/CSR
   intent only. It uses the same checked create/reread/sync protocol as a candidate
   but is a different record type and can never be overwritten or promoted as the
   final bundle. Only after file and
   directory sync may B2c call `begin_initial` or `begin_renewal` with that operation
   id. Exact replay unwraps the key-intent capsule and reproduces the same CSR and
   digests; it never generates a new key for a persisted pending intent. Missing or
   mismatched intent for a pending issue is integrity until B2b expires/quarantines it.
3. Load the CA only through `kb_pki_ca_load_custodied`, sign with
   `kb_pki_sign_kb_management_csr(...,3600,...)`, and cleanse the loaded private CA
   material immediately after signing. B2c then independently parses and verifies:
   the chain under the exact loaded public CA; CSR proof and leaf-SPKI equality;
   exact subject `CN=p5-kb-management`; exactly `clientAuth`; critical CA:FALSE and
   digitalSignature usage; the single noncritical management marker; issuer and CA
   fingerprint; canonical lowercase serial; unique leaf fingerprint; and validity
   bounds accepted by B2b. The stack CA struct is always
   `OPENSSL_cleanse(&ca,sizeof(ca))` immediately after the sign attempt, matching
   existing enrollment call sites. B2c derives issuer/fingerprint from that
   custodied CA; B2b activation authoritatively compares them to the offline grant
   pins in the same transaction.
4. Encode a versioned strict binary plaintext bundle containing PKCS#8 private-key
   DER, leaf DER and CA DER as bounded big-endian length-prefixed fields. The whole
   plaintext is at most 16384 bytes and has no trailing bytes. Compute its public
   bundle digest over exactly that plaintext. The plaintext never embeds that
   digest, nonce, storage id, binding digest or ciphertext, so the construction is
   acyclic; those values exist only in the outer header/transcript.
5. Build the domain-separated B2c custody binding from version, installation id,
   immutable lineage root, generation, the canonical B2b binding digest, both B2a
   anchors, certificate issuer/serial/fingerprint, leaf SPKI digest, public bundle
   digest, fresh bundle nonce, fresh storage id, and the configured provider kind.
   Every variable field is big-endian length-prefixed. Call B2a `wrap` with a fresh
   challenge into a buffer of at least B2a's required 32768 bytes. Exact returned
   literal issuer/subject and both anchors must still match the initial attestation,
   and must recompute the same B2b binding digest. B2a independently verifies the
   current JWT, proof, time and transcript on every call.
6. Durably stage one immutable `candidate.<operation-id>` file. It is separate from
   and cannot alias the key-intent record for the same operation. Its strict public
   header contains a distinct magic/version, installation,
   lineage, generation, operation id, nonce, storage id, public certificate
   metadata/digests, custody-binding digest, ciphertext length and ciphertext.
   Create with `openat(O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600)`, immediately
   `fstat` a root-owned regular 0600 one-link inode, write completely, `fdatasync`,
   verify by reread/parse, then `fsync` the directory. Partial/oversize writes are
   cleansed, unlinked and followed by directory sync. A complete file surviving a
   crash before directory sync is still treated only as an untrusted candidate and
   must unwrap and verify. No mutable `current` pointer is written yet.
7. Call B2b activation with only the independently verified public metadata. On
   exact success/replay, write a unique same-directory `current.tmp.<random>` via
   `openat(O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600)`, immediately apply the same
   inode checks, complete/reread/`fdatasync` it,
   `renameat` it over `current`, then `fsync` the directory. Recovery opens
   `current` with `O_NOFOLLOW` and requires a root-owned regular 0600 one-link file.
   The strict manifest names the exact immutable candidate operation id/generation
   and public digest.
   Activation therefore never precedes recoverable ciphertext. A crash after stage
   but before activation retries exact activation; a crash after activation but
   before manifest promotion reads the B2b snapshot, unwraps and verifies the matching staged
   candidate, and promotes it. A manifest can never authorize a generation that is
   not the current primary B2b snapshot.

Failed signing/wrapping/staging leaves the B2b issue pending for bounded exact
retry. A mismatch between a pending intent and candidate is integrity and never
overwritten. A candidate rejected by B2b is retained as quarantined evidence until
bounded cleanup proves it is neither current nor pending. Cleanup never deletes the
current manifest target and uses directory descriptors only.

## Active load and renewal safety

The local renewal-window calculation uses the snapshot's integer not-after epoch
and wall clock only as a no-write optimization; B2b rechecks its own primary clock
and exact inclusive threshold authoritatively. `load_active` first obtains one primary B2b snapshot, reads the exact current
manifest/candidate through the checked directory descriptor, revalidates every
public field and custody-binding digest, calls B2a `unwrap` with a fresh challenge,
requires the returned workload tuple/anchors to match, parses the strict plaintext,
and re-verifies private-key↔leaf, leaf↔CA, profile, fingerprints, SPKI, generation
and expiry. It then takes a second primary snapshot and requires the same active
installation, generation, enrollment, certificate tuple and revocation generation
before releasing plaintext. A transient snapshot change gets one bounded retry;
stable contradictory state is integrity. Any mismatch clears output. Revoked, expired,
replaced, pending-only, replica/read-only, provider-outage or missing-ciphertext
state never yields a usable key. Unwrap always provides B2a's required 16384-byte
plaintext capacity. DER is re-encoded to canonical PEM with OpenSSL's standard
writer into the fixed output fields, with exactly one terminal NUL outside each
reported length and no retained DER scratch.

Renewal keeps the prior active manifest usable until B2b atomically activates the
new enrollment. Once generation N+1 is active, generation N is revoked and
`load_active` refuses its candidate even if the on-disk manifest is stale. No B2c
call can prevent revocation immediately after its final snapshot; B3 therefore owns
the required primary/status admission before every request and must not cache B2c
plaintext across that boundary.

## Status mapping and retry

B2a disabled maps to `DISABLED`; invalid maps to `INVALID`; unavailable maps to
`UNAVAILABLE`; B2a integrity maps to `INTEGRITY`. B2b
invalid/denied/conflict/integrity remain distinct; retry and
unavailable are the only retryable database results. Filesystem/CA/provider outage
is unavailable, while malformed persisted bytes, changed identity/anchor, stale
manifest, impossible profile or key mismatch is integrity. No failure falls back to
plaintext, file custody, generic enrollment, a previous identity, or a new key for
an existing pending operation. Retry/unavailable use capped exponential backoff
with jitter and an absolute caller deadline; no reconcile call loops indefinitely.

## Validation

Unit/fuzz/ASAN/UBSAN/leak tests cover strict bundle/header/transcript codecs,
overflow/truncation/trailing bytes, output clearing, key/CSR/certificate/profile
verification, provider and DB status mapping, aliasing, and cleansing. Deterministic
injected crash-point tests cover every boundary around key-intent sync, begin, sign,
wrap, candidate sync, activate, manifest file sync, rename and directory sync. They
prove that a crash after begin always retains the exact wrapped key/CSR intent.
Adversarial file tests cover symlinks, ownership/mode/link-count swaps,
short writes, stale manifests, duplicate candidates and planted unrelated files.

CT260 composes the real B2a root-owned mock provider, custodied CA profile and real
PG17 B2b functions. It proves first boot, restart/load, two independent subjects,
cross-subject unwrap denial, provider outage, primary outage, exact pending recovery,
crash after durable stage, crash after activation, renewal at the inclusive
threshold, old-generation refusal, revocation/replacement refusal, changed workload
or either anchor, corrupted ciphertext/header/manifest, and absence of private/PEM/
token/proof bytes in PostgreSQL and WORM. B3/listener behavior remains out of scope.

Run an adversarial plan roundtable, bake every valid minority finding, delegate the
bounded implementation, validate locally and on CT260, then run an adversarial full
branch roundtable to convergence before merge.
