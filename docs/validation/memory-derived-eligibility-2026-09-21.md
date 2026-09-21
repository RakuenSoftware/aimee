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
  retired/restored parents. Fresh execution and image evidence are pending.

This closes confirmed current-serving gaps. It does not certify historical
reconstruction, final release/revocation checks or all MR-01–18 acceptance.
