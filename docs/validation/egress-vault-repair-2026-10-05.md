# Published egress and Cognee qualification

The published `testing-e0ef1c5` image passes **119 real Cognee checks**, **18 installed
Vault helper checks**, **207 native T1 checks** and **699 native T3 checks** in owned
CT9210 on `192.168.1.253`. The Cognee run includes operator-authorized subject
erasure, provider-outage refusal, completion retry and restart cleanup of restored
erased data. This supersedes the Cognee startup failure in the
[original validation](pr-3003-testing-2026-10-05.md).

## Published image and evidence

The application is pinned to
`ghcr.io/rakuensoftware/aimee@sha256:47e888daf7f2f01f985b6ed15e94d7faea29e4726e0d52db9000ec2fefa7039b`
and its installed CLI reports `aimee vtesting-e0ef1c5`. Its source is
`e0ef1c5e9a43658cb8d916dd22ae927ed348a9cb`, the testing merge of
[PR #3009](https://github.com/RakuenSoftware/aimee/pull/3009).
The [publisher](https://github.com/RakuenSoftware/aimee/actions/runs/37362783973)
completed successfully.

The other images remain pinned to PostgreSQL
`sha256:182070945b59ad942a8fc753b3c8066db45cc446a07576d413df2fb2dfd61e48`
and embedder `sha256:b03199bee881bf632f7194f472de7bc370e66d16b7215bb6aa506fb2b1510209`.

| Gate | Published verdicts |
| --- | --- |
| Real Cognee 1.6.2 | [119 of 119 pass](egress-vault-repair-2026-10-05/published-cognee.json) |
| Installed Vault handoff | [18 of 18 pass](egress-vault-repair-2026-10-05/published-handoff.json) |
| Native shared KB, T1 | [207 of 207 pass](egress-vault-repair-2026-10-05/published-native-T1.json) |
| Native standalone Server, T3 | [699 of 699 pass](egress-vault-repair-2026-10-05/published-native-T3.json) |

Cognee used real SQLite, LanceDB and graph state. It made
[140 embedding and 39 completion calls](egress-vault-repair-2026-10-05/published-model-calls.json)
to deterministic local fixtures. These CPU checks qualify wiring and lifecycle;
they do not measure model quality or native GPU plugin performance. Public artifacts
contain named verdicts and model-call counts. Credentials, environment snapshots,
provider output and application logs remain in the private fixture directory.

## Aimee's Cognee paths are covered

| Path | Validated behavior |
| --- | --- |
| Get and Put | Canonical reads and writes remain in Aimee; provider authentication and capacity failures preserve exact canonical reads |
| Search | Private/shared retrieval, immutable node namespaces, scope isolation, canonical version correction, HTTP/CLI/MCP recall and native primitive export |
| Provider protocol | Catalog, add, cognify, search and dataset-delete failures refuse retrieval or destructive completion; retry recovers through the same runtime |
| Reply validation | Malformed catalog, invalid dataset UUID, incomplete cognification and foreign retrieval results are refused |
| Credentials | Invalid Vault bearer rotation refuses retrieval; restoring the bearer works through the existing hardened credential pipe |
| Eligibility | Expired, retired, suppressed and future-valid records are withheld; restored records reindex through real Cognee |
| Capacity | 257 eligible canonical records fail before a provider request; exact reads remain available and retrieval recovers after fixture removal |
| Public input | Empty search keywords are rejected; canonical listing makes no Cognee request |
| Activation | Shared recall respects its native cooldown; repeated entry-point comparisons use an explicit policy on one synthetic record |
| Delete and Forget | Provider failure refuses a retirement receipt, preserves the active canonical record, and supports the same conditional retry without duplicate completion |
| ResetDerived | Restart removes restored retired data; managed erasure removes both node namespaces and verifies their absence through the provider API |
| Managed subject erasure | Outage cannot certify completion; the same request retries to completion, removes private history and shared canonical targets, preserves another principal, and resists restored erased data after restart |

The adapter regression tests in [PR #3009](https://github.com/RakuenSoftware/aimee/pull/3009)
cover cancellation, limits, filters, duplicate and foreign results, source/version
changes, protocol errors, lost deletion outcomes and reset verification. Local Go
race tests pass. Adapter statement coverage rises from 77.9% to 96.3%; all 58 remote
checks passed before that PR merged. This is coverage of the paths Aimee uses, not
an audit of every upstream Cognee API.

The privileged erasure fixture uses an explicit unscoped owner bearer and an
operator-controlled Server loopback bridge to the private KB network. Ordinary
retrieval and lifecycle checks first run through the existing scoped mTLS service
enrollment. No host or LAN ports are published. The erasure case has no registered
multi-owner fleet; it qualifies the two application stores and both Cognee
namespaces in this operator fixture. Provider namespace verification does not
establish filesystem-wide removal from external logs, snapshots or backups.

Expiry withholds records and preserves canonical history. It is distinct from
subject erasure. Scoped reconciliation is tested while an eligible companion keeps
that scope in the query. Temporal fixture updates use the same explicit authority
context as the existing private temporal tests; the storage guard remains enabled.

## The repaired startup retains process hardening

[PR #3007](https://github.com/RakuenSoftware/aimee/pull/3007) establishes a
parent-attested private Vault pipe before the egress owner becomes non-dumpable.
Only a readiness marker crosses the pipe at startup. Requests read current Vault
state after bus admission and hardening. Both processes remain non-dumpable;
helper failure terminates the owner so supervision establishes fresh attestation.
Unbuffered output and explicit clearing avoid retaining a stdio credential copy.

The final repair branch `f2da8e224df289d2ea62b0c13ae0137fe336c982` passed all 56
jobs in [CI run 37354588562](https://github.com/RakuenSoftware/aimee/actions/runs/37354588562).
[PR #3008](https://github.com/RakuenSoftware/aimee/pull/3008) supplied the release
README's security guarantees, trust boundaries and harness compatibility.
The initial local overlay passed [18 helper checks](egress-vault-repair-2026-10-05/local-handoff.json)
and [37 Cognee checks](egress-vault-repair-2026-10-05/local-cognee.json).
The published-image results above replace those local candidates as the current
runtime qualification.

## Fixture repair and remaining promotion work

Private bootstrap diagnostics identified exhausted Docker address pools from
retained synthetic networks. Removing only owned synthetic containers and unused
networks resolved bootstrap; evidence and volumes were retained. Failed bootstrap
output is now saved with mode 0600 under the mode-0700 private fixture directory.
Public exceptions contain a fixed message. Synthetic checks verify private file
permissions, secret-free exceptions and preserved bootstrap input and working directory.

Three concurrent Cognee fixtures reached the original 10 GiB CT limit and timed
out. The final complete run uses one fixture in the owned CT with 16 GiB memory;
it passes all 119 checks. Those resource-limited runs are not qualification evidence.

The promotion module-inventory failure was a stale baseline for the two reviewed
host dispatch changes. [PR #3010](https://github.com/RakuenSoftware/aimee/pull/3010)
refreshes those hashes. The local baseline check, eight baseline regression tests
and module-source-ownership check pass. Its later CI T3 job received a runner
shutdown signal and exited 143; its rerun was queued after the parent workflow completed.
Two other failed-looking promotion runs contained cancelled jobs.

Main promotion still requires its protected checks and approval. Native plugin
0.3.2 has 14 reviewed signed assets staged for release, but release-upload
authentication remains unavailable. No main merge or native asset publication is
claimed by this validation record.
