# translation module

## Purpose and non-goals

`translation` is required core and converts canonical IR to and from provider and client wire shapes without
changing the selected task, identity, permissions, or route. It owns structural mapping for messages,
system blocks, tools, thinking, usage, stop reasons, and streams. It does not choose providers, perform
HTTP retries, authorize tool calls, or own the external protocol listener.

## Public contracts

The canonical ingress adapters are
`src/modules/translation/aimee_frontend_anthropic.c`,
`src/modules/translation/aimee_frontend_openai.c`, and
`src/modules/translation/aimee_frontend_responses.c`. Their public parse-to-canonical contract is
`src/modules/translation/include/aimee/translation/aimee_frontend.h`, included through the canonical
namespace `aimee/translation`. These adapters only map supported client/provider request shapes into the
IR-owned `aimee_request_t`; they do not listen, dispatch, select a route, or send provider requests.

Remaining translation seams include the `aimee_ir_build_provider_body` entry point in
`src/headers/aimee_ir_serve.h`, provider response parsers, and backend serializers under `src/server`.
The target module owns provider-body conversion; the current IR-named symbol remains a
relocation/compatibility seam until callers and installed headers migrate. `anthropic_http.c` still
contains legacy `translate_request` and `build_provider_body` paths beside the typed IR path, while
`openai_chat.c` has its own builder. Backend egress adapters and the mixed `aimee_ir_serve.c` and
`aimee_ir_stream.c` implementations remain explicitly deferred. `router_advise.c` is workflow-owned
despite its name and is outside translation.

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

The descriptor owns `src/tests/test_aimee_frontend.c` as the direct contract for the three canonical
ingress adapters. Adjacent coverage includes `test_anthropic_ingress.c`, `test_anthropic_shape.c`, `test_openai_shape.c`,
`test_ir_crossproto_egress.c`, `test_ir_legacy_parity.c`, `test_aimee_ir_serve.c`, and backend shape tests.
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
