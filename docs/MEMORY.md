# Memory behavior and migration

The [module contract](modules/memory.md) defines ownership, dependencies and activation.
This guide retains the detailed behavior and migration record.

## Language and bus boundary

The memory module, including its bus producer and consumer, must be pure Go.
The existing bus in `src/core/event_bus` remains C. `server-go/bus` is the Go
binding to that bus, not a replacement implementation. Server and KB use one
shared Go memory implementation with placement-specific storage and permissions.

External C hosts may retain transport callers and their protocol declarations.
Memory policy, retrieval, ranking, persistence orchestration and fallback behavior
belong in Go; moving those into a C host does not complete the migration.
The memory process must build and communicate without cgo. Integration tests
must exercise its Go producer and consumer against the actual C bus.

Both memory trees now contain zero native files, and the memory descriptor has
no native sources, headers or tests. `check_memory_c_boundary.py` enforces this
recursively, rejects cgo and retains the repository-wide retired-policy checks.
`check_memory_go_only.py --module-only` exposes the same source/descriptor gate.
The memory executable and live Go probe must also build with `CGO_ENABLED=0`.
This language boundary does not certify all historical behavioral parity or the
numbered reliability proposals.


The host missing-context marker is a read restriction, never a writable project
or workspace. Shared store refuses that marker with `active_context_missing`;
MCP callers must provide real context or explicitly request `scope=all` for a
global write. Unscoped legacy global writes and private writes retain their
existing behavior. Canonical Go admission also protects internal store callers.

Shared KB get and ID-based mutations also accept canonical positive decimal-string
IDs, preserving int64 identities through native JSON transports. Unsafe numeric
IDs, noncanonical strings and overflow remain invalid. Server delete reports
retirement versus destruction from the admitted Go authority, and preserves
`review_required` and `conflict` refusals. The legacy Server context-read route
forwards the Go envelope too; a retrieval outage is no longer an empty context.

The unlinked `cmd_memory*` console family and its obsolete record wrappers are
retired. The shipping served CLI, HTTP and MCP surfaces use the shared Go owner.
Go renders live session context, agent search text, MCP diagnostic text and full
record envelopes, preserving exact int64 IDs, complete content and explicit owner
refusals. Native adapters retain host authorization and transport duties.

The [G0 closeout](proposals/pending/memory-reliability-g0-closeout.md) records
file/API dispositions and validation. Complete DB2 retirement is a
[future proposal TODO](proposals/pending/db2-as-a-go-module.md#future-proposal-todo-retire-db2-completely),
separate from the memory language boundary.

## Personal/shared recall composition

The Server placement owns the composition of a scoped KB recall envelope with
personal identity and preferences. Personal rows override matching shared keys
within those sections; equal numeric IDs from different placements do not
collide. The Go owner reads current personal rows, retains full content and exact
int64 IDs, preserves scoped handles, and rebudgets the final serialized bundle.
Personal rows are never sent to the KB. Shared-only requests skip composition.

The Server personal-recall adapter also forwards the complete Go envelope as
text, including exact IDs and owner errors; the native integrity check remains
at materialization.

The internal `memory.runtime` operation `compose-recall` accepts the shared
response as JSON text and returns the complete composed response as text, so C
transport cannot round IDs while decoding it. Plugins and the KB placement
cannot invoke this personal-store operation. A failed private read refuses the
whole composition; shared errors/quarantine remain refusals. The retired
`src/user_memory_merge.c` policy and mutable-array ABI are guarded against return.
This does not make cross-store reads one atomic snapshot or provide durable
release receipts. Activation snapshots transport canonical decimal strings; the
Go owner validates them and returns scoped handles. Native receipt transport
uses those handles without double conversion and refuses ambiguous legacy IDs
above 2^53-1.

## Current-state retrieval validity

The Go `current-validity-v6` predicate applies active lifecycle, suppression and
half-open valid time before shared lexical, whole-record/unit semantic, graph/
PageRank, compatibility-window, recall-bundle, activation and briefing limits.
Pending commitments retain their lifecycle while sharing the time predicate;
sticky activation cannot extend expired validity. Briefing episode summaries and
entity counts require current parents. Memory-backed graph evidence and
semantic edge intervals share the same timestamp normalization. Every memory
evidence source on a graph edge must resolve inside the request audience; a
visible source cannot admit an edge with hidden or inapplicable dependencies. UTC legacy wall
time and explicit offsets compare as instants against one transaction clock;
malformed nonempty values refuse retrieval. Scope/RLS and current embedding
fingerprints remain additional admission requirements.

Direct-ID current reads apply the same lifecycle, suppression and valid-time
predicate. Legacy `get --as-of` remains an inspection of a specific retained
version with a separate `valid_at` label; it does not reconstruct belief time.
It permits superseded, archived and retired versions, including their ordinary
activation suppression, but excludes deleted, rejected, revoked, quarantined and
unknown states and suppressed active rows. Scope checks still apply. Mutation
admission reads the scoped identity independently of serving eligibility so an
authorized caller can retire an excluded active row.

Exact-ID `get` also accepts a versioned `read_policy` object through the data
stage, public command and HTTP endpoints. With `schema_version: 1`, `mode:
"current"` uses the storage transaction clock; `mode: "historical"` requires an
absolute `valid_at` and returns only a retained KB version whose half-open
interval contains that instant. The response's `read` object reports the applied
policy and normalized historical time. Unlike legacy `as_of` inspection, an
out-of-interval version returns `not_found`. Revocation, quarantine, suppression
and scope rules still apply. The object cannot grant principal or scope authority.
`believed_at`, unsupported modes/versions, personal historical reconstruction and
use on other operations explicitly fail; legacy `as_of` cannot be combined with
the new contract. HTTP classifies unsupported modes/versions as caller errors.
Historical selection adds no SQL round trip. This is a temporal read contract,
not yet the complete evidence decision, validity CLI or release-generation receipt.

Directive/reminder matching, recall fallback and briefing views share the same
normalized expiry gate. Sweeps expire a row at the exact upper boundary; serving
does not wait for a sweep. Operator lists/dashboard counts retain stored lifecycle
state so unswept records remain inspectable.

Assertion search normalizes stored offsets and fractions for both world-valid
and belief-time intervals, including its current/historical projection. The
request contract still accepts second-precision UTC anchors; historical access
authorization and parent-version closure remain separate acceptance work.

Current typed-fact blocks apply the same assertion time checks before their
limit and require every memory source to resolve to a current visible parent.
Entity discovery and later fact reads propagate errors; a failed read cannot
leave a successful partial block. Operator review/history semantics are separate.

The baseline policy fingerprint includes this version. This current-state slice
does not certify all MR-01 surfaces, privileged historical/belief-time access,
utility horizons or a final release/revocation generation check.

## Context projection and limits

The Go ingress planner emits version-one `context_limits.max_context_bytes` for
its assembler. The assembler checks the serialized memory envelope, returns
exact UTF-8 byte accounting and retained memory IDs, and treats an explicit zero
as zero. Token caps and reserves currently return `unsupported_mode` because
complete provider-bound token counting is unavailable. The result certifies
neither the complete provider request nor source freshness. Typed context keeps
packing diagnostics outside the prompt, renders reviewed procedures once, and
reports unknown task coverage until requirements are evaluated.

Typed context optionally accepts `evidence_requirements` schema 1 with a bounded
`task_revision`, `query_mode: current_state`, and up to 16 subject/relation
obligations. The Go owner reports role coverage over retained, versioned current
assertions. Missing, budget-dropped, conflicted and unavailable roles differ from
ranking confidence. Repacking reevaluates coverage and cannot cure an earlier
conflict by dropping evidence. Other query modes remain unknown. This opt-in
coverage describes the memory projection; it does not authorize actions, prove
answer correctness, attest provider dispatch, or replace source revalidation.

An optional `evidence_requirements.recovery_budget` produces bounded current-state
lookup proposals for missing required roles. `evidence_recovery` is metadata with
`authority=proposal_only`; the host must separately admit and execute work. The
plan is regenerated after packing and never changes coverage by itself. Caps are
one round, 16 new items, 4096 estimated tokens, 2000 ms and zero external model
cost. Budget-dropped/conflicting/unavailable evidence is not blindly retried.
See the [planner validation](validation/memory-recovery-plans-2026-09-23.md).

The host's assembly allocation is an inherited ceiling: an explicit byte cap may
reduce it but cannot increase it. An absent cap inherits that ceiling. Versioned
limits reject duplicate fields (including escaped aliases), case aliases, null
values, unknown fields and invalid integers. Explicit null limit objects are
rejected at the ingress and typed-context command boundaries.

## Memory invalidation producers

Personal storage now records a monotonic `record_revision` and a content-free
invalidation event atomically with each governed row mutation. A collection row
serializes event positions in commit order; rollback restores both position and
event. Deletes retain an invalidation. Runtime roles may read the stream but
cannot advance or erase its progress. Source IDs are immutable. Counter-only
updates do not invoke the capture trigger, avoiding content serialization and
invalidation during ordinary reads.

The host-only data operation `change-feed` accepts `changes` with
`schema_version: 1`, an optional `after: {owner_id, generation}` cursor and a
bounded `limit` (default 64, maximum 256). It reads the current head and page in
one SQL snapshot. A new consumer, changed owner, cursor beyond the current head or missing
event requires a new canonical snapshot (`snapshot_required: true`); the feed
does not supply that snapshot or acknowledge consumer application. Events remain
retained. Server/personal placement uses the instance-local collection. Shared
KB placement uses the request's explicit primary scope and includes `collection`
in its cursor. A cursor from another collection requires resynchronization.
All-scope feed requests and non-host access are refused.

Shared record mutations advance separate counters for affected primary scopes;
a scope move advances both the old and new collections. Unrelated scopes can
commit independently. Secondary tag changes advance the parent record's revision
and primary collection, and secondary tags inherit parent visibility through RLS.
The journal never exposes a hidden parent's identity through a secondary tag.
Empty collections retain owner identity with generation zero without a read-side
write. Existing KB audit envelopes also record the new row revision.

This is the producer foundation for MR-02. Personal content history, further
governed child/dependency coverage, durable consumer checkpoints and release checks remain
separate work; the feed cannot certify derivative freshness by itself. Backup
restoration must rotate the producer identity before replay resumes; automatic
restore identity rotation and restore-resistant erasure intent are not implemented
by this producer slice.

## Canonical KB mutation admission

KB same-key store, edit, supersede and legacy store/content-edit adapters now use
one Go replacement admission path. Changed content gets a new row, a closed old
validity interval and a supersession link. User corrections preserve history too.
Primary/secondary scopes, owner principal and sensitivity survive replacement;
new authorship and confidence ceilings are derived from the admitted writer.

Model replacement or retirement of user-authored or unknown-origin content returns
`review_required`. This refusal preserves the active source; linked review proposals
remain to be implemented. Episode/experience content stays immutable, and policy or
instruction content requires the reviewed replacement path. These checks also
apply to same-key conflicts. Identical same-author upserts retain their ID and
original captured author. Same-key creators serialize on the scoped identity,
including before any row exists. Ambiguous legacy duplicate keys fail explicitly.

The old data-stage `update-content` operation returns the new identity in `ids`;
callers must use that identity after a successful correction. Scoped legacy delete
uses the same model retirement policy as the public command. Explicit authorized
hard deletion remains distinct. Transactional extraction capture/job failures roll
back the entire replacement. This is the initial MR-02 KB slice, not completion of
expected-version/idempotency keys, durable invalidation replay, personal-memory
versioning, reviewed proposals or the full mutation/retention policy.

## Server search orchestration

Server KB search forwards keywords to `memory.search` with the Server view. The
Go owner validates and joins them, executes fact and compatibility-window reads
in one scoped transaction, and returns complete owner envelopes. A failed lane
fails the operation; it cannot masquerade as an empty window list. The C host
only resolves placement/scope and transports the result. Integer tokens and
content survive without fixed native result structs.

Both placements retain complete Unicode queries up to the Go data-stage limit
of 16,384 bytes and reject longer queries explicitly. The old 2,047-byte silent
truncation is retired. This changes long-query behavior intentionally; the frozen
retrieval manifest must be used for comparisons.

## Live benchmark owner

`memory.benchmark` parses the live code-graph corpus, chooses scoped live-memory
self-retrieval cases, runs retrieval, scores and renders reports in Go. CLI and
Server transport raw host-owned file bytes and complete responses. Corpus text is
bounded at 1 MiB; malformed cases or labels fail before retrieval. Integer labels
are preserved exactly and duplicate labels are rejected. The retired native
case loader, ablation resolver and live scoring loops cannot be restored.

Every successful report includes per-case queries, expected/retrieved IDs and
owner latency. Any failed query refuses the entire report. Unlabelled queries
contribute latency only; quality metrics divide by the labelled-query count.
Live self-retrieval is identified separately from labelled evaluation. Reports
explicitly state that these are live diagnostics without snapshot isolation,
not frozen release gates. Latency measures owner retrieval, not the Server/KB
network hop. Dataset suites point to the isolated Go evaluator.

## Labelled audit and calibration

`memory.audit` and `memory.calibrate` are Go command-owner operations. They accept
bounded TSV labels (`query<TAB>positive memory ID`), preserve full Unicode queries
and integer IDs, and collect scoped diagnostics through the ordinary owner read
path. Audits score the actual candidate order. Expected IDs are never inserted
into the candidate pool; an unretrieved answer remains a miss.

Calibration freezes each candidate pool once and performs bounded deterministic
coordinate search over diagnostic feature multipliers. Its artifact explicitly
says `deployable: false`: these multipliers are an offline experiment, not the
Go serving ranker. `--apply-config` is refused before any read. The legacy CLI
adapter transports requests and copies Go-rendered output; `--write` now writes
a JSON analysis artifact, not an unused native ranker configuration. Invalid
labels, missing owners and failed reads cannot publish successful partial scores.
Legacy explain rendering no longer calls the deleted native effective-importance
formula; owner diagnostic score parts remain authoritative. Its unregistered C
fixture is retired with that formula. Go scope-validation and restricted-role
replay cover the supported filter boundary; unknown scopes are rejected instead
of silently becoming workspace scope.

The retired native fusion fixture is covered by production Go RRF tests for
arm fairness, per-arm deduplication, candidate limits and stable record metadata.
Repeated IDs within one arm contribute one vote; agreement between distinct arms
still contributes to fusion. This is a correctness regression, not a quality claim.

## Isolated Go evaluation transport

`server-go/modules/memory/cmd/aimee-memory-eval` runs the ordinary Go KB memory
handler against a fresh PostgreSQL database. The PostgreSQL provider owns the
database lifecycle and the same SQL client/provider wire used by production.
Seeding, searches, context construction, diagnostics and scoring within one
process therefore share one store. Each new process starts with an empty store.

Set `AIMEE_DB2_EVAL_URL` to an explicit disposable PostgreSQL admin DSN with
database-creation rights. There is no fallback to live store configuration.
The provider creates a random database from `template0`, applies the supplied
packaged schema there, and drops that database at EOF, on protocol failure or
on a handled termination signal. Cleanup rolls back abandoned transactions;
failed drops produce an error naming the database so cleanup can be retried.
A forced process kill can still leave a database requiring cleanup.

From `server-go`, with the environment variable already set:

```sh
go run ./modules/memory/cmd/aimee-memory-eval \
  -schema ../src/modules/kb/c/schema.sql -embedding-dim 1024 <<'JSONL'
{"stage":"data","body":{"operation":"insert-epistemic","tier":"L2","kind":"fact","key":"eval-fixture","content":"eval-fixture content","confidence":0.9,"project":"evaluation"}}
{"stage":"data","body":{"operation":"search","query":"eval-fixture","project":"evaluation","limit":10}}
{"stage":"command","command":"runtime","body":{"operation":"benchmark-context","query":"eval-fixture","project":"evaluation"}}
JSONL
```

One JSON response per input line carries the owner's numeric module `status`
and JSON `body`; nonzero statuses remain failures. IDs retain their integer
precision. Requests and responses use the owner contracts, not a second memory
implementation. The local evaluation transport has no model/network executor:
operations needing one report that absence. This is not a replacement for an
embedding-enabled retrieval benchmark. SQL uses the disposable database owner's
permissions; the separate restricted-runtime-role replay remains necessary.

The `locomo` and `longmemeval` retrieval suites use this Go evaluator, with a
separate disposable database for each conversation or question. QA, session-support
and miss-report suites now use the same injected Go module setup. The native
dataset runners and memory scratch-store hooks are deleted.

Corpus evaluations now emit a versioned manifest and one result per case, with
stable fixture IDs rather than ephemeral database IDs. The manifest binds the
semantic corpus digest, exact schema bytes used to create the database, embedding
serving identity/dimension, ordered case IDs and effective ranking policy. Baseline
comparison rejects missing manifests and changed identities even when the case
count matches. Baselines retain the case receipts; failed runs cannot overwrite
them or publish partial scores. The checked-in 105-case input is frozen by
`tests/eval/memory_retrieval_manifest_v1.json` and a required unit assertion.

The [retrieval compatibility decisions](proposals/pending/memory-reliability-retrieval-compatibility.md)
state what is retained and retired. This first manifest does not certify real-model
quality, historical C ranking parity, temporal reproducibility or the full release
matrix. Existing aggregate-only baselines require explicit regeneration.



## Migration history

[Earlier implementation checkpoints](validation/memory-migration-history.md) retain the
chronological record. Counts and pending statements there describe their original
checkpoint, not the current inventory. Current scope and validation limits appear
above; the [delivery tracker](proposals/pending/memory-reliability-delivery.md)
tracks remaining program work.


### Observed diagnostic ranking

The diagnose_scoped response returns parts.score_evidence: observed_ranking_steps
and parts.ranking_steps for the selected candidates. Stages retain actual RRF arm
ranks and votes, candidate-order resets, negation overlap and PageRank additions.
Each stage replaces or transforms the preceding score; only the last stage's
contributions sum to the final score. Earlier ranks and contributions are retained
as zero-weight metadata in persisted feature_values, with ranking_trace_schema: 1.
This does not supply native SQL/cosine scores or a complete excluded-candidate trace.

Exact-ID explain_match labels its legacy text-match estimate separately.
Its MCP scores map remains numeric; score_evidence is an adjacent field.
Automatic ingress previews retain their established score and byte commitments.

Generated relation search, entity edges and profile aggregation check the exact
versions and current eligibility of all recorded copied inputs. Generator-owned
rows without observations await reindexing; authored relations retain their
existing parent policy. See [linked input validation](validation/memory-linked-relation-inputs-2026-09-23.md).

### Utility horizons (MR-10)

`AIMEE_MEMORY_UTILITY_HORIZON_POLICY` is an optional operator-owned JSON artifact
in the memory process environment. Restart both owners to switch it atomically
with the deployment. An absent artifact preserves the existing eligibility
rules. Malformed configuration refuses owner initialization. Requests and stored
model text cannot supply this policy.

The artifact has `mode` (`shadow` or `enforce`), a `policy`, and up to 128 exact
record-version `overrides`. Policy schema 1 declares its revision,
`transient_kinds`, `safety`, `domains`, `kinds`, and `unknown_rule` (`exclude` or
`allow`). There is no wildcard or default duration for other kinds. A rule has
an ID, `duration_seconds` (0 through ten years), `anchor` (`created` or
`confirmed`), and an optional absolute `deadline` which can only shorten that
rule's deadline. Explicit deadlines must have at most microsecond precision,
matching the canonical storage clock; finer values are rejected. An example
shadow artifact is:

```json
{"mode":"shadow","policy":{"schema_version":1,"revision":"task-state-1","transient_kinds":{"task_state":true},"kinds":{"task_state":{"id":"task-day","duration_seconds":86400,"anchor":"created"}},"unknown_rule":"exclude"}}
```

Precedence is safety rule, matching admitted override, domain rule, then kind
rule. Shared domains are canonical `scope_type:scope_value` values; the private
domain is `personal`. Overrides carry the complete `MemoryRecordVersion` and a
rule. A confirmed-anchor override additionally names `confirmation_generation`:
the protected MR-02 journal's update event for that exact current revision and
scope. The operator's artifact is the confirmation admission; the existence of
an arbitrary update alone does not confirm usefulness. A stale version or
nonexistent event cannot renew a horizon. Creation anchors come from the
protected insertion journal, including for records whose editable creation
metadata changes. Legacy records without journal evidence remain unknown under
the declared rule. Access counters never create an anchor.

Enforcement joins the shared Go eligibility predicates before lexical, dense,
graph and bundle limits and again at source release. Retained historical reads
keep their existing authorization/lifecycle gates. Horizon expiry does not
change valid time, retire a row, delete history, or remove its vector. Disabling
the policy restores only horizon eligibility. Validity diagnostics expose the
actual version, policy digest, anchor, deadline, mode and decision. Selected
transient records and native ranking traces carry the observed decision; health
reports distinguish measured would-exclude occurrences from unknowns.

Collection projections bind the artifact digest and the next visible horizon
boundary as well as canonical generations. A policy revision or elapsed deadline
invalidates reuse even if there was no memory write. Current payloads retain
release-time checks; diagnostics are not transferable authorization.

The default stays off. Domain durations require separate historical-task and
outcome evidence before operator promotion. No learned duration, universal
expiration, or automatic policy tuning is installed by this implementation.

## Served views and claim cards (MR-12)

`aimee memory serve briefing --task "review the deployment" --store kb --json`
requests an explicit projection from the selected memory owner. The other views
are `active_constraints`, `current_state`, `recent_decisions`, `relevant_context`,
`known_failures`, `reviewed_procedures`, `open_contradictions`, and
`historical_context`. Historical requests require `--valid_at` or `--believed_at`
with second-precision UTC coordinates. The default store is private (`user`);
shared views require `--store kb`. An unavailable private capability is reported
as an omission, without reading shared data to fill it.

The Go owner applies current eligibility and scope before candidate limits.
Briefings protect hard rules and scoped constraints ahead of discretionary
context. Contradiction sides travel as one bundle; a budget cannot retain half
a contradiction. `--limit 0` and
`--context_limits '{"schema_version":1,"max_context_bytes":0}'` retain no model
text. Byte accounting covers the exact rendered memory envelope. Provider-bound
hard token limits remain explicitly unsupported by this operation, consistent
with the existing context-budget contract.

HTTP clients use `POST /v1/memory/serve` with `view`, `task`, and the same typed
options. KB clients use `memory.serve` through the existing Go module action.
MCP tools `memory_serve` and `memory_claim_card` use the same owner; the
`memory` family and native core/review toolsets expose them too.
The named CLI command requires a client with its compiled view marshaller:
current served argument specs cannot encode nested budget/requirement JSON, so
no partial spec is published that could silently discard those fields.

Responses separate minimal `rendered_context` from versioned selected-record,
coverage, omission, freshness and per-channel bundle-count diagnostics. A
projection receipt binds exact bytes and source versions for this invocation;
it is not a durable provider-dispatch receipt. Provider handoff must still use
the normal source-release and MR-06 dispatch receipt gates. Without explicit
`evidence_requirements`, task sufficiency is unknown, not inferred from a
nonempty result. Reviewed procedure candidates retain their scope and review
record; unknown applicability/outcomes are labeled explicitly. Known failures
require complete recorded origin lineage; a failure label alone is omitted
with an evidence gap, and does not establish causal generalization.

Projection caching never skips owner reads. Each invocation recomputes current
scope, eligibility, temporal selection, collection dependencies and coverage on
the owner. A cache hit reuses serialization only. Request identity includes the
principal, scope, view, task, exact temporal coordinates, effective budgets,
requirements, renderer/eligibility policy, collection and selected versions.
Standby databases cannot certify current views. Empty selections retain their
collection dependencies, and each invocation receives a new receipt identifier.

`aimee memory claim_card <id> --store kb --json` projects a canonical memory
record, exact version, authorship, authority, lineage uncertainty, visible
contradictions and current freshness. `--expand-evidence` additionally reads up
to sixteen accessible exact-version lineage records within a 16 KiB expansion
allocation. It does not serve stale copied evidence. The card's `correction`
descriptor targets the existing `memory.supersede` expected-version operation;
that owner still decides whether replacement or a review proposal is authorized.
Cards have no separately editable canonical text. Confidence calibration remains
unknown unless the underlying owner can establish it.
