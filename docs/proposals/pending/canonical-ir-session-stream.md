# Canonical IR session stream

State: implementation in progress. Authorized by the project owner on 2026-10-17.

Implementation checkpoint, 2026-10-20: durable primary history/loading, model changes and canonical
compaction are active in the migration branch. Qualified Linux Codex/Claude execution uses original
host model/tool sources, supervised effects and closed credential-free SDK launch resources. Typed
seeded runs preserve complete history, workspace scope, final content and cumulative usage.
Sixteen actual-SDK API/OAuth journeys pass locally, including credential rotation and resume.
Additional root-UDS browser operator journeys require fresh CI qualification. Remaining consumers,
independent local learning/grading/reuse, broader native platforms, live subscription acceptance
and reviewed production promotion remain open. Production `.100` remains KB-free.

## Invariant

A native model/harness protocol ends at its entry adapter. After that boundary, conversation
content is canonical typed IR. Provider-specific serialization is permitted only at egress.
Browser chat, delegates, model adapters, persistence, replay, and rendering share this contract.
A rendered terminal screen is not a model protocol and cannot supply canonical conversation data.

## Ownership

- `ir`: typed messages, blocks, deltas, harness control events, and their versioned serialization.
- `translation`: native protocol entry/egress adapters. Unknown content requires an explicit typed
  extension or conversion error; it must not silently disappear or become answer text.
- Server sessions / the `aimee` store family: ordered durable event log, idempotent append,
  session/turn/message identity, replay cursors, authorization, and pending control requests.
- Harness adapters: process lifecycle and permission response delivery. A model cannot resolve
  a permission request; only the authenticated operator and the original pending request can.
- `runtime-web`: authentication, authorization, request submission, and byte transport. It does
  not translate, reconstruct, persist, or interpret model output.
- Frontend: one canonical reducer, keyed by message and block identity. Tool results pair to
  uses by tool ID, not arrival position or tool name.

## Contract

The transport envelope carries schema version, session ID, monotonically ordered sequence,
producer event ID, turn ID and actor. Message identity belongs to the canonical event. Sequence values travel as decimal strings to avoid
JavaScript integer precision loss. Session identity is never embedded in a model delta.

Payloads are typed model deltas, whole IR messages, typed harness control events, or host execution
lifecycle events. Model `TURN_STOP` ends one response; it does not end the host execution. Messages
have roles. Tool results and cross-surface user messages are first-class messages. Permission
request/response events are harness-authored, never model blocks or inferred model approvals.
Provider resume handles are runtime metadata, not transcript identity or credentials.

Events commit before broadcast. A subscriber disconnect does not cancel execution. A replay
cursor resumes after the last committed sequence; browser storage is only a cache. Append retries
reuse producer event IDs. A conflicting retry is rejected. Multiple native observations of the
same interaction have one declared capture owner: model ingress or the harness, never both.

## Required migration

1. Establish canonical serialization and reducer fixtures before changing live streams.
2. Persist ordered canonical events in the existing Server session owner, with subject erasure,
   bounded pages, idempotency, authorization, restart replay, and storage-failure propagation.
3. Retain parsed IR through the native execution loop and tool dispatch. Remove the lossy bridge
   to `parsed_response_t`; render provider request histories only at egress.
4. Convert Claude stream-json, Codex app-server, and ACP updates at their adapters. Replace tmux
   scraping for model conversation capture. Preserve CLI-local controls through typed events.
5. Make native/CLI primaries and delegates publish into the same session owner. Model ingress
   capture requires explicit session binding and a verified Vault-backed upstream path.
6. Replace browser NDJSON names with canonical envelopes. The web backend forwards bytes;
   the frontend reducer handles live/replayed messages, attachments, tools, thinking and controls.
7. Migrate historical transcripts explicitly. Old text cannot recover lost structure and must
   remain marked as legacy rather than being claimed as complete reconstructed IR.

## Acceptance gates

- Native fixtures for every supported wire/harness, including interleaved tool IDs, reasoning,
  attachments, errors, cancellation, user questions, approvals, and unknown native content.
- CLI and browser produce equivalent canonical logs for equivalent interactions.
- A fresh browser replay renders the same state as the original live stream.
- Switching devices mid-turn retains intermediate content and pending operator controls.
- Repeated ingress/harness observations do not duplicate messages or tool results.
- Cross-session cursor/control replay and forged actor/provider identities are rejected.
- Vault credentials stay out of canonical events, browser state, model context, and diagnostics.
- No supported model-output path falls back to scraping or a legacy event vocabulary.

PR #3030 contains a narrow repair of the old browser stream. Its event-name additions are not the
canonical design and are to be replaced, not copied into another adapter.

## Implemented browser path

- Canonical C serialization/sink for messages, deltas, controls and host execution lifecycle.
- Migration 44 and DB1 append/page operations persist events before delivery, with transactional
  actor checks, bounded replay pages, exact decimal sequences and cascading session erasure.
- Migration 45 stores owner-checked, per-harness resume handles. Claude projects and Codex rollout
  files survive credential-home cleanup; credentials are materialized from the Vault per run.
- `/v1/chat/stream` with `ir_version: 1` emits canonical envelopes. The authenticated session events
  route with `format=ir` replays the same log, then follows new commits. The web backend passes both
  through unchanged; Chat uses one reducer with ordered deduplication across POST and SSE.
- Browser Claude primaries run `claude -p --output-format stream-json --include-partial-messages`.
  Claude, Codex app-server and ACP entry adapters emit typed events before legacy consumers.
  API model responses publish typed lifecycle, messages, usage and errors at entry. Native
  execution and tool dispatch retain owned IR and full tool identities. Tool results are canonical
  blocks; persisted execution histories contain canonical messages rather than provider arrays.
- Responses SSE parsing is translation-owned and retains ordered completed items and encrypted
  reasoning when the terminal snapshot omits them. Anthropic signed/redacted thinking and URL or
  inline media survive canonical persistence and egress.
- Canonical compaction retains user/policy messages exactly and keeps whole tool cycles at cuts.
  The Go economizer accepts the same canonical messages and emits canonical summary messages.
- Native adapter, process lifecycle, capture ordering/failure/cancellation and frontend replay
  fixtures cover the integrated path. The PostgreSQL concurrency, ownership and cascade fixture
  runs in the required PostgreSQL replay CI job with `AIMEE_TEST_PG_URL`.

## Remaining migration boundaries

The native execution loop and simple completion runtime no longer consume `parsed_response_t`.
Compatibility wrappers, legacy ingress/driver/rescue consumers and flat `agent_result_t` callers
remain to be removed for the global invariant. Primary history now writes canonical messages,
but publication from every delegate worker and reconstruction from the session log still need
one session-owned path. The Go economizer handles canonical messages; the shared typed IR bus
contract and remaining native ingress transforms still require cutover.
The browser displays typed harness controls but does not yet offer interactive permission replies.
Existing primary approval policy still applies. Model ingress capture is not simultaneously enabled
for these harness-owned browser turns: each native harness adapter is the capture owner.
Historical text is retained in a labelled legacy section, without inferred tools or reasoning.
Live subscription authentication and native resume also require deployment verification with
Vault-held credentials; deterministic process fixtures do not establish provider acceptance.
