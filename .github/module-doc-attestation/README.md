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
resolver before opening the resolved candidate commit. Function names alone do not establish those
properties, so deployment integration tests must exercise invalid signatures, stale keys,
redirects, repository mismatches, replay, and publisher permissions.

Each decision has explicit ceilings for evidence records, distinct blob bytes, Git operations, and
Git wall-clock time. Immutable blobs are cached by repository, commit, and path, so repeated evidence
records cannot multiply object reads. Replay bindings use canonical JSON over every normalized
workload and candidate-target field; the token identity separately binds the exact issuer and token
identifier without delimiter ambiguity.

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
operation.

## Current GitHub repository binding

The current binding uses a protected-base `pull_request_target` workflow and a repository-scoped
check-publisher app. Branch protection or a repository ruleset, not this source tree, must pin the
required `module-doc-attestation` context to that app and prohibit bypasses. Installing this
repository-scoped app does not require organization-wide administration; specifically, it does not
require GitHub's `admin:org` scope.

The app credential belongs in the deployed service vault. Its permissions are limited to reading
pull-request and Git objects and creating its named check. It must not push, merge, administer the
repository, or create another app's check.

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
