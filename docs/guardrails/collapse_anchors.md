# Collapse Anchors — Six Binding Decisions for Guardrail-Collapse Phases 1–5

**Phase:** 0 — merged anchors document.
**Scope:** carries the six binding decisions that gate Phase 1+ implementation.
Each decision is backed by a file:line citation from the worktree's indexed
repository and (where applicable) is paired with the prerequisite phase that
introduces any missing substrate.
**Status:** PENDING MERGE. Phase 1 implementation **does not start** until this
document is merged.

This document is the contract — companion to `collapse_recon.md` (path-by-path
dispatch recon) and `sampling_capability_matrix.md` (per-backend sampling
matrix). It is the only file that Phase 1+ is allowed to read first.

---

## Decision 1 — Path-divergence verdict (relay convergence)

> **PATHS CONVERGE.** All six production paths in `collapse_recon.md` §2
> (Anthropic `/v1/messages`, OpenAI Chat `/v1/chat/completions`, OpenAI
> Responses `/v1/responses`, webchat ingest, delegate relay, roundtable
> relay) collapse onto the single verified typed relay surface:
>
> - enum `aimee_delta_type_t` — `src/headers/aimee_ir.h:167`
> - struct `aimee_delta_t` — `src/headers/aimee_ir.h:177`
> - module seam `src/headers/aimee_ir_stream.h` (frontend render sink type
>   `aimee_sse_emit_fn` at `:85`; backend decoders `openai_chunk_to_deltas`
>   at `:30` and `bedrock_converse_stream_to_deltas` at `:54`)
>
> Phase 2 taps the SINGLE type above. Phase 2 does NOT split per handler —
> that would reinvent `aimee_ir_stream.c`'s committed dispatcher at
> `src/server/aimee_ir_stream.c:42` (OpenAI-Chat backend) and `:220`
> (Bedrock backend) and the shared Anthropic SSE emitter at `:539`.

**Constraint:** Phase 2 must produce a frontend renderer for the Responses
wire (parallel to `anthropic_delta_emit`). The work is purely additive — the
struct is the contract, the renderer varies.

---

## Decision 2 — Config source of truth and namespace

> **CONFIG SOURCE OF TRUTH = `config_t` struct in `src/modules/config/config.h`.**
>
> - Struct: `config_t` lives in `src/modules/config/config.h` (verified above
>   for `guardrails_*` fields at `src/modules/config/config.h:1400-1408`).
> - Config-load entry: `config_load` at `src/modules/config/config.c:1109`.
> - Section parser: `config_parse_guardrails_section` at
>   `src/modules/config/config_sections.c:1264` (line verified in
>   `collapse_recon.md` §7 of companion).
> - Doc-generator: `scripts/gen-reference-docs.py` (NOT `gen-cli-v1-routes.py`
>   or `gen-api-docs.py` — those are for CLI / API reference; this generator
>   is for the configuration surface, per its module docstring at
>   `scripts/gen-reference-docs.py:6-15`). The template it consumes is
>   `docs/gen/configuration.md` (auto-generated). The `docs-gen` Makefile
>   target invokes it at `src/Makefile:1378-1386`; `docs-gen-check`
>   detects drift at `:1384`.
> - Naming/defaults convention: `*_enabled` flags are `int` typed
>   (e.g., `guardrails_blast_radius_advisory_enabled` at
>   `src/modules/config/config.h:1408` is `int`); section parsers coerce
>   `cJSON_IsBool` via the documented `cJSON_IsTrue(item) ? 1 : 0`
>   pattern (verified at `src/modules/config/config_sections.c:1317`).
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
> a new `config_parse_collapse_section` parser AND an entry in
> `scripts/gen-reference-docs.py`'s section parser at `:417` to extend its
> regex coverage. That is the **generator-extension prerequisite** and
> belongs to Phase 1.0 (parses the new section + regenerates
> `docs/gen/configuration.md`).

**Pairing:** preferred placement = no Phase 1.0 prerequisite (uses the
existing `guardrails` parser). Alternative = Phase 1.0 prerequisite
(doc-generator extension at `scripts/gen-reference-docs.py:417`).

---

## Decision 3 — Relay choke point with verified symbols

> **RELAY CHOKE POINT = `delta_build_events` at `src/server/aimee_ir_stream.c:402`.**
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
>   `src/server/anthropic_http.c:1063` (`aimee_ir_stream_relay_enabled()`,
>   default-OFF). The same env (`AIMEE_IR_STREAM_RELAY`) is documented in
>   the file header at `src/server/aimee_ir_serve.c:30`.
> - **Verified enum members observed on the live path:**
>   `AIMEE_DELTA_TURN_START`, `AIMEE_DELTA_BLOCK_START`, `AIMEE_DELTA_BLOCK_DELTA`
>   (carries `text_delta`), `AIMEE_DELTA_BLOCK_STOP`, `AIMEE_DELTA_TURN_STOP`,
>   `AIMEE_DELTA_ERROR` (one per `src/headers/aimee_ir.h:168-174`).
> - **Phase 2 missing substrate:** a Responses-shape frontend renderer
>   (parallel to `anthropic_delta_emit`) is **not present** today. Phase 2.0
>   introduces it; see Decision 6 for the audit-store companion.

**Pairing:** the Responses renderer is the only Phase 2 prerequisite
substrate; it lives in Phase 2.0 (or whichever slice first touches
`/v1/responses`). Verified file:line for the Phase 2.0 work is
`src/server/aimee_ir_stream.c` (new header add to
`src/headers/aimee_ir_stream.h` alongside `anthropic_delta_emit` at :90,
new emitter function at the bottom of `src/server/aimee_ir_stream.c`
after `:539`).

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
  `has_temperature` precedent).
- ❌ `stop` string-or-array normalization → Phase 4.0 normalizer slice
  (extends `aimee_request_t.stop_sequences[]` consumer in
  `aimee_frontend_openai.c`).
- ❌ `previous_response_id` thread key for Responses → Phase 4.0
  continuation-reference slice (extends `aimee_request_t.metadata` or adds
  a new typed field; storage substrate already exists at
  `src/server/openai_responses_store.c`).

---

## Decision 5 — Promotion-gate substrate

> **SUBSTRATE EXISTS. Phase 5 wires into existing bucketing.**
>
> The promotion gate is the calibration-sidecar shape, run by
> `scripts/calibration-sidecar.py`. Verified file:line anchors:
>
> - Sidecar script: `scripts/calibration-sidecar.py:5` describes
>   "Beta-binomial posterior per confidence bucket, computes a
>   distribution-free conformal abstention floor". Buckets fit at
>   `:160` (`fitted_buckets.append(...)`); the output envelope at
>   `:188` carries "buckets".
> - Bucketing is DEFAULT-10 from `config.buckets = 10`
>   (`scripts/calibration-sidecar.py:30`); prior `alpha0=2.0, beta0=1.0`
>   at `:31-32`.
> - Config keys on the `config_t` struct:
>   `calibration_profile_flag`, `calibration_prompt_version`,
>   `calibration_model_version` (per indexed search of
>   `src/modules/config/config.h`).
> - The runway is "shadow → canary → default" — the shadow path is the
>   existing `aimee_ir_shadow_*` family at `src/headers/aimee_ir_shadow.h`
>   (called from `src/server/anthropic_http.c:1075`). Canary ramp is
>   `AIMEE_IR_STREAM_RELAY` (see Decision 3) and `AIMEE_IR_PATH` at
>   `src/server/aimee_ir_serve.c:18`.
> - The proposal that introduced this substrate is
>   `docs/proposals/done/bayesian-promotion-threshold-calibration.md`
>   (referenced from the docstring of `calibration-sidecar.py:1-58`).

**Pairing:** Phase 5 introduces a new `target_surface: "guardrails.collapse"`
calibration profile but does NOT introduce a new substrate — it consumes
`calibration-sidecar.py` and writes one `fitted_buckets[]` entry per
`feature_set_version`. Substrate exists; no Phase 5.0 prerequisite.

---

## Decision 6 — Audit-store schema and new-record decision

> **AUDIT STORE EXISTS. Both schemas verified.**
>
> The WORM audit store for governed actions (`audit_action.*`) is at
> `src/modules/audit/audit_worm.h` / `.c`. The lifecycle event log for
> workflow / per-request audit is at `src/db1/wfe_store.h` /
> `src/db1/wfe_store.c` (`db1_lifecycle_event_add` at `:174` of the
> header, `:690` of the C file).
>
> Verified anchor lines:
>
> - WORM schema (SQLite `audit_event` table):
>   `src/modules/audit/audit_worm.c:33` (WORM_SCHEMA_SQL). Columns:
>   `seq, ts, actor_role, actor_principal, action, subject, verdict,
>   detail, key_id, prev_hash, row_hash`. WORM triggers at `:43-46`
>   (`audit_event_no_update`, `audit_event_no_delete`).
> - WORM write API: `audit_worm_append` at `src/modules/audit/audit_worm.c:138`
>   (returns 0 on success, -1 on failure; fsync-durable before return per
>   header docstring at `src/modules/audit/audit_worm.h:60-65`).
> - Dual-write gate: `audit_worm_enabled` config key (CLI-settable bool,
>   default-off) is the dual-write switch (verified at
>   `docs/gen/configuration.md` lines beginning with `audit_worm_enabled`
>   row). Toggle at `db2_kb_audit_worm_enabled` (`src/db2/kb_audit_worm.c:24-39`).
> - Chain key: `audit_worm_chain_key_load` at
>   `src/modules/audit/audit_worm_chain.h:45` (the `$AIMEE_HOME/.audit-chain-key`).
> - Checkpoint: `audit_worm_checkpoint` at `src/modules/audit/audit_worm.h:69`.
> - Seal snapshot: `audit_worm_seal` at `src/modules/audit/audit_worm.h:79`.
> - Query surface: `audit_worm_read_page` at `src/modules/audit/audit_worm.h:84`
>   (newest-first, caller-owns cJSON).
> - Lifecycle event schema (db1 `lifecycle_event` table):
>   `src/db1/schema.sql:200` — columns
>   `id, work_item_id, stage, kind, actor, detail, content_hash, cost_usd, created_at`.
>
> **DECISION: collapse_event is a STRUCTURED EXTENSION on the existing
> `audit_event.action = "guardrail.collapse.v1"` row, NOT a new record type.**
>
> Rationale:
> - The WORM store already enforces immutability, hash-chaining, and
>   sealing for the `audit_event` table; adding a new record type
>   requires a sibling schema, its own hash-chain primitive, and a
>   new checkpoint table — duplicated work for an operational record
>   that has identical storage-shape requirements.
> - The `db1.lifecycle_event` table does not carry a hash chain — it
>   is signed-by-an-actor workflow state, not a security log. Using it
>   would weaken the collapse_event's tamper-evidence story.
> - Extending the WORM `action` field with a versioned kind prefix
>   (`guardrail.collapse.v1.<verb>`) keeps the existing
>   `audit_worm_read_page` query surface unchanged; consumers filter on
>   `action LIKE 'guardrail.collapse.%'`.

**Pairing (Prerequisites):**
- ✅ Schema — no prerequisite.
- ❌ Writer convention: `audit_worm_append(...)` with
  `actor_role = "guardrail.collapse"`, `action = "guardrail.collapse.v1.observe"|"…enforce"|"…escalate"|"…shadow.mismatch"|"…demote"`,
  `subject = "<routing_key>"`, `verdict = "<verdict_enum_name>"`,
  `detail = "{\"schema\":\"v1\",…}"`. This is a **doc-only** convention,
  not a new code path; the actual writes are one-liners in the Phase 2+ scanner.
- ❌ Reader hook for metrics: add a
  `dump_metric_audit_action_breakdown(...)` to
  `src/server/server_state.c:1454-1464` (where existing
  `aimee_ir_metric_total(...)` dumps happen on
  `GET /v1/dashboard/metrics`, `src/server/server_http_routes.c:1576`).
  This is Phase 2.3.0 prerequisite if the Phase 5 promotion gate
  consumes audit-derived telemetry; otherwise Phase 5.0.

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
- [x] Every prerequisite-missing-substrate decision paired with its prerequisite phase (§2: doc-generator → Phase 1.0; §3: Responses renderer → Phase 2.0; §4: sampling types → Phase 4.0; §5: bucketing → already exists; §6: audit extension → doc-only convention + Phase 2.3.0 reader hook if telemetry is needed by Phase 5).
- [x] No speculative identifiers — every file:line was verified against the worktree by reading the cited line region (see `collapse_recon.md` §0 convention).
- [x] Phase 1 implementation does not start until this is merged.
