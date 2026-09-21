# Derived memory parent eligibility

Some derived reads checked only active lifecycle, and episode cards and scenes
also omitted suppression. A current source could additionally admit an edge that
had another hidden or expired memory source. The Go owner now applies its shared
current-state predicate at these serving boundaries before result limits.

Affected reads include episodes, cards, relations, entity profiles/edges, direct
summaries, scene lists/members, assertion search, CSS conventions and typed
watermark metadata. Hybrid graph file selection and path feedback reject any
ineligible memory evidence. Existing live-evidence filtering and profile support
requirements remain intact. Mutation/review admission retains its separate policy.
The current eligibility policy is `current-validity-v7`.

The SQL uses the existing transaction clock and timestamp normalization. It adds
no module calls or database round trips. No whole-request P95 improvement is
claimed. The shared memory implementation and module-side transport remain Go;
the C bus is unchanged.

Local validation:

- The full Go/PostgreSQL memory suite passes (51.169 seconds), including the
  packaged runtime role. Its derived serving tests exercise inclusive starts, exclusive ends, offsets,
  subsecond differences, expiry, suppression, retirement and malformed governed
  times through the production adapters. Mixed valid/expired sources cannot
  authorize assertion text, profile counts, conventions or feedback.
- Public command regressions cover hidden mixed-source profile counts. Hybrid
  retrieval has more high-weight expired/mixed-source distractors than its cap.
- Minimal public test schemas now include real validity/suppression columns and
  use complete timestamps instead of year-only lexical sentinels.
- Native build and memory routing, standalone Aimee export/build, all 77 lint
  checks and all 17 S1 contract checks pass. Targeted runtime-role and public command race checks pass.
- Fresh MCP cases traverse the C host/bus and Go owner for episode lookup,
  graph search, profiles and edges across current/future/expired/suppressed/
  retired/restored parents. All 24 cases pass in the fresh run below.

This closes confirmed current-serving gaps. It does not certify historical
reconstruction, final release/revocation checks or all MR-01–18 acceptance.

Fresh application/harness `1989160bdfb697ffb891f4fd23eee38994d1c120` passes
**1,553/1,553 checks**: **961 enrolled T2**, **592 standalone T3**. Shared memory
passes 297 checks, including the 24 derived-parent cases. Both placements retain
214 private-memory, 298 provider-boundary and 37 native asynchronous checks.

- [T2 verdicts and provider/native receipts](memory-derived-eligibility-2026-09-21/fresh-t2-1989160bdf.json)
- [T3 verdicts and provider/native receipts](memory-derived-eligibility-2026-09-21/fresh-t3-1989160bdf.json)
- [Exact images and cleanup](memory-derived-eligibility-2026-09-21/image-identities-1989160bdf.json)

Application image: `sha256:5d99f0520b84d5b25da1ffe63416c23a63ae6b133201e6538d5d445f574c2e10`.
All nine container identities and all three application instances' actual 32 KiB
provider caps were verified. The nine owned containers and nine empty networks
were removed after collection; images, volumes and raw receipts remain.
Subsequent source-version binding work is not covered by these images.
