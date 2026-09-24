# MR-01: legacy graph and derived reads apply their audience

The public query_edges route omitted commandScope even though entity_edges
uses it for the same owner operation. A [restricted-role regression](memory-mr01-legacy-graph-2026-09-24/before-race.txt)
reproduces relations from both projects returned for an explicit app project.
The adapter now applies the shared scope parser before invoking the owner.

The [targeted race replay](memory-mr01-legacy-graph-2026-09-24/after-race.txt)
passes in 1.169 seconds. It checks project selection with and without the legacy
marker, selection of the other authorized project, and include_all=false limiting
the request to shared records. Existing unscoped limits and envelopes still pass.
Full race/export and HTTP verification are pending. Schema and C policy are unchanged.

The [HTTP reproduction](memory-mr01-legacy-graph-2026-09-24/http-before.json)
on `6b79ec2f2` passes the existing 40 common-fixture checks, then fails the legacy
project graph check. Its first disposable startup failed before application checks;
the retry completed startup and reproduced the defect. The graph-only correction
passed a [full race/export run](memory-mr01-legacy-graph-2026-09-24/graph-full-race-export.txt)
in 271.041 and 4.841 seconds before the adjacent adapter audit below.

The same omission affected get_provenance, link_query and list_conflicts.
[Public-command regressions](memory-mr01-legacy-graph-2026-09-24/derived-before-race.txt)
reproduce all three foreign-parent disclosures under the non-owner runtime role.
Each adapter now passes its supplied audience to the existing owner policy.
The [combined targeted replay](memory-mr01-legacy-graph-2026-09-24/derived-after-race.txt)
passes in 1.419 seconds. The [final full race suite and export](memory-mr01-legacy-graph-2026-09-24/full-race-export.txt)
pass in 212.842 and 4.965 seconds. Combined candidate process validation remains
pending.
