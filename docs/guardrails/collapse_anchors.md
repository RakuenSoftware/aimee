# Collapse Anchors — Six Binding Decisions for Guardrail-Collapse Phases 1–5

**Phase:** 0 — merged anchors document.
**Scope:** carries the six binding decisions that gate Phase 1+ implementation.
Each decision is backed by a file:line citation from the worktree's indexed
repository and (where applicable) is paired with the prerequisite phase that
introduces any missing substrate.
**Status:** REVIEW-CORRECTED. Phase 1 implementation **must not start** until
the workflow-managed merge gate that records this document as merged is
satisfied (see §"Merge gate" below). This document stays limited to stable
source anchors and never embeds commit SHA / branch name metadata — those
live in the workflow gate (F004).

This document is the contract — companion to `collapse_recon.md` (path-by-path
dispatch recon) and `sampling_capability_matrix.md` (per-backend sampling
matrix). It is the only file that Phase 1+ is allowed to read first.

**Open dependency:** Phase 1, Phase 2, and Phase 5 may not land until Phase 2.3.0 introduces the `collapse_event` writer and query helper.

---

## Decision 1 — Path-divergence verdict (relay convergence)

> **PATHS DIVERGE.** `collapse_recon.md` verifies the typed relay only for the
> IR-enabled `/v1/messages` branch. Responses, Chat, Webchat, Delegate, and
> Roundtable are direct compute-then-chunk emitters, polls, or non-streaming
> proxies; none of them reaches `aimee_delta_t` today. Phase 2 is split per
> handler/relay. Missing decoder/renderer/route-trace substrate is the Phase
> 2.0–2.4 prerequisite surface.

The verified type remains `aimee_delta_t` (typedef-open at `src/headers/aimee_ir.h:196`;
struct member `text_delta` at `:204`; struct closes at `:209`), with
`aimee_delta_type_t` (typedef-open at `src/headers/aimee_ir.h:186`;
enum members at `:188-193`; enum closes at `:194`); it is not treated as evidence of
reachability on divergent paths.

## Decision 2 — Config source of truth and namespace

> **CONFIG SOURCE OF TRUTH = `config_t` struct in `src/modules/config/config.h`.**
>
> - Struct: `config_t` is defined as `typedef struct config { ... }` opening at
>   `src/modules/config/config.h:265` (F-CITE-002 closure: this is the primary
>   definition; a forward-declaration `typedef struct config config_t;` also
>   exists at `src/headers/aimee.h:140` — Phase 1+ extends the `config.h:265`
>   struct, not the forward decl; the doc's prior claim that the struct
>   'lives in config.h' was correct in spirit but cited the `guardrails_*`
>   field row rather than the struct opening line). The `guardrails_*` field
>   cluster verified at `src/modules/config/config.h:1395-1408` (one field
>   per line, the last row is `guardrails_blast_radius_advisory_enabled` at
>   `:1408`).
> - Config-load entry: `config_load(config_t *cfg)` at
>   `src/modules/config/config.c:1109` (definition verified directly in the current source; declaration at `src/modules/config/config.h:2063`).
> - Section parser: `config_parse_guardrails_section(config_t *cfg, cJSON
>   *root)` at `src/modules/config/config_sections.c:1264` (F-CITE-002
>   closure: the symbol-index reports `:1258`, which is the comment/header
>   line preceding the function def; the live-source line `:1264` is
>   authoritative).
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

> **Honest qualification of the "preferred" claim (F001):** the parser
> side is verified — the existing parser at
> `src/modules/config/config_sections.c:1264` walks nested sub-objects
> inside `guardrails` (verified `semantic` at `:1270`,
> `blast_radius` at `:1311`). A new `collapse` sub-object requires only a
> per-field extension of the same pattern. However, the doc-generator
> at `scripts/gen-reference-docs.py:417` (`parse_config_sections`) does
> **not** render nested sub-objects as a visual hierarchy — it scans
> all `cJSON_GetObjectItemCaseSensitive(<var>, "<key>")` patterns
> (`scripts/gen-reference-docs.py:407-411`) and flattens the children
> into a single section table labelled by the top-level section name.
> The current rendered output (`docs/gen/configuration.md`) prefixes
> nested children with the section name (e.g.
> `guardrails_blast_radius_advisory_enabled` at line 62;
> `guardrails_semantic_mode` at line 64), regardless of which sub-object
> they live under. The live index line at `docs/gen/configuration.md:259`
> lists `guardrails` with `Keys: blast_radius, semantic` — adding
> `collapse` would extend that list, but the rendered scalars
> (`guardrails_collapse_*`) would appear in the same flat table as
> `guardrails_semantic_*` with no visual indentation. The current
> autogenerated `docs/gen/configuration.md` is the live verification
> target.
>
> **Phase 1.0 prerequisite (F001 closure):** Phase 1.0 must (a) add the
> struct members to `config_t` in `src/modules/config/config.h` (alongside
> `guardrails_blast_radius_advisory_enabled` at `:1408`), (b) extend
> `config_parse_guardrails_section` at `src/modules/config/config_sections.c:1264`
> with a `cJSON *col = cJSON_GetObjectItemCaseSensitive(gr, "collapse")`
> block mirroring the `semantic` block at `:1270` and the `blast_radius`
> block at `:1311`, (c) add `CFG_KEY_DESC` entries for each new key in
> `scripts/gen-reference-docs.py` (the dict at `:150-157` is where
> `guardrails_blast_radius_advisory_enabled` and the `guardrails_semantic_*`
> descriptions live), and (d) run `make -C src docs-gen` and verify the
> regenerated `docs/gen/configuration.md` lists the new keys with the
> correct descriptions. The `docs-gen-check` `make` target (referenced
> in `src/Makefile`) is the CI gate.

> **Alternative placement (NOT preferred, requires Phase 1.0 prerequisite):**
> Adding fields under a separate top-level `collapse` section would require
> a new `config_parse_collapse_section` parser AND a doc-generator extension
> so `scripts/gen-reference-docs.py:417` (`parse_config_sections`) recognises
> the new section. That is the **generator-extension prerequisite** and
> belongs to Phase 1.0 (parses the new section + regenerates
> `docs/gen/configuration.md`).

**Pairing:** preferred placement = Phase 1.0 prerequisite (parser +
struct + `CFG_KEY_DESC` + `make docs-gen` verification of the rendered
output). Alternative = Phase 1.0 prerequisite (parser + doc-generator
extension to `scripts/gen-reference-docs.py:417`).

## Decision 3 — Relay choke point with verified symbols

> **RELAY CHOKE POINT = `aimee_delta_t` (typedef-open at `src/headers/aimee_ir.h:196`;
> struct member `text_delta` at `:204`; struct closes at `:209`).**
>
> - **Producer (backend → IR):** `openai_chunk_to_deltas` at
>   `src/server/aimee_ir_stream.c:42`; `bedrock_converse_stream_to_deltas`
>   at `src/server/aimee_ir_stream.c:220`. State types:
>   `openai_stream_state_t` (typedef-open at `src/headers/aimee_ir_stream.h:18`;
>   decoder declared at `:31`) and `converse_stream_state_t`
>   (typedef-open at `src/headers/aimee_ir_stream.h:41`; decoder declared at
>   `:64`) (F-CITE-003 closure: the prior cites of `:30` and `:54` were
>   off-by-one / off-by-ten; the live-source lines are authoritative).
> - **Field on the struct carrying the bytes:** `aimee_delta_t.text_delta`
>   at `src/headers/aimee_ir.h:204` (typed `const char *`; BORROW lifetime
>   per the comment header at `:204-205`).
> - **Consumer (IR → frontend):** `anthropic_delta_emit` at
>   `src/server/aimee_ir_stream.c:539` (this is the Anthropic-shape
>   renderer — `delta_build_events` writes
>   `delta.type = "text_delta"`, `delta.text = d->text_delta` at
>   `src/server/aimee_ir_stream.c:445`). State type
>   `anthropic_stream_state_t` typedef-open at
>   `src/headers/aimee_ir_stream.h:68`; emitter declared at `:90`
>   (F-CITE-003 closure: prior cite `:85` for the typedef was off; the
>   typedef sits at `:82` for `aimee_sse_emit_fn` and at `:90` for
>   `anthropic_delta_emit`).
> - **Live wire-up today:** `messages_stream_ir_relay` at
>   `src/server/anthropic_http.c:973` (definition verified directly in the current source); gate at `src/server/anthropic_http.c:1251` and call at `:1253`
>   (`aimee_ir_stream_relay_enabled()`, default-OFF). The same env
>   (`AIMEE_IR_STREAM_RELAY`) is referenced via `aimee_ir_stream_relay_enabled()`
>   at `src/server/aimee_ir_serve.c:30`.
> - **Verified enum members observed on the live path:**
>   `AIMEE_DELTA_TURN_START` (`:188`), `AIMEE_DELTA_BLOCK_START` (`:189`),
>   `AIMEE_DELTA_BLOCK_DELTA` (`:190`, carries `text_delta`),
>   `AIMEE_DELTA_BLOCK_STOP` (`:191`), `AIMEE_DELTA_TURN_STOP` (`:192`),
>   `AIMEE_DELTA_ERROR` (`:193`) — enum open at
>   `src/headers/aimee_ir.h:186`, enum close at `:194`.
> - **Reachable on which path today:** only `/v1/messages` (verified
>   `messages_stream_ir_relay` gated by `aimee_ir_stream_relay_enabled()` at
>   `src/server/anthropic_http.c:1251`). Responses, Chat, Webchat, Delegate,
>   Roundtable are NOT reliable on this choke point without per-path
>   substrate (see Decision 1 and `collapse_recon.md` §2.2–§2.6).
> **Pairing:** each path's missing substrate is paired with its prerequisite
> slice (Phase 2.0, 2.1, 2.2, 2.3, 2.4). Verified file:line for the work is
> `src/server/aimee_ir_stream.c` (new header add to
> `src/headers/aimee_ir_stream.h` alongside `anthropic_delta_emit` at :90,
> new emitter function at the bottom of `src/server/aimee_ir_stream.c` after
> `:539`).

---

## Decision 4 — Sampling matrix scope (Phase 4 surface)

> **PHASE 4 SCOPE = `sampling_capability_matrix.md` columns.**
>
> Phase 4 collapses per-backend sampling knob duplication by moving knobs
> onto the canonical typed-sampling surface at
> `src/headers/aimee_ir.h:115-132` (the `aimee_request_t` struct opens at
> `:105` and closes at `:151`; the sampling-field cluster is
> `max_tokens` (`:115`), `has_max_tokens` (`:116`), `temperature` (`:117`),
> `has_temperature` (`:118`), `top_p` (`:125`), `has_top_p` (`:126`),
> `top_k` (`:127`), `has_top_k` (`:128`), `stop_sequences[]` (`:131`),
> `n_stop` (`:132`) — F-CITE-005 closure: the prior cite `:115-129` was
> an approximate range; the precise field-line list above is the
> authoritative pinning).
>
> Per-backend honours are enumerated in `sampling_capability_matrix.md` §1
> matrix. Knobs already modelled on the IR (✅/⚠ rows) need no Phase 4.0
> prerequisite. The ❌ rows in the matrix (`presence_penalty`,
> `frequency_penalty`, single-string `stop: "."` normalization,
> `previous_response_id`) are the Phase 4.0 prerequisite substrate.

**Pairing (each ❌ row → Phase 4.0 slice, F-CITE-005 closure):**
- ❌ `presence_penalty`/`frequency_penalty` on the IR → Phase 4.0 type-add
  (extend `aimee_request_t` in `src/headers/aimee_ir.h`; mirror
  `has_temperature` precedent at `:118` — F-CITE-005 closure: prior
  cite `:119` was off by one; the `has_*` companion pattern sits at
  `:116` for `has_max_tokens` and at `:118` for `has_temperature`, so
  new `has_presence_penalty` and `has_frequency_penalty` companions go
  at lines >118 to keep the parallel).
- ❌ `stop` string-or-array normalization → Phase 4.0 normalizer slice
  (extends `aimee_request_t.stop_sequences[]` consumer at `:131`;
  F-CITE-005 closure: the prior cite `:131` was correct in spirit;
  the precise field declaration is at `:131` for the array, with the
  `n_stop` companion at `:132`).
- ❌ `previous_response_id` thread key for Responses → Phase 4.0
  continuation-reference slice (extends `aimee_request_t.metadata` at
  `:139-145` — F-CITE-005 closure: prior cite `:134-138` was off; the
  metadata-related cluster actually lives at `metadata` (`:139`),
  `service_tier` (`:144`), and `thinking` (`:147-148`); the annotation
  block above the metadata field is at `:133-138`); a new typed
  `previous_response_id` field goes after `:148`, parallel to the
  `service_tier` precedent. The storage substrate already exists at
  `src/server/openai_responses_store.c`.

---

## Decision 5 — Promotion-gate substrate

> **STATUS: UNVERIFIED ABSENCE (F-003). The "no shadow→canary→default
> controller exists" claim is supported by a bounded search, not by a
> repository-wide negation. Phase 5.0 must include a Phase 5.0
> discovery task that either re-confirms the absence with a more
> targeted scan or surfaces the missing controller.**
>
> Bounded evidence for the absence claim (all four checks return no
> hits in the indexed codebase):
>
> | Check | Method | Result |
> | --- | --- | --- |
> | Promotion-controller symbol | `find_symbol` / `index_find_callers` for `promote_to_default`, `shadow_canary_default`, `bucket_labels`, `shadow->canary`, `bucket_assignment` | **0 hits** in `src/` and `scripts/` (4 distinct symbol queries, all empty) |
> | Promotion-controller substring | lexical search across `src/` and `scripts/` for `promote`, `canary`, `bucket_assignment`, `shadow->canary` | hits limited to: (a) `scripts/calibration-sidecar.py` (the sidecar's own bucket-fit code at lines `139-162` and default-config row at `:30-32`), and (b) a comment in `src/modules/config/config.h` referring to "promote (default 3)" inside the existing `guardrails_blast_radius_advisory` threshold preset — **not a transition controller**. |
> | Calibration consumer | `index_find_callers` for `calibration-sidecar` | **0 hits** in `src/` (the sidecar has no verified C caller) |
> | Rollout traffic-split | lexical search across `src/` for `rollout`, `traffic_split`, `routing_decision`, `default-flip` | only comments and unrelated config strings (e.g. `AGENT_ADMISSION_DEFAULT_GLOBAL_MAX` constant, ingress-compression proposal text) |
>
> The only verified calibration code is the standalone
> `scripts/calibration-sidecar.py` script (bayanesian bucket code at
> `scripts/calibration-sidecar.py:139-162`, default config at `:30-32`,
> `fitted_buckets` assembly at `:160-188`). It is a sidecar that takes
> JSON-on-stdin and writes JSON-on-stdout; it has no verified caller or
> consumer in the C pipeline. Environment gates alone (e.g.
> `aimee_ir_stream_relay_enabled()` at `src/server/aimee_ir_serve.c:30`)
> are NOT a promotion controller — they are binary feature flags.
>
> **Phase 5.0 prerequisite (before declaring absence confirmed, run `git log --all -S shadow_canary --pickaxe-regex` and `git grep -P "bucket_(label|assignment)"` across the full repository) (F-003):**
>
> 1. **Re-run the bounded search with the Phase 5 feature set in scope**
>    (search for `shadow_rollout_*`, `bucket_*`, `*_transition_state`,
>    `promote_*`, `canary_*` across the full repo, not just `src/` and
>    `scripts/`). Document the search terms and the empty result.
> 2. **If still absent**, Phase 5.0 introduces:
>    - bucket assignment (consume `scripts/calibration-sidecar.py`'s
>      `fitted_buckets` output via a new C reader, or re-implement the
>      beta-binomial + conformal floor inline),
>    - persisted calibration consumption (e.g. an `audit_worm_read_page`-
>      backed outcome histogram, gated on Phase 2.3.0's audit-event schema),
>    - explicit shadow→canary→default transition state machine
>      (env-gated today is not sufficient).
> 3. **Wire the collapse scanner into it** (Phase 5 mainline, gated on
>    Phase 5.0 completion).
>
> **Pairing (F-003):** if the bounded search surfaces an existing
> controller in scope (e.g. an unreferenced `bucket_*` symbol in a
> feature branch), Phase 5 wires into it; otherwise Phase 5.0 introduces
> bucketing + transition-state, and Phase 5 wires the collapse scanner
> into it.

---

## Decision 6 — Audit-store schema and new-record decision

> **AUDIT STORE EXISTS. The WORM row substrate is verified AND its
> registration/integration into the serving pipeline is now traced end
> to end (F-004 closure). The structured-detail contract for
> `collapse_event` is NOT** and is a Phase 2.3.0 prerequisite. The
> Phase 2.3.0 work introduces both the writer wiring and the
> detail-schema contract AND the (optional) query helper that
> consumers will use to filter on the new subkind.

### 6.1 Verified WORM substrate (file:line, end-to-end traced)

**Schema & lazy-open:**
- `WORM_SCHEMA_SQL` constant at `src/modules/audit/audit_worm.c:33`
  declares the `CREATE TABLE audit_event (seq, ts, actor_role,
  actor_principal, action, subject, verdict, detail, key_id, prev_hash,
  row_hash)` statement, plus append-only triggers
  `audit_event_no_update` and `audit_event_no_delete` at `:46-48`.
- Schema-apply call site `sqlite3_exec(db, WORM_SCHEMA_SQL, ...)` at
  `src/modules/audit/audit_worm.c:106`, gated by `worm_open_locked(db_path)`
  at `:88` (lazy-open under `g_worm_mu`).
- Lazy-open default at `src/modules/audit/audit_worm.c:118` —
  `worm_open_locked_default()` writes to
  `$AIMEE_HOME/audit/worm-live.db`.

**Registration / lifecycle integration (F-004 closure — the part the
prior draft glossed):**

| Step | Symbol | Where | Notes |
| --- | --- | --- | --- |
| Open/init | `audit_worm_init_at(db_path)` | def `src/modules/audit/audit_worm.c:125`, decl `src/modules/audit/audit_worm.h:43` | **PRODUCTION CALLERS: 0.** All current callers are in `src/tests/test_audit_worm.c` (`index_find_callers` returned 0 production callers, 12 test callers). The store is therefore opened **lazily** on first `audit_worm_append` (via `worm_open_locked_default()` at `src/modules/audit/audit_worm.c:159`, `:242`, `:450`, `:514`, `:597`, `:647`, `:684` — all gated on `!g_worm_db && worm_open_locked_default() != 0`). |
| Chain-key load | `audit_worm_chain_key_load(key, key_id)` | decl `src/modules/audit/audit_worm_chain.h:45`; consumer `src/modules/audit/audit_worm.c:237` (inside `audit_worm_checkpoint`) and again at `:409` (inside `audit_worm_seal`) | Lazy: the key file `$AIMEE_HOME/.audit-chain-key` is loaded only when checkpoint/seal is invoked. |
| Append | `audit_worm_append(...)` | def `src/modules/audit/audit_worm.c:135`; decl `src/modules/audit/audit_worm.h:50` | **PRODUCTION CALLERS:** (1) `src/server/server_mgmt_audit.c:23` (`append`, used by `server_mgmt_audit_intent` and `_outcome` — the management-intent / management-outcome audit trail), (2) `src/modules/guardrails/guardrails_action_audit.c:87` (`emit_worm_row` — tool-action audit), (3) `src/kb/kb_vault_rewrap.c:15` and `:19` (`kb_vault_rewrap_principal` — KB DEK rewrap audit, "intent" then "allow"/"deny"), (4) self-call from `audit_worm_metric_snapshot` at `src/modules/audit/audit_worm.c:678`. **Each production caller uses a free-form `action` string** (`management.intent`, `management.outcome`, `tool.<name>`, `vault.dek_rewrap`, `metric.snapshot`). |
| Checkpoint (seal path #1) | `audit_worm_checkpoint()` | def `src/modules/audit/audit_worm.c:233`; decl `src/modules/audit/audit_worm.h:64` | **PRODUCTION CALLERS:** (a) self-call from `audit_worm_seal` at `src/modules/audit/audit_worm.c:510` (attests the head before VACUUM-into-snapshot), (b) `audit_sub_checkpoint` (the `aimee audit checkpoint` CLI subcommand) at `src/cmd_audit.c:42`. |
| Seal (seal path #2) | `audit_worm_seal(path, sizeof path, &immutable)` | def `src/modules/audit/audit_worm.c:508`; decl `src/modules/audit/audit_worm.h:87` | **PRODUCTION CALLERS:** `audit_sub_seal` (the `aimee audit seal` CLI subcommand) at `src/cmd_audit.c:58`. Writes a snapshot to `path`, sets `immutable=1` if `CAP_LINUX_IMMUTABLE` is granted, otherwise `immutable=0` (crypto-only). |
| Verify (sanity check) | `audit_worm_verify_chain(...)` | decl `src/modules/audit/audit_worm.h:57`; full verify `audit_worm_verify(...)` at def `src/modules/audit/audit_worm.c:559` | CLI surface: `aimee audit verify` (not separately enumerated; CLI integration). |
| Query | `audit_worm_read_page(...)` | def `src/modules/audit/audit_worm.c:587`; decl `src/modules/audit/audit_worm.h:93` | **PRODUCTION CALLERS: 0.** All current callers are in `src/tests/test_audit_worm.c` (4 test callers — `test_read_page`, `test_metric_snapshot`, `test_detail_capped`). There is **no production query consumer** today; the API exists and is callable but is only exercised by tests. The `metric.snapshot` row (written by `audit_worm_metric_snapshot`) is also read only by `test_metric_snapshot` at `src/tests/test_audit_worm.c:274`. |
| Metric snapshot precedent | `audit_worm_metric_snapshot(...)` | decl `src/modules/audit/audit_worm.h:97`; def `src/modules/audit/audit_worm.c:638` | Self-calls `audit_worm_append("audit", "metric", "metric.snapshot", ..., "ok", detail)` at `:678` (where `detail` is a JSON blob assembled at `:675-677`, e.g. `{"total":N,"allow":N,"block":N,...}`). **Note:** there is **no structured-detail schema** enforced on the `detail` column, no registration path, and no query/index helper for the `metric.snapshot` subkind. The same shape caveat applies to `collapse_event` until Phase 2.3.0 introduces one. |

**End-to-end verdict (F-004 closure):** The WORM row substrate is
**verified reachable end-to-end** on the management, tool-action, and
KB-vault audit paths (each writes a hash-chained row at the relevant
call site, and each is gated by the per-feature `audit_*_enabled` /
`audit_worm_is_enabled` flag). The checkpoint and seal paths are
verified reachable through the `aimee audit checkpoint` / `aimee audit
seal` CLI subcommands at `src/cmd_audit.c:42/58`. **The query surface
(`audit_worm_read_page`) is NOT exercised by any production caller
today** — it is callable but no operator-dashboard, CLI, or HTTP
handler currently consumes it. The Phase 2.3.0 work that introduces a
`collapse_event` query helper will be the first production query
consumer of `audit_worm_read_page`.

### 6.2 Decision rationales

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
  hash-chained". **Note:** the precedent writes a free-form JSON
  `detail` string (the body assembled at `src/modules/audit/audit_worm.c:675-677`,
  e.g. `{"total":N,"allow":N,"block":N,...}`) — there is **no
  structured-detail schema** enforced on the `detail` column, no
  registration path, and no query/index helper for the `metric.snapshot`
  subkind. The same shape caveat applies to `collapse_event` until
  Phase 2.3.0 introduces one.

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
  `audit_worm_read_page` query surface unchanged for non-collapse
  consumers; collapse-aware consumers will need a Phase 2.3.0-introduced
  filter helper (or a JSON predicate over `audit_worm_read_page`'s
  cJSON array) — see §"Pairing" below.

**Pairing (Prerequisites) — F002 / F-004 closure:**

- ✅ Schema — no prerequisite (existing `audit_event` table is the substrate).
- ✅ Write API — no prerequisite (`audit_worm_append` at
  `src/modules/audit/audit_worm.c:135` is the call site).
- ✅ Checkpoint / seal — no prerequisite (`audit_worm_checkpoint` at
  `src/modules/audit/audit_worm.c:233` and `audit_worm_seal` at
  `src/modules/audit/audit_worm.c:508` are the lifecycle integration points).
- ⚠ **Query surface (raw rows) — PARTIAL prerequisite.** The
  `audit_worm_read_page` API exists and is callable, but **no production
  caller exists** today. Phase 2.3.0 must introduce the first production
  consumer in the form of a `audit_worm_collapse_event_query(...)` helper
  that walks the cJSON array returned by `audit_worm_read_page` and
  filters on the `guardrail.collapse.` prefix.
- 🟡 **Phase 2.3.0 implementation work (F002):** the structured-detail
  contract for `collapse_event` is NOT yet defined and NOT yet wired.
  Phase 2.3.0 must:
  1. Write a `docs/guardrails/collapse_event_detail.md` (or extend the
     Phase 2.3.0 design doc) defining the JSON contract for `detail`
     (required keys: `schema: "v1"`, `phase: "observe|enforce|escalate|shadow.mismatch|demote"`,
     `bucket: "<bucket_label>"`, `routing_key: "<path>"`, `counter: <integer>`,
     and any path-specific fields). The `detail` column in
     `src/modules/audit/audit_worm.c:42` is `TEXT` with no
     structure enforcement; the contract is the contract for the
     *interpretation* of the bytes, not for the SQLite schema.
  2. Add a `audit_worm_collapse_event_log(...)` helper to
     `src/modules/audit/audit_worm.c` (alongside `audit_worm_metric_snapshot` at
     `:638`) that (a) builds the JSON `detail` per the contract, (b)
     calls `audit_worm_append("guardrail.collapse", "", "guardrail.collapse.v1.<verb>",
     "<routing_key>", "<verdict>", detail)` per the convention.
  3. Add a `audit_worm_collapse_event_query(...)` helper to
     `src/modules/audit/audit_worm.c` that walks the
     `audit_worm_read_page` cJSON array and filters rows whose
     `action` starts with `guardrail.collapse.` (an in-memory prefix
     comparison over each bounded page returned by `audit_worm_read_page`;
     no SQL `LIKE` or index is introduced, so complete scans must page
     to exhaustion). **No SQLite index change is required** — the
     existing `audit_event` table has no index on `action` today; the
     filter is linear over the page, which is acceptable for the page
     size (`audit_worm_read_page` returns bounded pages per the header
     docstring at `:93`). **This helper will be the first production
     caller of `audit_worm_read_page`** — it is the integration that
     closes F-004.
  4. Wire the lifecycle integration: `audit_worm_checkpoint` at
     `src/modules/audit/audit_worm.c:233` invoked at scan-end and
     `audit_worm_seal` at `:508` invoked at operator-triggered snapshot.
- (Optional) Reader hook for metrics: extend `dump_metric_audit_action_breakdown(...)`
  via the existing `metric.snapshot` precedent at
  `src/modules/audit/audit_worm.c:638`; Phase 2.3.0 prerequisite if the
  Phase 5 promotion gate consumes audit-derived telemetry, otherwise Phase 5.0.

**Summary of the F-002 / F-004 corrections:** the prior draft claimed
the `collapse_event` representation was already a "structured extension
of `audit_event`" backed by an existing structured-detail schema and
query helper, AND it glossed the registration/sealing/query-surface
verification with a list of "callable function definitions" rather


---

## 6.3 F-002 binding decision (audit-store `collapse_event`)

> **BINDING REPRESENTATION DECISION:** `collapse_event` is a
> **structured-field extension of the existing `audit_event` row** in
> the WORM store (`src/modules/audit/audit_worm.c:33`).  It is **NOT**
> a new record type.

Concretely, every collapse event is written as one row in the existing
`audit_event` table, using the existing write API and lifecycle
integration:

- `action = "guardrail.collapse.v1.<verb>"` (e.g.
  `guardrail.collapse.v1.observe`, `guardrail.collapse.v1.enforce`,
  `guardrail.collapse.v1.escalate`, `guardrail.collapse.v1.shadow.mismatch`,
  `guardrail.collapse.v1.demote`).  The versioned prefix lets collapse
  rows coexist with management, tool-action, KB-vault, and metric-snapshot
  rows on the same hash-chained `audit_event` table; non-collapse
  consumers of `audit_worm_read_page` (the only existing production
  query surface — see §6.1) continue to function unchanged.
- `subject` and `verdict` are populated from the per-path relay choke
  point (§3).  `detail` is a JSON object whose contract is defined by
  Phase 2.3.0 (see "Phase 2.3.0 prerequisites" below).
- Registration / sealing lifecycle is the existing WORM lifecycle:
  `audit_worm_chain_key_load` at
  `src/modules/audit/audit_worm_chain.h:45`,
  `audit_worm_append` at `src/modules/audit/audit_worm.c:135`,
  `audit_worm_checkpoint` at `:233`, `audit_worm_seal` at `:508`,
  `audit_worm_verify` at `:559`, `audit_worm_read_page` at `:587`.

**Why not a new record type (rationale, F-002 closure):**

- The existing `audit_event` table already enforces immutability
  (WORM triggers at `src/modules/audit/audit_worm.c:46-48`),
  hash-chaining (`prev_hash` / `row_hash` columns), and sealing
  (`audit_worm_seal`).  A sibling `collapse_event` table would require
  its own schema, its own hash-chain primitive, and its own checkpoint
  table — duplicated work for an operational record that has identical
  storage-shape requirements.
- The `db1.lifecycle_event` table does not carry a hash chain — it is
  signed-by-an-actor workflow state, not a security log.  Using it
  would weaken the collapse event's tamper-evidence story.
- The versioned `action` prefix keeps the existing
  `audit_worm_read_page` query surface unchanged for non-collapse
  consumers and gives collapse-aware consumers a stable prefix to
  filter on without a SQL `LIKE` / index change.

**Phase 2.3.0 prerequisites (the only remaining work for collapse-event
substrate):**

1. **`docs/guardrails/collapse_event_detail.md`** — JSON contract for
   the `detail` column when `action` starts with
   `guardrail.collapse.v1.`.  Required keys:
   `schema` (`"v1"`), `phase` (`"observe"|"enforce"|"escalate"|"shadow.mismatch"|"demote"`),
   `bucket`, `routing_key`, `counter`, plus path-specific optional
   fields.  The `detail` column at `src/modules/audit/audit_worm.c:42`
   is `TEXT` with no SQLite schema enforcement; the contract is the
   contract for the *interpretation* of the bytes, mirroring the
   opaque-JSON precedent for `metric.snapshot` rows at
   `src/modules/audit/audit_worm.c:675-677`.
2. **`audit_worm_collapse_event_log(...)`** writer helper alongside
   `audit_worm_metric_snapshot` at `src/modules/audit/audit_worm.c:638`.
   Builds the JSON `detail` per the contract and calls
   `audit_worm_append("guardrail.collapse", "", "guardrail.collapse.v1.<verb>",
   "<routing_key>", "<verdict>", detail)` per the convention.
3. **`audit_worm_collapse_event_query(...)`** filter helper near
   `audit_worm_read_page` at `src/modules/audit/audit_worm.c:587`.
   Walks the cJSON array returned by `audit_worm_read_page` and
   matches rows whose `action` starts with `guardrail.collapse.`
   (in-memory prefix comparison over each bounded page; no SQL `LIKE`
   / index change is required, because the existing `audit_event`
   table has no index on `action`).  **This helper will be the first
   production caller of `audit_worm_read_page`.**
4. **Lifecycle integration of the writer** at the per-path relay
   choke points in §3 — wired by Phase 2.0–2.4 prerequisite slices,
   each gated on Phase 2.3.0.

**Pairing:**

- `collapse_event` JSON contract → Phase 2.3.0 prerequisite.
- `audit_worm_collapse_event_log` writer → Phase 2.3.0 prerequisite.
- `audit_worm_collapse_event_query` query helper → Phase 2.3.0
  prerequisite (closes the F-004 "0 production callers" gap as the
  first production consumer of `audit_worm_read_page`).
- Lifecycle integration of the writer at the per-path relay choke
  point → Phase 2.0–2.4 prerequisite slices, gated on Phase 2.3.0.
- Optional reader hook for Phase 5 promotion-gate telemetry
  (`dump_metric_audit_action_breakdown(...)`) → Phase 5.0 if needed,
  else Phase 2.3.0.

---
## Cross-references to the companion documents

- `collapse_recon.md` §1: binding decision (this doc §1 mirrors it, with citations).
- `collapse_recon.md` §2: handler + SSE/delta emitter per path.
- `collapse_recon.md` §3: text_delta production/consumption lines.
- `collapse_recon.md` §4: scanner precedents.
- `sampling_capability_matrix.md` §1: per-backend matrix.
- `sampling_capability_matrix.md` §3: Phase 4.0 prerequisites.

## Merge gate (F-005 closure)

**Status:** **ENFORCED in CI by `scripts/check-collapse-anchor-gate.py`,
invoked as a step in the `lint` job of `.github/workflows/ci.yml`.**

The gate is enforced by the workflow artifact this document ships:

- `scripts/check-collapse-anchor-gate.py` — a Python script that runs
  in the `lint` job's `Enforce guardrail-collapse Phase 0 anchor
  contract` step at `.github/workflows/ci.yml:100-103`.  The step
  invokes the script with no arguments; the script resolves its
  comparison base from `BASE_SHA` (when set by the runner for
  `pull_request` events) or from `HEAD~1` (when available) and falls
  back to `git diff HEAD` only if neither is reachable.
- Phase 1+ surfaces watched by the gate (see
  `PHASE_ONE_PREFIXES` in the script):
  - `src/server/`
  - `src/modules/guardrails/`
  - `src/headers/aimee_ir.h`
  - `src/modules/config/`
  - `src/modules/audit/`
  - `scripts/check-collapse-anchor-gate.py`
- When the validated diff touches any of those paths, the gate
  requires `docs/guardrails/collapse_anchors.md` to be present in the
  working tree and to contain every `## Decision N` heading for
  N in 1..6.  The anchor file is **not** required to appear in the
  same diff — once it is merged, its on-disk presence (not its
  re-modification) is what gates subsequent Phase 1+ work.
- Diff-discovery failures (`git` binary missing, invalid `BASE_SHA`,
  shallow checkout with no usable parent) cause the gate to exit 2
  rather than silently passing — see `tests/test_check_collapse_anchor_gate.py`
  for the behavioural coverage of both accepted and rejected histories.
- The gate is wired into the same `lint` job that runs formatting and
  `check_tier_deps.sh`, so any failure blocks the PR's required CI
  status in the same way as those long-running checks do.

**Pairing (F-005 closure):** the workflow rule is the artifact this
Phase 0 commit ships — no Phase 0.5 housekeeping task remains.  Future
changes to the watched-path list or to the comparison-base resolution
must update both `PHASE_ONE_PREFIXES` in the script and this section.

## Acceptance check

- [x] Six binding decisions, each with file:line citations.
- [x] Every prerequisite-missing-substrate decision paired with its prerequisite phase (§2: doc-generator + parser + struct + `CFG_KEY_DESC` + rendered-output verification → Phase 1.0; §3: Responses renderer + Chat emitter + Webchat / Delegate / Roundtable observers → Phase 2.0–2.4; §4: sampling types → Phase 4.0; §5: bucketing → Phase 5.0; §6: audit writer + JSON detail contract + query helper + lifecycle integration → Phase 2.3.0).
- [x] No speculative identifiers — every file:line was verified against the worktree by reading the cited line region (see `collapse_recon.md` §0 convention).
- [x] F-001 closure (config side): Decision 2's "preferred" claim is qualified with the doc-generator rendering caveat and the Phase 1.0 prerequisite is annotated with the verification step (`make docs-gen` + render check). Per-path handler/emitter citations are in `collapse_recon.md` §2.2–§2.6, not duplicated here.
- [x] F-002 closure: §6.3 now records the **binding representation decision** -- `collapse_event` is a structured-field extension of the existing `audit_event` row (action = `guardrail.collapse.v1.<verb>`, subject/verdict populated from the per-path relay choke point, detail = JSON object whose contract is Phase 2.3.0's deliverable).  Phase 2.3.0 prerequisites are listed explicitly: JSON contract document, `audit_worm_collapse_event_log` writer helper, `audit_worm_collapse_event_query` filter helper, and lifecycle integration at the per-path relay choke points.
- [x] F-003 closure: Decision 5 is now marked UNVERIFIED ABSENCE with a bounded-search evidence table (4 distinct queries, 0 hits in production code) and a Phase 5.0 re-discovery task before binding the architecture.
- [x] F-004 closure: Decision 6.1 now traces the full lifecycle integration (init / chain-key / append / checkpoint / seal / verify / query) with production-caller counts from `index_find_callers`; the query-surface is honestly flagged as having 0 production callers and Phase 2.3.0 will introduce the first.
- [x] F-005 closure: "Merge gate" now states the gate is **ENFORCED in CI** by `scripts/check-collapse-anchor-gate.py`, wired as the `Enforce guardrail-collapse Phase 0 anchor contract` step at `.github/workflows/ci.yml:100-103` (the `lint` job, after `check_tier_deps.sh`).  The script resolves its comparison base from `BASE_SHA` (set by the runner for `pull_request` events), then `HEAD~1`, then `git diff HEAD`, and fails closed (exit 2) when no comparison base is reachable.  Behavioural coverage of the accepted and rejected histories lives in `tests/test_check_collapse_anchor_gate.py`.
- [x] Phase 1 implementation does not start until this document is merged.  Enforcement is automated by the CI step introduced by this artifact (see "Merge gate" above); reviewer discipline is a backstop, not the primary mechanism.
- [x] Review-feedback closures recorded in this round:
  - F-001 / F-005: `scripts/check-collapse-anchor-gate.py` hardened in
    this round so that (a) diff-discovery failures fail closed with
    exit 2 instead of silently passing (regression covered by
    `test_gate_fails_closed_when_diff_discovery_cannot_run` and
    `test_gate_fails_closed_when_base_sha_is_invalid`) and (b) the
    gate no longer requires the anchors file to appear in the same
    diff as a Phase 1+ change -- once merged, its on-disk presence is
    what gates subsequent work (regression covered by
    `test_gate_passes_when_anchors_already_merged_and_phase_one_touches_other_files`).
  - F-002: §6.3 rewritten to record the **binding representation
    decision** (`collapse_event` = structured-field extension of the
    existing `audit_event` row), with the rationale (WORM substrate
    reuse, tamper-evidence parity with the metric-snapshot precedent)
    and the explicit Phase 2.3.0 prerequisite list (JSON contract
    document, `audit_worm_collapse_event_log` writer helper,
    `audit_worm_collapse_event_query` query helper, lifecycle
    integration at the per-path relay choke points).  The earlier
    UNRESOLVED framing was retracted -- Phase 0 acceptance for the
    audit-store substrate is now marked complete on the
    representation axis.
  - F-003: `docs/guardrails/sampling_capability_matrix.md` §1 rewritten
    so every backend × knob cell carries a verified file:line citation
    (✅/⚠/❌/n/a) traced from parser → typed IR → backend-builder.
    Remaining ⚠ "unverified" cells are scoped to provider-behaviour
    questions (cloud OpenAI Chat silent-drop) and paired with Phase 4.0
    fixtures.
