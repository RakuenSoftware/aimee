# Memory reliability delivery tracker

This records implementation evidence and remaining acceptance work for the
18-proposal program. PR #2983 merged as `7aea4e5221`; G0 and its performance
follow-up form the starting point for the remaining work.
It does not turn proposal requirements into passing release gates. The scope is
one Go memory implementation in Server/private and KB/shared placements, with
Go module-side producers/consumers using the existing C bus. The bus stays C.

Full DB2 retirement is deferred to the [future proposal TODO](db2-as-a-go-module.md#future-proposal-todo-retire-db2-completely).
G0 removes native memory behavior; it does not retire unrelated DB2 consumers
or replace the C bus.

## Authorized work

| Work | Current evidence | Remaining acceptance |
|---|---|---|
| 1. G0 language/ownership cutover | Pure Go shared memory owner and module-side bus transport; native implementations and dead console retired; original file/API and external-owner ledger | [Closeout evidence](memory-reliability-g0-closeout.md). Go/PostgreSQL race, actual C-bus placements/restart, native transport, HTTP and P1 isolation pass. [Fresh `.253` release evidence](../../validation/memory-g0-2026-09-19.md) records T1/T2/T3, 0.4.1 upgrade/rollback, exploratory concurrency/int64/failure recovery and tested-revision CI. Whole-DB2 retirement is deferred. |
| 2. Retrieval compatibility | Versioned whole-record/unit/temporal semantic recall, opt-in Go PageRank, independent-arm RRF with per-arm deduplication | [Compatibility decisions](memory-reliability-retrieval-compatibility.md) are recorded; the [paired quality/latency run](../../validation/memory-g0-2026-09-19.md) is recorded. [Performance follow-up](../../validation/memory-performance-2026-09-20.md) records reduced overhead and unchanged Go rankings. Extend the frozen corpus to the full adversarial matrix. |
| 3. Evaluation and release evidence | Shared injected Go module; isolated corpus/dataset/QA/support/miss runners; scoped runtime-role and transport regressions; complete owner/evaluator race suite added to required packaged-DB2 CI | Initial frozen 105-case input and manifest-bound corpus baselines/per-case receipts are implemented; live CLI/Server benchmarks share the Go owner and refuse partial results; fresh-image paired quality/performance and bounded CLI/MCP/HTTP restart/failure coverage are [recorded](../../validation/memory-g0-2026-09-19.md). The complete MR-18 adversarial/cross-surface matrix remains. |
| 4. MR-01–18 | MR-01 through MR-05 complete; 5 of 18 proposals | [MR-01 final closeout](../../validation/memory-mr01-closeout-2026-09-24.md). [MR-02 final closeout](../../validation/memory-mr02-closeout-2026-09-24.md). [MR-03 final closeout](../../validation/memory-mr03-closeout-2026-09-24.md). [MR-04 final closeout](../../validation/memory-mr04-closeout-2026-09-25.md). [MR-05 final closeout](../../validation/memory-mr05-closeout-2026-09-25.md). MR-06–18 remain pending. |

The [MR-02 closeout checklist](memory-reliability-02-closeout.md) records its eight
unchanged acceptance gates and completed validation.

The [MR-03 closeout checklist](memory-reliability-03-closeout.md) records its six
unchanged acceptance gates and completed verification.

## Post-merge execution

Completion of all MR-01–18 implementation and acceptance work is the active goal.
Execution is sequential by proposal, as requested on 2026-09-24: complete MR-01,
then MR-02, continuing numerically through MR-18. MR-01 through MR-05 are now complete
(5 of 18); MR-06 is next and has not been started. Previously implemented work for later proposals is retained, but new
work on those proposals waits for the preceding proposal's closeout. Integration
work required to satisfy the active proposal and its validation remains in scope.
The [MR-01 closeout checklist](memory-reliability-01-closeout.md) maps its seven
frozen acceptance clauses and required diagnostics to completed evidence.
The remaining entries below are historical implementation checkpoints, not
current MR-01 blockers.
PRs #2988 and #2989 have merged. All remaining proposal implementation and
evidence now accumulate in [PR #2990](https://github.com/RakuenSoftware/aimee/pull/2990),
retargeted to `testing`. Push the continuing work to its existing
`agent/memory-mr02-change-journal` head; do not open separate slice PRs.
The [frozen acceptance inventory](../../../tests/eval/memory_reliability_acceptance.json)
pins 123 acceptance clauses from the merged proposal revision and the measured
Go performance baseline. It is an inventory, not a claim that those gates pass.
The requirement text remains bound to that source revision while implementation
and validation evidence are added here.

The [creation retry implementation](../../validation/memory-creation-retries-2026-09-21.md)
extends both Go placements with durable store keys, preserving replacement,
unchanged-result and review admission. Local PostgreSQL/race, upgrade, native,
export and contract checks pass. Fresh creation HTTP/MCP/restart validation at
`15b756234f` passes **1,529/1,529 checks** (937 T2, 592 T3), with all nine image
identities and three actual provider caps verified. These receipts do not certify
downstream consumer freshness.

The [derived-parent eligibility repair](../../validation/memory-derived-eligibility-2026-09-21.md)
extends current-state gates to episodes, relations, summaries, scenes, typed
metadata and graph feedback. Mixed-source hybrid files and profile counts cannot
use a visible parent to admit a hidden or expired one. Full Go/PostgreSQL and
race tests pass. Fresh application/harness `1989160bdf` passes **1,553/1,553 checks**
(961 T2, 592 T3), including all 24 new MCP boundary cases. All image identities
and provider caps were verified. Full MR-01 remains open.

[Typed assertion source-version commitments](../../validation/memory-typed-source-versions-2026-09-21.md)
now bind exact owner/assertion/revision observations into selection identity and
preserve them through outer packing. Unversioned channels remain explicit. Local
PostgreSQL/race checks pass. Fresh application/harness `91a1f02969` passes
**1,558/1,558 checks** (966 T2, 592 T3), with all images and actual caps verified.
[Direct supporting-parent revisions](../../validation/memory-parent-version-lookups-2026-09-21.md)
are now bound into assertion selections, with a measured 48.3× improvement in the
isolated parent-eligibility query. [Episode versions](../../validation/memory-episode-source-versions-2026-09-21.md)
add independent episode and parent revision commitments under shared schema 30;
local PostgreSQL, race and migration checks pass. Fresh application/harness
`97fcb4ae04` passes **1,566/1,566 checks** (974 T2, 592 T3).
[Indexed-deletion audit isolation](../../validation/memory-evidence-event-isolation-2026-09-21.md)
at `800f4ef2f4` passes **1,561/1,561 fresh checks** (969 T2, 592 T3).
[Plain-text fact projections](../../validation/memory-fact-source-versions-2026-09-21.md)
now carry exact assertion/parent versions and emit only Go-retained references
after host integrity acceptance. Runtime-role and native transport tests pass;
fresh `f055d212f5` passes **1,570/1,570 checks** (978 T2, 592 T3). [Fact query batching](../../validation/memory-fact-query-batching-2026-09-21.md)
reduces the measured eight-entity recall from 11 SQL calls to three, with 16.2%
lower local query time and 38.5% fewer allocated bytes. These observations do not
certify owner revalidation at final release.

Delivery follows the program’s dependency order: eligibility/mutations and final
payload budgets first, then evidence/receipts/coverage, views and indexing,
followed by task contracts, outcomes, governed actions and disposable state.
MR-18 conformance and matched performance measurements accompany each wave.
Optional fitted policies retain the proposals’ observe/canary gates.

The first MR-01 follow-up closes direct-ID read gaps: current reads now apply
normalized valid-time and suppression rules already used by search. Legacy
`as_of` inspection permits retained superseded/archived/retired versions but
withholds deleted/rejected/revoked/quarantined/unknown states and suppressed
active content. It preserves legacy `valid_at` labeling; it is not yet the full
historical/belief-time contract. Mutation scope lookup remains independent of
serving eligibility, so an admitted retirement can still target an expired or
suppressed active row. Restricted-role replay covers those positive/negative
cases, malformed governed timestamps and hidden IDs.

The next MR-01 slice adds version-one `read_policy` for exact-ID reads. Current
mode keeps the captured storage clock; historical KB mode filters by the supplied
half-open valid interval and returns `not_found` outside it. Unsupported belief
time, personal temporal history, schemas and operations fail explicitly. Personal
exact-revision inspection now has a separate `at_version` contract. Public/HTTP
forwarding preserves the object; its response describes the applied temporal
policy without claiming a final release receipt. The host's existing C forwarding
adapter only copies the new JSON field; all interpretation remains in Go and the
C bus is unchanged. Restricted-role replay covers both modes and exact endpoints.
PRs [#2988](https://github.com/RakuenSoftware/aimee/pull/2988) and
[#2989](https://github.com/RakuenSoftware/aimee/pull/2989) carry these slices.
[Fresh `.253` validation](../../validation/memory-read-policy-2026-09-20.md)
records 224 passing topology/placement/identity verdicts, including the new HTTP
contract. These results do not close the remaining MR-01 acceptance gates.

[Fresh shared reliability validation](../../validation/memory-shared-reliability-2026-09-20.md)
now records 320 passing T2 verdicts for implementation and harness `abfa42e5d4`.
This includes linked correction drafts, authenticated exact-draft approval and
rejection, preserved model authorship/confidence, restart-safe retries, scope
isolation and erasure. The existing versioned update/supersede, real MCP receipts,
rollback and outage/recovery checks continue to pass. Actual concurrent database
connections cover committed-response loss, uncommitted disconnection and competing
review decisions.

Fresh testing exposed redundant primary-scope indexing invalidating pending
reviews. Schema 26 preserves versions for this compatibility projection while
locking classification against concurrent primary-scope moves. This avoids an
unnecessary parent rewrite, audit and invalidation. Update also reuses its locked
confidence read, and canonical admission precedes its audit commit. No new whole-
request P95 claim is made. Remaining work stays on the single branch described
above; these foundations do not certify all MR-01–18 acceptance gates.

[Private revision and restart-permission validation](../../validation/memory-private-versions-2026-09-20.md)
adds the latest complete fresh T2 receipt: **348/348 checks** on `68eaab6d44`,
including verified private caller-context transport and the enrolled restart
readiness check.
Private history and conditional correction pass HTTP/MCP, restart, rollback and
erasure checks. This run caught and repaired PostgreSQL restart reconciliation
restoring forbidden journal/history grants. It uses corrected application and
PostgreSQL images; migration 28 repairs already weakened private ACLs. Both
legacy database upgrade paths and real C-host/Go-process conformance pass.

[Private correction review validation](../../validation/memory-private-review-2026-09-20.md)
records **572/572** fresh checks on `d66c9bb860`: 396 in enrolled T2 and 176 in
standalone T3. Both placements exercise private model drafts, exact human review,
atomic rollback, retained authorship, restart-safe decisions and erasure. Existing
shared review, semantic and exploratory gates continue to pass. This adds private
review decisions. Private conditional-correction keys are now implemented as
described in the [retry validation](../../validation/memory-private-retries-2026-09-20.md);
create/delete keys and the remaining acceptance clauses are still open.
Its [fresh retry evidence](../../validation/memory-private-retries-2026-09-20.md)
records **638/638** checks on `88ebfc8460` (429 T2, 209 T3), including
33 additional private retry checks in each topology.

[Fresh provider-bound validation](../../validation/memory-provider-boundary-2026-09-20.md)
records **760/760** checks on `1f25b57f67` (490 T2, 270 T3). Final HTTP captures
cover three client APIs and two provider formats, including separate constraints,
tools, Unicode, continuation and tool-call relay. They exposed and verified fixes
for dropped buffered Responses instructions/tools and incorrect Anthropic wire
serialization. Hard provider token caps and durable dispatch receipts remain open.

[Fresh final-assembly validation](../../validation/memory-provider-stage-once-2026-09-20.md)
records **800/800** checks on `928919a6ea` (510 T2, 290 T3). Responses performs
memory planning once at provider assembly; Go uses actual host-provided resource
facts to avoid repeating guidance already included in the persona. Structured
Chat avoids unused legacy recall/persona preparation. Buffered/streaming Responses
and Chat now produce identical provider bodies for matched inputs. The existing
Go economizer planners also reject token evidence reused after equal-length edits.
Full hard-budget, release-binding and durable-dispatch acceptance remains open.

[Typed projection byte limits](../../validation/memory-typed-byte-budgets-2026-09-20.md)
add explicit exact UTF-8 caps, literal zero handling and digest-bound accounting
at both Go entry points, with unchanged host forwarding. Cached row serialization
removes quadratic repacking work while preserving the existing projection format.
The local stress benchmark is about 34.3 times faster; this is not a whole-request
P95 result. Fresh application/harness `c9d4a9a9ec` passes **818/818** checks
(528 T2, 290 T3), including 18 authenticated typed-budget checks. Final provider
caps and release/dispatch acceptance remain open.

[Typed outer packing](../../validation/memory-typed-outer-packing-2026-09-20.md)
keeps the owner response opaque through the C host, verifies projection/selection
commitments in Go, and repacks rows to the actual remaining envelope allocation.
The assembly result retains exact IDs and source/final digests. Fresh
application/harness `e88fe83a20` passes **824/824** checks (534 T2, 290 T3),
including 24 typed-budget/identity checks. This does not yet constitute a
provider release or durable dispatch receipt.

[Typed assembly evidence](../../validation/memory-typed-assembly-evidence-2026-09-20.md)
now forwards Go-selected projection references only after host integrity acceptance,
including typed-only assemblies. A bounded-read repair prevents truncated stored
events from being reported as complete traces. This is an optional assembly event
path. Fresh application/harness `440144e437` passes **835/835** checks
(545 T2, 290 T3), including 11 authenticated evidence writer/reader checks.
Source versions, full channel coverage and durable dispatch remain open.

[Exact evidence source identity](../../validation/memory-exact-evidence-ids-2026-09-20.md)
repairs large-ID transport in the external event, attribution and provenance
adapters. Safe numeric IDs remain compatible; larger IDs use decimal strings,
while ambiguous legacy IDs are refused without inventing a source. Native codec,
merge, storage and consumer regressions pass. Application/harness `7bb36b1551`
passes **853/853** fresh checks (563 T2, 290 T3), including 18 authenticated
identity and refusal checks. The matching automatic clients and exact-ID read
wrapper preserve source identities, and the isolated DB2 export builds.
This does not close source-version binding or durable dispatch acceptance.

[Final provider byte admission](../../validation/memory-final-request-budgets-2026-09-20.md)
adds an explicit bounded HTTP limit contract, interpreted by Go at the common
final-wire boundary. A refusal prevents provider dispatch even when optional
reduction is off; streaming refusals are explicit failures. Native and Go race
regressions pass. Fresh application/harness `140a546a15` passes **1,093/1,093**
checks (683 T2, 410 T3), including 200 provider checks per placement. This caught
and fixed the admission process being packaged but omitted from the default
Server composition. A paired fixture benchmark records admission overhead; it
does not certify the MR-18 P95 gate. Provider token counts,
inherited limits, source versions and durable release/dispatch remain open.

[Admission latency validation](../../validation/memory-admission-latency-2026-09-20.md)
records **1,093/1,093** fresh checks on `f110e9b873` and real C-host/Go-process
conformance. Only the Go economizer consumer's scheduled idle ceiling changes
from 10ms to 1ms; the C bus is unchanged. Paired median admission overhead falls
from 7.67 to 2.19ms for OpenAI format and 9.83 to 2.70ms for Anthropic format.
Idle CPU rises from 0.30% to 1.60% of one core. These are bounded fixture results,
not full MR-18 certification. The independently exported process now builds and
uses the same owner-defined polling setting as the bundled process.

[Inherited context-limit validation](../../validation/memory-inherited-context-limits-2026-09-20.md)
records **1,100/1,100** fresh checks on `f2340b1d4b` (690 T2, 410 T3).
The Go memory planner and assembler now prevent explicit byte limits from raising
the inherited host allocation. Strict decoding refuses duplicate/aliased fields
and null limits, including through the authenticated native KB HTTP adapter.
The startup descriptor policy separately recognizes default-enabled admission
without allowing live toggling; all 46 descriptor regressions pass. Full-request
operator/task inheritance, provider token counting and protected packing remain
open. This evidence does not certify any proposal complete.

[Deployment-owned request limits](../../validation/memory-operator-request-limits-2026-09-21.md)
record **1,246/1,246** fresh checks on `72bf98b032` (763 T2, 483 T3), with all
three applications configured with a verified 32 KiB operator ceiling. Go
intersects deployment and caller byte caps at the common final serialization
gate, including calls without an HTTP limit header. Buffered and streaming
refusals send zero provider requests. Real C-host/Go-process conformance and all
77 local lint checks pass; the standalone Go export also builds and passes native
conformance. All 58 remote CI checks pass on the implementation revision.
Task-composition limits, token accounting, protected
packing and the remaining release/receipt gates are still open.

[Complete hard-rule recall validation](../../validation/memory-protected-recall-2026-09-21.md)
records **1,261/1,261** fresh checks on `f6e544e060` (778 T2, 483 T3).
Go preserves the complete stored hard-rule set or explicitly refuses, including
private/shared composition. Bounds on candidate count and cumulative text prevent
silent 8/16-rule truncation and oversized database replies. MCP preserves the
owner refusal before session guidance; HTTP classifies overflow as 413 through
the status provider. Bounded prefix packing is about 11 times faster with about
91% fewer allocated bytes in the local 64-row stress fixture, not a whole-request
P95 result. Rule-promotion authority, all-provider protected-context enforcement,
provider tokens and source-version/dispatch binding remain open.
All 58 remote CI checks pass on `2269c6c1b9`, including sanitizers and encrypted
upgrade/rollback; its sole follow-up change is the exact MCP source-review record.

[Native context refusal propagation](../../validation/memory-context-refusals-2026-09-21.md)
now preserves explicit Go recall failures through initial native assembly and
refresh, stopping before the next provider call and preventing provider fallback.
MCP also retains quarantine/degraded responses before session guidance. Local
production-path fixtures cover zero initial dispatches, refusal after five turns
and successful recovery on the same thread. Fresh `2123e9a592` passes
**1,261/1,261** deployment checks; the separate credential retry/release follow-up
`b3a2578ae3` passes its targeted tests, native build and all 77 lint checks.
The HTTP follow-up now carries required gateway-plan/assembly failures through
request-scoped and asynchronous context to final dispatch, including native
execution inheriting that context. Local tests cover malformed/failed plans and
assemblies, successful empty plans, thread isolation and zero provider calls.
Fresh application/harness `9deb1efc14` passes **1,311/1,311** checks (803 T2,
508 T3), including real Go-owner outages, zero dispatch for required-memory paths,
recovery, and unchanged memory-disabled native Anthropic passthrough. The
subsequent Go briefing follow-up preserves explicit legacy allocations when a
promoted style requests more evidence; required database replay verifies six
allocations. Complete native protected rendering, configured Anthropic opt-in,
provider tokens and the remaining MR-03 acceptance work remain open.

## Conditional lifecycle follow-up

[Conditional shared rejection/restoration](../../validation/memory-lifecycle-versions-2026-09-23.md)
adds scoped row-locked owner/record/revision preconditions to both verbs. The native
server and KB console preserve the conditions and owner refusals. Restricted-role
replay and competing-correction tests cover stale versions, hidden records,
owner mismatch and successful restoration. Lifecycle retry receipts, complete
mutation coverage, and full MR-02 certification remain open.

## Supporting indexed-lookup repair

The repeated `scope_required: no active project` during this migration came from
using a linked checkout under `/tmp` against a remote service that had registered
the main checkout under `/home/virant/dev/aimee`. Sending only the client's `cwd`
gave that service neither a matching root nor access to the worktree's Git metadata.

The POSIX thin client now resolves standard linked-worktree metadata locally and
matches the main checkout against registered index roots for indexed reads and
`kb search`. It retains the real `cwd`, explicit/launcher project choices and
`--scope all`. Actual worktree registrations take precedence; unknown or ambiguous
roots do not invent a project. Missing Git or unavailable registry responses leave
the existing server fallback intact. Both native and served-argspec requests use
the same forwarding step. This changes CLI project discovery, not memory identity
or the C event bus. Windows' subprocess stub and nonstandard Git layouts retain
the previous explicit-project/server-fallback behavior.

Regression evidence includes real Git repositories and linked worktrees, nested
directories, shell characters in paths, root boundaries and ambiguity; HTTP tests
exercise both marshallers, registry failure and explicit/environment/all-scope
precedence. Live `index hybrid` and `index investigate` succeed from the migration
worktree without `--project` against the existing remote service.

## Proposal acceptance ledger

| Proposal | Implemented foundation | Work still required for full acceptance |
|---|---|---|
| MR-01 eligibility | Scoped transactions, placement separation, current-hash filters and Go current-validity predicates before lexical/semantic/unit/graph/window/bundle/activation/briefing limits; shared directive/reminder and protected-rule expiry; normalized assertion world/belief-time intervals and current typed-fact evidence closure; request-bound source rechecks before provider attempts, including private/shared native record revisions | Unified temporal/utility policy on every surface, remaining unversioned channels, transitive dependency release checks and the post-check mutation race |
| MR-02 mutations | Shared KB admission across same-key/edit/legacy verbs, user/model versions, original-author preservation, identity locking, tombstones and atomic extraction actor/job writes; personal and primary-scope shared revisions/generations/outboxes with bounded host replay; tag changes invalidate their visible parent; owner/ID/revision-bound shared corrections reject stale versions under the row lock; authenticated keyed update/supersede corrections commit immutable actor-isolated retry references with the existing audit and recheck result eligibility/version on replay; MCP forwards preconditions and preserves receipts; update reuses the locked confidence read; linked model drafts preserve authoritative parents and durably deduplicate rejected proposals; authenticated exact-draft reviews atomically record the reviewer while preserving model authorship/confidence, with rollback and concurrent decision tests; private updates retain owner-bound revisions, support exact-version history and conditional supersede, and atomically publish invalidation; verified private author capture and replacement admission protect user/unknown records, cap model confidence, and retain historical authorship; private drafts and exact-draft reviews preserve model authorship and atomically record decisions with canonical history/invalidation; private keyed corrections atomically retain actor-isolated receipts, reject changed payloads and recheck current results on replay | Complete; [eight-gate closeout](memory-reliability-02-closeout.md) and final process evidence above. |
| MR-03 budgets | Go typed-context assembly, final personal/shared recall rebudgeting and existing packing limits; minimal typed projection with one reviewed-procedure rendering, exact byte count/digest and retained IDs; automatic ingress evidence uses only Go-retained items after host integrity acceptance; buffered Responses retains instructions/tools and Anthropic targets use the correct final serializer; Responses stages run once at final provider assembly; Go economizer token evidence is digest-bound to the exact request; typed projection byte caps count complete JSON and wrappers with literal zero handling and linear repacking; verified typed rows fit the remaining outer envelope allocation with source/final projection and selection commitments; explicit HTTP final-provider byte caps use Go admission after serialization, with bound metadata and fail-closed dispatch; inherited memory-envelope and deployment-owned final request byte ceilings cannot be raised by callers at the common serving fence | Complete; [six-gate closeout](memory-reliability-03-closeout.md), final image/process receipts and explicit unsupported-counting boundaries. |
| MR-04 lineage | Fact lineage/review and invalidation paths | Independent-family accounting, full derivative closure and restore-resistant erasure |
| MR-05 sufficiency | Typed context and answer abstention; retrieval availability separated from unknown task coverage rather than nonempty-to-complete inference | Requirement/coherence coverage of retained evidence and bounded recovery |
| MR-06 receipts | Go-canonical provider body/source bindings; native synchronous WORM preparation and dispatch admission; separate HTTP acknowledgement or unknown outcomes for participating buffered/streaming transports; supplied-receipt verification with independent evidence dimensions; confirmed-cache reclamation; actual-provider and host-SIGKILL component tests | Authorized stored lookup, dispatch ownership recovery, observed transport-start evidence, complete source/trace coverage, external checkpoints and remaining client surfaces |
| MR-07 exploration | Existing adaptive recall limits | Host-issued task contracts, calibrated starvation recovery and operator ceilings |
| MR-08 health | Go lane counters and PageRank timing | Served-population concentration/entropy/fanout/reentry metrics with exact denominators |
| MR-09 ranking | Eligible semantic lanes, RRF, per-arm deduplication, bounded opt-in PageRank | Aggregate prior bounds, final exposure diversity/type floors and adversarial quality gates |
| MR-10 utility | Existing temporal/lifecycle fields | Deterministic utility horizons, authenticated time anchors and historical-access rules |
| MR-11 embeddings | Pinned identity/current hashes, versioned rebuild/cutover/rollback | Full freshness/temporal-activation acceptance under concurrent rebuild/failure |
| MR-12 views | Go briefing/alerts and visible parent checks | Named view/claim-card contracts and full scoped collection-generation cache identity |
| MR-13 task projections | No release claim | Audience intersection, disposable provenance and promotion gates |
| MR-14 hygiene | Bounded, explicitly scoped, SELECT-only exact-duplicate preview with current revisions, partial coverage, Server HTTP and candidate CLI transport | Durable proposals/rejection deduplication, narrow worker roles, resumable jobs/scheduling, additional detectors and projection cleanup |
| MR-15 outcomes | Existing feedback/workflow and learning paths | Verified application versus exposure, delayed outcomes and complete task cost |
| MR-16 actions | Existing host authorization boundaries | Exact action evidence reauthorization, idempotent effects and composition budgets |
| MR-17 retries | No release claim | Clean reasoning context with retained real-action journal and replay prevention |
| MR-18 release gates | Go owner tests, live C bus with process restart, isolated evaluators, frozen corpus and manifest-bound per-case baselines | Complete adversarial manifests, temporal reproducibility and full surface/restart/failure gates beyond the recorded initial paired run and fresh-environment matrix; owner/evaluator PostgreSQL replay is now wired into required CI; final-wire memory/constraint/tool captures cover three client APIs into two provider formats |

Historical notes are preserved in the [program migration history](memory-reliability-migration-history.md)
and [module migration history](../../modules/memory-migration-history.md). Their
counts and pending statements are dated checkpoints, not current certification.

The full cross-language bus conformance suite passes locally, including memory
termination/restart in both placements. Its egress fixture advertised one
uninitialized extra stage; the fixture now matches the seven shipping stages.
Remote CI exposed a reset-discovery boundary: unconstrained Go-owned embedding
versions were treated as unknown global-dimension tables. Reset discovery now
includes only dimension-bound vector columns; unknown fixed-dimension tables
still refuse reset. Memory replay reports independently after earlier failures.
The unit-semantic fixture now accounts for the existing dimension-specific floor;
restricted-role replay passes locally at both 384 and 1024 dimensions.
The shared-database bootstrap fixture now writes through the real Go memory
handler so canonical KB mutations get their scoped transaction and Server writes
use user scope. Both bootstrap orders and concurrent bootstrap pass with the
existing rows preserved.


[Native recall projection validation](../../validation/memory-native-recall-2026-09-21.md)
moves native recall rendering, complete hard-rule reservation and retained
reminder selection into the existing Go operation. The host forwards its remaining
byte allocation and verifies the opaque result without another RPC. Exact int64
reminder transport now survives public selection, marking and completion.
Local Go/PostgreSQL and native refusal regressions cover this change; fresh-image
native acceptance and the remaining MR-03/MR-06 clauses stay open.

The generic native context fallback now also passes its remaining byte allocation
to Go. Whole-row packing and explain selection are owned by Go; the C host verifies
and appends the opaque projection. Legacy rule/caller-prompt paths, exact provider
token budgets and durable dispatch receipts remain outstanding.

Fresh recall image `1f198a6b07` passes **1,311/1,311** checks: 803 enrolled T2
and 508 standalone T3. Image identities, operator limits and sanitized receipts
are linked from the native recall validation above. All nine owned containers
are stopped with volumes retained. Generic assembly and asynchronous worker
configuration fixes postdate that image; live native/async acceptance remains open.


The later native/async checkpoint is now linked from the same validation page:
`7ebee54d83` passes all 527 fresh standalone checks and a 33-check live refresh
follow-up. Its enrolled matrix correctly refuses an over-budget post-restart
request and is not accepted. A bounded model-allocation follow-up then passes all
33 enrolled native lifecycle/refresh checks under the unchanged 32 KiB operator
ceiling. These tests use actual Go owners and five real tool calls. They do not
close automatic packing against the complete native request. The worker also
serializes quoted/multiline error diagnostics correctly; complete fresh
`64c1871cc0` acceptance of the new fixture and error fix is pending.


Complete fresh application/harness `64c1871cc0` now passes **1,387/1,387 checks**
(841 T2, 546 T3), including 37 real asynchronous native checks per placement.
Five tool calls, newly committed memory on refresh, refusal before the next
provider dispatch, owner restart and escaped provider-error events pass in both
placements. Exact images, unchanged 32 KiB caps, actual request byte counts and
per-run receipts are linked from the native validation page. Complete native
request packing, exact token/reserve accounting and durable dispatch acceptance
remain open. No MR-01–MR-18 proposal is fully certified by this checkpoint.


[Private conditional retirement](../../validation/memory-private-retirement-2026-09-21.md)
adds expected-version checks and durable keyed receipts to non-destructive private
delete/forget. Migration 32 preserves existing correction receipts and guards the
retired canonical result. PostgreSQL tests cover rollback, concurrent duplicates,
replay after connection changes, authority, stale versions, reactivation and
erasure. Fresh application/harness `e697581ac9` passes **1,441/1,441** checks
(**868 T2**, **573 T3**), including 27 new real HTTP/MCP retirement checks in
each topology. The independently exported Go schema owner now carries all 32
embedded migrations and shares its database/schema/peer startup assembly with
the bundled executable. Creation retries and the remaining MR-02 clauses remain
open.

[Shared conditional deletion](../../validation/memory-shared-deletion-2026-09-21.md)
adds expected versions and durable retries for model retirement and the existing
verified-user destructive operation. Schema-two receipts distinguish target and
result, and omit a current version for destruction. Scope-aware outcome checks
refuse a restored ID even when RLS hides it. PostgreSQL rollback, concurrency,
disconnect/reconnect, forged-receipt, authority and full schema-upgrade checks
pass locally. Fresh application/harness `037dc8f8c3` passes **1,472/1,472** checks
(**899 T2**, **573 T3**), including 254 shared checks. Fresh testing exposed a
contextless MCP store writing the host read-restriction marker as a project;
canonical Go admission now refuses it before writing. Explicit context/global
writes retain their contract. Embedded SQL now has a distinct `go_assets`
descriptor role; the complete script suite and standalone Go export build pass.
Creation retries and the remaining MR-02 acceptance clauses are still open.


[Direct memory-parent revision binding and lookup optimization](../../validation/memory-parent-version-lookups-2026-09-21.md)
now commit observed direct memory dependencies into assertion selections, refusing
parent overflow. Runtime-role tests prove that a changed parent changes selection
identity despite identical rendered assertion content. The bounded primary-key
lookup measures 48.3× faster than the prior join in the documented local fixture;
this is not a whole-request P95 result. Episode/non-memory/transitive dependencies,
collection generations and final release revalidation remain open. Fresh deployment
of this implementation is pending; no proposal is certified by this slice.


The parent-version implementation at `5aee043dd6` passes **1,560/1,560 fresh checks**
(968 T2, 592 T3); exact images/caps and cleanup are linked from its validation page.
A subsequent [indexed-deletion repair](../../validation/memory-evidence-event-isolation-2026-09-21.md)
fixes a reproduced audit-trigger error: per-object reference updates were applied
to every event in a changeset, corrupting references and causing mixed purge/update
batches to fail. Schema 29 isolates the emitted event, preserves the purge invariant,
and passes the runtime regression and 28→29→29 preservation check. Its broader
and fresh validation remain pending; existing history is not retroactively certified.

[Source revalidation at provider handoff](../../validation/memory-source-revalidation-2026-09-21.md)
now uses retained versioned facts, typed assertions and episodes at the common
provider fence. Go compares the roots and complete direct-parent sets in one
scoped statement snapshot; the external C host carries opaque handles and owner
messages. Historical read policy survives selection. Refused attempts select no
provider bytes, retries recheck, and request completion releases transient state.
Fresh validation is pending. This does not close unversioned channels, transitive
or collection dependencies, the post-check race, or durable dispatch receipts;
all 18 proposals remain uncertified against the frozen 123 acceptance clauses.

[Memory preview source versions](../../validation/memory-preview-source-versions-2026-09-21.md)
now cover canonical fallback previews and independently revised summaries under
schema 31, with exact parent revisions and owner-formatted rendered scores.
Ingress rehydrates source metadata in one scoped statement instead of one public
metadata query plus unused per-row epistemic queries. Outer packing and final
owner revalidation retain only accepted sources. Go/PostgreSQL, race, migration,
native transport and lint checks pass. Fresh application/harness `d8154014bb` passes
**1,688/1,688 checks** (103 T1, 993 T2, 592 T3), with all 12 images and four
actual provider caps verified. This advances
MR-01/MR-06 source coverage without certifying either proposal or durable dispatch.


[Shared lifecycle retry receipts](../../validation/memory-lifecycle-retries-2026-09-23.md)
extend conditional reject/restore with caller-scoped durable outcomes in shared
schema 32. Restricted-role rollback/replay, migration reapplication and real
concurrent commit/disconnect checks cover this slice. It does not certify the
remaining frozen clauses or deploy the draft PR onto the released 0.4.5 service.


[MR-01/MR-06/MR-18 follow-up](../../validation/memory-program-gates-2026-09-23.md)
closes optional negation validity re-entry before the lane cap, records actual
returned-candidate ranking contributions in request-local diagnostics, and repairs
standalone export inventory for lifecycle mutations. The existing ownership
validator and an actual independent memory export build cover packaging.
These are additional acceptance dependencies; all 18 proposals remain open
against their frozen clauses.

[Current-state evidence coverage](../../validation/memory-evidence-coverage-2026-09-23.md)
now evaluates bounded subject/relation obligations against the actual retained
owner-versioned assertions and reevaluates outer packing. Restricted-role replay
covers hidden parents and budget-dropped evidence. Timeline planning, independent
origins and recovery are still open; no frozen proposal is certified by this slice.

MR-10 now has a pure Go shadow evaluator with bounded policy artifacts and
owner-version/event-bound anchors. Boundary, precedence, unknown-anchor and
nonrenewal fixtures pass. It is not wired into serving or public diagnostics;
canonical anchor admission and gated rollout remain open.

[Fair semantic-assertion arms](../../validation/memory-fair-assertion-arms-2026-09-23.md)
now compete before final top-k and expose bounded graph work. The old lexical
capacity veto is reproduced by a restricted-role fixture. This advances MR-09;
prior/diversity policies and quality gates remain unfinished.

[Bounded recovery proposals](../../validation/memory-recovery-plans-2026-09-23.md)
add opt-in current-state lookup planning with explicit work ceilings, stable
attempt keys and post-packing regeneration. Proposals grant no execution authority
and cannot improve coverage without new retained evidence. Host admission,
durable attempt tracking and outcome application remain open. The shared decoder
also rejects ambiguous requirement and budget fields. Targeted race, exported
owner and native ingress checks pass. Fresh application/harness `0a63ad568`
passes **1,110/1,110 checks** across T1/T2, including both new HTTP cases.

The [linked relation input repair](../../validation/memory-linked-relation-inputs-2026-09-23.md)
binds generated relation text to every directly copied memory revision. Search,
entity edges and profiles withhold changed, expired, hidden or unobserved inputs
before limits and aggregation; rebuilding refreshes the observations. A packaged
PostgreSQL replay reproduces the previous leak. The full PostgreSQL/race suite
and exported owner pass. Fresh `09aa330ea` passes **1,140/1,140 checks** across
T1/T2, with all nine images and three actual provider caps captured. Transitive
closure remains open; direct dependent rebuilding is extended below.

[Discovery classifier robustness](../../validation/memory-discovery-policy-2026-09-23.md)
fixes a reproduced panic on bare `grep`, `rg` and `ripgrep` commands while preserving
baseline operator prohibitions. Execution-policy race tests pass. This does not
certify MR-07's task-contract and recovery requirements.

The [durable relation consumer](../../validation/memory-relation-consumer-2026-09-23.md)
atomically queues copied-input dependants and advances bounded replay progress.
Committed-connection tests cover restart, competing workers, rollback and retention
resynchronization; full PostgreSQL/race and export checks pass. Fresh schema-33 candidate `1baa020a6` passes **1,146/1,146 checks**, including
actual regeneration and restart in both KB topologies. All nine images and three
actual provider caps are captured. Progress records queued invalidation,
not serving freshness or complete MR-02/MR-04 acceptance.

[Link-only mutation invalidation](../../validation/memory-link-journal-2026-09-23.md)
now advances source dependency revisions and canonical events, including both
sources of a moved link. No-op updates remain silent. Creation receipts bind the
final revision after supersession links. PostgreSQL/race and export checks pass.
Fresh `d0e3c5752` passes **1,150/1,150 checks**, with actual regeneration/removal,
all nine images and three actual provider caps captured.

[Future-valid index admission](../../validation/memory-future-index-admission-2026-09-23.md)
separates active input processing from current recall, preventing premature retry
exhaustion. Whole-record/unit model work blocks suppressed inputs, while raw
vector results enforce current canonical eligibility before limits. PostgreSQL/race
and export checks pass. Fresh T1 `d5b3980f2` passes **134/134 checks**, including
actual clock-only activation with no new write or embedding. Its three images
and actual provider cap are captured.

Generation backfill/cutover now share active/unsuppressed input admission, including
suppression after a draft was already prepared. Other raw vector families retain
their own owner contract. The final PostgreSQL/race suite passes in 132.763 seconds;
the fresh T1 real-clock evidence is recorded above.

Future-valid copied relations are now prepared ahead of time under index admission,
then withheld by their direct input fences until applicable. The final PostgreSQL
race suite passes in 138.321 seconds; combined fresh T1/T2 real-clock graph
validation remains pending. This follow-up keeps shared schema 34.

[Provider retry admission](../../validation/memory-provider-retry-admission-2026-09-23.md)
rechecks the Go source owner after backoff before native or buffered Responses
resends. A local refusal remains `context_refused` through native primary and
model-fallback paths. Socket, real-Go ingress and agent propagation tests pass;
durable provider receipts and the post-check mutation race remain open.

The combined future-record/copied-relation application `921d7f3c4` passes
**1,162/1,162 fresh checks** (136 T1 and 1,026 T2), with all nine image identities
and three provider caps checked. The [receipts](../../validation/memory-future-index-admission-2026-09-23.md)
retain the initial public-field harness failure and the corrected reruns.

The provider retry application `1e61d4833` passes **1,630/1,630 fresh checks**
(1,032 T2 and 598 T3), including the six new exact-body resend assertions. All
nine image identities and three operator caps match the
[retained evidence](../../validation/memory-provider-retry-admission-2026-09-23.md).

[Buffered provider attempt receipts](../../validation/memory-provider-receipts-2026-09-23.md)
add Go-canonical exact-body bindings and synchronous WORM preparation/admission
for retained versioned source handles. Acknowledged HTTP responses and unresolved
network outcomes remain distinct; missing observation cannot prove non-dispatch.
Incremental streaming, recovery/inspection and remaining MR-06 coverage stay open.


[Durable provider receipts](../../validation/memory-provider-receipts-2026-09-23.md)
now cover participating buffered and incremental streaming transports with
synchronous preparation/admission and separate response or uncertainty records.
Both pinned candidates passed their 1,630-check fresh adapter matrices.
[Supplied receipt verification](../../validation/memory-receipt-verification-2026-09-23.md)
adds independent evidence dimensions. Its direct ledger experiment exposed an
unversioned native-recall coverage gap; the follow-up records body commitments
with that gap explicit, while fresh repair and host-loss validation continue.
No MR-06 or program completion is claimed by these component checks.

[Recall record revisions](../../validation/memory-recall-versions-2026-09-23.md)
now accompany private/shared ordinary and shared activation-selected payloads
from the same statement snapshot. Full PostgreSQL race and export checks pass.
Native retained-source binding and mixed-owner release revalidation remain open.

[Native retained-source release](../../validation/memory-native-source-release-2026-09-23.md)
adds selection commitments and request-bound mixed-owner checks before each
provider attempt. Private references stay on the Server. Missing/stale owner
answers refuse dispatch; private expiry and pending lifecycle retain their own
contracts. Local PostgreSQL, native and export checks pass; fresh-image validation
and remaining unversioned channels/post-check race work stay open. The preceding
receipt-cache candidate separately passed 1,652/1,652 T2/T3 checks.

The native source-release candidate passed its focused 49-check fresh Server
fixture, including actual private revision commitments and forced host restart.
[Rule expiry](../../validation/memory-rule-expiry-2026-09-23.md) now applies before
protected recall allocation and before new feedback/style derivation. Full local
PostgreSQL/race tests pass; the next image also adds a real private-correction
between provider attempts regression. Existing derivative invalidation remains open.

[Graph candidate revisions](../../validation/memory-graph-versions-2026-09-23.md)
now accompany graph and optional PageRank neighbor payloads from the same read.
Fusion preserves earlier candidate evidence rather than attaching newer versions
to older text. Full PostgreSQL/race and export checks pass; graph-path dependency
checks and the other unversioned retrieval lanes remain open.

The combined native-source and rule-expiry candidate completed all 1,662 T2/T3
checks, including private correction before retry and forced-host-loss receipts.
[Bounded hygiene preview](../../validation/memory-hygiene-preview-2026-09-23.md)
adds the first MR-14 read-only exact-duplicate detector with explicit shared scope,
row/content budgets, versioned candidate findings and honest partial coverage.
It does not yet persist proposals, resume jobs, schedule work or run model detectors.

[Briefing source revisions](../../validation/memory-briefing-versions-2026-09-23.md)
now bind facts and recent episode summaries to their same-statement owner/record
and direct-parent revisions. Non-owner PostgreSQL tests verify that parent/child
edits invalidate the old source references. Aggregate entities and transitive
closure remain open. The first hygiene image exposed a strict-handler transport
envelope mismatch; its failed evidence is retained and a narrow repair is under
validation rather than counted as passing.

[Generated summary input observations](../../validation/memory-summary-inputs-2026-09-23.md)
bind deterministic summaries to the parent revision actually used by their
producer. Current reads and release checks withhold unknown or stale observations;
canonical fallback remains available. Episode and unit producers apply the same
check before copying summary text. Full MR-04 lineage and erasure remain open;
validation results are tracked in the linked report.

The repaired [hygiene preview deployment](../../validation/memory-hygiene-preview-2026-09-23.md)
passes **1,683/1,683** fresh checks (1,069 T2 and 614 T3) on application
`377b5309d`. T2 uses corrected harness `95f85ef53`; T3 uses its matching
`377b5309d` harness. All nine images and three actual provider limits are recorded.
This validates bounded read-only hygiene through direct KB, Server HTTP and the
candidate CLI; durable hygiene proposals and the other MR-14 gates remain open.

[Generated episode observations](../../validation/memory-episode-inputs-2026-09-23.md)
extend the producer-input fence to deterministic episode text and its optional
summary input. Ordinary and typed reads, briefing, profile episode labels and
release checks withhold mismatched observations before limits. Authored episode
policy remains explicit. Full MR-04 lineage and erasure remain open; final
validation is tracked in the linked report.

The [summary input fence](../../validation/memory-summary-inputs-2026-09-23.md)
passes **1,689/1,689** fresh deployment checks (1,075 T2 and 614 T3) on matching
application/harness `83ff06d9c`, with all nine image identities and three provider
limits verified. Current previews fall back to canonical text when a summary's
producer observations are absent or stale. Full MR-04 remains open.

[Generated unit input observations](../../validation/memory-unit-inputs-2026-09-23.md)
withhold stale deterministic units from raw vector search, shared/unit semantic
recall, direct embedding and re-embedding. Parent revision, unit field commitment
and optional summary observations are producer-owned. Local packaged race and
export checks pass; combined unit/card candidate `76be02a42` passes all 1,694
fresh deployment checks with nine image identities and three provider limits verified. Other intermediate
source contracts and full MR-04 closure remain open.

[Episode-card source eligibility](../../validation/memory-card-source-eligibility-2026-09-23.md)
now excludes suppressed and temporally inapplicable inputs before the model call.
The public PostgreSQL regression covers all unavailable source states and an
eligible positive case. Later invalidation of generated cards remains open.

The [generated episode input fence](../../validation/memory-episode-inputs-2026-09-23.md)
passes **1,693/1,693** fresh checks (1,079 T2 and 614 T3) on matching
application/harness `74ed997e7`. All nine image identities and three provider
limits are verified. This covers generated episode observations and source-release
refusal after parent changes; full MR-04 remains open.

[Legacy query and coreference eligibility](../../validation/memory-query-coref-eligibility-2026-09-23.md)
applies current-state filtering before the five query-mode limits and the prior
context window, and blocks suppressed coreference targets from the resolver.
Explicit evaluation-corpus scope narrowing is preserved. Local packaged race,
public-scope and export checks pass; matching application/harness `b1692776e`
passes all 1,699 fresh deployment checks (1,085 T2 and 614 T3), with nine
actual image identities and three provider limits verified.
These repairs do not certify the remaining MR-01 release-race and parity gates.

[Public enrichment consistency](../../validation/memory-public-enrichment-2026-09-23.md)
refuses changed payloads and mismatched observed owner/revisions across metadata
reads. Historical selection remains separate. Packaged runtime replay, corrected
restricted-role fixtures and export checks pass; fresh-image validation and
unchanged-payload unversioned observation coverage remain open.

[Legacy read observations and history admission](../../validation/memory-read-observations-2026-09-23.md)
retain internal same-statement owner/revisions without changing legacy JSON.
Public enrichment rechecks current or historical eligibility as appropriate.
Fact history applies retained-history exclusions before limits and honors explicit
scope narrowing. The full packaged race suite passes in 254.529 seconds and
export passes; fresh deployment validation remains pending.

[Literal history identity](../../validation/memory-literal-history-2026-09-23.md)
prevents '%' and '_' in a canonical key from admitting unrelated predecessors.
The public PostgreSQL race regression and export pass; combined fresh validation
with the read-observation repair is pending.
