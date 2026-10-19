# translation module

## Purpose and non-goals

`translation` is required core and converts canonical IR to and from provider and client wire shapes without
changing the selected task, identity, permissions, or route. It owns structural mapping for messages,
system blocks, tools, thinking, usage, stop reasons, and streams. It does not choose providers, perform
HTTP retries, authorize tool calls, or own the external protocol listener.

## Public contracts

`src/modules/translation/module.yaml` is the complete module-local ownership inventory for shipping
translation units, canonical public headers, direct tests, and this module document.
`ownership_complete: true` rejects an unlisted module-local C or private-header file and omission of the
canonical document. Canonical public-header placement remains enforced by the descriptor-derived
header-layout gate. This physical ownership claim does not imply that every exported adapter helper is
selected by a production journey.

The canonical ingress adapters are
`src/modules/translation/aimee_frontend_anthropic.c`,
`src/modules/translation/aimee_frontend_openai.c`, and
`src/modules/translation/aimee_frontend_responses.c`. Their public parse-to-canonical contract is
`src/modules/translation/include/aimee/translation/aimee_frontend.h`, included through the canonical
namespace `aimee/translation`. These adapters only map supported client/provider request shapes into the
IR-owned `aimee_request_t`; they do not listen, dispatch, select a route, or send provider requests.

The canonical egress and response adapters are
`src/modules/translation/aimee_backend_anthropic.c`,
`src/modules/translation/aimee_backend_openai.c`,
`src/modules/translation/aimee_backend_responses.c`, and
`src/modules/translation/aimee_backend_bedrock.c`. Their public wire-to-canonical and
canonical-to-wire contract is
`src/modules/translation/include/aimee/translation/aimee_backend.h`. These backend wire-format
adapters build provider request bodies and parse provider responses; they do not send requests,
select providers, sign AWS requests, load credentials, or retry transport failures. Direct contract
coverage lives in `src/tests/test_aimee_backend.c` and `src/tests/test_aimee_backend_bedrock.c`.

Canonical streaming conversion lives in `src/modules/translation/aimee_ir_stream.c`, with the public
contract at `src/modules/translation/include/aimee/translation/aimee_ir_stream.h`. Its bounded state
machines convert Anthropic events, OpenAI Chat chunks and decoded Bedrock ConverseStream event JSON into IR deltas, then
render IR deltas as Anthropic stream events. They do not decode Bedrock binary framing, write HTTP/SSE
responses, or select the live relay. Direct coverage lives in `src/tests/test_aimee_ir_stream.c` and
`src/tests/test_aimee_converse_stream.c`.

Incremental Responses conversion lives in `aimee_responses_stream.c` and
`aimee_responses_frontend_stream.c` under this module. The backend emits canonical text,
reasoning and function-argument deltas before completion, then reconciles prefixes and item
snapshots through the existing full Responses decoder. The frontend preserves authored message
groups and opaque reasoning and defers item-done/completion to the host's durable-history boundary.
Native framing remains bounded inside the entry adapter; transport, policy and persistence stay
with their existing owners. `test_aimee_responses_stream.c` and
`test_server_responses_stream.c` cover conversion and fragmented host transport.

Native harness conversion lives in `src/modules/translation/harness_ir.c`, exposed by
`src/modules/translation/include/aimee/translation/harness_ir.h`. Claude stream-json, Codex
app-server and ACP session/update frames are converted at their reader boundaries, before legacy execution callbacks.
The adapter emits canonical typed session events synchronously; it does not load credentials,
choose a session owner, persist, or deliver browser events. Host execution code owns those steps.
`src/tests/test_harness_ir.c` covers typed tools/results, thinking signatures, streamed text,
control correlation, unknown items, and sink failure propagation.

Remaining translation seams include the `aimee_ir_build_provider_body` entry point in
`src/headers/aimee_ir_serve.h`.
The target module owns provider-body conversion; the current IR-named symbol remains a
relocation/compatibility seam until callers and installed headers migrate. `anthropic_http.c` still
contains legacy `translate_request` and `build_provider_body` paths beside the typed IR path, while
`openai_chat.c` has its own builder. The mixed `aimee_ir_serve.c` implementation remains explicitly
deferred: it owns rollout gates, memory-stage configuration and registration, and a transitional legacy
response bridge. `router_advise.c` is workflow-owned despite its name and is outside translation.

### Native runtime entry boundaries

Native tool execution and simple completions use owned IR response decoders. Anthropic request
and response parsing share one block adapter, including signed and redacted thinking. Responses
SSE framing lives in `aimee_backend_responses.c`; it preserves completed output items, native item
order, encrypted reasoning and full opaque tool IDs when the terminal response omits streamed
items. Incomplete/error terminal events become typed IR outcomes. Unknown content and unfinished
streams fail admission; execution does not re-parse provider JSON or recover calls from text.

`history_ir.c` owns legacy-history entry conversion through the three frontend adapters.
Canonical persisted histories decode through the IR message codec and are rendered only when
building an outbound request. Media and reasoning remain in that history even where a target
wire cannot replay foreign opaque reasoning state. Native Responses requests request encrypted
reasoning so stateless continuation can retain it. Tool results retain structured content through
the canonical history; protocol-specific representation limits apply at egress.

### Host-observed native replay state

The native request codec exposes a borrowed, thread-bound state guard. A server scope may
restore opaque replay bindings from independently observed canonical responses; the translation
module itself performs no I/O or authorization. Hosts recheck the owned request immediately
before provider rendering, after restored history and policy edits. A guard refusal releases and
zeros that request. Scoped callbacks must be restored before their context is released.

Raw-sidecar equality checks parse the immutable reference without invoking this guard. Guarding
both the transformed request and its comparison reference could make removed opaque state look
unchanged and accidentally replay the original bytes. The canonical equality and actual native
prefix fences still decide whether a byte-preserving fast path is admissible.

## Dependencies and consumers

- `ir`: supplies the canonical request, response, block, tool, usage, and streaming representation.
- `module-runtime`: supplies required lifecycle and extension contracts for the core conversion path.

Consumers include gateway and protocol ingress/egress, agent/delegate provider drivers, response
composition, OpenAI and Anthropic compatibility routes, Bedrock backends, and shadow/parity checks.
Provider selection and route failover remain routing concerns; provider transport retries stay with the
provider driver or execution owner. Translation must not silently retry or duplicate a wire call.

## Providers and readiness

Translation is deterministic core with `aimee_ir` implementations selected by source wire and destination driver,
not an optional remote provider. A provider adapter is ready only when request and response mappings cover
its advertised capabilities. Unknown block or stop-reason semantics must fail explicitly or use a tested
loss policy; a successful HTTP connection does not prove translation readiness.

## Configuration and activation

- `runtime_toggle.supported`: `false`; translation is required for every supported non-identical wire path.

Settings may tune reasoning handling, caching, streaming, model fields, or compatibility behavior, but
they must not create a second untracked translation pipeline. Provider and protocol availability determine
which adapters are exercised; configuration fields should be exposed only when a compiled adapter reads
them and their effect is covered by `test_ir_*` or shape tests.

## Surfaces

The target translation surface exposes `provider-body` conversion, response parsers, stream-event maps,
and shape/parity fixtures. It owns no listener, HTTP route table, CLI command, dashboard, credential store,
or network retry loop. OpenAI, Anthropic, Bedrock, MCP, and ACP JSON framing remains with protocols while
the typed field mapping belongs here.

## Data and migrations

Translation owns transient JSON and canonical objects, not an independent durable schema. Stored runs,
transcripts, provider traces, and shadow comparisons rely on stable `aimee_response_t` semantics and may
capture wire payloads. A mapping change must migrate or version any persisted interpretation and refresh
fixtures/baselines without rewriting historical provider evidence as if it used the new mapping.

## Security and privacy

Converters must preserve system/user role boundaries, hidden reasoning, tool identifiers, and untrusted
arguments without promoting content into authority. Provider-specific raw fields may contain credentials
or private prompts and require bounded redaction. Translation must never silently expose `THINKING` as
answer text or drop policy-relevant tool data to satisfy a weaker destination shape.

## Supported journeys

An Anthropic, OpenAI, ACP, or other ingress is parsed into `IR`; after routing and policy, translation emits
the selected provider's request and converts returned response/stream events back into canonical blocks;
the client protocol then serializes them. Native same-wire passthrough may preserve bytes in legacy-parity
mode (covered by `test_ir_legacy_parity.c`), but any mutated request must use the canonical conversion journey.

## Tests and failure behavior

The descriptor owns `src/tests/test_aimee_frontend.c`, `src/tests/test_aimee_backend.c`,
`src/tests/test_aimee_backend_bedrock.c`, `src/tests/test_aimee_ir_stream.c`, and
`src/tests/test_aimee_converse_stream.c` as direct contracts for canonical ingress, backend wire-format,
and streaming adapters. Adjacent coverage includes `test_anthropic_ingress.c`,
`test_anthropic_shape.c`, `test_openai_shape.c`, `test_ir_crossproto_egress.c`,
`test_ir_legacy_parity.c`, and `test_aimee_ir_serve.c`.
Malformed or unsupported input must return a bounded error without partial tool execution; stream mapping
must emit one coherent terminal state and free partially built structures.

## Operational diagnostics

Use `source_wire` and destination wire labels, driver capability reports, shape fixtures, IR shadow mismatches, and
provider-body diagnostics to locate mapping failures. Diagnostics should distinguish parse, canonical
mutation, serialization, provider rejection, and response conversion, and should report a field path or
block type without logging full sensitive payloads.

## Compatibility

External wire shapes, canonical field meanings, cache placement, tool schema mapping, stop reasons,
stream event ordering, and parity-mode bytes are compatibility contracts. Consolidating legacy builders
into `src/modules/translation` must preserve tested provider behavior; removal of a fallback requires a
definition/caller inventory proving no supported journey still selects it.

## Extension and removal

Add an adapter through the shared IR conversion interface and capability contract rather than copying an
ingress handler. The parallel legacy and typed builders in `anthropic_http.c` and `openai_chat.c` are
consolidation candidates, not automatically dead code; parity and mutation branches must be traced first.
Translation cannot be removed while Aimee supports more than one external/provider wire.

The [slice 32 liveness audit](../validation/core-modularization-slice-32.md) proves that every owned source
ships and that each translation unit contains a production path. It separately records seven helpers
whose exact current-worktree callers are tests only. Removing, privatizing, or activating those intended
egress and Bedrock-streaming contracts requires focused compatibility/readiness slices; they are not
silently treated as live merely because their source files ship.

Native request framing is owned by `wire_codec.c`. Unchanged same-protocol requests can retain
exact original bytes; typed comparison also checks edits whose mutation flag was missed. Native
Anthropic edits retain unchanged content forms, client cache markers and TTLs. Explicit stable-prefix
policy normalizes translated requests. Authored Responses message groups are stored independently of
presentation separators, so rendering a transcript does not change replay text.

The live Chat-to-Anthropic incremental ingress uses canonical deltas unconditionally. Late usage,
thinking signatures, redacted content and initial tool input survive this boundary. Missing completion
fails with a typed error; an EOF cannot manufacture a successful end-of-turn. Other request/response
bypasses described above still require cutover.

The Anthropic HTTP native builder attaches its original body with
`aimee_frontend_parse_native_frame` before rendering. Its existing context-injection option selects
the shared canonical stage runner; disabling injection does not bypass canonical admission. Exact
body replay is selected only after comparing the host-adjusted typed request with the original.

Opaque JSON numeric values survive native entry, canonical ownership and explicit egress without a double roundtrip. The shared C JSON codec retains decimal tokens, including tool arguments and schemas; numeric mutation invalidates that token. Completed argument strings require full JSON framing. Browser canonical parsing/rendering likewise preserves decimals outside JavaScript's roundtrip range rather than displaying a rounded tool argument. Embedded NUL strings are refused because the C string contract cannot represent them faithfully.

Codex URL-mode MCP elicitation enters as a canonical external-interaction request containing the server, message, destination and opaque elicitation identity. The browser displays the host and requests explicit navigation consent; accepted URL replies contain no form data. HTTP(S), authority and length checks reject unsafe schemes, userinfo and controls. Consent records acceptance, not completion of the external flow. Unsupported device-verification and custom form modes remain decline/cancel only. See the [Codex protocol](https://github.com/openai/codex/blob/main/codex-rs/app-server-protocol/src/protocol/v2/mcp.rs) and [MCP URL elicitation contract](https://modelcontextprotocol.io/specification/2025-11-25/client/elicitation).

Browser-owned Claude and Codex controls persist the admitted canonical operator decision before native delivery. Native serialization does not become a second history source: the adapter resolves its pending identity before sending the generated native response. This retains cancel intent and complete answer arrays even when the native protocol uses a coarser denial or joined answer string. A capture failure prevents approval bytes reaching the child.

### Native tool hook entry

`hook_ir.c` admits complete bounded native hook frames into owned canonical tool-use/result values. CLI forwarding carries a versioned hook intent; marked intents cannot fall back to native fields. Legacy installed clients convert once at the server entry. Scope and authenticated harness metadata remain separate transport fields, and policy/storage authority stays with its existing owner. Missing legacy tool IDs are retained as missing: hook intents are control inputs and cannot be published as model history without real identity. Unknown namespaces are preserved by admission and refused by the current flat hook policy catalog before routing or policy side effects.

### Native Codex history adoption

`aimee_codex_history_items` renders admitted canonical messages through the existing
Responses output owner. Both native Codex entry points accept an optional typed
history request and adopt its items with the stable `thread/inject_items` method
after thread start/resume and before `turn/start`. Roles, block order, opaque
reasoning and full tool call/result IDs remain typed until this output boundary.
Unsupported SDKs and invalid history refuse before model execution. The host must
prove account/connection scope and supply only an unseen canonical tail on resume;
the migration has not yet activated this option at the browser/session callers.
The selected-model gateway still owns opaque replay verification.

The real SDK qualification runs in CI against the pinned SDK, using the compiled
adapter, an empty native HOME and a loopback Responses provider. Run it locally:

```sh
python3 src/tests/test_codex_native_adoption.py \
  --adapter-binary /path/to/unit-test-cli-codex --codex-binary /path/to/codex
```

It checks the actual provider input, then starts a second adapter/SDK process and
resumes the native thread. This proves history adoption and native persistence,
not upstream cache billing. Mock subprocess fixtures also cover both native entry
points, resumed/fresh threads, malformed history and method refusal without a
subsequent model turn. Codex startup uses the supported `on-request` approval
policy; the installed 0.160.1 SDK refuses the old `untrusted` policy.
