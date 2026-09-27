# Inherited memory context ceilings and strict limits

Application and matrix harness: `f2340b1d4b`. Application image:
`sha256:3bca781b5fabe49d5d49662ddea133bc2b9fb3b338e58a724fb499dbdfc9e9ff`.

The Go memory owner previously replaced the inherited host assembly allocation
with a caller's explicit byte limit. It now uses the lower of the two, including
literal zero. Absent limits inherit the host allocation. The actual Server
producer supplies that allocation from `config_ingress_preinject_assembly_budget`;
both the Go ingress planner and assembler enforce it.

The versioned limits decoder previously allowed JSON duplicate fields, null
values and case-insensitive aliases to replace or erase a limit. It now accepts
each exact field name once, rejects nulls and invalid integers, and validates
UTF-8. Ingress and typed-context command boundaries also reject an explicit null
limit object. Unsupported token limits remain explicit refusals.

This changes only Go memory policy. The C event bus and memory module ownership
boundary remain unchanged. Final provider-request token counting, inherited
operator/task limits for the complete request, protected packing and durable
release/dispatch receipts are still required by MR-03 and the other proposals.

## Regression coverage

- Go race tests pass for memory, economizer, the Go bus client and bundled module
  launcher, with the memory evaluation PostgreSQL URL supplied and required.
- Tests cross real memory command encoding/decoding in Server and KB placements.
  They exercise absent, equal, raised, lowered and literal-zero byte limits;
  rejected limits cannot trigger retrieval. An oversized optional fact cannot
  displace the retained small memory by raising the host ceiling.
- Malformed-limit tests cover duplicate schema and budget fields, escaped duplicate
  names, case aliases, null objects/fields, fractional/string/overflow integers
  and invalid UTF-8. Unsupported token modes retain their existing error contract.
- All 77 local lint checks and memory ownership/C-boundary checks pass.
- The separate CI startup-policy repair passes all 46 descriptor regressions.
  Economizer starts by default for admission but does not support live toggling;
  optional reduction remains disabled unless configured.

## Fresh deployment evidence

Fresh isolated stacks in owned CT 9498 on `.253` passed **1,100/1,100** checks:
**690 T2** and **410 T3**, including 200 provider-boundary checks per placement.
All nine application, PostgreSQL and embedder image identities were verified.

- [T2 checks](memory-shared-reliability-2026-09-20/fresh-t2-f2340b1d4b.json)
- [T3 checks](memory-shared-reliability-2026-09-20/fresh-t3-f2340b1d4b.json)
- [Final provider accounting](memory-shared-reliability-2026-09-20/provider-accounting-f2340b1d4b.json)
- [Image identities](memory-shared-reliability-2026-09-20/image-identities-f2340b1d4b.json)

All nine owned test containers are stopped; volumes and evidence are retained.
Raw logs remain under `/opt/aimee-memory-proposals-evidence/` in CT 9498.
The retained stack names are `aimee-e2e-kb-da1b782b02`,
`aimee-e2e-server-a1f258757c` and `aimee-e2e-server-e849eab7bc`.
Sanitized receipts contain check names/results and byte/digest/image provenance,
without credentials or provider body capture.

The authenticated KB HTTP checks send raw JSON to preserve duplicate fields
through the native adapter. They verify refusal without rendered context for
null objects, null byte/token caps, duplicate/escaped duplicate caps and case
aliases, then prove that an ordinary request still returns the identical
projection and selection identity. These checks supplement the direct Go command
regressions; they do not establish all-channel release or token-budget acceptance.

No new latency claim is made for this correctness change. Prior admission latency
and its measured CPU tradeoff are recorded in
[admission latency validation](memory-admission-latency-2026-09-20.md).
None of MR-01–MR-18 is certified complete by this result.
