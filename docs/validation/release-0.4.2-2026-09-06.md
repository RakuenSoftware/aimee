# 0.4.2 published candidate validation — historical NO GO

This records the original published image findings. See the [repair verification](release-0.4.2-repairs-2026-09-06.md) for the corrected candidate and regression coverage.

The published `testing-6083b85` candidate is **not ready for 0.4.2**. Fresh deployments
and substantial regression coverage pass, but normal memory commands address incompatible ID
namespaces, and upgrading from 0.4.1 makes existing memories inaccessible through the tested APIs.
The candidate's required CI aggregate also failed.

Validation ran on 2026-09-06 using a new Debian 13 guest, CT **9422**, on
`root@192.168.1.253`. The guest had 8 CPUs, 16 GiB RAM and an 80 GiB disk. Tests used
independent Compose projects/volumes and disposable PostgreSQL databases. The existing CT 103
appliance was observed running 0.4.1; it was not used as a test fixture.

The candidate is commit `6083b85e4664f06abd5275ee5e893388734a6181`. All 6,669 source files
were checked against its Git archive before accepting the clean candidate results. The testing
branch still pointed to this commit at the final check. The
[publication run](https://github.com/RakuenSoftware/aimee/actions/runs/34050017504) completed
successfully. Images were pulled from GHCR, without rebuilding or replacing their binaries.

| Artifact | Registry digest / artifact SHA-256 |
| --- | --- |
| `aimee-server:testing-6083b85` | `sha256:3f7854a12249f9d9249cd5873ace9b5cad727771662f20b7f4df96e372728076` |
| `aimee-kb-a25m:testing-6083b85` | `sha256:12fe349e6a578b8c598dc1d7bd8f856edac2a134911d94dcf41fde6105693fd2` |
| Published Linux x86-64 client | `6b23f3c717ee0d449a7007d8181fc0eea9d9d659ada34933e278fae2b22fe910` |

Full image IDs, the control-web/PostgreSQL images, and the original 0.4.1 image digests are in
[deployed-images.json](release-0.4.2-2026-09-06/deployed-images.json).

## Release blockers

### R042-1: a stored memory cannot reliably be read by its returned ID

The **published client**, connected to the published server's Unix socket, reproduced:

```text
aimee --json memory store release042-cli-canary release042-cli-value
=> {"id":19}

aimee --json memory list
=> includes id 19 and release042-cli-value

aimee --json memory get 19
=> memory not found, HTTP 404, client exit 1
```

An ID collision is worse than a missing result. `memory list` identified user memory **1** as
`release042-first`, containing `release042-probe`. `memory get 1` returned KB memory **1**,
`e2e-as-of`, containing `as-of smoke value`.

The source confirms incompatible routing in `src/server/server_state.c`: store/list/delete use
the local user-memory module; get and supersede use KB memory. The supersede probe returned
404. Direct SQL inspection confirmed that it changed no KB record; no unintended write is
claimed. See [the exact responses](release-0.4.2-2026-09-06/memory-routing-summary.json) and
[the SQL verification](release-0.4.2-2026-09-06/supersede-sql-verification.json).

Release requires consistent public memory addressing and regression coverage for a real
store → list → get → supersede → delete sequence, including colliding user/KB numeric IDs.

### R042-2: 0.4.1 memories become inaccessible after upgrading

A separate fresh stack ran the **published 0.4.1 server and KB**, with the 0.4.1 Compose files
and bootstrap scripts. It passed its 10-check split-stack smoke. Two canaries, one global and
one project-scoped, were stored and successfully read by both get and list before upgrading.

The stack was then recreated with the candidate images/current Compose files over the **same
volumes and credentials**. Both services became healthy, but:

| Check | Before upgrade | After upgrade |
| --- | --- | --- |
| Get global canary, ID 2 | Correct content | `memory not found` |
| Get project canary, ID 3 | Correct content | `memory not found` |
| List canaries | Both present | Empty successful list |
| PostgreSQL rows | Present | Both still present, active, correct contents and scopes |

The failing get responses carried HTTP 200 with an application-error body. Another KB/server
restart reproduced the failures with both containers healthy. This proves an access regression;
the inspected rows were **not physically lost**. The underlying upgrade defect still needs repair.

Evidence: [before](release-0.4.2-2026-09-06/upgrade-before.json),
[after](release-0.4.2-2026-09-06/upgrade-after.json),
[SQL read-back](release-0.4.2-2026-09-06/upgrade-sql.json), and
[repeat restart](release-0.4.2-2026-09-06/upgrade-repeat-restart.json).

### R042-3: required CI is red

The candidate's [CI run](https://github.com/RakuenSoftware/aimee/actions/runs/34050017571)
finished with failed `unit-tests-pg (2/3)` and `unit-tests` aggregate jobs. The underlying failure
was `unit-test-workspace`, `tests/test_workspace.c:614`, after Git reported that
`.git/worktrees/carry-applyback-child/locked` did not exist.

Both complete native runs passed locally. That does not clear the failed CI gate or establish
why its workspace fixture failed. The other CI jobs, including the sanitizer/static-analysis
jobs, completed successfully. The final job snapshot is retained in
[ci-status.json](release-0.4.2-2026-09-06/ci-status.json).

## Passing coverage

| Area | Result and scope |
| --- | --- |
| Clean shipping source build | `make -C src -j5 all` passed |
| Native regression registry | 654 executables with SQLite; 654 with real PostgreSQL; both complete runs passed |
| Go services | Complete `go-unit-tests`, including cross-language bus conformance, passed |
| Shared DB, memory, providers | 124 top-level tests passed with `-race`, with required shared-DB PostgreSQL configuration |
| Workflow database/API/engine | 221 top-level tests passed with `-race` and required live PostgreSQL configuration |
| DB2 process replay | Passed in a fresh database, including restricted-runtime memory replay and shared migration/workflow checks |
| Retrieval corpus | Passed separately against PostgreSQL in both KB and server placements |
| Frontend | 196 tests passed; runtime and console production builds passed |
| Published thin client | 26 proxy tests passed with Codex 0.153.4 required and installed |
| Published Docker topologies | KB-only 9/9; split server/KB 10/10; standalone server 4/4 |
| Embedding | KB-only suite exercised the bundled A25M model and checked vector-width compatibility |
| PostgreSQL store upgrade | Data/ownership preservation, credential rejection, and repeat-start checks passed |
| Learning loops | 46/46 checks passed after correcting the test copy's module placement and companion grants |
| Provider/model browser acceptance | 9 exercise checks, 4 post-restart checks, 5 exploratory checks, 1 provider-outage check passed |
| GUI navigation | 15 pages rendered without JavaScript errors; desktop and mobile screenshots inspected |
| Concurrent writes and persistence | 16 concurrent writes returned distinct IDs; 17 exact contents, including Unicode, survived a server restart when read through list |
| KB dependency outage | Rules returned HTTP 502 during a real KB stop and recovered after restart; server remained reachable |
| Store dependency outage | Returned an explicit application error; recovered after database restart |

Counts describe the named test units, not exhaustive feature acceptance. The two memory tests
skipped in the initial race invocation required separate corpus/replay variables; both were
subsequently run with real PostgreSQL and passed. Optional platform/vendor-specific cases in the
broader native registry are not promoted to live integration coverage.

Browser tests used real PAM login, server/web/modules, Vault-backed provider operations,
PostgreSQL and container restarts. Only the external model vendor was replaced with
`scripts/validation/providers/fixture.py`. A stopped provider process caused visible errors.
Resuming it alone did not restore its detached bus connection; terminating that process let the
packaged supervisor replace it, after which the repeated navigation pass had no provider/model
API errors. The final navigation report retains Chat's `/api/plugins` and unconfigured
workflow-channel 404 responses.

## Additional findings and test qualifications

**R042-4:** stopping the store PostgreSQL container caused `/v1/memory/list` to return HTTP 200
with `{"status":"error","message":"user memory module unavailable"}`. It did not pretend to
return an empty successful list, but its transport status is unsuitable for ordinary HTTP
failure detection. [Outage/recovery responses](release-0.4.2-2026-09-06/dependency-recovery.json)
are retained.

The checked-in native liveness/learning harnesses omit the memory module's required placement
and storage companion grant. Correcting those in disposable test copies made all seven KB
modules attach and all six learning loops pass. The manual liveness run finished 16/17: its
remaining blocker was the unprovisioned WORM observer's accumulated backlog. This manual
environment is not counted as a fully healthy packaged deployment. The published Docker stacks
had their packaged supervision and remained healthy during the sustained checks.

The original PostgreSQL provisioning helper grants broad runtime permissions in its test DB.
That contaminated an initial restricted-role assertion. The complete replay target was rerun
in a separate fresh DB, where it passed; the earlier result is not a product privilege finding.

An initial source transfer contained the wrong tree. All results from that first staging are
excluded. A new directory was populated from the explicit candidate archive; the build, native,
Go, frontend and configured database tests were rerun. Final verification again matched all
6,669 source files. The first registry transfer also encountered `unexpected EOF`; retrying
downloaded the pinned images successfully.

The exploratory HTTP script produced 79 passing and four failing hypotheses. These were
investigated rather than counted as four independent defects. In particular, treating a server
user memory as project-isolated was an invalid assumption under the module's documented
placement contract. The confirmed command and upgrade failures above remain decisive.

Live vendor subscriptions, managed wizard provisioning, every optional image variant, and full
feature workflows on all 15 GUI pages were not covered. This is extensive Linux release
validation, not a claim that every optional product configuration was exercised.

## Evidence and cleanup

[summary.json](release-0.4.2-2026-09-06/summary.json) indexes the verdict and counts. The companion
directory contains compact machine-readable evidence and browser reports. Raw logs, screenshots,
the exploratory scripts and intermediate diagnostic results are retained locally in
`/tmp/aimee-release-042-20260906/release042-evidence.tar.gz`, SHA-256
`e1311394668e3baab5e6b9feaa6dc658b87684b854c79fb9e034dc9fbbeb95e2`.
The archive intentionally retains failed/ineligible attempts; only the qualified final runs
listed above support the verdict. Generated credential stores and guest home directories were
excluded from that archive.

Final cleanup verification is recorded in the companion summary. The release verdict remains
NO GO until both memory regressions are repaired and the repaired published artifacts pass
these round-trip/upgrade reproductions and the required CI gate.
