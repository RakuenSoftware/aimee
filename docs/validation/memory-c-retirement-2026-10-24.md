# Retired C memory declarations and orphaned fixtures

Source boundary: Aimee testing `35418e47e13a1d690e402a093d72002c0e1ad7b3`.
This change removes no active memory owner or host transport. The current Go
owner remains until its commands have replacement coverage in private aims-db /
aimee-memory. Existing native C memory implementations were already retired.

## Removed declarations

`src/memory_context_internal.h` only declared an unused candidate structure and
helpers from deleted memory translation units. The `memory_health.h` and
`memory_promotion.h` KB headers likewise had no implementation or consumer. The
`memory_lifecycle.h` header declared an unimplemented function used only by the
removed orphaned fixture. Removed private-header manifest entries and deterministic
C-boundary checks prevent restoring these stale files. This is removal of a false
API surface, not removal of working datastore behavior.

## Removed orphaned fixtures

All four files below were explicitly recorded in `UNBUILT_SOURCES` before this
change; none had a target in `src/tests/Rules.mk` or an active runner. Their direct
calls depended on previously retired native memory entry points. Removing them
does not increase or decrease the number of executed tests. The following are
current behavior owners and relevant running regressions, not a claim that each
historical assertion was ported one for one:

| Retired fixture | Current behavior and regressions |
| --- | --- |
| `test_memory_advanced.c` | Go memory runtime/admission, lifecycle, temporal eligibility, lineage and mutations: `runtime_role_test.go`, `validity_test.go`, `derived_eligibility_test.go`, `lifecycle_version_test.go`, `mutation_retry_test.go`. Its old native SQL/SQLite-shim tests cannot exercise the current owner. |
| `test_workspace_memory.c` | Go owner scope selection and rendering: `scope_owner_test.go`, `scope_pool_test.go`, `data_test.go`, `context_assembly_test.go`, `selection_policy_test.go`. |
| `test_memory_embed_http_auth.c` | Governed Go embedding and egress: `embed_egress_test.go`, `embed_test.go`; retired direct native embedding HTTP functions have no production implementation. |
| `test_memory_recall_pivot.c` | The experimental native topic-pivot function has no implementation or production caller. Its heuristic is retired, not advertised as a replacement feature. |

The standalone aims-db project separately qualifies these datastore mechanisms
and the private memory frontend through real shared/personal wire tests. This
C cleanup does not certify replacement of all Go commands.

`test_context_assembly.c` and `test_memory_retrieval_eval.c` remain explicitly
unbuilt debt: they mix obsolete memory calls with still relevant agent-context or
benchmark regression ideas. Removing those entire files would discard unrelated
specifications. They need splitting/porting before retirement.

## Setup after external-backend retirement

Setup offers only the still-supported native backend. A saved unsupported engine
remains visibly selected and blocks readiness even when old credentials exist.
An administrator must explicitly select native after exporting/migrating old data;
setup never silently rewrites the saved backend. Whole-document configuration,
admin-only writes, restart requirement and deployment-environment precedence remain.
Retired endpoint/credential setup and its duplicate browser URL validator are removed.
The generic Vault still owns retained credentials; this change does not delete them.

## Validation for this slice

- C setup fixture built with the shipping Make rule and `-Werror`, then passed.
- Frontend TypeScript project check and production Vite single-file build passed.
- 25 frontend unit tests passed across memory setup, readiness and wizard tests.
- The actual built HTML passed Chromium browser regressions: four clone-recovery
  scenarios, native setup, three unsupported-saved-selection scenarios and first
  boot. API responses are controlled fixtures; this checks the shipped browser
  behavior, not a deployed backend migration.
- 16 C-boundary checker tests and seven test-runner inventory tests passed.
- Live-tree C-boundary, test-runner inventory, module-source ownership and temp-path
  checks passed. The four removed fixtures were already unbuilt; the active
  runner count remains 676.
