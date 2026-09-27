# MR-14 proposal-only hygiene — 2026-09-27

State: functional acceptance complete on application candidate `6a5dc7630`.
The [manifest](memory-mr14-evidence-2026-09-27/manifest.json) pins the application image,
separately shipped operator scheduler, and retained evidence.

The shared Go memory owner now provides bounded deterministic findings and
immutable, version-bound learning proposals. A separate NOLOGIN worker has no
canonical mutation or review privileges. Normal runs record idempotent page jobs
in the existing queue; dry runs create neither jobs nor proposals. Rejection and
expiry suppress equivalent proposals. An explicit operator scheduler tick resumes
bounded pages and exposes incomplete coverage. Private expired task projections
have separate principal/CAS/retention/workflow-lease guarded cleanup.

The [operator contract](../memory-hygiene.md) defines detector coverage and
limits. Model assistance makes no calls and remains disabled. Delivered-exposure
detection has no shared-store adapter. Scheduling is not enabled automatically;
synthetic acceptance does not establish reviewer usefulness or production
false-positive rates. Accepting a finding is not approval to merge, delete,
supersede or promote canonical memory; those actions retain their existing gates.

## Validation

- [Full memory race suite and exported module build](memory-mr14-evidence-2026-09-27/memory-race-export.txt): passed, 358.855 seconds for the race suite.
- [Runtime role and governed hygiene replay](memory-mr14-evidence-2026-09-27/runtime-role-replay.txt): passed after the administrator bootstrap correction, 347.254 seconds.
- [Isolated full DB1 owner race suite](memory-mr14-evidence-2026-09-27/owner-race.txt): passed, 2.440 seconds. An earlier invocation incorrectly used the shared fixture database and failed because the unrelated `agent_jobs` fixture could not drop a referenced table; the isolated rerun is the full-suite result.
- [Cluster-limit resume regression](memory-mr14-evidence-2026-09-27/cluster-regression.txt): a later page cannot conceal an earlier truncated cluster; the terminal result retains partial coverage.
- [Scheduler tests](memory-mr14-evidence-2026-09-27/scheduler-tests.txt): bounded resume, scope binding, private atomic state, preserved cursor on errors, and stalled partial results.
- Native CLI, memory HTTP and task transport tests passed. All 77 lint checks ran; the missing SQLite metadata mirror was repaired and schema checks passed. The generated-document check passed after committing its inputs.
- [PostgreSQL upgrade tests](memory-mr14-evidence-2026-09-27/platform-upgrade-summary.json): both legacy database names preserve data across role separation and repeated startup. Worker membership is non-inherited, and the migrator remains without CREATEROLE.

The [final live run](memory-mr14-evidence-2026-09-27/live-checks.json) passed all 22
checks through the actual owners, HTTP, compiled CLI and MCP. It includes normal
proposal admission/retry, rejected/expired/stale review, bounded resume, worker
privileges, scheduler execution, and private retention/lease guarded cleanup.
The [harness](memory-mr14-evidence-2026-09-27/live-acceptance.py) removed its exact
synthetic canonical/session fixtures; review/job metadata remains as test history.

The first live attempt stopped at an incorrect harness assumption that CLI JSON
retains the HTTP success `status`. Native CLI intentionally removes that field.
The standalone scheduler now accepts its established success shape while requiring
exit success, the typed owner receipt, a job ID, and zero canonical writes. Three
scheduler regressions and the subsequent complete native run pass. This companion
script is shipped from the checkout, not embedded in the application image; its
exact validated digest is separately pinned in the manifest.

## Upgrade and rollback evidence

The initial `d8e061e59` candidate failed its KB startup gate because the restricted
migrator could not create a PostgreSQL role. The [failed attempt](memory-mr14-evidence-2026-09-27/upgrade-first-failed.txt)
restored both exact owned database snapshots and recreated the previous candidate.
Both owners were then independently observed healthy on `9c1242b1d`.

The correction provisions the role through the administrator-owned PostgreSQL
bootstrap and verifies it during restricted schema application. The candidate
upgrade helper uses the same checked-in SQL prerequisite when retaining the
existing PostgreSQL image. It records the prerequisite digest, snapshots both
quiesced stores, and preserves workspace/enrollment/volume identities. Private
dumps stay mode 0600 on CT109; evidence contains only their hashes.

The [production health record](memory-mr14-evidence-2026-09-27/production-health.txt)
confirms CT100 remains healthy on the released 0.4.5 bridge and PostgreSQL/embedder
images. Optional MR-07/MR-09/MR-10 policies remain unpromoted.
