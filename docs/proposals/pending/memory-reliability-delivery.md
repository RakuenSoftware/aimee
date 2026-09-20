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
After the existing #2988–#2990 stack merges, all remaining proposal implementation
and evidence will accumulate in one continuing PR. Do not open separate slice PRs.
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
time, personal history, schemas and operations fail explicitly. Public/HTTP
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
records 257 passing T2 verdicts for `5541025ba3`, including owner/revision-bound
HTTP corrections, stale/repeated conflict responses, transactional scope-copy
rollback and durable journal history across restart. Counter-only writes no longer
acquire the collection-generation lock through generated search columns; a
concurrent-write regression verifies independent read accounting. The native
HTTP status contract accepts the Go classifier's 409 response. Remaining work
continues in the single PR described above.

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
| MR-02 mutations | Shared KB admission across same-key/edit/legacy verbs, user/model versions, original-author preservation, identity locking, tombstones and atomic extraction actor/job writes; personal and primary-scope shared revisions/generations/outboxes with bounded host replay; tag changes invalidate their visible parent; owner/ID/revision-bound shared corrections reject stale versions under the row lock | Linked review proposals, personal content versioning, expected versions on remaining verbs and idempotency contracts, further governed child/dependency coverage, durable consumer application/checkpoints and release checks |
| MR-03 budgets | Go typed-context assembly, final personal/shared recall rebudgeting and existing packing limits; minimal typed projection with one reviewed-procedure rendering, exact byte count/digest and retained IDs | Final serialized provider byte/token caps, source-version binding and protected-projection accounting |
| MR-04 lineage | Fact lineage/review and invalidation paths | Independent-family accounting, full derivative closure and restore-resistant erasure |
| MR-05 sufficiency | Typed context and answer abstention; retrieval availability separated from unknown task coverage rather than nonempty-to-complete inference | Requirement/coherence coverage of retained evidence and bounded recovery |
| MR-06 receipts | Owner diagnostic parts and postcommit observations | Durable pre-inference receipts, dispatched/acknowledged stages and crash uncertainty |
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
| MR-18 release gates | Go owner tests, live C bus with process restart, isolated evaluators, frozen corpus and manifest-bound per-case baselines | Complete adversarial manifests, temporal reproducibility and full surface/restart/failure gates beyond the recorded initial paired run and fresh-environment matrix; owner/evaluator PostgreSQL replay is now wired into required CI |

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
