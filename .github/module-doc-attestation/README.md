# Module document attestation deployment contract

This directory defines the protected repository half of module-document attestation. The verifier
is an independently deployed service. Pull requests cannot replace its code, issuer profile, trust
policy, repository credential, or SSHSIG toolchain.

## Provider-neutral OIDC configuration

The service accepts named issuer profiles conforming to `issuer-profile.example.json` and
`scripts/module_doc_contract.py`. A profile contains an exact issuer, audience, pinned JWKS
transport, algorithm allowlist, normalized claim mappings, and immutable workload predicates. It
does not contain a provider enum. An administrator may configure GitHub, Keycloak, Entra ID, Okta,
or another conforming issuer without changing verifier code.

The example uses reserved `.invalid` endpoints and synthetic claims. It cannot authenticate a real
deployment. A deployment stores its real profile in the verifier's protected configuration, not in
a pull-request-controlled branch.

## Repository trigger

`module-doc-attestation-trigger.yml` runs only from the protected base through
`pull_request_target`. It receives an OIDC token for the configured audience and sends exactly the
token and pull-request number to the verifier. It never checks out candidate content and has no
repository credential or secret. The trigger check is not the required attestation check.

The verifier uses its repository-scoped installation to resolve the pull request, protected base,
candidate commit, refs, run, attempt, and trigger-check identity. It ignores candidate coordinates
from requests. Its publisher credential may read pull-request and Git objects and create only the
named `module-doc-attestation` check. It may not push, merge, administer the repository, or create a
different app's check. The required check is pinned to that publisher app by an administrator.

## Human trust and SSHSIG

Module owners and reviewers are authenticated with distinct Ed25519 SSHSIG signatures in namespace
`aimee.module-doc.v1`; CI OIDC never substitutes for a human signature. Real identities and public
keys are enrolled only after this verifier release is deployed. No test key or generated agent key
is an enrollment candidate.

Before activation, the deployment lock must pin an OpenSSH toolchain image by immutable digest and
the image's `ssh-keygen` binary by SHA-256. The service verifies both pins before accepting work.
The repository intentionally does not publish a fabricated image digest or a host-specific binary
hash; release publication produces those two values and the administrator records them in protected
service configuration.

## Activation order

1. Deploy the reviewed verifier release and its closed issuer profile.
2. Install the repository-scoped publisher app without organization `admin:org`.
3. Configure `MODULE_DOC_VERIFIER_URL` and `MODULE_DOC_VERIFIER_AUDIENCE` as protected repository
   variables.
4. Enroll genuine owner and reviewer public keys in the protected service trust policy.
5. Pin the required `module-doc-attestation` context to the publisher app.
6. Only then open the descriptor-v2 and signed-document migration PR.
