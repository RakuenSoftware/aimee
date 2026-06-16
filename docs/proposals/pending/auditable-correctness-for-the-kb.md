# Proposal: auditable correctness for the knowledge base

- **State:** reviewed — roundtable sign-off (rev. 9). Eight review rounds (R1–R8);
  R4–R8 cleared the bar (no blockers, all six seats endorse), R8 zero-major.
  Ready for implementation per the phasing below.
- **Implementation status (2026-06-16):** PARTIAL — **P1 appears landed**. The
  P1 surface is present on `testing`: the dispatch-layer `turn_id` mint +
  `X-Aimee-Retrieval-Event` header (`server_http.c`, `ingress_preinject.h`), the
  single-writer `retrieval_event` (schema + `kb_service`/`demotion`), the
  `kb_evidence_emit_enabled` flag, and the `aimee audit trace` CLI. (The P1
  dependency on PR #185 cost-accounting is satisfied — #185 is merged.) **Verify
  P1 functional completeness end-to-end before moving this proposal to done.**
  **P2 in progress:** Layer-1 code-search provenance landed in #344
  (`code_search_hit_t`/`/v1/code/search` carry `content_hash`); the
  `/v1/audit/provenance` read surface + `aimee audit provenance <turn_id>` landed
  next (resolves each surfaced source id to `{id, kind, source, version, present}`,
  version = the row's live `updated_at`). Emit-time point-in-time version capture
  + per-source drift flag landed in #350. The D7 drift *detection* (a default-off
  `drift` maintenance mode + `db2_code_index_drift_candidates`: count code
  embeddings whose source file was re-scanned after the embedding, with timestamp-
  format normalization) landed next, and the D7 re-ingest *requeue* landed after
  it (`db2_code_index_requeue_drifted`: the `drift` maintenance mode now enqueues
  each distinct drifted project into `kb_ingest_queue` with `force`, deduped
  against an already pending/running row, skipped under `dry_run`; reported as
  `drift_requeued` in the maintenance summary). **Precise content-hash drift
  landed next**: the code-embed write path now captures `code_embeddings.source_hash`
  (the source file's `files.hash` at embed time, threaded through
  `pgvec_code_upsert`/`pgvec_kb_service_code_upsert` from `kb_service_code_embed`),
  and the D7 detector + requeue use a hybrid predicate — precise `files.hash <>
  source_hash` for embeddings that have it (no false positives from a re-scan that
  changed nothing), falling back to the scanned-since-embed staleness heuristic only
  for legacy rows with `source_hash=''`. **P3 storage substrate landed next**:
  `src/db2/fidelity.{c,h}` records answer-level `fidelity_report` (supported /
  unsupported / abstained buckets + four-state status, upserted per turn_id) and
  per-chunk `fidelity_attribution` (`accepted`/`irrelevant`, `operator_id`
  `fidelity-judge`) as **non-scored** artifact kinds — structurally invisible to
  `db2_demotion_score` (which reads only `retrieval_attribution`), so fidelity is
  demotion-inert by construction. The `fidelity_check_enabled` flag + the
  fail-closed eligibility gate (`fidelity_check_eligible`, deps on
  `kb_evidence_emit_enabled` + `ingress_preinject_enabled`) landed next, and then
  the **`/v1/audit/fidelity` read** (`aimee audit fidelity <turn_id>`) — the full
  server→kb-forward vertical mirroring `/v1/audit/provenance`, returning the
  turn's `fidelity_report` buckets + `attribution_count` with a four-state
  `fidelity_status` (`not_evaluated` when the default-off judge has not run).
  The **P1.5 two-writer merge core** landed next:
  `db2_demotion_retrieval_event_merge_turn` (D14) — the first writer creates the
  turn's `retrieval_event`; a later writer (e.g. the code-search surface) MERGES its
  surfaced refs into that same event (deduped by id, point-in-time version captured
  per merged ref) instead of being dropped, and re-merging is idempotent.
  The **D3 unified-ref data model** landed next (author-approved shape: unified
  list / file-level code refs / KB-handler emit): the `retrieval_event` now carries
  a canonical `surfaced_refs` list of typed entries — `{type:"memory",id,v}` and
  (forthcoming) `{type:"code",ref:"code:<project>:<file_path>",v:<content_hash>}` —
  with the legacy `surfaced_ids`/`surfaced_items` kept as DERIVED projections of the
  memory-typed entries (every existing reader byte-identical), plus migration-on-read
  that back-fills `surfaced_refs` for pre-existing events.
  The typed **`merge_refs_turn`** primitive landed next
  (`db2_demotion_retrieval_event_merge_refs_turn`): merges `{type,ref,v}` code/doc
  refs into the unified `surfaced_refs` (deduped by `type`+`ref`, idempotent; create
  path reuses `write_turn` for a bare turn event; same CAS retry contract — the CAS
  write is now a shared `cas_update_event_payload` helper). **`/v1/audit/provenance`
  code-ref resolution landed next**: it now resolves the unified `surfaced_refs`
  code entries into a `code_sources[]` — each `{ref, version, live_hash, present,
  drifted}` where `drifted` = the live `files.hash` (via the new
  `db2_code_file_hash` resolver) differs from the version captured on the turn.
  (`/v1/audit/trace` already returns the raw payload, so it surfaces code refs for
  free.) **Remaining P1.5:** wiring the code-search KB surface to *emit* its hits'
  code refs via `merge_refs_turn` (serving-runtime: needs `turn_id` threaded to the
  `code.search` path + `kb_evidence_emit_enabled`; live-stack verification). Plus
  the P3 LLM entailment judge *producer* (default-off until validated) and P4
  (labelled gold corpus — human, not autonomous).
- **Author:** JBailes
- **Date:** 2026-06-12
- **Charter roles:** Recall (provenance + per-turn evidence on the recall /
  ingress path), Evaluate (fidelity verdicts as an answer-level quality signal),
  Calibrate (the fidelity threshold and the staleness ranking half-life),
  Gate-Promote (default-off flag rollout per the readiness program).
- **Scope:** `src/db2/kb_service_memory.c` + `src/memory_context.c` (emit the
  `retrieval_event` **KB-side**, where the row ids and a DB2 connection exist;
  accept a caller-supplied turn id so the ingress turn and the recall share one
  canonical event), the code-search KB handler (`src/kb/kb_service_*.c`) +
  `src/headers/index.h` (carry a stable code ref + `content_hash` out through
  `code_search_hit_t` / the code_search response so there is something to bind),
  `src/server/server_http.c` + `src/server/server_http_routes.inc` (mint the
  per-turn `turn_id` in the **dispatch layer, before any response headers are
  written**, and add the header out-channel — see D9 — that today does not
  exist: `route_handler_fn` returns only `(int status, char *body)` and
  `send_response` writes a fixed header set), `src/server/ingress_preinject.c` +
  `src/server/kb_client_memory.c` (receive the `turn_id` as a parameter, thread
  it to both KB calls, return surfaced refs out instead of discarding them),
  `src/server/openai_chat.c` (accept `turn_id`; emit it in the `response.created`
  SSE frame on the streaming path), `src/db2/demotion.c` +
  `src/learning_evidence.c` (a `..._typed` **event and attribution** writer pair
  — string `scope_id` — alongside the existing int64 writers; the new non-scored
  `fidelity_attribution` + `fidelity_report` artifact kinds; the
  `db2_demotion_candidates` numeric-`scope_id` SQL guard), `src/db2/schema.sql`
  (the D14 migration: nullable `turn_id` column + partial unique index `WHERE
  kind='retrieval_event'` for the idempotent upsert — a column+index, **not** a
  new table), `src/headers/server.h` (widen the stream-handler ctx to
  `{fd, turn_id}`, D9), a new `src/server/fidelity_check.{c,h}` (entailment check
  of the answer against the injected evidence, delegate-driven), a typed
  `/v1/audit/{trace,provenance}` surface (`server/server_http_routes.inc` +
  handler) with the four-state status (D15), an `aimee audit trace|provenance`
  CLI, staleness ranking folded into `memory_maintenance.c`, and config + flag
  plumbing. Unit + integration tests. **No new datastore, no new service, no new
  artifacts table** — evidence reuses the charter artifacts table the demotion
  spine already uses (the D14 migration only adds a column + partial index).

> **Revision note.** Rev. 2 incorporated roundtable R1 (five design-error
> blockers → the **Decisions** D1–D8 below). Rev. 3 incorporates R2, where five
> of six seats endorsed and one convergent blocker remained: the
> `X-Aimee-Retrieval-Event` HTTP header was **unrealizable as written** — on the
> streaming Responses path the dispatch layer flushes headers (`write_sse_headers`,
> server_http.c:1163) *before* the handler runs and mints any id, and on the
> buffered path `send_response` writes a fixed header set with no handler hook.
> D9 resolves this by minting a server-side `turn_id` in the dispatch layer
> before headers and adding an explicit header out-channel; `turn_id` (not the
> KB-internal `retrieval_event_id`) becomes the durable, caller-visible audit
> key. R2's other refinements (typed *attribution* writer, upsert concurrency,
> fail-closed flag dependency, a labelled validity corpus for the fidelity gate,
> explicit `kb_client_*` signatures) are folded into D9–D13 and the design.
>
> Rev. 4 incorporates R3, where five of six seats endorsed and the Fidelity seat
> raised a **real, deeper** blocker: rev. 3's "fidelity never demotes" was false
> at the system level, because `db2_demotion_score` uses a **relative p10
> percentile** (so even `accepted` rows shift the cut) and a **shared 64-row
> window** (so machine verdicts can evict a human `contradicted`). D4 is
> rewritten to make fidelity demotion-inert *by construction* — judge output
> goes to a separate `fidelity_attribution` kind the scorer's
> `WHERE kind='retrieval_attribution'` query never sees. R3 also pinned the
> `turn_id` upsert to a concrete schema migration (D14), added a fourth
> `evidence_unavailable` trace state for soft-failed/DB2-down writes (D15), and
> tightened the `kb_client_*` / `..._typed` signatures (nullable `turn_id`
> appended last; memory `version = updated_at`).
>
> Rev. 5 incorporates R4 — a **clean sweep: zero blockers, all six seats
> endorsed.** It folds in the one remaining major (the buffered-path `turn_id`
> *in-channel*, distinct from the header out-channel — D9 now specifies both per
> path) and the seats' open questions: all OpenAI-family ingress paths are
> instrumented, not just responses (D6); trace state is computed at **read time**
> by row-presence, reconciling the async-write tension and adding an `ok+partial`
> state for one-surface soft-fails (D15); the judge-family≠answer-family guard is
> enforced at config time and abstentions get their own `fidelity_report` bucket
> (D11/D13); the emit flag's rollout bar is cost/latency neutrality since it is
> observation-only; and P1 is split into **P1** (single-writer reconstructibility)
> + **P1.5** (typed refs + the two-writer merge SQL), keeping the riskiest part
> off the foundation.
>
> Rev. 6 incorporates R5 — **the second consecutive clean sweep (zero blockers,
> all six seats endorsed).** It folds in three implementation-precision items, no
> design changes: D9 now names the *streaming* ingress callsite
> (openai_chat.c:798) explicitly so the highest-volume path isn't missed, with a
> streaming-specific wire-test; the memory surface carries a per-chunk `score`
> (`surfaced_scores`) so the `(id,source,version,score)` grain holds on both
> surfaces; and the standing invariant — `retrieval_attribution` is written only
> by the human/outcome path, never for machine/doc/code refs — is stated, with
> the D14 jsonb-append SQL validated by a standalone DB test before P1.5.
>
> Rev. 7 incorporates R6 — **the third consecutive clean sweep (zero blockers,
> all six seats endorsed).** It fixes two substantive catches and defers the rest
> to the implementing PR. (1) **Memory-surface misattribution (D16):** the
> ingress memory block comes from `memory_assemble_context`
> (memory_assemble.c:1176, pure text) — **not**
> `memory_recall`/`memory_context.c:1106`, which is the
> *proactive* surface; rev. 5/6's citations were wrong and are corrected
> throughout (ingress is a two-writer merge, not three-way; the memory surface is
> heterogeneous and only id-bindable rows carry a per-chunk ref). (2)
> **Disabled-vs-broke trace (D9/D15):** the `turn_id` mint + header are now gated
> on `kb_evidence_emit_enabled`, so a default/off server advertises no header and
> never traces `evidence_unavailable` — that state now unambiguously means a real
> write failure. PR-level completeness items (exact header declarations, the
> jsonb payload schema, the invariant CI guard) are pinned in an Implementation
> notes block.
>
> Rev. 8 incorporates R7 — zero blockers (fourth straight), but two seats
> (Recall + Red-team) **independently** flagged that rev. 7's D16 was *still*
> non-binding: `memory_assemble_context_explain` (memory_assemble.c:1230)
> produces the real text and then **re-runs an independent scoring pass** that
> doesn't reproduce the actual selection and ignores the untasked-context +
> entity/graph sections — so binding to it would attest to a chunk set that can
> differ from what grounded the answer (the precise failure the Goal forbids).
> D16 now **commits to the binding route**: thread an `(id, score)` out-array
> through the real `memory_assemble_context` path so the emitted ids are exactly
> the injected rows; the explain pass is *rejected as non-binding*. Untasked KV
> sections (which have row ids) bind; entity/graph lines surface as an explicitly
> *unbound* sub-block. Also: corrected the `turn_id` signature change to "not
> source-compatible — a mechanical NULL edit at every callsite" and added the
> missed `server_mcp.c:591` callsite.
>
> Rev. 9 incorporates R8 — **zero blockers, zero majors, all six seats endorsed
> (sign-off).** It folds five honesty/precision corrections from the 10 residual
> minors: the event upsert is **synchronous during assembly** (one PG insert on
> the request path), not "off the hot path" as earlier worded; the neutrality
> claim is scoped to *injected context + answer text* (the audit header is the
> only on-wire delta); untasked-branch memory rows surface an explicit
> `score=null` (no native relevance score); the fidelity gate pins **both
> precision and recall** floors; and the delegate-as-judge precedent is cited for
> **plumbing only**, never judging fitness. The remaining residual minors
> (segmenter pinning, an unbound-evidence claim bucket, commit-splitting, reusing
> the `request_id` header rail) are forward-looking P3/PR-level items recorded in
> the phasing + Implementation notes, not design gaps.

## Goal

Make every answer aimee grounds in the knowledge base **reconstructible**, and
every claim in that answer **falsifiable**. A knowledge base cannot be shown to
be *correct* by inspecting its outputs — an answer that looks right and an
answer that is right are indistinguishable at the text. Correctness is only
establishable by tracing an answer back through retrieval to its sources and
checking three things:

1. **Provenance** — which sources grounded this answer, at which version.
2. **Evidence** — what was actually retrieved and injected for this turn.
3. **Fidelity** — does the answer follow from that evidence, or did the model
   synthesize past it.

These are a chain, not a menu: provenance proves the KB *had* the right
material; evidence binds it to the turn; fidelity proves the answer is *entailed
by* it. Drop any link and "the KB is correct" becomes a claim taken on faith,
and — worse — uncorrectable, because there is no way to localize where it went
wrong. This proposal builds the chain. It is the trust substrate that lets the
KB be cited, debugged, and improved, rather than hoped-at.

The target grain is **per chunk, claim-linked**: the evidence record names each
surfaced chunk (id, source, version, score), and the fidelity layer attempts to
bind individual claims in the answer to the chunks that ground them.

## §0 What already exists (so we don't rebuild it)

aimee already has most of the spine — it just doesn't reach the path that feeds
generated answers.

- **A retrieval-evidence model already exists.** `db2/demotion.h` defines two
  evidence kinds stored in the charter artifacts table (no bespoke schema):
  `retrieval_event` (one row per recall invocation, carrying the surfaced ids)
  and `retrieval_attribution` (one row per `(retrieval_event, surfaced_row)`
  with a verdict and a contribution weight). `learning_evidence_write_retrieval_event`
  / `learning_evidence_write_retrieval_attribution` are the public seam. This is
  layer 2 and the skeleton of layer 3, already built.
- **The verdict vocabulary is already the right one.** `DEMOTION_VERDICT_*`
  (`accepted`, `corrected`, `contradicted`, `rolled_back`, `irrelevant`) is a
  verdict vocabulary, and `db2_demotion_score` already turns those verdicts into
  a time-decayed correctness signal that demotes contradicted knowledge. **But
  it keys strictly on int64 memory-row ids and resolves demotion via
  `db2_memory_get(row_id)`** — see Decision D2; we feed it only what it already
  understands.
- **A memory path already emits events — but NOT the ingress one.**
  `memory_recall` (`memory_context.c:1106`, the *proactive/session-recall* seam,
  called from dashboard_kb.c:370) writes a `retrieval_event` and stamps
  `retrieval_event_id`; `kb_service_agent.c` exposes
  `memory.record_retrieval_outcome` to write attributions back. So the pattern is
  proven end-to-end inside the KB process (this is why emission must stay KB-side,
  D1). **Critically, the ingress turn does not use this path.** Ingress calls
  `kb_client_memory_context_block` (ingress_preinject.c:243) →
  `memory.context_block` → `db2_kb_service_memory_context_block_json`
  (kb_service_backend_memory.c:1189) → `memory_assemble_context`
  (memory_assemble.c:1176), which returns a **pure text block with no ids and no
  scores** today. So the ingress memory surface emits nothing and must be taught
  to surface `(id, score)` — see D16. This was a real misattribution caught in
  R6; `memory_context.c:1106` is relevant only if the proactive path is *also*
  instrumented (a separate surface, not P1).
- **Document provenance partly exists.** `db2/kb_docs.h` already stores
  `content_hash` + `converter` + `converter_version` + `scope` per document, and
  the "always keep the whole origin artifact" rule means the origin is retained.
  `pgvec_transport.h` carries `content_hash` / `body_hash` on code embeddings,
  has `pgvec_code_exists_by_hash`, and code rows have a stable `node_key`. Layer
  1's data largely exists at *write* time — but is **not surfaced at read time**
  (`code_search_hit_t` carries only `{project, file_path, snippet, rank}`).

### The gap

- **The ingress / answer path writes no evidence.** `ingress_preinject_build`
  (`ingress_preinject.c:205`) pulls code hits (`kb_client_index_code_search` →
  `code_search_hit_t`) and a memory context block
  (`kb_client_memory_context_block`), formats both into the `<aimee-context>`
  envelope, and then **discards everything** (`free()`s the lot). The richest
  grounding event in the system — the context actually injected into the prompt
  — leaves no trace. This is the single most important thing to fix; everything
  else hangs off it.
- **The surfaced ids don't even reach the server.** `code_search_hit_t` has no
  id and no hash; `kb_client_memory_context_block` returns only a flat `block`
  string and drops the rest. So today there is literally nothing to bind at the
  ingress seam — the binding requires widening the `/v1` responses, not just
  adding a call (D1, D3).
- **Provenance is not surfaced at read time.** `content_hash` /
  `converter_version` / source mtime exist in storage but are not carried out
  through retrieval, so an answer cannot name the *version* of what grounded it.
- **No fidelity check is automatic.** Attributions today are written only when a
  caller explicitly invokes `memory.record_retrieval_outcome`. Nothing
  automatically checks a generated answer against the evidence it was given.

## Decisions (resolving roundtable R1)

These are deliberate choices, several of which answer "open questions for the
author." They are load-bearing for the design below.

- **D1 — Event emission is KB-side, never server-side.** The server process has
  no DB2 connection; `learning_evidence_*` only works in-process with the KB.
  So the `retrieval_event` is written by the KB handlers that assemble the
  context (`memory.context_block` and the code-search handler), which already
  hold the ids and a connection. They return an **opaque `retrieval_event_id`**
  in the `/v1` response. The server never touches raw ids — it threads one
  string.
- **D2 — One canonical event per turn; doc/code refs are AUDIT-ONLY.** The
  server mints a `turn_id` (UUID) and passes it to both KB calls; each KB
  handler **upserts** its surfaced refs into the single `retrieval_event` keyed
  by that `turn_id` (first writer creates, second appends). On the ingress turn
  the two writers are the **code-search handler** and the **context_block
  handler** (D16) — a *two-writer* merge; the proactive `memory_recall` event
  (memory_context.c:1106) is a **separate surface** that does not share the
  ingress `turn_id` and is left byte-identical. Crucially, **only memory-row
  attributions ever feed `db2_demotion_score`.** Memory `scope_id` stays a bare
  int64 string exactly as today; doc/code refs use namespaced keys
  (`code:<project>:<node_key>`, `doc:<doc_id>`). The guard is pinned to the
  `db2_demotion_candidates` **SQL**, not just the writer: add
  `AND scope_id ~ '^[0-9]+$'` so the scorer's `atoll(sid)` can never see a
  namespaced key and mint a spurious `row_id=0` candidate (demotion.c:363).
  Result: the id-space collision the roundtable flagged **cannot occur** — the
  scorer never sees a non-memory key. The standing invariant making this true:
  **`kind='retrieval_attribution'` is written only by the human/outcome path with
  bare-int64 memory `scope_id`s**; every machine/doc/code attribution uses a
  non-scored kind (`fidelity_attribution`, D4) and never `retrieval_attribution`.
  The `~ '^[0-9]+$'` guard is then belt-and-suspenders for any *future*
  human/outcome doc attribution. Generalising the scorer to typed
  keys is explicitly out of scope and not claimed.
- **D3 — The surfaced-ref payload is widened on the `/v1` contracts.**
  `code_search_hit_t` (and the code_search response JSON) gains a stable
  `node_key` + `content_hash`; `memory.context_block` returns the surfaced
  memory row ids and the `retrieval_event_id`. A ref is
  `{kind: memory|doc|code, id, source, version}` where `version` = the stored
  `content_hash`/`converter_version` (identity is `id`/`node_key`, **not** the
  hash — D5). Built with cJSON growable nodes, not fixed buffers (kills the
  4096/8192 truncation bug).
- **D4 — Fidelity is demotion-inert by *construction*: its verdicts go to a
  separate, non-scored artifact kind.** Rev. 3 claimed "fidelity never demotes"
  on the strength of "no negative verdict," but R3 showed that is false at the
  system level for two reasons: (i) `db2_demotion_score` demotes against a
  **relative per-class p10 percentile** (kb_demote.c:166-205), so even `accepted`
  rows (`verdict_sign=+1.0`) inflate the baseline and can push a rarely-surfaced
  row *below* the cut; and (ii) the scorer reads the most-recent `window_size`
  (≈64) `retrieval_attribution` rows for a `scope_id`, so a per-turn flood of
  machine verdicts can **evict a genuine human `contradicted`** out of the
  window — silently erasing the only authoritative demotion signal. The fix is
  structural, not policy: the fidelity judge writes its per-chunk verdicts to a
  **new `fidelity_attribution` artifact kind** and the answer-level rate to
  `fidelity_report` — **never** to `retrieval_attribution`. Since
  `db2_demotion_score`'s query is `WHERE kind='retrieval_attribution'`, judge
  output is *structurally invisible* to the scorer: no percentile shift, no
  window eviction, no path to demotion. The `retrieval_attribution` kind (and
  thus all demotion input) remains the exclusive territory of the human/outcome
  path (`memory.record_retrieval_outcome`). Judge verdicts stay
  `accepted`/`irrelevant` for readability, but their *kind* — not their sign —
  is what guarantees inertness. As defense-in-depth, judge writes also carry
  `operator_id="fidelity-judge"`. **Fidelity feeds nothing into demotion** — this
  is now true by table partition, resolving the relative-percentile and
  window-eviction blockers and the contradicting-surfaces open question at once.
- **D5 — Durable identity.** memory = int64 row id (today). code =
  `code:<project>:<node_key>` (`node_key` survives re-ingest; the pgvec row id
  and `content_hash` do not). doc = `doc:<doc_id>`. `content_hash` /
  `converter_version` / mtime are carried as the **version** of a ref (citation
  + staleness), not as identity.
- **D6 — P1 instruments every OpenAI-family path that calls `ingress_preinject`;
  only the Anthropic relay is out.** That is the *responses* path (streaming +
  buffered) **and** legacy `/v1/chat/completions` + `/v1/completions`
  (openai_chat.c:105/469/576/641) — they all call `ingress_preinject_build`
  today and therefore leak the exact evidence this proposal closes, so leaving
  them un-instrumented would defeat the goal (resolves R4 open-Q on
  chat-completions ambiguity). `anthropic_http.c` (`/v1/messages`, the Claude
  Code relay) does **not** call `ingress_preinject` at all — it is a pure
  streaming relay, **explicitly out of scope**, traced as `not_instrumented`;
  wiring ingress into it is the named follow-up P1b. No `/v1/audit/trace` ever
  claims a chain for an un-instrumented path, and never an empty-but-successful
  trace.
- **D7 — Staleness is a ranking heuristic, not a calibrated correctness knob.**
  No labelled corpus ties source-change-at-T to answer-invalidation-at-T+Δ, so
  we do not ship a "freshness half-life" as a correctness threshold. `aimee
  audit` exposes provenance/drift as a *read*; the requeue-by-drift logic lives
  in `memory_maintenance.c` and ranks re-ingest order — nothing more.
- **D8 — `/v1/audit` is a pure read surface: `trace` + `provenance` only.**
  `stale` is a maintenance concern (D7), not an auditability primitive, and
  lives in the maintenance path.
- **D9 — The audit key is a dispatch-minted `turn_id`, carried by a new header
  out-channel.** The HTTP header cannot be written from inside the handler: on
  the streaming Responses path `handle_responses_stream` (server_http.c:1161)
  flushes headers via `write_sse_headers` *before* `ingress_preinject_build`
  runs, and on the buffered path `send_response` (server_http.c:1016) writes a
  fixed `head[320]` with no handler hook (`route_handler_fn`,
  server_http_routes.inc:31, returns only `(int status, char *body)`). So the
  **dispatch layer mints a UUID `turn_id` before any header is written** — but
  only when `kb_evidence_emit_enabled` is on (D15): a default/off server mints
  nothing and advertises no header, so there is no turn to trace and no spurious
  `evidence_unavailable`. When on, that string is the audit key — not the
  KB-internal `retrieval_event_id`, which isn't known until after the KB calls
  return. Carrier, per path:
  - **Streaming:** `handle_responses_stream`/`handle_stream` write
    `X-Aimee-Retrieval-Event: <turn_id>` into the SSE header block up-front, and
    `openai_chat.c` also emits `turn_id` in the `response.created` SSE frame
    (clients that can't read the header still get it in-band).
  - **Buffered:** the OUT-channel for the header threads a response-meta
    out-param through `route_completion → send_response` (a
    `send_response_with_headers` variant) so the dispatch emits the header.
  `turn_id` is a **UUIDv4**, globally unique across restarts (reusing the
  existing artifact-id UUID generator).
  **Both an IN-channel and an OUT-channel are required, and they differ by
  path** (the R4 major): the header on the way *out* is not enough — `turn_id`
  must also get *into* the handler so `ingress_preinject_build` writes the event
  under the same id the header advertises. Per path:
  - **Streaming** in-channel: the stream-handler typedefs
    `server_http_responses_stream_fn` / `server_http_stream_fn`
    (server_http.h:151/166) carry it via a small ctx struct (`{int fd; const
    char *turn_id}`) replacing today's bare `int fd` ctx. The streaming forward
    callsite is **openai_chat.c:798** (`responses_stream_handler`, the dominant
    Codex/agent path) — it receives `turn_id` from this widened ctx, **not** from
    `route_req_t`. This is the single most important turn to instrument, so P1's
    wire-test asserts the header *and* the under-`turn_id` event specifically for
    a **streaming** responses turn (the buffered in-channel test does not cover
    this path). The same ctx makes the `turn_id` used for the KB upsert (798)
    identical to the one emitted in the `response.created` frame (816) — pinned as
    a test invariant.
  - **Buffered** in-channel: `server_http_completion_fn` is `(body, resp, cap)`
    (server_http.h:139) with no `turn_id` slot, and `route_completion`
    (server_http.c:884) / `server_http_route` (:965) collapse every call to
    `(body, resp, cap)`. So add a `turn_id` field to `route_req_t` and a
    parallel completion typedef that receives it — mirroring how `request_id` is
    already threaded from the connection handler to `send_response`. The
    buffered callsites that must forward it into `ingress_preinject_build` are
    openai_chat.c:105/469 (responses) and :576/641 (legacy chat/completions, D6).
  `ingress_preinject_build` no longer mints the id — it **receives `turn_id`** as
  a parameter and forwards it to both KB calls, which upsert the
  `retrieval_event` keyed by `turn_id` (D2, mechanism in D14).
  `/v1/audit/trace` is keyed on
  `turn_id`. Consequence (answering an R2 open question): the event is emitted
  for **every instrumented turn**, including tool-loop / `function_call` relay
  turns that never produce terminal text — so the injected context is always
  reconstructible by `turn_id`, even when fidelity is `not_evaluated` (D12).
- **D10 — P1 lands after PR #185 (cost-accounting), rebased on it.** PR #185 has
  already rewritten `ingress_preinject.c` (to ~441 lines); both touch the same
  file and are by the same author. This is purely merge-ordering — no functional
  overlap (cost-accounting emits no evidence) — but the order must be fixed to
  avoid a self-conflict.
- **D11 — The fidelity→{evidence-emit, ingress} dependency is enforced
  fail-closed at the read site, not by config validation.** `config_validate`
  (config.c:319) is per-key schema only — it has no cross-field capability and,
  in default non-strict mode, only warns. So instead: `fidelity_check`
  **no-ops and emits `not_evaluated`** whenever `kb_evidence_emit_enabled` or
  `ingress_preinject_enabled` is off — it can never run against absent evidence.
  A startup check additionally logs (and force-disables) an inconsistent combo —
  and **also** force-disables fidelity when the configured judge model is the
  same family as the answer model (D13's self-grading guard, enforced here at
  config time so a deployment can't silently invalidate the validity floor). The
  "startup validation refuses" language from rev. 2 is dropped; fail-closed at
  the read site is the mechanism.
- **D12 — Fidelity evaluates terminal-text turns only.** Tool-loop /
  `function_call` relay turns (the dominant Codex/agent path) emit an explicit
  `not_evaluated` marker, never silence — so the unsupported-claim-rate
  denominator is honest. The `retrieval_event` is still emitted for these turns
  (D9), so `/v1/audit/trace` still reconstructs what was injected.
- **D13 — The fidelity gate needs a labelled *validity* corpus, not just
  reproducibility.** Inter-run agreement measures only that the judge agrees
  with itself; an LLM judge can be reproducibly wrong about entailment, and a
  same-family judge grading its own grounding is self-referential. So P4 names a
  labelled `(claim, chunk, entails?)` gold set (a few hundred human-adjudicated
  pairs sampled from real turns) and pins a **validity** metric — judge-vs-gold
  precision/recall on unsupported-claim detection — *separately* from the
  reproducibility floor. That validity floor is pinned **numerically** in
  `flag-rollout-readiness.md` — **both a precision and a recall floor** on
  unsupported-claim detection (e.g. ≥0.8 each; exact numbers set in P4), since
  precision alone bounds false alarms but a poor-recall judge would silently
  under-report unsupported claims and the headline rate would read clean while
  hiding ungrounded answers. Not deferred indefinitely. A **claim** is one
  declarative assertion (sentence grain; segmentation refined in P3). The judge
  runs on a **different model family** than the answer model to break the
  self-grading loop (enforced at config time, D11). Crucially, **abstentions are
  recorded as their own bucket** in `fidelity_report` (supported / unsupported /
  abstained) — a below-floor-confidence judgment maps to `irrelevant`/no
  attribution but is *counted as abstained*, never folded into "supported," so an
  uncertain judge cannot silently inflate the supported rate.
  `fidelity_check_enabled` stays **Tier C / default-off** until that corpus
  exists and the validity floor is met.
- **D14 — The `turn_id` upsert is a pinned schema change, not a hand-wave.** The
  artifacts table has its only unique constraint on `id`, and
  `db2_artifact_write` does `INSERT … ON CONFLICT (id) DO NOTHING`
  (artifacts.c:168) — so `ON CONFLICT (turn_id)` cannot be expressed today.
  Concretely: (1) `ALTER TABLE artifacts ADD COLUMN IF NOT EXISTS turn_id text`
  (the existing migration pattern, schema.sql:465-481 — still **no new table**);
  (2) a **partial** unique index `CREATE UNIQUE INDEX … ON artifacts(turn_id)
  WHERE kind='retrieval_event'` (a global unique would collide across every
  other kind's NULL/default); (3) the second (racing) writer appends atomically
  via `… ON CONFLICT (turn_id) WHERE kind='retrieval_event' DO UPDATE SET
  payload = jsonb_set(payload, '{refs}', payload->'refs' || excluded.payload->'refs')`
  — a jsonb array concat under the row lock `ON CONFLICT` already takes, so the
  writers yield exactly one row with merged refs. **The two ingress KB calls run
  sequentially today** (code-search *then* memory.context_block inside
  `ingress_preinject_build`), so an ordered append already suffices; the
  `ON CONFLICT` path is retained as cheap insurance against future
  parallelisation, not as load-bearing complexity. `turn_id` is **nullable**, and
  a Postgres partial unique index does not constrain NULLs — so the many
  concurrent *un-instrumented* events (standalone/proactive recall, `turn_id`
  NULL) keep their fresh-id inserts and never collide; uniqueness is enforced
  only for non-NULL `turn_id` rows of `kind='retrieval_event'`. That is intended,
  not a gap. The proactive `memory_recall` writer (memory_context.c:1106) passes
  `turn_id`=NULL (it is a separate surface, D16) and is therefore **byte-identical
  to today** — it never lands in an ingress turn's event. (Reusing the empty
  `scope_id` column on `retrieval_event` rows is an alternative carrier, but a
  dedicated nullable `turn_id` column avoids coupling to the
  `scope_kind='system'`/empty-`scope_id` shape those rows have today.) The
  "exactly one event" test exercises the genuine **two-writer** ingress
  convergence (code-search + context_block under one `turn_id`), not two
  sequential calls; three-way (with proactive recall) is a forward-looking test
  gated on D16-style proactive instrumentation.
  The `ON CONFLICT … DO UPDATE … jsonb` append statement is **validated by a
  standalone DB test before P1.5 lands** (it is the only DB-specific complexity
  in the design and is split out of the P1 foundation for exactly this reason).
- **D15 — A fourth trace state, `evidence_unavailable`, for soft-failed writes.**
  The event writers fail *soft* (`learning_evidence_write_retrieval_event` returns
  -1, logs at DEBUG, swallows; the whole emit is `#ifdef`'d out under
  `AIMEE_DB2_DISABLED`). Per this deployment's ops history (DB2 "never started",
  server fast-fails `db2_ok=false`) that is a **real, recurring** runtime state.
  In it, an instrumented turn mints `turn_id`, emits the header, answers — and
  writes **no** `retrieval_event`. Keyed on that `turn_id`, `/v1/audit/trace`
  would otherwise return an empty-but-successful chain — exactly the
  falsely-empty success D6 forbids, in the most likely degraded state. Fix: the
  trace state is **computed at read time by row-presence keyed on `turn_id`**,
  not by a synchronous write rc — which is the only design consistent with the
  best-effort/async attribution writes (an async write's durability isn't known
  at request time anyway). `/v1/audit/trace` probes for the event row: present
  ⇒ `ok`; **present but only one surface's refs landed** (the other KB call
  soft-failed) ⇒ `ok` annotated `partial:[missing surface]`, so completeness is
  never overclaimed; **absent for a minted `turn_id`** ⇒ `evidence_unavailable`.
  The soft-fail itself is unchanged behaviour (`learning_evidence_write_*`
  returns -1, logs at DEBUG, no throw, no request-path retry; the async batch
  may retry). **The `turn_id` mint + header emission are gated on
  `kb_evidence_emit_enabled` (D9 amended).** Without this, a default server
  (`ingress_preinject_enabled` and `kb_evidence_emit_enabled` both default-OFF —
  config_fields.c:42) would advertise a header on every turn and then trace
  `evidence_unavailable` — making a healthy default box indistinguishable from a
  DB2-down one, the exact misleading-trace failure this proposal exists to
  prevent (R6 catch). With the gate, a default server mints no `turn_id` and
  there is simply no turn to trace; `evidence_unavailable` then unambiguously
  means "emission was on, we tried, the write didn't land." The four trace states
  are thus exhaustive: `ok` (with optional `partial`) · `not_instrumented` (path
  emits no evidence, or emission disabled, D6) · `not_evaluated` (fidelity
  skipped on a tool-loop turn, D12) · `evidence_unavailable` (emission on, minted
  `turn_id`, no row landed).
- **D16 — Bind the memory surface to the ids ACTUALLY injected, by instrumenting
  the real assembly path — NOT the explain pass, NOT `memory_recall`.** R6 caught
  that rev. 5 mis-cited the ingress memory binding point; R7 caught that rev. 6's
  fix was *still* non-binding. The ingress memory block comes from
  `memory_assemble_context` (memory_assemble.c:1176), which returns pure text
  with no ids. The tempting shortcut — `memory_assemble_context_explain`
  (:1230) — is **rejected as non-binding**: it produces the real text via
  `memory_assemble_context` (:1252) and then **separately re-runs** an
  *independent* candidate-scoring pass over `db2_memory_list_candidates(PRIMARY,
  …,200)` with its own formula (:1289) and its own cap (:1345). That parallel
  pass does **not** reproduce the actual selection inside
  `append_task_aware_context` (:684-998, its own cap + fallback), and scores
  **none** of the untasked-context sections (key facts / active tasks /
  constraints, :572-684) or the entity/graph expansion (:1197-1205) that also
  reach the envelope. Binding to the explain set would attest to a chunk set that
  can *differ* from what grounded the answer — the exact "looks-right vs
  is-right" failure the Goal forbids. **The committed mechanism:** thread an
  optional `(memory_id, score)` out-array through the real path
  (`memory_assemble_context` → `append_task_aware_context` /
  `append_untasked_context`) so the ids emitted into the `retrieval_event` are
  exactly the rows whose content reached the block, returned in
  `mem_context_result_t`. `memory_recall`/`memory_context.c:1106` is the
  *proactive/session* surface (dashboard_kb.c:370), not part of the ingress
  binding. The ingress memory surface is **heterogeneous**: rows with a backing
  `memory_id` — including the untasked KV sections (key facts / tasks /
  constraints), which *do* carry row ids — bind as `{id, version=updated_at,
  score}` via the out-array; entity/graph-expansion lines (:1204-1205) that have
  no single backing row are surfaced as an **explicitly-marked unbound
  sub-block**, never force-fit to a row id and never silently dropped. A P1.5
  test asserts every id in the `retrieval_event` corresponds to content actually
  present in the returned block, and that graph/entity sub-blocks are marked
  unbound.

## Design

### Layer 1 — versioned provenance (surface what storage already holds)

Widen the read path so each surfaced item is `(kind, id, source, version)`, not
just formatted text (D3, D5):

- `code_search_hit_t` / the code_search `/v1` response carry `node_key` +
  `content_hash`.
- `memory.context_block` returns the surfaced row ids + the
  `retrieval_event_id`.

Two consequences:

- **Citation:** `/v1/audit/{trace,provenance}` can name the exact version of
  each source.
- **Drift detection:** an item whose live source hash no longer matches its
  stored `content_hash` is *suspect*. The drift report and requeue live in
  `memory_maintenance.c` and rank re-ingest order (D7) — they are not a
  correctness verdict.

### Layer 2 — per-turn retrieval evidence (KB-side, one canonical event)

The mechanism (D1, D2, D9):

1. The **dispatch layer** (`handle_responses_stream`/`handle_stream` →
   `server_http_route`, server_http.c) mints a UUID `turn_id` **before any
   response header is written**, and writes `X-Aimee-Retrieval-Event: <turn_id>`
   into the header block up-front (streaming) or via the
   `send_response_with_headers` out-channel (buffered). This is the seam R2
   showed does not exist today; adding it is named P1 scope.
2. The dispatch passes `turn_id` into `ingress_preinject_build`, which forwards
   it to both `kb_client_index_code_search` and `kb_client_memory_context_block`;
   both `/v1` calls carry it KB-side.
3. KB-side, the two ingress handlers — the **code-search handler** and the
   **context_block handler** (whose memory ids/scores come from the
   real `memory_assemble_context` assembly path via an `(id, score)` out-array,
   D16) — **upsert** their surfaced typed
   refs into the single `retrieval_event` keyed by `turn_id`, via a new
   `learning_evidence_write_retrieval_event_typed` (typed refs) +
   `learning_evidence_write_retrieval_attribution_typed` (string `scope_id`).
   The existing int64 writers are left byte-identical and still serve standalone
   and **proactive** recall (`memory_context.c:1106`, which passes `turn_id`=NULL
   and never joins an ingress turn, D14/D16).

**The upsert is idempotent on `turn_id`** via the pinned schema change in D14
(nullable `turn_id` column + partial unique index `WHERE kind='retrieval_event'`
+ atomic jsonb-array append in `ON CONFLICT … DO UPDATE`). The two racing KB
writers (code-search + `memory.context_block`) yield exactly one row with merged
refs — never two. (Today `db2_demotion_retrieval_event_write` does a single
fresh-id insert, so this append-by-key path is genuinely new and is part of P1.)

**New signatures (D3, explicit per R3).** `turn_id` is appended **last** and is
**nullable** — `NULL` ⇒ no event emission, so every existing caller is
source-compatible and only the ingress path passes a non-NULL id:

```c
/* code search: keep (query, project, hits, max); APPEND nullable turn_id.
   Hits gain node_key + content_hash (version). */
typedef struct { char project[64]; char file_path[…]; char snippet[…];
                 int rank; char node_key[…]; char content_hash[65]; } code_search_hit_t;
int kb_client_index_code_search(const char *query, const char *project,
                                code_search_hit_t *hits, int max_hits,
                                const char *turn_id /* nullable */);

/* memory context: keep (query, block_type, limit); APPEND nullable turn_id +
   an out struct. Returns a cJSON-backed result; free with mem_context_result_free
   (cJSON_Delete on the backing node), NOT free(). */
typedef struct { char *block;                  /* formatted text, as today */
                 char  retrieval_event_id[40];  /* KB-internal id, opaque */
                 int64_t *surfaced_ids;
                 double  *surfaced_scores;      /* per-id recall score, mem_ctx.c:721 */
                 int n_surfaced; } mem_context_result_t;
int kb_client_memory_context_block(const char *query, const char *block_type,
                                   int limit, const char *turn_id /* nullable */,
                                   mem_context_result_t *out /* nullable */);

/* KB-side typed writers (string scope_id), alongside the byte-identical int64
   originals at learning_evidence.c:593/610. ref = {kind,id,source,version}. */
int learning_evidence_write_retrieval_event_typed(
        const char *turn_id, const char *role,
        const retrieval_ref_t *refs, int n_refs,   /* cJSON-serialised payload */
        char *out_event_id, size_t out_sz);
int learning_evidence_write_retrieval_attribution_typed(
        const char *retrieval_event_id, const char *scope_id /* "code:…"/"doc:…"/int64 */,
        const char *kind, const char *verdict, double weight, const char *operator_id);
```

`version` per kind (D5): doc/code = stored `content_hash`/`converter_version`;
memory rows have no native hash, so their version is `memory.updated_at` (a
monotonic stamp), `id` stays the int64 row id. **`score`** is carried per chunk
so the advertised `(id, source, version, score)` grain is honoured on *both*
surfaces: code from `code_search_hit_t.rank`, memory from the selection score computed on
the **real assembly path** and surfaced via the `(id, score)` out-array
(`surfaced_scores`, D16 — *not* the non-binding explain pass) — so the fidelity
layer has a per-chunk relevance signal. The two emitters differ (R8): the
**task-aware** branch (`append_task_aware_context`) carries the candidate score;
the **untasked** branch (`append_untasked_context`, key facts / tasks /
constraints) has no native relevance score, so those id-bound rows surface an
explicit `score=null` (a documented state, not an accidental gap) — the P1.5
test asserts the score field is present-and-typed (null or number) for every
bound row. Appending `turn_id` is a **signature change**,
not source-compatible — every callsite must be edited to pass `NULL` (a
mechanical, semantic no-op edit, but the old call shape will not build).
**Callsites to update** (grep `kb_client_memory_context_block` /
`kb_client_index_code_search` for the complete set): the ingress path
(`ingress_preinject.c`), `kb_client_search.c`, `agent_tools.c`,
`delegate_prompt.c`, `server_mcp.c:591`, and their tests.

One event per turn, one attribution row per surfaced chunk, kind-tagged. This is
the binding: *answer ↔ exactly these chunks, at these versions.* **Cost, stated
honestly (R8):** the `retrieval_event` upsert is **synchronous during context
assembly** — it runs before generation and adds one PG insert per ingress turn
(two on the merge), so it is *not* free on the request path, though it is off the
*generation* hot path. The per-chunk fidelity *attribution* writes and the judge
are the parts that are genuinely deferred/async. If the event-write latency
proves material, moving it fully async (write-behind keyed on `turn_id`, trace
reads tolerate the lag via D15's read-time row-probe) is the preferred mitigation
and is compatible with the rest of the design.

### Layer 3 — fidelity check (an answer-level quality signal, not a demotion lever)

A new `fidelity_check` module runs on the answer path once a **terminal text**
turn completes, behind a default-off flag. Tool-loop / `function_call` relay
turns (the dominant Codex/agent path) are **not** evaluated and emit an explicit
`not_evaluated` marker, so the metric denominator is honest — silence is never
read as "all supported" (D12). Given the answer and the turn's
`retrieval_event`, it:

1. Segments the answer into claims.
2. For each claim, attempts to bind it to the chunk(s) that entail it
   (claim-linked grain), or records it as **unsupported** at the answer level.
3. Emits, per chunk it relied on, a **`fidelity_attribution`** artifact (the new
   non-scored kind, D4) with verdict `accepted`/`irrelevant` and
   `operator_id="fidelity-judge"` — **never** a `retrieval_attribution`, so it is
   structurally invisible to `db2_demotion_score` (no percentile shift, no window
   eviction, no demotion path). The answer-level **unsupported-claim rate** goes
   to the non-scored `fidelity_report` artifact. Demotion input remains the
   exclusive territory of the human/outcome path.

The check is an LLM-judge entailment pass run **via a delegate** (no GPU), so it
stays off the hot answer path and can be deferred/batched. The delegate-as-judge
precedent establishes the **calling mechanism only** (transport/polling) — *not*
that the judge is fit to grade entailment; that fitness is established solely by
D13's not-yet-built validity corpus, and no rollout proceeds without it. Per that
precedent: foreground is blocked over `/v1`, so it runs **background + poll**
(success state `"done"`), `--persona` is required, judge `max_tokens` is set high
enough for reasoning models, and a failed / empty / timed-out delegate writes
**no** attribution and no report (a unit test pins this). The judge model + prompt are **pinned at zero temperature**; a
confidence/abstention below a floor maps to `irrelevant`/no row but is **counted
as `abstained`** in `fidelity_report` (its own bucket, distinct from
`supported`/`unsupported`, D13) so an uncertain judge never inflates the
supported rate. Unsupported-
claim rate becomes a first-class quality signal: a turn whose answer is largely
unsupported by its own injected evidence is the precise definition of a KB
answer you should not trust, and now it is *measurable* rather than invisible.

## Surface

- **`/v1/audit/trace`** — keyed on the caller-visible `turn_id` (the
  `X-Aimee-Retrieval-Event` value; the KB-internal `retrieval_event_id` is an
  implementation detail), return the chain: answer → injected chunks
  (kind, id, source, version, score) →
  per-claim fidelity report + accepted/irrelevant `fidelity_attribution`s. The
  trace status is one of four exhaustive states (D6/D12/D15): `ok` ·
  `not_instrumented` (path emits no evidence, e.g. the Anthropic relay) ·
  `not_evaluated` (fidelity skipped on a tool-loop turn) · `evidence_unavailable`
  (minted `turn_id`, but the event write did not durably land — e.g. DB2 down).
  **Never a falsely-empty success.**
- **`/v1/audit/provenance`** — given a doc/code/memory ref, return source,
  version, ingest time, and live-vs-stored hash status.
- **`aimee audit trace|provenance`** — thin-client CLI over the above, wired
  through the standard `/v1` route table.

(Drift/stale reporting + requeue is a maintenance feature, not an audit read —
D7/D8 — and lives in `memory_maintenance.c`.)

## Phasing (each independently shippable, default-off)

- **P1 — Reconstructibility foundation (single-writer).** Add the dispatch-layer
  `turn_id` mint + the `X-Aimee-Retrieval-Event` header in/out-channels (D9 — the
  seam does not exist today, both streaming and buffered); emit a **single-writer**
  `turn_id`-keyed `retrieval_event` from the memory surface; thread `turn_id`
  through `ingress_preinject_build` → `openai_chat.c`; `aimee audit trace` reads
  it back by `turn_id` with the four-state status (D15). This proves end-to-end
  reconstructibility with **no doc/code refs and no two-writer SQL** — the
  riskiest part is deferred. Memory int64 path stays byte-identical. Instruments
  every OpenAI-family ingress path (D6). Lands **after** PR #185 (cost-accounting),
  rebased on it (D10).
- **P1.5 — Typed refs + two-writer merge.** Widen the two `/v1` contracts to
  carry typed doc/code refs (D3); add the `..._typed` event **and attribution**
  writers and the D14 idempotent-upsert merge so the code-search surface joins
  the same turn event. This is the split-out of the only DB-specific complexity
  (per R4), kept off the foundation so P1 is independently demonstrable.
- **P1b — Anthropic relay instrumentation (follow-up).** Wire `ingress_preinject`
  into `/v1/messages` *first*, then bind evidence there. Separate slice; until
  it lands, Anthropic turns trace as `not_instrumented`.
- **P2 — Versioned provenance + drift ranking.** Carry `content_hash` /
  `converter_version` / mtime through retrieval; `aimee audit provenance`;
  `memory_maintenance` requeue ranked by drift (D7).
- **P3 — Fidelity check.** `fidelity_check` delegate pass on terminal-text
  turns; `accepted`/`irrelevant`-only attributions (D4); answer-level
  `fidelity_report`; unsupported-claim-rate metric.
- **P4 — Calibrate + gate.** Build the labelled `(claim, chunk, entails?)` gold
  corpus (D13) and pin a **validity** metric (judge-vs-gold precision/recall on
  unsupported-claim detection) — *separately* from the inter-run
  re-segmentation **reproducibility** floor (which alone is insufficient: a
  reproducible judge can be reproducibly wrong). Pin the judge model
  (different family from the answer model) / prompt / abstention floor. Run
  `kb_evidence_emit_enabled` and `fidelity_check_enabled` through the
  rollout-readiness 6-criterion bar; `fidelity_check_enabled` stays **Tier C /
  default-off** until the corpus exists and the validity floor is met.

## Flags

- **`kb_evidence_emit_enabled`** — gates P1/P2 evidence emission (default off).
  It is **observation-only for the answer**: the **injected context and answer
  text are byte-identical** with it on or off (the precise neutrality claim —
  R8), so the rollout bar is **cost/latency neutrality**, not a correctness A/B
  arm. The only on-wire delta is the `X-Aimee-Retrieval-Event` header/frame, and
  the only added work is the synchronous event upsert (above) — both are exactly
  what the cost-neutrality bar measures. This keeps the missing correctness arm
  off P1/P2's critical path (resolves an R4 open-Q).
- **`fidelity_check_enabled`** — gates P3 (default off). It has a **hard
  dependency** on `kb_evidence_emit_enabled` *and* `ingress_preinject_enabled`
  (the events it binds against exist only when pre-injection runs). The
  dependency is enforced **fail-closed at the read site** (D11), not by
  `config_validate` (which is per-key only and merely warns in non-strict mode):
  `fidelity_check` no-ops and emits `not_evaluated` whenever a dependency is off,
  so it can never run against absent evidence; a startup check additionally logs
  and force-disables an inconsistent combo. The A/B isolation needed for
  readiness-criterion 2 pins ingress on. Both flags are registered in
  `docs/validation/flag-rollout-readiness.md` with tiers + pinned numeric
  criteria before any rollout run.

## Non-goals

- No governance/compliance framing, control catalogues, or regulator-facing
  reports. This is about whether the *knowledge* is correct, not about mapping
  the system to external controls.
- No new datastore, service, or artifacts table. Evidence reuses the charter
  artifacts table; the fidelity judge reuses the delegate path.
- **No generalisation of `db2_demotion_score` to typed keys.** Doc/code
  attributions are audit-only (D2); only memory rows feed demotion, exactly as
  today. Scoring non-memory surfaces is possible future work, not this proposal.
- **Fidelity never demotes — by construction** (D4). Judge verdicts are written
  to the `fidelity_attribution` kind, which `db2_demotion_score`
  (`WHERE kind='retrieval_attribution'`) structurally never reads — so they cause
  neither a percentile shift nor window eviction. The only demotion inputs remain
  the human/outcome path's verdicts. (This is a stronger guarantee than rev. 3's
  "no negative verdict," which R3 showed was insufficient under a relative cut.)
- The chain establishes **verifiability**, not automatic truth — a source can
  itself be wrong. But verifiability is the precondition for every correctness
  judgment downstream, human or automated, and is what makes a wrong answer
  *localizable* and therefore fixable.

## Risks / honest limits

- **P1+P2 stand alone.** If P3 fidelity only fires on terminal-text turns and
  no-ops on the dominant tool-loop path, the capstone is partial — but the
  provenance + per-turn evidence ledger (P1+P2) is independently valuable
  (reconstructible answers, drift detection, citation) and is the honest floor
  of this proposal. P3 is additive, not load-bearing for P1/P2's value.
- **`content_hash` churns on every edit.** That is why identity is `node_key` /
  `doc_id`, not the hash (D5); the hash is the version stamp only.

## Tests

- Unit: the KB-side `..._typed` event+attribution writers persist mixed
  memory/doc/code refs, including a non-memory attribution whose namespaced
  `scope_id` (`code:<project>:<node_key>`) survives the round-trip;
  **interleaved** (not sequential) `..._typed` calls with the same `turn_id`
  produce **exactly one `retrieval_event`** holding the union of refs (the D14
  partial-unique-index + jsonb-append race guard); `db2_demotion_candidates`
  **excludes** namespaced scope_ids via the `~ '^[0-9]+$'` SQL filter; **a flood
  of judge `fidelity_attribution` rows for a row_id does NOT evict or outweigh a
  prior human `contradicted` `retrieval_attribution` for that row** (D4 — assert
  via `db2_demotion_score` that the human verdict still drives the score); the
  fidelity judge writes **only** `fidelity_attribution`, never
  `retrieval_attribution` (assert `db2_demotion_score` is unchanged by any number
  of judge writes); the int64 memory path is byte-identical to today (regression
  guard for `memory_context.c:1106` + `kb_service_agent.c`); cJSON ref array
  round-trips a payload that would have overflowed the old 4096/8192 buffers; the
  **two-writer ingress convergence** (code-search + context_block under one
  `turn_id`) yields exactly one event (D14); a failed/empty delegate
  writes no attribution and no `fidelity_report`; an **abstaining** judge is
  counted as `abstained`, not `supported`, in `fidelity_report` (D13); a
  tool-loop turn yields `not_evaluated`; `fidelity_check` no-ops + emits
  `not_evaluated` when `kb_evidence_emit_enabled`/`ingress_preinject_enabled` is
  off (D11); a DB2-down ingress turn traces as `evidence_unavailable` and a
  one-surface-soft-fail traces as `ok`+`partial` — both **computed at read time
  by row presence**, never empty-success (D15); a same-family judge config
  force-disables fidelity (D11/D13); **a turn with emission DISABLED mints no
  `turn_id`/header and never traces `evidence_unavailable`** (D9/D15 gate — the
  default-server / DB2-down disambiguation).
- Integration: the `X-Aimee-Retrieval-Event` header actually appears **on the
  wire** for both a **streaming** (openai_chat.c:798 ctx path) **and a buffered**
  Responses turn — and the event is written under the **same** `turn_id` the
  header advertises (the R4/R5 in-channel test, not merely that the id was
  computed); the streaming KB-upsert id matches the `response.created` id; that
  `turn_id` → `/v1/audit/trace` reconstructs answer → chunks → fidelity report;
  a legacy `/v1/chat/completions` turn is instrumented (D6); an Anthropic-relay
  turn traces as `not_instrumented`; `aimee audit` CLI over `/v1`.
- Reuse the demotion spine's existing test idiom (`test_demotion.c`).

**Implementation notes (pinned in the implementing PR, not the design):** the
exact C declarations for `learning_evidence_write_retrieval_{event,attribution}_typed`
and `retrieval_ref_t` land in `learning_evidence.h`; the buffered completion
typedef and the `route_req_t.turn_id` field land in `server.h`/`server_http.h`;
the `retrieval_event` payload JSON schema (`{refs:[{kind,id,source,version,score}]}`)
is fixed in the writer + the standalone jsonb-append DB test (D14); and the D2
standing invariant (only the human/outcome path writes `kind='retrieval_attribution'`)
is enforced by a CI/grep guard plus a `db2_demotion_candidates` regression test,
so a future non-memory `retrieval_attribution` writer is caught at build time.
