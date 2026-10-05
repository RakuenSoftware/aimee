# Egress credential repair qualification

The failed `testing-ed1d778` Cognee startup in the
[original validation](pr-3003-testing-2026-10-05.md) has a repair in
[PR #3007](https://github.com/RakuenSoftware/aimee/pull/3007). The candidate establishes a
parent-attested private Vault pipe before the egress owner becomes non-dumpable.
Only a readiness marker crosses the pipe at startup. Requests read current Vault
state after bus admission and hardening. Both processes remain non-dumpable;
helper failure terminates the owner so supervision establishes fresh attestation.
Unbuffered helper output and explicit clearing avoid retaining a stdio credential
copy.

## The final local candidate passes

Source `1975526d38df7ff2e73df2bb7fa45e1b9e7a0f75` was built in owned CT9210 on
`192.168.1.253`. Its two native hosts and Go egress executable were overlaid on the
previous published application image. This is a local candidate, not a published
release qualification.

All [18 real helper checks](egress-vault-repair-2026-10-05/local-handoff.json)
pass for Server and KB: authenticated startup, absence of credentials from
container metadata, non-dumpability, refusal of an unattested caller, live Vault
rotation with the same egress owner, helper-death supervision and recovery.
The same gate is now required by the T3 Docker CI job.

All [37 real Cognee checks](egress-vault-repair-2026-10-05/local-cognee.json)
pass with Cognee 1.6.2 and synthetic SQLite, LanceDB and graph state. The checks
cover private/shared retrieval, scope isolation, immutable node namespaces,
provider-outage refusal of deletion, versioned retry and idempotency, canonical
ownership, and removal of restored derived data after supervised memory restart.
The provider made [21 embedding and nine completion calls](egress-vault-repair-2026-10-05/local-model-calls.json)
to local deterministic fixtures. These CPU checks establish wiring and lifecycle;
they do not measure model quality or native GPU plugin performance.

Personal deletion is retirement and leaves no active canonical record. Shared
user-authority deletion removes its canonical row. Neither is a claim of complete
subject erasure. The initial driver tried the owner-only managed subject-erasure
route through a scoped service enrollment; its authorization refusal was correct.
The corrected driver tests the public versioned deletion contract. The owner-only
managed subject-erasure workflow needs a separate operator-authorized fixture.

Go race tests for memory, Cognee, egress and the module launcher pass. Native Vault
bootstrap tests pass, including refusal of an unattested resource invocation.
Source ownership, module boundaries, generated documentation and formatting pass.

## Published-image validation is pending

The source CI, testing merge and publisher must finish before qualifying the
published image. The local verdicts above do not remove that release gate.
