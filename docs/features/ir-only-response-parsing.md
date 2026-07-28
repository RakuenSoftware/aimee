# Canonical response parsing

Provider adapters parse wire responses directly into canonical IR. Core routing, retries, tools,
accounting, economizer stages, and clients do not inspect provider JSON.

The canonical response can contain:

- text segments;
- reasoning segments and signatures where the provider exposes them;
- tool calls with stable IDs and structured arguments;
- usage and cache accounting;
- finish/stop reason;
- provider error and retry class;
- streaming deltas that assemble into the same final IR.

## Rules

- Keep provider aliases and quirks in its translator.
- Preserve unknown but bounded metadata only when a later contract needs it.
- Reject malformed tool calls instead of converting them to prose.
- A body that parses but yields no usable content is a typed empty-response error with a bounded
  diagnostic of the received shape.
- Retry preserves the requested tool surface.
- Serialize public responses from canonical IR, never by forwarding provider bytes.

## Add a provider

1. map canonical request to the provider wire;
2. parse non-streaming and streaming responses into the same IR;
3. map tool IDs, arguments, usage, cache, stop, and errors;
4. add fixtures for text, reasoning, tools, mixed content, empty content, malformed bodies, and rate
   errors;
5. round-trip through public OpenAI/Anthropic ingress where applicable;
6. verify economizer and policy stages remain provider-neutral.

Fixtures under `src/tests/fixtures/ir/` are the contract.
