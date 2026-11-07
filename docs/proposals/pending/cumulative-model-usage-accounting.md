# Proposal: cumulative model usage and reporting completeness

- **State:** PENDING; removed from the IR/browser migration as separate work.

The migration branch added cumulative retry accounting and aggregate
unknown-usage/completeness reporting in storage, API results and the UI. These
change accounting behavior, rather than merely translating provider messages.

Review how known usage from failed attempts combines with a final response,
which calls count as unknown, and how uncertainty affects cost totals. Define
one accounting owner and backwards-compatible reporting before changing SQL,
DTOs or dashboards. Canonical IR must continue to preserve absent versus zero
usage regardless of whether this reporting feature is accepted.

The implementation is preserved at pre-removal commit
`9d257322da116facd5c9590d531dbd781d633916` and is preserved on branch
`proposal/deferred-execution-and-accounting`. Acceptance needs dedicated accounting tests and an explicit
schema/rollout plan; it is not a prerequisite for the IR migration.
