# Canonical learning projection delivery

The execution log remains authoritative. Migration 47 adds delivery metadata,
not another history: `learning_projection_outbox` stores only exact source
pointers, scheduling/lease state, and retained delivered/revoked acknowledgments.
Canonical append and initial ticket creation commit atomically. Erasing the
canonical event cascades its ticket.

Automatic coverage begins with canonical appends after migration 47. Its exact
UTC cutover is retained in `learning_projection_coverage` and reported by the
host-only claim contract and daemon startup log. Pre-cutover missing projections
cannot distinguish crash loss from intentional retention; they remain recoverable
through explicit authenticated history replay. This increment does not claim
unlimited historical backfill.

Eligible sources are completed/failed/cancelled execution terminals and protected
running child references carrying an admitted host learning binding. Ordinary
child references cannot create delegate learning. Source owner and exact
session/turn/sequence/event/actor fences are rechecked before publication and
acknowledgment. Delegate delivery additionally rechecks the actual job, both
session owners and the unique exact child-turn terminal through existing
op64/op63 owners. A still-running child is retried, never inferred complete.

Telemetry operations 65/66 are HostOnly. Claim leases at most four complete
source envelopes and 1.5 MiB of record bytes per transaction; complete maximum
1 MiB events plus envelope metadata fit without proof truncation. Each opaque
128-bit lease lasts 180 seconds. Retry uses bounded backoff; a crash or missing
ack leaves the source eligible after lease expiry. A stale token cannot
acknowledge a replacement lease. Source ownership loss and definitive job/child
custody loss revoke delivery rather than claiming success.

The daemon owns one startup/timer worker independent of browser activity,
delegate heartbeat settings and KB availability. It invokes the existing
Learning observation/grade owners and private storage publishers. A terminal is
delivered only after its ordinary ungraded observation and every retained proof's
admitted grade. A delegate ticket also requires immutable experience and its
corresponding evidence row. Failed publication is explicit and retryable; a
proof-bearing completion cannot silently become permanently ungraded.

Worker calls use one-second Learning deadlines and check shutdown between proof
calls; DB operations retain their two-second bound. Normal synchronous completion
and explicit replay retain their existing command timeout. No model/provider,
verifier/tool rerun, KB instance, synthesis, automatic application or causal
procedure promotion is involved.

Delivered acknowledgments survive intentional interaction-row retention for the
lifetime of their canonical source. Missing retained projection rows therefore
do not trigger endless automatic resurrection. Duplicate delivery repairs are
idempotent, and a later human review of delegate evidence is preserved.

Qualification includes real PostgreSQL race fixtures for atomic rollback,
leases, exact source fencing, source erasure, child readiness, job revocation and
retention; a real C timer/publisher fixture under normal and ASan/UBSan builds;
and an actual-daemon Docker journey that pauses its owned daemon, removes only
fixture-derived rows and resets its existing tickets to the crash-window state,
kills/restarts that exact daemon and modules under the parent harness, then
waits for unattended startup repair without SSE/history/model/tool requests.
The last journey requires Docker in CI; local absence remains exactly the
existing reported journey skip. Synthetic helper/contract fixtures do not
qualify real configured execution by themselves.

Current local evidence: the focused PostgreSQL race suite, Aimee/Learning/Memory
race suites without the optional shared-PostgreSQL cases, normal C timer/session
fixtures, ASan/UBSan fixtures, private full server build and CMake timer fixture
passed. The parent-shell pause/restart success and failure contracts passed with
owned subprocess surrogates. Docker is unavailable locally, so literal actual-
daemon/Docker restart qualification remains pending mandatory CI. A broad optional
PostgreSQL suite against an already-populated shared database encounters existing
public-table/FK and all-schema-count fixture assumptions; those were not weakened.
