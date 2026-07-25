# Core modularization slice 7 plan: descriptor v2 and module documentation

## Scope and sequence

This document describes optional governance hardening for signed descriptor-v2 attestations. It is
not the delivery sequence for physical source ownership, ordinary module documentation, or normal
review and CI. Those structural slices proceed independently and may land before this program.

The structural policy is recorded in `core-modularization-slice-6.md`; the first independent
physical move is recorded in `core-modularization-slice-8.md`. This program resumes only after the
external verifier is deployed, the publisher app is pinned as the required-check source, and real
human owner and reviewer SSH public keys are enrolled in protected-base policy.

If this governance program is resumed, it lands as two ordered PRs because a candidate cannot
establish the verifier that authorizes itself.

Slice 7a lands the provider-neutral CI OIDC policy schema, external protected verifier, deterministic
module-document parser, SSHSIG verifier, fixtures, protected trigger workflow, and check-publisher
contract. After 7a merges, administrators deploy the verifier, install its repository-scoped check-
publisher app, pin that app as the required-check source, and enroll real owner and reviewer SSH
public keys in a separate protected-base policy change. This requires repository administration,
not organization `admin:org` or an Enterprise required-workflow rule.

Slice 7b then atomically migrates all twenty-six canonical descriptors to version 2, adds one
substantive document per module, and adds two human SSHSIG signatures per document. It cannot merge
until the 7a verifier is deployed, the publisher app is pinned as the required-check source, and
real human owner and reviewer SSH public keys are enrolled in the protected-base trust policy.

That deployment gate applies only to the signed descriptor-v2 candidate described here. It cannot
block source relocation, refactoring, ordinary module documents, design notes, contributor
documentation, or transition ledgers. Those artifacts use normal technical review, roundtable
review where required, repository CI, and branch protection.

Descriptor v2 is metadata-only. It does not claim complete source migration, surface ownership,
build inclusion, runtime registration, provider readiness, or generated-profile coverage. V1
dependency, default-selection, and runtime-toggle semantics remain unchanged.

## OIDC boundary

There are two separate OIDC consumers and neither chooses an identity provider in code.

Product OIDC belongs to optional `governance`. Users manage named issuer profiles from Aimee Control
Plane, currently `aimee-kb`, with equivalent headless config, CLI, environment, and API operations.
GitHub, Keycloak, Entra ID, Okta, or another conforming issuer is user configuration, not a provider
enum or privileged implementation. That contract is normative in
`product-governance-web-and-config.md` and `tiered-llm-p5-oidc-control-plane.md`.

This slice's CI OIDC token authenticates a protected trigger to the external documentation verifier.
It uses the same provider-neutral design principle but a separate repository policy: an issuer
profile declares issuer, JWKS trust, audience, standard claim names, optional namespaced claim
mappings, and predicates that bind a protected workflow to immutable repository, run, and trigger-
check identities. The external verifier resolves base and candidate objects independently through
its read-only repository installation. It contains no GitHub issuer URL or GitHub claim name. The
current deployment may configure a GitHub Actions profile; another CI system can supply another
profile without changing verification code or document subjects.

OIDC does not authenticate the document authors. SSHSIG authenticates the human owner and reviewer.
OIDC supplies a short-lived, independently signed workload identity and verification time for the
required check.

## Descriptor v2

Every v2 descriptor adds exactly four fields:

- `docs`: exact path `docs/modules/<id>.md`;
- `sources`: sorted unique repository-relative source patterns;
- `public_headers`: sorted unique repository-relative public-header patterns; and
- `surfaces`: required sorted unique arrays named `routes`, `commands`, `protocols`, and `stages`.

Mixed versions, unknown keys, partial coverage, changed v1 semantics, and missing documents fail.
Empty ownership arrays mean migration is incomplete, not that an implementation does not exist.

All four v2 surface arrays must be empty. No protected canonical surface inventory exists yet, so
the candidate cannot claim or classify surfaces. Any item fails with `v2-surfaces-deferred`.
Documents record known legacy registrations as migration gaps. A later generated-registration
slice establishes the protected inventory and advances the descriptor schema atomically.

## Immutable Git inputs and ownership patterns

Validation receives an explicit forty-hex candidate commit. It reads governed entries through
`git ls-tree -z` and `git cat-file`, never through the candidate working tree. Descriptors,
documents, claimed sources and headers, subjects, signatures, and index entries must be mode
`100644` Git blobs. Symlinks, executables, gitlinks, trees, missing paths, duplicate entries, and
other modes fail before parsing or hashing.

Governed JSON is strict UTF-8 without BOM. The parser rejects duplicate keys before schema checks,
unknown keys, floats, non-finite values, lone surrogates, and invalid encodings. SHA-256 always
covers exact blob bytes after mode validation.

A source pattern is printable ASCII and has exactly one form:

```text
src/modules/<id>/*.c
src/modules/<id>/*.h
src/modules/<id>/<literal>/*.c
src/modules/<id>/<literal>/*.h
```

`<literal>` matches `^[a-z][a-z0-9_-]*$`. No other glob metacharacter, recursion, range, negation,
escaping, Unicode, dot-entry, or platform behavior exists. Expansion enumerates direct Git-tree
children and sorts by unsigned ASCII bytes. Every pattern matches at least one `100644` blob;
overlap, duplicate claims, and cross-module claims fail.

`sources` contains `.c` implementation and private `.h` files physically beneath the module.
`public_headers` accepts only `.h` files beneath
`src/modules/<id>/include/aimee/<id>/`. That final public-header layout does not yet exist, so all
v2 public-header arrays are empty. Legacy headers are evidence, not claimed ownership.

## Deterministic document language

Module documents use printable ASCII plus LF only. CR, tab, control bytes, BOM, Unicode, HTML,
links, images, tables, Setext headings, inline markup, code fences, and indented code are forbidden.
The grammar accepts only one exact H1, the exact H2 lines below, blank lines, ordinary prose lines,
`- ` prose list items, and the metadata/evidence productions defined here. Slice 7a commits the
byte-level grammar and positive/negative fixtures; the protected parser is its single authority.

The H1 is `# <id> module`. These H2s occur exactly once in this order:

1. `## Purpose and non-goals`
2. `## Classification and lifecycle`
3. `## Public contracts`
4. `## Dependencies and consumers`
5. `## Providers and readiness`
6. `## Configuration and activation`
7. `## Routes, commands, protocols, and stages`
8. `## Data and migrations`
9. `## Security and privacy`
10. `## Supported journeys`
11. `## Tests and failure behavior`
12. `## Operational diagnostics`
13. `## Compatibility aliases`
14. `## Extension and removal rules`

Every section has this exact record order: its H2 line; one `State:` line; the three none-state lines
when applicable; the projection block when applicable; one or more prose records; and one or more
evidence records. A single blank line may separate records; leading, trailing, or consecutive blank
lines fail. No record may occur twice unless its production explicitly says one-or-more.

`State: present` or `State: none` occurs exactly once on the line immediately after the H2, with no
blank line between them. This describes the section topic and is not inferred from a descriptor
field. `State: none` is followed on consecutive lines by exactly one `Reason: <prose>`,
`Implication: <prose>`, and `Evidence: <reference>` record in that order. `<prose>` uses the ordinary
prose production below. Those three labels are forbidden for `State: present`.

Descriptor projections are separate and exact. They occur once, immediately after the state block
and before prose, only in the named sections:

- Purpose contains `Sources: none` or all source patterns joined by `, `.
- Public contracts contains `Public headers: none` or all header patterns joined by `, `.
- The surfaces section contains `Routes: none`, `Commands: none`, `Protocols: none`, and
  `Stages: none` in that order.

An ordinary prose record matches
`^[A-Za-z0-9][A-Za-z0-9 ,.;:()'/_-]*[A-Za-z0-9.)]$`. A prose list record is `- ` followed by the same
production. An evidence record is `Evidence: <reference>` for the none-state block or
`- Evidence: <reference>` in the section body. A reference is exactly `path:<repo-path>`,
`path:<repo-path>#L<positive-decimal>`, or `module:<canonical-id>`. Paths resolve to `100644`
candidate blobs and lines cannot exceed the LF-delimited line count. This slice validates paths and
lines, not inferred symbols. Slice 7a's committed grammar is byte-equal to these productions; its
fixtures cover every production, ordering boundary, cardinality, and malformed prefix.

The parser excludes headings, blank lines, metadata, evidence, and list markers from prose counts.
It counts non-overlapping tokens matching `[A-Za-z0-9]+(?:['-][A-Za-z0-9]+)*`. Each H2 requires 25
prose tokens and the document 600. It rejects the case-insensitive whole words `TODO`, `TBD`, and
`placeholder`, plus the exact phrase `coming soon`. Diagnostics include stable rule ID, module,
candidate SHA, path, line, expected, and actual.

Mechanical length is only a floor. A technical writer and roundtable must approve actual clarity,
specificity, evidence, migration gaps, and the truth of each state claim.

## SSHSIG human attestation

For each module, `docs/modules/attestations/<id>.subject.json` is the exact fixed-key canonical JSON
serialization plus one LF of:

```json
{
  "descriptor_sha256": "<64 lowercase hex>",
  "document_sha256": "<64 lowercase hex>",
  "module_id": "<id>",
  "owner_identity": "<trust-policy identity>",
  "reviewer_identity": "<different trust-policy identity>",
  "schema": "aimee.module-doc-attestation.v1",
  "signed_at": "<YYYY-MM-DDTHH:MM:SSZ>"
}
```

The serializer accepts ASCII values matching field-specific regexes and emits exactly the shown
order, indentation, separators, escaping rules, and LF. Golden vectors independently verify this
project-specific deterministic serialization. It is not described as RFC 8785/JCS because its
required indentation and trailing LF are outside the JCS representation. Descriptor and document
hashes cover exact mode-checked candidate blobs.

`<id>.owner.sig` and `<id>.reviewer.sig` are standard ASCII-armored SSHSIG files over the same
subject bytes, namespace `aimee.module-doc.v1`, Ed25519 keys, and SHA-512. Slice 7a pins the OpenSSH
toolchain image by immutable digest and the `ssh-keygen` binary by SHA-256. The verifier rejects any
other armor, namespace, hash, key type, leading/trailing bytes, CR, or missing final LF.

The protected-base trust policy is strict canonical JSON with schema version and monotonically
increasing epoch. Each identity has exact identity, role (`owner` or `reviewer`), Ed25519 public key,
`not_before`, `not_after`, and optional `revoked_at`. Duplicate identities or keys fail. Owner and
reviewer must be distinct and authorized for their roles.

`signed_at` matches `^[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}Z$`, denotes a real UTC
Gregorian time, and forbids leap-second `60`. It is signed audit metadata, not the authorization
clock. The authoritative CI OIDC profile maps a verified standard `iat`; `signed_at` must be within
the preceding 24 hours and not after that `iat`. Both SSH identities must be enrolled, in role and
valid, and not revoked at OIDC `iat`. Backdating cannot revive a removed, expired, or revoked key.

The sorted `attestations/index.json` has set equality with all twenty-six module IDs and subject
files. It records `{module_id, subject_sha256}` only. Missing, extra, duplicate, stale, or misordered
subjects, signatures, or entries fail. The index grants no authorization.

The human helper uses no shell and never reads raw private-key bytes. It accepts only public keys
whose matching private keys are available through a user-owned SSH agent or hardware token, invokes
the pinned `ssh-keygen` with a sanitized environment, and builds all subjects and fifty-two
signatures in an outside mode-`0700` temporary directory. It independently verifies every result
before atomically replacing the whole attestation directory. Cancellation, unavailable keys,
identity or role mismatch, partial signing, malformed prior state, or self-test failure leaves the
repository and Git history unchanged. Renewal uses the same full replacement.

## Provider-neutral CI OIDC verification

Slice 7a defines a strict issuer-profile schema. It contains exact issuer and audience, JWKS trust
or pinned discovery metadata, allowed JWS algorithms, maximum token lifetime and clock skew,
required standard claims, optional namespaced claim mappings, and predicates over normalized
workload fields. Duplicate JSON keys, unknown fields, redirecting discovery, unpinned trust, stale
JWKS, algorithm substitution, issuer or audience mismatch, and missing claims fail closed.

The profile-driven verifier creates two typed records. `workload_identity` contains issuer, subject,
audience, issued-at, not-before, expiry, token ID, repository identity, workflow identity, workflow
revision, run identity, attempt, trigger-check identity, event type, actor identity, and runner class;
each field is derived from a verified standard or profile-mapped token claim. `candidate_target`
contains the API-resolved protected-base revision, candidate revision, base and head refs, and pull-
request identity. The external verifier obtains every field through its read-only repository
installation, keyed by the token-bound repository, run, and trigger-check identity plus the pull-
request number in the request. It ignores request-supplied SHAs and refs. The profile declares the
HTTPS endpoint and strict response schema; redirects, write operations, and unmapped fields fail. A
profile cannot omit a field required by either record.

The trigger request carries only pull-request number and the OIDC token; the token itself carries
the profile-mapped trigger-check identity, so the request does not duplicate that identifier.
Candidate processes never run in the protected-base trigger job. The external verifier validates
`workload_identity` first, uses its installation credential to fetch `candidate_target`, then
requires repository, workflow, run, attempt, and trigger-check identities from the token to match
the API-resolved pull request. The API-resolved base and candidate revisions must match the trigger
check's recorded Git object. It requires
`nbf <= wall_time <= exp`, `abs(wall_time - iat) <= 60 seconds`, and
`0 < exp - iat <= 10 minutes`. Reuse is replay-safe rather than falsely one-time: a token can produce
only the same deterministic decision for its exact repository, run, trigger check, pull request,
base, and candidate tuple; any changed tuple fails. The publisher posts the result as a new check on
the resolved candidate SHA. Missing issuer/JWKS/provider API access, mapping ambiguity, clock
failure, or mismatch fails closed.

The current deployment may instantiate this schema for GitHub Actions, including its namespaced custom
claims. That profile is configuration stored on the protected base. It is not the OIDC contract,
does not affect product OIDC, and can be replaced by another CI profile without changing verifier
code or SSH subjects.

## Protected workflow and merge binding

The trigger check is the protected-base workflow run that requests OIDC. The required check is the
separate `module-doc-attestation` result the external publisher posts on the candidate SHA. They are
linked by pull-request number and the token's trigger-check identity; the required check's
`external_id` records that trigger-check identity. They are never treated as the same check run.

The external service runs verifier, parser, OIDC profile, trust policy, grammar, fixtures, and pinned
tools from the already-deployed 7a release. It fetches the current protected base and candidate by
immutable object ID through its read-only repository installation. It reads candidate and base Git
objects directly; the trigger never transmits candidate content, file bytes, or diffs. Candidate
versions cannot alter the service or authorize the candidate containing them. Trust-policy or
verifier changes land, deploy, and become the configured service release before later candidates
use them.

The repository's protected-base `pull_request_target` trigger requests an OIDC token and sends only
that token plus pull-request number to the service. It has no repository secret, never checks out or
executes candidate content, and is accepted only when its workflow identity and revision match the
configured CI issuer profile. The external service owns a repository-scoped publisher credential
stored in its vault; that credential can read pull-request and Git objects and create only its named
check. It cannot merge, push, administer the repository, change rules, or issue another app's check.

When the CI issuer profile is configured for GitHub Actions, its claim mappings and predicates bind
`workflow_ref` and `workflow_sha` to the protected-base trigger workflow and require
`job_workflow_ref` and `job_workflow_sha` to be absent unless the profile explicitly names a reusable
called workflow. If one is configured, caller and called fields are separately pinned according to
their documented meanings. The profile also binds repository IDs, run, attempt, trigger-check ID,
actor, event, runner environment, and token ID. No profile treats caller and called claims
interchangeably.

Classic branch protection pins the required `module-doc-attestation` context to the installed
publisher app, requires the PR head to include the current base, dismisses results on head or base
change, blocks direct and force pushes, and allows only ordinary merge commits. It has no bypass
actor for this check. A candidate workflow cannot impersonate the publisher app even if it uses the
same display name. Merge queue batching is disabled.

The external publisher posts its check only on the independently resolved candidate SHA, so all
required checks refer to the exact PR head. Because that head already contains the current base, the
ordinary merge has the protected base as first parent, the checked head as second parent, and a root
tree equal to the PR head tree. A protected post-merge audit verifies those IDs and tree equality.
Squash, rebase, batch merge, stale head, conflict-resolution tree changes, direct push, and bypass
are policy violations.

## Delivery and human boundary

Slice 7a can be implemented, tested, reviewed, and merged autonomously under existing branch rules.
Installing the repository-scoped publisher app, pinning its required-check context, and enrolling
the real human SSH identities require repository administrators and signers; agents cannot invent
them. No organization `admin:org` grant is required. Slice 7b can then be implemented and brought to
a draft PR autonomously. Its final green check requires the two genuine human signatures per module.

No bot, provider account label, CODEOWNERS approval, roundtable output, CI OIDC identity, or agent-
generated SSH key substitutes for the human owner and reviewer SSHSIG signatures.
