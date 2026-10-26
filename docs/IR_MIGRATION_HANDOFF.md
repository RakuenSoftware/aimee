# Canonical IR migration continuation

## Objective and constraints

Replace provider-specific model communication inside Aimee with the shared IR.
Convert incoming model/provider communication to IR at the ingress edge, and
convert outgoing IR to the required provider, IDE or CLI format at the egress
edge. Ordinary internal application messages, policy metadata and domain records
do not automatically become IR. Browser agent sessions must work, including
retained history and continuation. Retire replaced provider implementations and
their obsolete tests so the migration reduces code.

Continue on `fix/native-mcp-startup-and-editor` in this worktree. Keep one eventual
PR; PR administration and authentication questions are not implementation
prerequisites. Keep implementation and validation within the two goals below.
Do not call the migration complete while implementation or required live checks
remain. Historical handoff versions are evidence, not current instructions.

Do not investigate or modify `.100`. No KB instance should be configured: unset
mode/endpoint are intentional. No WOL work belongs here. Keep automatic reviews
and strict attribution. Prefer registered Aimee Git commands, then SSH Git or
personally authenticated gh. No connector writes, coauthor trailers or branding.
Production promotion follows merge to main and the independent review requirement
in `OWNERS.md`.

## Code reduction requirement: unfinished

The two requirements are working browser agent sessions and IR for internal
provider-specific model communication.
The migration must remove duplicate implementations and reduce total code. The previous completion claim did
not establish this requirement and is withdrawn for the overall task.

PRs target `testing` under CONTRIBUTING.md. The earlier `main` compare link was
wrong: it included Rust bus support already integrated into `testing` through
separate PRs. Do not remove that existing work as part of this migration.

At handoff commit `4f063a775`, the actual `testing` comparison was891 files,
69,059 additions and41,703 deletions, net27,356. Path/extension accounting is:
implementation/operational scripts +578; tests/fixtures +31,045; documentation
-4,121; other build/config/schema -146. Documentation removal does not establish
production code reduction. The audit receipt is
`/var/tmp/aimee-ir-correct-base-growth-audit.json`.

The corrected scope keeps provider codecs at the actual external edges. Old
OpenAI/Anthropic economizer planners and the unused wire orchestrator, together
with their dedicated tests, are being retired. Shared request shaping, Responses
decoding and canonical model execution are consolidated. The unrelated attempt
to make memory policy metadata use IR was set aside with exact restoration of
the earlier source; no workflow/domain-record or history-archive migration was
performed. Browser session cache cleanup is a separate browser simplification. Measure implementation,
tests, documentation and generated/build data separately. Preserve meaningful
regression coverage and supported history; deleting it is not architecture
simplification. No new source is qualified merely because the previous image
passed. The active QA still runs the b61 image described below.

## Independent model-path trace after edc58e19c

Three parallel audits followed production callers across HTTP providers/KB,
internal orchestration, and browser/native/ACP/CLI execution. The shared C
Request/Response/Message types and the Go/browser bindings represent the same
canonical contract. Native request bodies, response frames and SDK control
protocols belong to the translation or external adapter boundaries.

| Model path | Canonical ownership and external conversion |
| --- | --- |
| Anthropic, Chat Completions and Responses ingress | Translation frontends admit Request; HTTP frontends share `server_model_execute_prepared`. |
| HTTP model execution | Agent profiles, policy, tools and history retain IR; translation renders the upstream body and admits the upstream Response/deltas. |
| Browser primary sessions | Browser Message, runtime-web admission, canonical session history and one live/replay reducer; legacy display archives do not feed execution. |
| Claude/Codex SDK execution | Source-bound canonical profile/history and captured upstream IR own model observations; SDK frame decoders handle external controls/lifecycle. |
| ACP, CLI and IDEs | Canonical request/result through the host; the ACP/CLI boundary owns its declared text/content projections. |
| Roundtable, delegates, workflow model steps and repairs | Canonical Request/Response through delegate invocation; structured repair retains prior typed output. Business artifacts remain domain records. |
| Memory/router/economizer stages | Typed IR or strictly admitted canonical history; policy views and retrieval records remain domain metadata. |
| KB curator and Bedrock egress | Canonical requests/results; existing translation adapters own native provider bodies. |
| Availability probes | Fixed canonical request and existing provider transport; the translation owner's bounded pipe client reuses the same C codecs in the existing server executable. |

The trace found and retired the unused `gw_request_t` request pipeline and its
registry/tests, the probe's separate Go provider codecs, repeated Codex frame
parsing, duplicate Go context serialization and the provider client's local
completed-response admission. JSON and Responses ingress now share one typed
rescue/admission/capture owner. A decoded Responses stream is admitted as canonical
IR, without consulting its inert native sidecar as a second completion authority.
The local-provider thinking extension now has one translation owner. No alternate provider transcript schema was confirmed in the
examined live execution chains. This is a source audit, not a claim that every
configured model or live browser interaction has been exercised on this source.

Detailed caller receipts and frozen-source checks are retained under:

- `/var/tmp/aimee-edc-browser-native-deep-trace.md`
- `/var/tmp/aimee-codex-single-frame-final-proof.json`
- `/var/tmp/aimee-ir-trace-root-proof.json`
- `/var/tmp/aimee-ir-internal-model-trace-edc58e19/trace.json`
- `/var/tmp/aimee-ir-request-stage-retirement/proof.json`
- `/var/tmp/aimee-edc-provider-edge-deep-trace.md`
- `/var/tmp/aimee-ir-native-materializer-retirement/proof.json`
- `/var/tmp/aimee-ir-model-dispatch-census/census.json`
- `/var/tmp/aimee-discord-canonical-migration/proof.json`
- `/var/tmp/aimee-discord-thinclient-wrapper-packaging-proof.json`
- `/var/tmp/aimee-model-generate-rich-proof.json`
- `/var/tmp/aimee-model-generate-rich-independent-review.json`
- `/var/tmp/aimee-discord-validation-owner-followup/proof-r2.json`

The dispatch census reconciles 43 concrete C execution calls, seven Go delegate
calls and 124 C transport/process sites with these traces. Two actual HTTP
execution paths also now reject trailing bytes after the native response JSON,
using the existing strict frame parser and a real plain/tool transport regression.
Discord is included in the migration and uses the installed thin client:
`aimee --json model generate` reads a canonical Request plus an optional registered
agent alias from stdin and returns the canonical Response. Its history and repair
turns retain canonical blocks; complete Response custody remains separate from the
sidecar-free Message view. Provider HTTP, credentials, TLS and tokenizer code are
removed from the bot. Memory/persona/workflow domain records remain unchanged.
The existing generation handler now admits full tool-free IR history through the
shared public attenuation and HTTP execution owners. Alias selection changes only
the request-local configuration; model URLs, credentials and tools remain server-owned.

The bot no longer makes a separate exact-token-packing claim. It sends bounded,
intact history/evidence to the configured server and preserves the prior delivered
history on errors or context refusal. The old tokenizer-only tests and qualification
branch are retired; existing memory, persona, workflow and delivery assertions are
retained against the canonical thin-client path.

Qualification includes the Discord 124-test local suite, real subprocess wrapper
checks, actual Python-to-C canonical admission, and normal/sanitizer handler,
CLI-marshal and HTTP-generation fixtures. These are local source checks. The
existing edc image and older b61 browser results do not qualify this newer source;
a final canonical image and browser qualification are still required.

## Qualified source cleanup, not yet deployed

The cleanup removes the four unused economizer wire/planner/proof owners and
992 lines of their dedicated tests. It also retires the request shadow observer,
raw provider-policy entry points, compatibility history-repair wrapper and
inactive predecessor test branches. Actual IR repair cases and live provider
edge codec tests remain. Registration, source ownership and test scheduling
were checked after the deletions.

Anthropic and OpenAI frontends now use the same prepared canonical model
executor in `src/server/server_model_execution.c`. Provider-native parse/render,
HTTP beta/error/Retry-After behavior and streaming codecs stay at their actual
external edges. Host assembly no longer manufactures Anthropic `raw.system`;
typed IR blocks retain cache flags and optional `system_scalar` layout preference.
The Anthropic backend alone creates the native scalar/array system form.
Old SDK/cache data is retained; changing the canonical profile can select a new
owner-issued cache scope instead of falsely reusing an incompatible one.

Current qualification includes focused normal and ASan/UBSan suites, exact
wire/cache-layout goldens, full Go model-contract compilation, frontend517 tests
and TypeScript,28 static gates, thin-client mock isolation and local compilation
of the client/server/KB/gateway/runtime-web products. These are source checks;
QA still runs b61. Proofs:

- `/var/tmp/aimee-ir-economizer-final-retirement-proof.json`
- `/var/tmp/aimee-obsolete-provider-test-retirement-proof.json`
- `/var/tmp/aimee-ir-common-model-final-proof.json`
- `/var/tmp/aimee-ir-system-scalar-final-proof.json`
- `/var/tmp/aimee-ir-final-static-integration-r2/proof.json`
- `/var/tmp/aimee-ir-final-local-build/proof.json`
- `/var/tmp/aimee-ir-final-go-model-compile/proof.json`

The exact `edc58e19c` image subsequently built successfully after verified retention
of unused source copies and a builder-cache-only cleanup. Image
`sha256:5b18af89bee6da7b107bff2ef41edeb3e34207d577cbe353c72ef4918e0336a6`
contains the verified 87 products and 19 modules. The build preserved all 1798 IR
rows, 35 Vault entries and protected active/archived QA state, with no deployment
or model calls. Custody proof:
`/var/tmp/aimee-ir-single-edc58e19c-build/single-QA-build-custody-proof.json`.
The further source audit below is newer than that image and requires its own build.

## Last deployed source and browser qualification

The installed and pushed application source is
`b61ebe1dd64d5ec5c652633999cedcd862001861`, version `1.0.2-491-gb61ebe1dd`,
image `sha256:58e482c132a1f034409eb1433094bb79300a7cb08a8b65e3621cf8176a0fd9d5`.
The canonical build verified 87 runtime products and 19 modules. The single-QA
upgrade preserved all 1680 existing IR rows, 35 Vault entries, native SDK/cache
state, workspace and configuration. Five installed CLI negative cases pass on
this exact image. Source and upgrade proofs:
`/var/tmp/aimee-ir-b61ebe1dd-source-qualification.json`,
`/var/tmp/aimee-single-QA-next-upgrade-held/proof.json`, and
`/var/tmp/aimee-ir-final-cli-negative-b61ebe1dd-held/proof.json`.

All six final browser sends pass: retained historical Claude continuation,
HTTP grounded file read and same-session follow-up, and retained native Codex
file read, follow-up and same-session continuation after app restart. These are
six browser sends, not a claim of six upstream model iterations. Actual provider
telemetry confirms Opus5 and gpt-6.1-sol for the respective native calls.
The final model-idle seal contains 1798 canonical IR rows with the original 1680
unchanged. Four additional `resp_` histories are owned canonical Responses
continuation checkpoints, verified against their actor, parent-session hash,
ordered history and final count commit. They are not additional browser sends.

The historical Claude call reused its existing connection and session. Five
existing derived credential slots materialized through the normal OAuth owner;
the imported source credential and other 30 slots remained exact. The retained
Codex thread, scope and context were reused, with prior SDK file prefixes
byte-exact and tool IDs preserved. The Claude SDK cache carries a fixed wrapper seed; authoritative
history is supplied by the host. Its final upstream HTTP body was not captured,
so cache comparison does not establish that final wire payload. No failed
historical case was relabeled or silently retried.
An earlier zero-send harness preflight and an overstrict seal assertion remain
as separate artifacts. Receipts:
`/var/tmp/aimee-live-single-QA-b61ebe1dd/qualification-index.json` and
`/var/tmp/aimee-live-single-QA-b61ebe1dd/final-idle-proof.json`.

The earlier f3 source exercised all 14 configured ChatGPT model/transport pairs
(seven HTTP and seven native), with actual grounded reads and same-session
follow-ups. The final b61 checks above exercise representative HTTP/native
profiles and retained Claude/Codex history; they do not claim another complete
14-profile run on b61. Source-owner equivalence records the precise intervening
changes: `/var/tmp/aimee-ir-f3-b61-source-owner-equivalence.json`.

Responsive replay passes nine views across 1280/390/320px: retained native and
HTTP tools/usage plus the historical Claude error. GET/reload reproduces the
same canonical history, with zero JavaScript errors, horizontal overflow or idle
mutations. The passive HTTP case uses a retained f3 history on the b61 runtime,
not the new six-send HTTP session. Proof:
`/var/tmp/aimee-single-QA-next-passive-held/proof.json`.

The isolated channel check passes exactly one new channel and one canonical
Unicode/JSON-looking literal USER Message, with exact GET/SSE/reload agreement,
correct sender, zero idle mutations and no model calls. The original attempt
stopped at a disappearing onboarding tooltip before any channel write; its
failure and zero-write custody are retained. The bounded helper correction
reused the same absent channel/tag and performed the one authorized write pair.
Proof: `/var/tmp/aimee-single-QA-next-channel-r2/proof.json`.
Root visually inspected actual desktop and320px replay screenshots and the
reloaded channel screenshot. Final custody preserves all1798 IR rows and all
protected state, with only the exact authorized channel/message added. Native
SDK/jobs remain idle. Proof:
`/var/tmp/aimee-single-QA-final-passive-channel-custody/channel-after-proof.json`.
Current live permission/question replies, cancel and steer flows were not
exercised in this QA. Gateway platform delivery remains unwired, as documented below; no live platform delivery
is claimed. Production promotion remains subject to `OWNERS.md`. All sections
headed previous checkpoint below are historical evidence.

## Current QA topology: one active instance and one Claude connection

The user paused the larger IR/browser goal, requested consolidation of the
duplicated QA connections, then requested resumed IR work with all agents. That cleanup keeps exactly one active QA
instance: `aimee-ir-clean-361fefa7f` at `https://192.168.0.67:8451`. Its existing
Claude connection is `ir-clean-361fefa7f-claude`, used by the existing default alias
`ir-clean-361fefa7f-primary`. All15 ChatGPT aliases remain unchanged.

The four other owned QA projects are offline fixtures, with all12 preserved
containers stopped and restart policy `no`: `aimee-internal-ir-fresh-1b3875369`,
`aimee-internal-ir-fresh-601aff47e`, `aimee-internal-ir-fresh-d8511ad41`, and
`aimee-ir-clean-ab1059c6f`. Their histories, volumes, images, configuration,
encrypted credentials and SDK caches are preserved. Their connection records are
archival data, not active connections. Ports8445/8447/8449 are closed;8451 is live.

Do not restart these fixtures, clone Claude credentials into another QA instance,
or reuse old multi-instance build/test wrappers. Future authorized work uses the
single active QA instance and its connection. Restoring an offline fixture needs
an explicit topology decision that preserves one active connection. No `.100`
or real user deployment was investigated or modified.

Authoritative consolidation proof:
`/var/tmp/aimee-five-qa-Claude-consolidate/proof.json`.
The live guest manifest is
`/opt/aimee-single-claude-consolidation-38f813090682565e-proof/active-archived-manifest.json`.
The consolidation preserved all1680 then-existing active rows; archived last-verified counts are76,128,360
and93 respectively. All protected configuration/cache/Vault/SDK/workspace hashes
were checked, and archived live SQL histories/native/jobs were compared after
app shutdown before stopping their databases. No images, volumes or data were
removed. The separately approved renewal completed once on this existing active Claude
connection: HTTP200 acknowledged, exactly one encrypted source entry changed,
all34 other entries and all1680 prior IR rows/config/cache remained exact. No
provider or model request was made during that cleanup. Proof:
`/var/tmp/aimee-fresh-Claude-renewal-next-held/import-custody-proof.json`.
The wrapper failure after acknowledgement is preserved separately; independent
acknowledgement/custody checks pass and the POST was never retried.

## Source-binding correction before deployment

The e26 candidate image built successfully but was held before deployment after
root found a real serialization loss: source/ASR hashes existed only in block.raw,
which canonical serialization intentionally omits. The first review and JSON
before/after equality did not establish preservation of those values and are
explicitly superseded for that claim. The candidate image and original receipts
remain retained; it was never installed on the active QA.

The bounded correction uses existing typed Context revision fields on assets and
ASR transcripts, with fixed source-binding namespaces, the full source SHA256 and
epoch0. It preserves USER/unverified authority and typed file identity. Parsed
messages retain these bindings after raw/original memory is freed; ASR validates
the parsed source and rejects changed content. Unknown hashes remain unbound.
Normal/sanitizer tests, actual C-to-Go acceptance and production gateway relink
pass. Proof: `/var/tmp/aimee-ir-gateway-source-binding-proof.json`.

Only unused Docker builder cache was pruned once to provide space for the
corrected image. All images, volumes,15 container identities/configuration,
renewed35-entry Vault,1680 IR history and native SDK/cache data remain exact.
Proof: `/var/tmp/aimee-single-QA-builder-cache-prune/proof.json`. The corrected
b61 image subsequently built, upgraded and passed the final model checks above.

## Gateway source follow-up: implemented and qualified

The gateway migration now retains owned canonical USER Message blocks at external
Telegram/webhook ingress, through local RPC, ordered CLI subscriber results,
process mirrors, delivery routing and TTS dispatch. Vendor JSON is released only
after admission has taken ownership. Every attachment remains a separate hidden
IMAGE/DOCUMENT file asset; ASR retains its original audio source and adds a
separate unverified USER task transcript. Unreadable non-audio attachments do not
discard valid text, and inert sidecars cannot change authority or role.

The typed collector preserves ordered full messages, opaque state, media and
exact tool JSON. Terminal execution cannot turn an unfinished snapshot into a
completed answer. Human CLI/ACP displays and actual external adapters own text
projection. The webhook freed-text bug is fixed and exercised by the real HMAC
HTTP branch under sanitizers. Eight focused gateway suites pass normally and
under ASan/UBSan; IR guards, format, full gateway linkage and thin CLI purity pass.
Independent frozen-source review:
`/var/tmp/aimee-ir-gateway-independent-final-review.json`.

A separate confirmed MCP defect returned a fabricated delivery acknowledgement
without a wired gateway owner. It now returns an explicit typed unavailable tool
error. The question formatter retains long UTF8 input safely and its descriptor
accurately assigns presentation to the client. Normal/sanitizer handler and
forwarding tests pass. No messaging owner was added and no external platform was
contacted. The standalone gateway entrypoint remains unwired; do not claim live
Telegram/ntfy/webhook delivery. Positive collector coverage is the production
HTTP-over-UDS canonical NDJSON fixture, not a fabricated scoped CLI chat call.

The numeric embedding and OCR artifact caller audit found no confirmed existing
conversation reconstruction in those specific producers; its exact domain-origin
traces are in `/var/tmp/aimee-ir-embedding-seam-audit.md`. This is finite evidence,
not an inference from a type name or a claim that numeric vectors are messages.

The b61 installed image includes these repairs and the typed source-binding
correction. Never restart archived fixtures or clone their credentials. All
earlier failures remain sealed. Current validation is summarized above.

## Historical evidence

The full chronological handoff, including all previous failures and qualification
limits, is retained in Git at `4f063a775:docs/IR_MIGRATION_HANDOFF.md`:

```sh
git show 4f063a775:docs/IR_MIGRATION_HANDOFF.md
```

The sealed proof index is `/var/tmp/aimee-ir-final-qualification-index.json`.
It records qualification of the old implementation, not satisfaction of the
code reduction requirement. The original archived7db Claude refusal remains a
failure; current c28bb continuation success does not relabel it. Earlier HTTP
empty finals, native empty finals and harness failures remain in their original
receipts. The source-binding candidate e26 was held, not deployed. No history,
credential, SDK cache, image or volume should be removed to improve diff counts.
