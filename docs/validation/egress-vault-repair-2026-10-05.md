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

## Source CI and publication pass

The final repair branch `f2da8e224df289d2ea62b0c13ae0137fe336c982` passed all 56
jobs in [CI run 37354588562](https://github.com/RakuenSoftware/aimee/actions/runs/37354588562).
[PR #3007](https://github.com/RakuenSoftware/aimee/pull/3007) merged into testing
as `5f61f036e51c5b2219f4c603f87ee7231ece731a`.
[PR #3008](https://github.com/RakuenSoftware/aimee/pull/3008) supplied the release
README's security guarantees, trust boundaries and harness compatibility.
The [testing publisher](https://github.com/RakuenSoftware/aimee/actions/runs/37358322025)
completed successfully for that merge.

## Expanded Cognee checks and remaining qualification

An expanded local-overlay run completed 63 live checks. It adds endpoint failure
and recovery for catalog, add, cognify and search; malformed catalog, invalid
UUID, incomplete cognify and foreign-result refusal; live Vault bearer rotation;
canonical version correction; and failed derived deletion followed by retry.
Those observations remain local-overlay evidence, not published-image evidence.

[PR #3009](https://github.com/RakuenSoftware/aimee/pull/3009) adds regression tests
for protocol failures, cancellation, source/version validation, limits, filters,
namespace isolation and reset recovery. Local Go race tests pass and adapter
statement coverage rises from 77.9% to 96.3%. All 58 remote checks passed and the
PR merged into testing as `e0ef1c5e9a43658cb8d916dd22ae927ed348a9cb`.
Coverage is not a claim that every
upstream Cognee API or application entry point has been qualified.

| Path | Qualification |
| --- | --- |
| Vault handoff, process hardening and supervised helper recovery | 18 installed-helper checks pass on the local overlay; source CI also passes |
| Real private/shared search, namespace isolation and versioned deletion | 37 committed live checks pass on the local overlay |
| Provider endpoint failures, malformed replies, credential rotation and retry | Expanded local-overlay run passes 63 checks in total |
| Adapter protocol, cancellation, limits and canonical validation | Race tests pass; regression tests are in PR #3009 |
| API, CLI and MCP recall; native primitive source versions | Expanded driver prepared; remaining run incomplete |
| Operator-authorized subject erasure and restored-provider startup cleanup | Expanded driver prepared; remaining run incomplete |
| Exact published testing-image qualification | Publisher passed; CT execution remains incomplete |

The later expanded run failed during Server Vault bootstrap before the remaining
checks. The production Compose wrapper intentionally suppresses output that may
contain credentials. The validation driver now invokes the same bootstrap module
and retains failed subprocess output in a mode-0600 file beneath its mode-0700
private fixture directory. Public exceptions retain only a fixed failure message.
The driver change preserves the repository working directory and bootstrap input.
Synthetic failure and success checks verify private file permissions, secret-free
exceptions and unchanged handling of ordinary commands.

After access was restored, private diagnostics identified exhausted Docker address
pools caused by retained synthetic test networks. Removing only owned synthetic
containers and unused networks resolved bootstrap. Evidence and volumes were retained.
The published image is pinned to
`ghcr.io/rakuensoftware/aimee@sha256:47e888daf7f2f01f985b6ed15e94d7faea29e4726e0d52db9000ec2fefa7039b`
and reports `testing-e0ef1c5`.

All [18 published helper verdicts](egress-vault-repair-2026-10-05/published-handoff.json),
[207 native T1 verdicts](egress-vault-repair-2026-10-05/published-native-T1.json) and
[699 native T3 verdicts](egress-vault-repair-2026-10-05/published-native-T3.json) pass.
The expanded Cognee qualification is still running. Its fixture now uses explicit
scope and token budgets, observes the default shared activation cooldown before
setting a repeatable policy on one synthetic record, rejects empty public search,
and uses the existing private temporal-test authority context. Expiry withholds
records; it does not certify physical erasure. Scoped reconciliation is tested
with an eligible companion in that scope. No main merge or complete subject-erasure
guarantee follows from the incomplete live erasure run.

The promotion module-inventory failure was a stale baseline for the two reviewed
host dispatch changes. [PR #3010](https://github.com/RakuenSoftware/aimee/pull/3010)
refreshes those hashes. The local baseline check, eight baseline regression tests
and module-source-ownership check pass. Two other failed-looking promotion runs
were cancelled jobs, not failed source checks.
