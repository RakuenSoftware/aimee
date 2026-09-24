# MR-01: common lifecycle fixture across independent retrieval lanes

The restricted non-owner runtime replay now reuses its exact current, future,
expired, suppressed, superseded, archived, retired, quarantined, deleted, revoked,
rejected, unknown-state and cross-scope population across lexical/list retrieval,
dense-only vector retrieval and graph-only PageRank candidate admission. It also
retains the two current timestamp-boundary records and existing historical tests.
Every vector is a perfect query match; every other fixture has a graph link to
the current record. Those ranking signals cannot admit excluded parents. All
three lanes return exactly the same three eligible record identities.

The [runtime race replay](memory-mr01-common-lanes-2026-09-24/runtime-race.txt)
passes in 232.255 seconds. Test setup uses the owner solely for fixture insertion;
retrieval runs under the existing restricted runtime role. No production code or
schema changes are part of this test extension.

A separate actual-process harness, `tests/e2e/memory-eligibility-lanes-e2e.py`,
uses one ten-state fixture for four lexical/list routes, evidence-backed ontology
traversal and current/historical exact-ID reads. Its [25 HTTP checks](memory-mr01-common-lanes-2026-09-24/http-checks.json)
pass against application `7eb4cf3b1`, with elapsed request times and
[actual container image identities](memory-mr01-common-lanes-2026-09-24/http-image-identities.json).
The runner exited zero and removed its disposable stack. The background relation
consumer is held at its existing advisory barrier and synthetic index jobs are
completed in the same transaction as fixture creation.

An initial harness attempt incorrectly addressed the host-only `memory.runtime`
interface over HTTP; its [404 diagnostic](memory-mr01-common-lanes-2026-09-24/harness-diagnostic-runtime-vector-search.json)
records the expected unavailable public method. The corrected harness uses the
advertised ontology route. It does not claim HTTP coverage for host-only vector
or PageRank operations; their isolated lane evidence is the runtime replay above.

This is MR-01/A6 evidence and additional A1/A2 coverage. It does not certify all
advertised endpoints, all derived inputs, historical reconstruction or the final
provider-release boundary; MR-01 remains active.

The [expanded runtime replay](memory-mr01-common-lanes-2026-09-24/expanded-surfaces-race.txt)
passes in 175.902 seconds. The canonical fixture additionally exercises
`top_l2_facts`, `load_eval_corpus`, `list_session_scope_priority`,
`list_session_scope_priority_like`, `search_facts_patterns_by_keyword` and
`diagnose_scoped` through their public command handlers. The derived fixture adds
superseded, archived, quarantined, deleted, revoked, rejected and cross-scope
states to its existing time/suppression cases. All 13 serving checks (episodes,
cards, summaries, scenes/members, relations, profiles, assertions, CSS conventions,
watermarks and graph feedback) continue to exclude the ineligible parent. Setup
moves the parent using the owner role; each serving check restores and uses the
non-owner runtime role.
