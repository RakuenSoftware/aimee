# Collapse Anchors — Six Binding Decisions for Guardrail-Collapse Phases 1–5

**Phase:** 0 — merged anchors document.
**Scope:** carries the six binding decisions that gate Phase 1+ implementation.
Each decision is backed by a file:line citation from the worktree's indexed
repository and (where applicable) is paired with the prerequisite phase that
introduces any missing substrate.
**Status:** REVIEW-CORRECTED / VALIDATION-PENDING MERGE. Phase 1 implementation
**must not start** until this document is merged; repository workflow metadata must
record that merge before Phase 1 is considered accepted.

This document is the contract — companion to `collapse_recon.md` (path-by-path
dispatch recon) and `sampling_capability_matrix.md` (per-backend sampling
matrix). It is the only file that Phase 1+ is allowed to read first.

---

## Decision 1 — Path-divergence verdict (relay convergence)

> **PATHS DIVERGE.** `collapse_recon.md` verifies the typed relay only for the
> IR-enabled `/v1/messages` branch. Responses, Chat, Webchat, Delegate, and
> Roundtable are direct compute-then-chunk emitters, polls, or non-streaming
> proxies; none of them reaches `aimee_delta_t` today. Phase 2 is split per
> handler/relay. Missing decoder/renderer/route-trace substrate is the Phase
> 2.0–2.4 prerequisite surface.

The verified type remains `aimee_delta_t` at `src/headers/aimee_ir.h:177`,
with `aimee_delta_type_t` at `:167`; it is not treated as evidence of
reachability on divergent paths.

## Decision 2 — Config source of truth and namespace

> **CONFIG SOURCE OF TRUTH = `config_t` struct in `src/modules/config/config.h`.**
>
> - Struct: `config_t` lives in `src/modules/config/config.h` (verified for
>   `guardrails_*` fields at `src/modules/config/config.h:1400-1408`).
> - Config-load entry: `config_load` at `src/modules/config/config.c:1109`.
> - Section parser: `config_parse_guardrails_section` at
>   `src/modules/config/config_sections.c:1264`.
> - Doc-generator: `scripts/gen-reference-docs.py` (the section-parser entry
>   lives at `scripts/gen-reference-docs.py:417` — `parse_config_sections`;
>   it scans `src/modules/config/config*.c` for `cJSON_GetObjectItemCaseSensitive`
>   calls to discover sections and keys, and the `guardrails` section row
>   description lives at `scripts/gen-reference-docs.py:340`; field descriptions
>   for `guardrails_*` keys live at `scripts/gen-reference-docs.py:150-157`).
>   The template it consumes is `docs/gen/configuration.md` (auto-generated).
>   The `docs-gen` Makefile target invokes it at `src/Makefile:1378-1386`;
>   `docs-gen-check` detects drift at `:1384`.
> - Naming/defaults convention: `*_enabled` flags are `int` typed
>   (e.g., `guardrails_blast_radius_advisory_enabled` at
>   `src/modules/config/config.h:1408` is `int`); section parsers coerce
>   `cJSON_IsBool` via the documented `cJSON_IsTrue(item) ? 1 : 0` pattern
>   (verified at `src/modules/config/config_sections.c:1317`).
>
> **NAMESPACE for new collapse fields = `guardrails.collapse.*` (preferred).**
>
> New collapse-phase fields land under the existing `guardrails` section
> (parser `config_parse_guardrails_section`,
> `src/modules/config/config_sections.c:1264`) under a `collapse`
> sub-object — e.g. `guardrails.collapse.enabled`,
> `guardrails.collapse.mode`, `guardrails.collapse.shadow_enabled`,
> `guardrails.collapse.bucket_labels[]`. This is the **preferred** placement
> because it re-uses an existing parser and the doc-generator already
> handles the `guardrails` section.

> **Alternative placement (NOT preferred, requires Phase 1.0 prerequisite):**
> Adding fields under a separate top-level `collapse` section would require
> a new `config_parse_collapse_section` parser AND a doc-generator extension
> so `scripts/gen-reference-docs.py:417` (`parse_config_sections`) recognises
> the new section. That is the **generator-extension prerequisite** and
> belongs to Phase 1.0 (parses the new section + regenerates
> `docs/gen/configuration.md`).

**Pairing:** preferred placement = no Phase 1.0 prerequisite (uses the
existing `guardrails` parser). Alternative = Phase 1.0 prerequisite
(doc-generator extension to `scripts/gen-reference-docs.py:417`).

---

## Decision 3 — Relay choke point with verified symbols

> **RELAY CHOKE POINT = `aimee_delta_t` at `src/headers/aimee_ir.h:177`.**
>
> - **Producer (backend → IR):** `openai_chunk_to_deltas` at
>   `src/server/aimee_ir_stream.c:42`; `bedrock_converse_stream_to_deltas`
>   at `src/server/aimee_ir_stream.c:220`. State types:
>   `openai_stream_state_t` (`src/headers/aimee_ir_stream.h:30`) and
>   `converse_stream_state_t` (`src/headers/aimee_ir_stream.h:54`).
> - **Field on the struct carrying the bytes:** `aimee_delta_t.text_delta`
>   at `src/headers/aimee_ir.h:184` (typed `const char *`; BORROW lifetime).
> - **Consumer (IR → frontend):** `anthropic_delta_emit` at
>   `src/server/aimee_ir_stream.c:539` (this is the Anthropic-shape
>   renderer — `delta_build_events` writes
>   `delta.type = "text_delta"`, `delta.text = d->text_delta` at
>   `src/server/aimee_ir_stream.c:445`).
> - **Live wire-up today:** `messages_stream_ir_relay` at
>   `src/server/anthropic_http.c:973` (dispatcher); gate at
>   `src/server/anthropic_http.c:1251` (`aimee_ir_stream_relay_enabled()`,
>   default-OFF). The same env (`AIMEE_IR_STREAM_RELAY`) is referenced via
>   `aimee_ir_stream_relay_enabled()` at `src/server/aimee_ir_serve.c:30`.
> - **Verified enum members observed on the live path:**
>   `AIMEE_DELTA_TURN_START`, `AIMEE_DELTA_BLOCK_START`, `AIMEE_DELTA_BLOCK_DELTA`
>   (carries `text_delta`), `AIMEE_DELTA_BLOCK_STOP`, `AIMEE_DELTA_TURN_STOP`,
>   `AIMEE_DELTA_ERROR` (one per `src/headers/aimee_ir.h:168-174`).
> - **Reachable on which path today:** only `/v1/messages` (verified
>   `messages_stream_ir_relay` gated by `aimee_ir_stream_relay_enabled()` at
>   `src/server/anthropic_http.c:1251`). Responses, Chat, Webchat, Delegate,
>   Roundtable are NOT reliable on this choke point without per-path
>   substrate (see Decision 1 and `collapse_recon.md` §2.2–§2.6).
> - **Phase 2 missing substrate (per path):**
>   - `openai_responses_chunk_to_deltas` (mirror of `openai_chunk_to_deltas`)
>     for `/v1/responses` — Phase 2.0.
>   - OpenAI-Chat-shape frontend emitter (parallel to `anthropic_delta_emit`)
>     for `/v1/chat/completions` — Phase 2.1.
>   - Webchat mirror observability (no SSE on the surface; tap upstream) —
>     Phase 2.2.
>   - Delegate provider observer (inside `agent_dispatch_one` or equivalent)
>     — Phase 2.3.
>   - Roundtable panel-verdict hook — Phase 2.4.

**Pairing:** each path's missing substrate is paired with its prerequisite
slice (Phase 2.0, 2.1, 2.2, 2.3, 2.4). Verified file:line for the work is
`src/server/aimee_ir_stream.c` (new header add to
`src/headers/aimee_ir_stream.h` alongside `anthropic_delta_emit` at :85,
new emitter function at the bottom of `src/server/aimee_ir_stream.c` after
`:539`).

---

## Decision 4 — Sampling matrix scope (Phase 4 surface)

> **PHASE 4 SCOPE = `sampling_capability_matrix.md` columns.**
>
> Phase 4 collapses per-backend sampling knob duplication by moving knobs
> onto the canonical typed-sampling surface at
> `src/headers/aimee_ir.h:118-132` (`temperature`, `has_temperature`, `top_p`,
> `has_top_p`, `top_k`, `has_top_k`, `max_tokens`, `has_max_tokens`,
> `stop_sequences[]`, `n_stop`).
>
> Per-backend honours are enumerated in `sampling_capability_matrix.md` §1
> matrix. Knobs already modelled on the IR (✅/⚠ rows) need no Phase 4.0
> prerequisite. The ❌ rows in the matrix (`presence_penalty`,
> `frequency_penalty`, single-string `stop: "."` normalization,
> `previous_response_id`) are the Phase 4.0 prerequisite substrate.

**Pairing (each ❌ row → Phase 4.0 slice):**
- ❌ `presence_penalty`/`frequency_penalty` on the IR → Phase 4.0 type-add
  (extend `aimee_request_t` in `src/headers/aimee_ir.h`; mirror
  `has_temperature` precedent at `:119`).
- ❌ `stop` string-or-array normalization → Phase 4.0 normalizer slice
  (extends `aimee_request_t.stop_sequences[]` consumer at `:131`).
- ❌ `previous_response_id` thread key for Responses → Phase 4.0
  continuation-reference slice (extends `aimee_request_t.metadata` at
  `:134-138` or adds a new typed field; storage substrate already exists at
  `src/server/openai_responses_store.c`).

---

## Decision 5 — Promotion-gate substrate

> **MISSING SUBSTRATE. Phase 5.0 introduces bucketing and promotion control.**
>
> The only verified calibration-bucket code is the standalone
> `scripts/calibration-sidecar.py` script (bayanesian bucket code at
> `scripts/calibration-sidecar.py:139-162`, default config at `:30-32`,
> `fitted_buckets` assembly at `:160-188`). It is a sidecar that takes
> JSON-on-stdin and writes JSON-on-stdout; it has no verified caller or
> consumer in the C pipeline. There is no verified shadow→canary→default
> transition controller anywhere in `src/` or `scripts/`. Environment gates
> alone (e.g. `aimee_ir_stream_relay_enabled()` at
> `src/server/aimee_ir_serve.c:30`) are NOT a promotion controller.
>
> Phase 5.0 must add bucket assignment, persisted calibration consumption
> (e.g. an `audit_worm_read_page`-backed outcome histogram), and explicit
> shadow→canary→default transition state before Phase 5 wires the collapse
> scanner into it. Pairing: Phase 5.0 introduces bucketing +
> transition-state; Phase 5 wires the collapse scanner into it.

---

## Decision 6 — Audit-store schema and new-record decision

> **AUDIT STORE EXISTS. Both schemas verified. Collapse_event will be a
> STRUCTURED EXTENSION on the existing `audit_event` row — NOT a new record
> type. PREREQUISITE: Phase 2.3.0 introduces the writer and lifecycle
> integration; the convention is implementation, not doc-only.**

Verified anchor lines (every function is implemented and callable today):

- **WORM schema (SQLite `audit_event` table):**
  `src/modules/audit/audit_worm.c:33` → `WORM_SCHEMA_SQL` (the `CREATE TABLE
  audit_event ...` statement). Columns: `seq, ts, actor_role,
  actor_principal, action, subject, verdict, detail, key_id, prev_hash,
  row_hash`. WORM triggers at `src/modules/audit/audit_worm.c:46-48`
  (`audit_event_no_update`, `audit_event_no_delete`). Schema-apply call
  site at `src/modules/audit/audit_worm.c:106` (`sqlite3_exec(db,
  WORM_SCHEMA_SQL, ...)`).
- **WORM write API (registration/lifecycle):** `audit_worm_append` at
  `src/modules/audit/audit_worm.c:135` (header signature at
  `src/modules/audit/audit_worm.h:50`; returns 0 on success, -1 on failure;
  fsync-durable before return per header docstring at `:60-65`).
- **Chain key (registration):** `audit_worm_chain_key_load` at
  `src/modules/audit/audit_worm_chain.h:45` (loads
  `$AIMEE_HOME/.audit-chain-key`); consumed at
  `src/modules/audit/audit_worm.c:237` (inside `audit_worm_checkpoint`)
  and again at `:409` (inside `audit_worm_seal`).
- **Checkpoint (sealing path #1):** `audit_worm_checkpoint` at
  `src/modules/audit/audit_worm.c:233` (header signature at
  `src/modules/audit/audit_worm.h:64`).
- **Seal (sealing path #2):** `audit_worm_seal` at
  `src/modules/audit/audit_worm.c:508` (header signature at
  `src/modules/audit/audit_worm.h:87`); calls `audit_worm_checkpoint` at
  `:510` to attest the head before VACUUM-into-snapshot.
- **Verify (sanity-check API):** `audit_worm_verify_chain` at
  `src/modules/audit/audit_worm.h:57` (returns 0 if intact, -1 on first
  break); full verify at `audit_worm_verify` (header at `:76`,
  implementation at `src/modules/audit/audit_worm.c:559`).
- **Query surface:** `audit_worm_read_page` at
  `src/modules/audit/audit_worm.c:587` (header signature at
  `src/modules/audit/audit_worm.h:93`; returns newest-first cJSON array,
  caller owns).
- **Dual-write gate:** `audit_worm_enabled` config key (default-off) is
  the dual-write switch (the KB-side implementation at
  `src/db2/kb_audit_worm.c:24-39`).
- **Metric-snapshot precedent:** `audit_worm_metric_snapshot` at
  `src/modules/audit/audit_worm.h:97` (called from
  `src/modules/audit/audit_worm.c:638` with `action="metric.snapshot"`);
  proves the existing pattern for "an opaque metric row, verifiably
  hash-chained".

**Decision rationales:**

- The WORM store already enforces immutability, hash-chaining, and
  sealing for the `audit_event` table; adding a new record type requires
  a sibling schema, its own hash-chain primitive, and a new checkpoint
  table — duplicated work for an operational record that has identical
  storage-shape requirements.
- The `db1.lifecycle_event` table does not carry a hash chain — it is
  signed-by-an-actor workflow state, not a security log. Using it would
  weaken the collapse_event's tamper-evidence story.
- Extending the WORM `action` field with a versioned kind prefix
  (`guardrail.collapse.v1.<verb>`) keeps the existing
  `audit_worm_read_page` query surface unchanged; consumers filter on
  `action LIKE 'guardrail.collapse.%'`.

**Pairing (Prerequisites):**
- ✅ Schema — no prerequisite (existing `audit_event` table is the substrate).
- ✅ Write API — no prerequisite (`audit_worm_append` at
  `src/modules/audit/audit_worm.c:135` is the call site).
- ✅ Checkpoint / seal — no prerequisite (`audit_worm_checkpoint` at
  `src/modules/audit/audit_worm.c:233` and `audit_worm_seal` at
  `src/modules/audit/audit_worm.c:508` are the lifecycle integration points).
- ✅ Query surface — no prerequisite (`audit_worm_read_page` at
  `src/modules/audit/audit_worm.c:587` is the consumer-side read).
- ❌ **Phase 2.3.0 implementation work:** the writer is NOT yet wired — a
  caller in the collapse scanner must invoke `audit_worm_append(...)` with
  `actor_role = "guardrail.collapse"`, `action = "guardrail.collapse.v1.<verb>"`
  (`verb` ∈ `{observe, enforce, escalate, shadow.mismatch, demote}`),
  `subject = "<routing_key>"`, `verdict = "<verdict_enum_name>"`,
  `detail = "{\"schema\":\"v1\",...}"`. The registration and lifecycle
  integration must also be wired at the existing lifecycle boundary (e.g.
  `audit_worm_checkpoint` invoked at scan-end and `audit_worm_seal` invoked
  at operator-triggered snapshot). This is real implementation, not a
  doc-only convention.
- (Optional) Reader hook for metrics: extend `dump_metric_audit_action_breakdown(...)`
  via the existing `metric.snapshot` precedent at
  `src/modules/audit/audit_worm.c:638`; Phase 2.3.0 prerequisite if the
  Phase 5 promotion gate consumes audit-derived telemetry, otherwise Phase 5.0.

---

## Cross-references to the companion documents

- `collapse_recon.md` §1: binding decision (this doc §1 mirrors it, with citations).
- `collapse_recon.md` §2: handler + SSE/delta emitter per path.
- `collapse_recon.md` §3: text_delta production/consumption lines.
- `collapse_recon.md` §4: scanner precedents.
- `sampling_capability_matrix.md` §1: per-backend matrix.
- `sampling_capability_matrix.md` §3: Phase 4.0 prerequisites.

## Acceptance check (the merge gate)

- [x] Six binding decisions, each with file:line citations.
- [x] Every prerequisite-missing-substrate decision paired with its prerequisite phase (§2: doc-generator → Phase 1.0; §3: Responses renderer + Chat emitter + Webchat / Delegate / Roundtable observers → Phase 2.0–2.4; §4: sampling types → Phase 4.0; §5: bucketing → Phase 5.0; §6: audit writer + lifecycle integration → Phase 2.3.0).
- [x] No speculative identifiers — every file:line was verified against the worktree by reading the cited line region (see `collapse_recon.md` §0 convention).
- [x] Phase 1 implementation does not start until this is merged.
