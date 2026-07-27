# Module document attestation deployment contract

This contract separates the provider-neutral verifier engine from the current repository trigger.
`scripts/module_doc_contract.py` contains no issuer selection or provider enum. The workflow in this
repository is the GitHub Actions binding; another CI platform needs an equivalent protected trigger
that produces the same normalized workload and candidate-target records.

## Repository guarantees

`scripts/module_doc_contract.py` supplies deterministic parsers, immutable Git-object reads,
descriptor-v2 validation, document validation, SSHSIG validation, workload-to-candidate binding,
replay keys, and publisher-result validation. Its decision engine invokes the deployment's
cryptographic JWT verifier before normalizing claims. It then invokes the read-only repository
resolver, which yields the candidate target and keeps the read-only object database live for every
Git read against it. The resolver context cleans up that database on success and on every failure
after entry. It must also clean partial resources if acquisition fails before entry. Resolver cleanup
cannot suppress an active contract or validation failure, and a cleanup error replaces no active
failure; a cleanup error on a normal exit propagates fail closed. The invocation-scoped validation
capability exposes only pre-bound candidate and protected-base readers over that one database and one
decision budget; it cannot open arbitrary revisions.
Those readers expose no repository path, budget, or cache, and reject every Git operation after
validation returns. Candidate-policy failures remain active through cleanup and become failure
results only afterward. Only detached normalized metadata survives cleanup for replay-key and
publisher-result construction.

Validation-reader operations hold synchronized leases for their full duration. Closing the
capability rejects new leases and waits for all in-flight operations before repository cleanup
begins. The same synchronization protects Git-operation and evidence counts, the distinct-blob
cache, and byte-ceiling accounting from concurrent updates.

Function names alone do not establish those properties, so deployment integration tests must
exercise invalid signatures, stale keys,
redirects, repository mismatches, replay, and publisher permissions.

Each decision has explicit ceilings for evidence records, distinct blob bytes, Git operations, and
Git wall-clock time. Immutable blobs are cached by repository, commit, and path, so repeated evidence
records cannot multiply object reads. Replay bindings use canonical JSON over every normalized
workload and candidate-target field; the token identity separately binds the exact issuer and token
identifier without delimiter ambiguity.

`trigger_check_identity` is a provider-neutral correlation value, not an OIDC claim or a GitHub
Checks check-run ID. The contract derives it as a canonical SHA-256 identity from the normalized
`repository_identity`, `run_identity`, and `attempt`; issuer profiles and repository adapters do not
supply it. This clarification does not make the overall external verifier deployable.

The derivation preimage is the JSON object with exactly the keys `attempt`, `repository_identity`,
and `run_identity`, serialized with keys sorted, compact `,` and `:` separators, JSON ASCII escaping,
and ASCII encoding. Hash those bytes with SHA-256 and prefix the lowercase hex digest with `sha256:`.
JSON string values are preserved exactly; no Unicode normalization is applied. For example,
`{"attempt":"2","repository_identity":"repository-\u00e9","run_identity":"run-\"42\""}` produces
`sha256:d56311b051331f765ff4651d87d88ce9f760aaed7443bd635d0a414d40771821`.

Issuer profiles copied from the earlier example must remove `trigger_check_identity` from
`claim_mappings`; the closed profile validator now rejects that obsolete mapping.

`module-doc-attestation-trigger.yml` is dormant unless the protected repository variable
`MODULE_DOC_ATTESTATION_ENABLED` is exactly `true`. While active, it fails closed unless the
verifier URL, audience, and TLS public-key pin are configured. It requests an OIDC token and sends
only the request schema identifier, token, and pull-request number. It does not send candidate
content, commits, or refs. The workflow grants only `id-token: write`, performs no checkout, and is
covered by a static contract test that rejects adding `actions/checkout`.

The trigger check and required `module-doc-attestation` check are different checks. The external
publisher records the trigger identity as the required check's external identifier and targets only
the candidate commit independently resolved through the repository API.

## Provider-neutral issuer profiles

Named issuer profiles conform to `issuer-profile.example.json` and
`scripts/module_doc_contract.py`. A profile contains the exact issuer and audience, pinned JWKS
transport and keys, an algorithm allowlist, normalized claim mappings, immutable workload
predicates, and a pinned repository API. It contains no provider enum. Users may configure any
conforming issuer without changing verifier code.

The committed example uses reserved `.invalid` endpoints, synthetic claims, and unusable pins. It
cannot authenticate a deployment. Real profiles belong to protected verifier configuration, never
to a pull-request-controlled branch.

## External deployment prerequisites

The repository does not supply service credentials or claim that these prerequisites are already
deployed. Before activation, operators must provide:

- a pinned JOSE implementation that verifies JWS signatures, JWKS key pins and freshness, issuer,
  audience, algorithm, and token time before returning claims to the engine;
- a read-only repository client that translates platform API fields into the normalized repository,
  workflow, revision, run, attempt, trigger, pull-request, base, and candidate records;
- a durable replay store that atomically binds each token replay key to one decision replay key and
  persisted result before publication. The decision key covers the complete normalized workload and
  candidate-target records. Exact retries reuse that result; any changed field for the same token
  fails;
- a repository-scoped publisher authorized only to create the named attestation check;
- a canonical trust policy containing genuine human owner and reviewer Ed25519 public keys; and
- an OpenSSH image selected by immutable digest plus a lock containing the SHA-256 of the exact
  `ssh-keygen` binary. The deployment enforces the image selection; `load_locked_toolchain` verifies
  the lock shape and binary hash before the engine can perform SSHSIG verification.

OIDC authenticates the protected workload, not document authors. The deployed trust policy must
reject test keys and agent-generated keys as human enrollments. Each module requires distinct owner
and reviewer identities and two Ed25519 SSHSIG signatures in namespace `aimee.module-doc.v1`.

`scripts/sign_module_docs.py` is the human signing helper. It accepts canonical public-key files and
copies only their public bytes into its mode-`0700` staging directory; matching private keys must be
available through `SSH_AUTH_SOCK`. It invokes no shell, signs every module, verifies every result,
and builds the complete index before changing the attestation directory. A malformed prior directory
or any signing failure leaves it unchanged. Initial installation uses an atomic rename; renewal uses
`renameat2(RENAME_EXCHANGE)` and fails without changing the directory when the filesystem lacks that
operation. The helper pins the parent, staging, and existing target directories by open descriptors
and rechecks their directory-entry identities around installation. It performs no automatic rollback
or recursive root cleanup: aborted staging trees and the verified prior generation from a renewal
remain as mode-`0700` quarantine directories for explicit operator inspection and removal.

## Current GitHub repository binding

The current binding uses a protected-base `pull_request_target` workflow and a repository-scoped
check-publisher app. Branch protection or a repository ruleset, not this source tree, must pin the
required `module-doc-attestation` context to that app and prohibit bypasses. Installing this
repository-scoped app does not require organization-wide administration; specifically, it does not
require GitHub's `admin:org` scope.

The app credential belongs in the deployed service vault. Its permissions are limited to reading
pull-request and Git objects and creating its named check. It must not push, merge, administer the
repository, or create another app's check.

## Immutable snapshot acquisition

`scripts/module_doc_git_snapshot.py` supplies the Git acquisition boundary used by a future
repository resolver. It is provider-neutral because it depends on Git Smart HTTP and full refs, not
on a provider API or webhook format. A trusted provider adapter supplies an HTTPS repository URL,
opaque full source refs, exact full lowercase SHA-1 commit IDs, and one owned authorization-header
`bytearray`. The module does not select an OIDC issuer, interpret pull requests, call a provider API,
or decide which refs and commits are authorized. Production callers must establish those facts
before calling it. They must also pass a nonempty operator-configured allowlist of canonical HTTPS
origins; the repository origin must match one entry exactly, including any non-default port.
Canonical origins use a lowercase host, omit the default port, and contain no path, query, fragment,
credentials, percent encoding, whitespace, or dot segments.

Production acquisition is Linux-only and fails closed without `memfd_create` and `/proc/self/fd`.
The authorization header uses the exact canonical field spelling `Authorization`, an arbitrary
provider-neutral auth scheme, and token68 credentials. It exists in an anonymous, sealed,
mode-`0600` Git-config descriptor only for the single atomic fetch. Its `extraHeader` setting is
scoped to the canonical repository URL. The header is not placed in a subprocess command line, an
environment value, or a named filesystem entry. The descriptor is closed and both the caller's buffer and the
derived mutable buffers are overwritten before commit validation can begin or a repository handle
is returned. Every reference to the caller's passed `bytearray` observes that overwrite. Callers must
not create a separate copy if they require the same lifetime guarantee because the module cannot
overwrite an object it was not given. Core dumps, swap, and other kernel-mediated copies are outside
this process-level guarantee and must be controlled by the deployment. Authenticated fetch failures
return a fixed diagnostic rather than remote-controlled stderr. Authenticated stdout and stderr are
drained into wipeable buffers and discarded, preventing a server from retaining reflected credential
fragments in process capture or an error. The subprocess environment is built from a fixed allowlist, so
caller-provided `GIT_TRACE*`, proxy, and Git configuration variables are not inherited.

Each acquisition creates a fresh bare repository and accepts local destination refs only when they
match `refs/aimee-snapshot/[a-z][a-z0-9-]{0,63}`; invalid destinations are rejected before fetch. It
disables redirects and interactive credentials, fetches atomically without tags or `FETCH_HEAD`, and
retains the fetched refs for the handle's lifetime. It resolves each ref with `^{commit}`, rejects a
non-commit target, compares the full object ID with the adapter's expected value, and runs
`git fsck --full --strict --no-reflogs --unreachable` before returning. Any unreachable object rejects
the snapshot, preventing a server from retaining reflected credentials in an extra object. All later Git commands use isolated
non-network configuration. The returned repository tree is owner-read-only and exposes no configured
remote. A
context-managed handle removes the tree after the body returns or raises any `BaseException`;
callers that do not use the context manager must call `close()`.

Subprocess time, stdout, stderr, fetch count, and ref length have explicit module constants. Bounded
tree scans while each subprocess runs detect repository growth, but they are not a storage quota;
production requires an operator-enforced storage quota on the dedicated temporary parent. The
operator creates that parent as mode `0700`, owned by the isolated worker UID; the module does not
create or change ownership of it. Linux
process-session support is also required: timeout handling terminates the Git process group before
cleanup. The caller must run the acquisition worker in process isolation without unrelated concurrent
forks; the module does not enforce that deployment boundary. Python passes the otherwise
close-on-exec descriptor only to the fetch process, whose Git transport child must also inherit it.
A host `SIGKILL` can still leave an orphaned snapshot containing the authorized source because no
process can run cleanup afterward. Deployment therefore
requires an operator-owned reclaimer for the dedicated temporary parent, with age and ownership
checks, before this boundary is production-ready. Mode-based read-only state is an accidental-write
guard, not protection from another process running as the same UID; deployment process isolation is
the security boundary. The private local-file transport exists only for tests, assumes a
non-adversarial fixture tree, is confined to a real caller-supplied fixture root, and rejects
symlinks observed during validation.

This primitive does not make the external verifier deployable. Provider API authorization,
OIDC-backed workload normalization, durable replay, the publisher, process isolation, and the
orphaned-snapshot reclaimer remain external deployment prerequisites.

## Activation order

1. Deploy the reviewed verifier release and a provider-neutral issuer profile.
2. Install the repository-scoped publisher and configure its least-privilege credential.
3. Configure `MODULE_DOC_VERIFIER_URL`, `MODULE_DOC_VERIFIER_AUDIENCE`, and
   `MODULE_DOC_VERIFIER_SPKI` as protected repository variables.
4. Enroll genuine owner and reviewer public keys in the protected service trust policy.
5. In branch protection or the repository ruleset, pin `module-doc-attestation` to the publisher and
   prohibit bypasses.
6. Set `MODULE_DOC_ATTESTATION_ENABLED` to exactly `true` and verify a negative canary PR fails.
7. Only then open the descriptor-v2 and signed-document migration PR.
