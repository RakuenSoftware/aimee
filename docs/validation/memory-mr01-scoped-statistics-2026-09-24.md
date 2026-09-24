# MR-01: statistics retain their requested audience

The [restricted-role regression](memory-mr01-scoped-statistics-2026-09-24/before-race.txt)
reproduces project-scoped statistics counting a foreign record and conflict.
The statistics/dashboard adapters omitted the supplied audience, and the console's
secondary effectiveness query explicitly requested all scopes. Dashboard conflict
aggregation also counted a visible endpoint whose other parent was hidden.

Both adapters now apply the common scope parser. Console effectiveness inherits
the primary request's audience. Dashboard conflict aggregation requires both
parents to be visible before counting either endpoint. These are operator
statistics: authorized archived and suppressed records remain included. No
current-only predicate or schema change is introduced.

The [targeted race replay](memory-mr01-scoped-statistics-2026-09-24/after-race.txt)
passes in 1.271 seconds, checking five authorized records including retained
states, 300 authorized conflicts, and consistent console/dashboard counts.
The [HTTP reproduction](memory-mr01-scoped-statistics-2026-09-24/http-before.json)
on `377c27b67` also fails the project statistics check after the preceding
retrieval checks pass. The [full race suite and export](memory-mr01-scoped-statistics-2026-09-24/full-race-export.txt)
pass in 214.122 and 6.027 seconds. Corrected candidate HTTP validation is pending.
