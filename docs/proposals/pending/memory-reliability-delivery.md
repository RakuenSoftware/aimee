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
| 4. MR-01–18 | Existing foundations below | Finish each proposal's acceptance gates and integration dependencies. None is certified complete by the language migration. |

## Post-merge execution

Completion of all MR-01–18 implementation and acceptance work is the active goal.
PRs #2988 and #2989 have merged. All remaining proposal implementation and
evidence now accumulate in [PR #2990](https://github.com/RakuenSoftware/aimee/pull/2990),
retargeted to `testing`. Push the continuing work to its existing
`agent/memory-mr02-change-journal` head; do not open separate slice PRs.
The [frozen acceptance inventory](../../../tests/eval/memory_reliability_acceptance.json)
pins 123 acceptance clauses from the merged proposal revision and the measured
Go performance baseline. It is an inventory, not a claim that those gates pass.
The requirement text remains bound to that source revision while implementation
and validation evidence are added here.

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
| MR-01 eligibility | Scoped transactions, placement separation, current-hash filters and Go current-validity predicates before lexical/semantic/unit/graph/window/bundle/activation/briefing limits; shared directive/reminder expiry; normalized assertion world/belief-time intervals and current typed-fact evidence closure | Unified temporal/utility policy on every surface, release recheck and race tests |
| MR-02 mutations | Shared KB admission across same-key/edit/legacy verbs, user/model versions, original-author preservation, identity locking, tombstones and atomic extraction actor/job writes; personal and primary-scope shared revisions/generations/outboxes with bounded host replay; tag changes invalidate their visible parent; owner/ID/revision-bound shared corrections reject stale versions under the row lock; authenticated keyed update/supersede corrections commit immutable actor-isolated retry references with the existing audit and recheck result eligibility/version on replay; MCP forwards preconditions and preserves receipts; update reuses the locked confidence read; linked model drafts preserve authoritative parents and durably deduplicate rejected proposals; authenticated exact-draft reviews atomically record the reviewer while preserving model authorship/confidence, with rollback and concurrent decision tests; private updates retain owner-bound revisions, support exact-version history and conditional supersede, and atomically publish invalidation; verified private author capture and replacement admission protect user/unknown records, cap model confidence, and retain historical authorship; private drafts and exact-draft reviews preserve model authorship and atomically record decisions with canonical history/invalidation; private keyed corrections atomically retain actor-isolated receipts, reject changed payloads and recheck current results on replay | expected versions and idempotency on remaining verbs, further governed child/dependency coverage, durable consumer application/checkpoints and release checks |
| MR-03 budgets | Go typed-context assembly, final personal/shared recall rebudgeting and existing packing limits; minimal typed projection with one reviewed-procedure rendering, exact byte count/digest and retained IDs; automatic ingress evidence uses only Go-retained items after host integrity acceptance; buffered Responses retains instructions/tools and Anthropic targets use the correct final serializer; Responses stages run once at final provider assembly; Go economizer token evidence is digest-bound to the exact request; typed projection byte caps count complete JSON and wrappers with literal zero handling and linear repacking; verified typed rows fit the remaining outer envelope allocation with source/final projection and selection commitments | Final serialized provider byte/token caps, source-version binding and protected-projection accounting |
| MR-04 lineage | Fact lineage/review and invalidation paths | Independent-family accounting, full derivative closure and restore-resistant erasure |
| MR-05 sufficiency | Typed context and answer abstention; retrieval availability separated from unknown task coverage rather than nonempty-to-complete inference | Requirement/coherence coverage of retained evidence and bounded recovery |
| MR-06 receipts | Owner diagnostic parts and postcommit observations; ingress assembly evidence excludes omitted/rejected context and feedback uses the rendered preview; Go-selected typed projection references merge after integrity acceptance; bounded trace reads refuse truncated payloads | Durable pre-inference receipts, dispatched/acknowledged stages and crash uncertainty |
| MR-07 exploration | Existing adaptive recall limits | Host-issued task contracts, calibrated starvation recovery and operator ceilings |
| MR-08 health | Go lane counters and PageRank timing | Served-population concentration/entropy/fanout/reentry metrics with exact denominators |
| MR-09 ranking | Eligible semantic lanes, RRF, per-arm deduplication, bounded opt-in PageRank | Aggregate prior bounds, final exposure diversity/type floors and adversarial quality gates |
| MR-10 utility | Existing temporal/lifecycle fields | Deterministic utility horizons, authenticated time anchors and historical-access rules |
| MR-11 embeddings | Pinned identity/current hashes, versioned rebuild/cutover/rollback | Full freshness/temporal-activation acceptance under concurrent rebuild/failure |
| MR-12 views | Go briefing/alerts and visible parent checks | Named view/claim-card contracts and full scoped collection-generation cache identity |
| MR-13 task projections | No release claim | Audience intersection, disposable provenance and promotion gates |
| MR-14 hygiene | Existing lint/review surfaces | Proposal-only privileged boundary and rejected-proposal deduplication |
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
