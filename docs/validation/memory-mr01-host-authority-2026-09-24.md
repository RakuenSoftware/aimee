# MR-01: host-bound scope and diagnostic authority

The [actual HTTP replay](memory-mr01-host-authority-2026-09-24/checks.json)
passes 20/20 checks and 128 concurrent requests on `377c27b67`, using separate
project- and workspace-scoped credentials. Request include_all=true cannot widen
either verified audience. Implicit reads retain allowed global rows; explicit
exact-scope reads exclude them. Foreign exact-ID reads remain refused.

Forged authenticated/user_authority/principal/service-scope fields in request
text cannot replace verifier-owned command context. The transport refuses the
forged diagnostic request with HTTP 403 and no decision payload. An ordinary
actorless credential also cannot acquire user diagnostic authority. The harness
initially expected an owner error envelope for the forged request; recorded
response metadata identified the earlier transport refusal, and the assertion
now checks that actual boundary. The runner exits zero and removes both disposable stacks.

The implementation separates argument JSON from kb_command_context in the native
transport and binds the verified restriction in the Go data owner before its
transaction. A named verified service identity retains its existing deployment
access; project/workspace identities cannot promote themselves to that identity.
These checks attest that restriction and do not certify the complete purpose,
policy/revocation-generation context or final provider-release contract.
