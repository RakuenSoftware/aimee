# Literal fact-history identity — 2026-09-23

Fact history now uses a literal prefix comparison for retained version keys.
Previously, a canonical key containing SQL wildcard characters could match
another key's predecessors through LIKE. Exact key matching and the `#v` version
prefix remain unchanged for ordinary keys; '%' and '_' now retain their literal
meaning. Retained-history eligibility and explicit scope still apply before the
result limit.

The [public PostgreSQL race regression](memory-literal-history-2026-09-23/public-race.txt)
passes in 3.546 seconds, including current and superseded versions of a literal
wildcard key and exclusion of an unrelated predecessor. The fresh deployment
harness adds the equivalent authenticated HTTP check. The
[export build](memory-literal-history-2026-09-23/export.txt) passes in 5.079 seconds;
ownership and module-boundary checks pass. Combined fresh validation remains
pending; no broader timeline or proposal completion is claimed.
