# Economizer

The economizer reduces provider context and cost after the request has become canonical IR. It must
preserve tool semantics, recoverability, and provider cache behavior.

## Tiers

| Tier | Behavior |
| --- | --- |
| `off` | no reduction or cache mutation |
| `safe` | lossless folding, stable cache boundaries, command-aware condensation with spills |
| `aggressive` | safe tier plus configured lossy compression/mutation on supported provider families |

`modules.economizer: false` is the hard off switch.

## Order

```text
provider ingress -> canonical IR -> invariant/policy checks
                 -> folding and tool condensation
                 -> provider cache alignment
                 -> provider translation
```

Provider-specific JSON is not the economizer's working format. Anthropic, OpenAI Responses, Chat
Completions, Gemini, Mistral, and Bedrock share the canonical stages; translation handles wire
differences at the edge.

## Safe tier

- Stable old context can be folded into a recoverable summary/reference.
- Recognized command output keeps failures and diagnostics while dropping repeated progress/noise.
- Full tool output is written to a bounded spill with a recovery pointer.
- Cache breakpoints are placed where provider semantics permit them.
- A disabled or failed reducer returns the original IR.

Lossless means the system can recover the omitted bytes, not that every provider receives them in
the first request.

## Aggressive tier

Lossy operations need an explicit provider and content contract. They must not alter tool-call IDs,
arguments, result pairing, system/developer authority, current user intent, or required evidence.

Unsupported provider families stay on the safe path. A global tier does not justify mutating a wire
format that has no tested adapter.

## Accounting

Telemetry records input bytes/tokens before and after each stage, spill bytes, cache decisions,
avoided cost, recovery, and fallback. Do not count a reduction twice when folding and provider cache
alignment touch the same prefix.

```bash
aimee economizer stats
```

## Verify

Test byte-identical off mode, tool-call/result pairing, retry, streaming, Unicode, large outputs,
spill recovery, cache boundaries, provider round trips, and failures in each stage. Quality gates
must compare task outcomes, not only token counts.

See [Tool-output condensation](tool-output-condensation.md).
