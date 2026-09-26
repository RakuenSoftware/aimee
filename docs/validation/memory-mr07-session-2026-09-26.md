# MR-07 durable session integration

MR-07 remains in progress. This implementation replaces the injected-store-only
checkpoint with authenticated PostgreSQL session ownership and real host calls.
It does not enable adaptive enforcement or claim a paired quality result.

Migration 38 adds bounded exploration state to the existing session row.
The private host operation locks the authenticated session directory and state,
then commits task revisions and shared session usage together. Legacy session
saves preserve the new column. Native dispatch and authenticated hooks use the
same Go policy/accounting implementation. Hard directives precede admission;
admission is atomically marked possibly dispatched, so lost replies and client
crashes cannot create refundable executions. Issuer failure falls back to baseline
when no operator accounting ceiling requires a durable admission.

Memory-owned offers include plan/source commitments, coverage provenance and
producer identity. Native code forwards this metadata and binds the authenticated
principal, session and durable job. Unknown worktree/index generations remain
explicitly unavailable and cannot support calibrated enforcement.

The native `context_contract_expand` tool accepts a reason and references to a
host-recorded indexed miss/failure. The actual `find_symbol` empty format is
recognized exactly; cross-project observations and invented gaps are rejected.
Turn completion derives starvation from persisted constraints and indexed gaps;
model text and trivial writes are not progress attestations.

The [evidence](memory-mr07-session-evidence-2026-09-26/) records passing Go race
tests, native policy/ingress tests, and an isolated-schema real PostgreSQL test:
24 concurrent hook/native checks across two tasks admit exactly the three allowed
scans, survive handler reopening, and do not double-charge retries. Foreign
principals and non-host calls are rejected. The full memory race suite passed;
after the native-offer addition, native/source-release/receipt regression tests
also passed. A repeated broad live family suite hit an existing test-table cleanup
dependency; it is not represented as passing. The new live owner test is isolated
and passed independently.

The full disposable CT109 matrix passed at `6008b3835`: both enrolled (T2)
and standalone (T3) processes exited zero. The captured application image was
`sha256:b9f73e533a381c6a0b16060fb8a98da2ec26e1b6bd56bfe3b669ff29704b82fa`;
all three application instances retained the 32768-byte provider request cap.
The evidence directory includes the actual exits and all nine image identities.
These runs include native/external recovery and exact exploration-state
preservation across the host crash; scripted provider fixtures validate process
contracts, not completion quality.

Subsequent changes bind final provider model/route/limits and receipt admission,
recheck the exact indexed generation, invalidate restarted memory owners, and
require a protected operator-reviewed calibration artifact before activation.
The targeted and full non-PG memory/policy/family race suites and native
policy/index-client/ingress tests pass. Artifact parsing tests use explicitly
synthetic numbers and confer no release calibration.

Native task requirements now reach the existing Go coverage evaluator through
both primary adapters. Native transport tests cover exact string revisions,
duplicate/null/oversized input and turn isolation; the direct adapter and worker
objects compile. All 77 lint checks pass.

Hook/native admission now shares freshness observations and Go policy decisions.
Native transport tests preserve structured recovery alternatives; detached
workspaces and outside-project paths cannot inherit adaptive controls from a
server checkout. The calibration scope also pins the executable build. Trusted
proxy ingress can bind an existing owned session; ordinary session headers remain
untrusted. Full non-PG policy/memory/family race tests, native caller tests, all
77 lint checks and the isolated concurrent live PostgreSQL test pass.

Remaining acceptance: fresh process acceptance of these later changes and
measured calibration with the predeclared MR-18 paired quality/efficiency gates. Unit and storage fixtures are not evidence
of task-quality noninferiority. MR-08 and MR-09 have not started.

The subsequent `4b67a119d` full matrix is **not green**: T3 exited zero, T2
exited one. In T2, the deliberate refresh-owner outage refused the sixth provider
send as required, and the supervised replacement answered an exact committed
memory read. The following asynchronous run nevertheless failed with
`unavailable` before provider dispatch; fixture retirement also failed. This
recovery failure is under isolated enrolled-stack investigation. Its process
exits and image identities are retained separately from the passing `6008b3835`
checkpoint.

A production observation during this work found CT100 changed independently to
`aimee-native-core:0.4.5-bridge.2` (image
`sha256:015e395b630bf12c42bacd905953027a02231aee7e1ac52a0b27325a665d4a5d`).
After startup its Docker health passed five consecutive probes, and the paired
thinclient reported the server `ok` and KB disabled. No MR-07 candidate was
deployed to CT100 by this work; candidate execution remains on CT109.

## Bounded experimental collection authorization

The collection bootstrap is separate from release calibration. A protected
root-owned experiment artifact and an explicit frozen-manifest opt-in authorize
only named sessions of one principal, exact live scope and typed raw-scan limits,
for at most six hours. Complete coverage, current Go owner/index observations,
clean host worktree and final provider receipt remain mandatory. Contracts mark
experimental approval distinctly; these observations cannot claim a passing
calibration without the independent paired scorer and evidence review.

Policy tests exercise both release and experiment approval through native and
hook session admission, revocation, owner restart and retained operator ceilings.
The non-PostgreSQL race suites for execution policy, memory and session families
passed, and all 77 lint checks passed. This is implementation validation, not
measured workload acceptance. No experimental or calibration artifact has been
installed on production, and no passing paired workload is claimed.

The `323ffd98b` full run was invalidated by CT109 disk exhaustion: driver exit 1,
reported T2 exit 1 and T3 exit 120. Its native report was absent and other evidence
files were incomplete. This does not diagnose the earlier enrolled recovery
failure and is not successful process acceptance. The [infrastructure record](memory-mr07-session-evidence-2026-09-26/323ffd98b-infrastructure-failure.json)
preserves the result. Obsolete task archives/build cache were removed, leaving
22 GiB free; owned failed-run stacks were cleaned. The dedicated test PostgreSQL
was retained. Candidate `0a25108fc` built successfully as
`sha256:10843abe298548f8e6560b79671537ed687d58b9e0c8071d5109d53e67daf6a0`;
a fresh sequential T2/T3 run now checks free space before each topology and
atomically replaces its image-capture evidence.

The `0a25108fc` sequential matrix completed with actual driver exit 0 and T2/T3
exits both 0. The [process results](memory-mr07-session-evidence-2026-09-26/0a25108fc-process-exits.json)
and [nine image identities](memory-mr07-session-evidence-2026-09-26/0a25108fc-image-identities.json)
bind the exercised applications, PostgreSQL images and all three 32768-byte
provider caps. T2 passed all 99 native checks and T3 all 81, including enrolled
refresh recovery, exact dispatch charges and ungraceful host restart persistence.
This fresh pass does not erase the earlier failed and infrastructure-invalid
runs. Measured workload promotion and an activated process experiment remain
open; these fixture providers are not task-quality evidence.

## Native requirement assembly discovered by activation testing

The activated-process fixture exposed a missing path: native primary requests
carried task requirements in worker TLS but never called the existing Go ingress
assembler. Their persisted contracts therefore remained unclassified, without
complete coverage or a current index observation. The native tool loop now calls
the existing assembler for explicitly declared requirements, using the actual
user query. Current-code-only permission still skips knowledge retrieval. The
assembled instructions remain intact during native memory refresh.

The OpenAI-compatible session path also retained the preceding turn's system
message despite preparing a current source receipt. It now replaces that owned
system content with the current assembly while preserving conversation history.
The ingress unit suite passes requirement forwarding and turn isolation through
the new native entrypoint; the native runtime compiles. Deployed activation and
the measured paired workload remain pending.

## Registered service scope during the native activation fixture

Candidate `c152612ad` fixes a real failure exposed after provisioning the test
Server's enrolled certificate, service/user memberships and attributed code
project. The TLS adapter queried the tenant-protected Server registry before
installing the verified service identity. Under the restricted migration owner,
the definer lookup returned false despite an exact active enrollment. The same
lookup returned true in the verified service's transaction-local tenant scope.
The adapter now enters that existing scope for the lookup and rolls it back
before resolving the separate content caller. It does not change RLS, runtime
roles, certificate checks or team-membership policy.

The candidate passed the TLS unit suite and all 77 lint checks. Both retained
CT109 applications upgraded from `2efe810ff` to `c152612ad` with existing
PostgreSQL volumes, Vaults, workspaces and enrolled identities preserved; both
became healthy. The live regression
`tests/e2e/memory-exploration-service-scope-e2e.py` passed seven checks: authorized
reads, denial without service membership, recovery after restoring membership,
denial with a mismatched registered certificate, and recovery after restoring
the actual certificate. [Checks](memory-mr07-service-scope-evidence-2026-09-26/c152612ad-service-scope.json)
and [image identities](memory-mr07-service-scope-evidence-2026-09-26/c152612ad-image-identities.json)
are retained. The test provisions tenancy; it does not bypass authorization.

A separate planner regression fixes repeated explicit contracts: the first-task
query cache previously omitted a fresh code packet on related turns, leaving no
current index generation for activation. Explicit obligations now request an
index observation for each assembly. Ordinary first-task suppression, request
opt-out, code-context opt-out and the minimum usable budget remain in force.
Focused Go race tests, native ingress tests and all 77 lint checks passed.
Deployed activation and the paired quality/cost experiment remain open.

## Activated native process journey passed on `be46915db`

The managed fixture passed all 13 native checks and seven controller checks,
with an actual zero process exit. It exercises the authenticated primary/native
path, complete typed obligations, a current indexed source packet, a literal-zero
raw-scan allowance, an actual structured restriction, a host-issued indexed-miss
reference, bounded expansion, and a fallback read of the contradictory source.
The persisted task state records exactly one raw scan and experiment approval.
After removing the root-owned experiment artifact, the same query still has
complete coverage and current owner/index observations but returns to observe.
[Native checks](memory-mr07-activation-evidence-2026-09-26/be46915db-inside-checks.json),
[controller checks](memory-mr07-activation-evidence-2026-09-26/be46915db-checks.json)
and [persisted state](memory-mr07-activation-evidence-2026-09-26/be46915db-persisted-exploration-state.json)
are retained. The fixture uses the explicit 65,536-byte functional-test ceiling;
MR-08's separate T2/T3 native runs use the ordinary 32,768-byte ceiling.

The fixture now stores its repository under the persistent application volume
and indexes its visible canonical root. Earlier fixture attempts indexed a
hidden `.aimee/worktrees` root, which lexical search excludes. The earlier
application replacement preserved Docker volumes but removed its disposable
`/tmp` corpus; that was not a corpus-preservation test. Authorization evidence
from that replacement remains valid, but it is not activation evidence.

Each primary turn records feedback episodes asynchronously. Starting the next
frozen-workload phase while their index projections are changing produced a
correct `stale_context` refusal. The successful fixture waits for all
`memory_index` jobs to finish and for canonical/projection generation sums to
remain unchanged for two seconds, bounded by 45 seconds, between phases.
Settlement measurements are retained for every phase. The serving source fence
and background feedback remain enabled. Earlier failed runs are retained on
CT109 under `mr07-managed-be46915db` and `mr07-managed-reuse-be46915db`.

The reproducible disposable-stack controller and synthetic inside provider are
`tests/e2e/memory-exploration-activation-e2e.py` and
`tests/e2e/memory-exploration-activation-inside.py`. They seed tenancy and test
claims, then exercise actual authorization, source checks, provider dispatch and
tool enforcement. They do not provide independent task-quality measurements or
justify calibration. MR-07's frozen paired quality, latency and complete-cost
gate remains open; production enforcement remains off.

## Real-provider collection smoke passed

The isolated native runtime on `be46915db` completed a real-model source lookup
through the paired `codex` provider route. The reusable collector run made four
provider calls, took 35.98 seconds end to end, and reported 26,397 total tokens
across those calls. The final text correctly named `actual-source-value`, not
the deliberately wrong supplied claim. The verifier examines only final text,
not tool output that happens to contain the answer. The actual process exited
zero and the temporary model roster was restored.
[The summary](memory-mr07-real-provider-evidence-2026-09-26/be46915db-smoke.json)
retains native-call metadata, provider usage, raw-evidence hashes and the final
answer. Full requests/responses remain in private collector storage.

`benchmarks/memory/native_provider_relay.py` records both native and forwarded
payloads, checks complete token usage, caps calls and keeps local pairing keys
out of the fixture. Five collector regressions pass, including rejection before
an unscoped request reaches the paired endpoint, missing usage, call-ceiling
enforcement and stderr backpressure. The paired endpoint reports no billing
cost. This single smoke test has no frozen independent-task corpus or treatment
comparison and cannot establish scan reduction, noninferiority, p95 or cost
parity. Those gates remain open.
