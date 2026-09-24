# MR-01: effective legacy scope arguments

Generic KB commands previously ignored project and workspace arguments unless
scope_context was also set. The [regression](memory-mr01-legacy-scope-2026-09-24/before-race.txt)
reproduces the unintended cross-audience retrieval. Command parsing now honors
nonempty legacy audiences, honors an explicit include_all=false, and rejects
malformed scope values rather than silently widening the query. Recall bundles
use the same parser. Verified credential restrictions still apply.

The [full memory race suite and export build](memory-mr01-legacy-scope-2026-09-24/full-race-export.txt)
pass in 223.365 and 8.125 seconds respectively. Actual HTTP reproduction on the
prior candidate fails the project-without-marker check; [candidate HTTP validation](memory-mr01-legacy-scope-2026-09-24/http-after/checks.json)
passes 40/40 checks on `763aa8c7d`. This includes five additional query routes,
compatibility search/windows, diagnostic retrieval and recall over the same fixture.
The runner exited zero and removed its disposable stack. An initial expanded
harness read active_context at the envelope root instead of inside recall; that
harness assertion was corrected before the successful run. No schema or native policy changes are introduced.
