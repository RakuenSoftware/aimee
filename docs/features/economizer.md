# The aimee Economizer

The economizer has three deliberately simple modes:

```yaml
economizer:
  mode: safe             # off | safe | aggressive (default: safe)
```

```sh
aimee config set economizer.mode safe
```

## Modes

| Mode | Behavior |
| --- | --- |
| `off` | Sends the normal request. No economizer transform or wire snapshot is used. |
| `safe` (default) | Compacts strict JSON returned by a local tool before that result is sent to a provider for the first time. It does not rewrite history or cache controls. |
| `aggressive` | Includes safe behavior and enables the existing lossy history and tool-output reducers on supported OpenAI-family routes. |

Safe compaction removes only insignificant JSON whitespace outside strings. It is deterministic,
rejects malformed or non-JSON text, and keeps the original unless the serialized result is shorter.
Because it runs at local tool completion, the provider has never seen the original bytes: there is
no existing OpenAI or Anthropic cache prefix to invalidate. Safe never summarizes, truncates,
rehydrates, folds previously sent messages, moves a cache breakpoint, or calls a remote token-count
endpoint.

Aggressive is explicitly lossy. It can reduce older context and condensed tool results. That can
lower fresh input, but it can also reduce cache hits or omit useful detail. Native Anthropic routes
are excluded from gateway history mutation because Anthropic caching depends on an exact matching
prefix. Use aggressive only when that tradeoff is acceptable.

## Provider behavior

OpenAI and Anthropic both cache stable prefixes, but expose different controls and accounting. The
economizer does not try to infer hidden automatic breakpoints, predict cache residency, or change
client-supplied cache controls.

- OpenAI-family requests may use aggressive history reduction. The selected request body is frozen
  into an exact-length snapshot and ordinary retries reuse those exact bytes.
- Native Anthropic requests preserve the caller's message prefix and `cache_control` placement in
  every mode.
  Safe fresh-result compaction is still available in aimee's own agent loop because it happens
  before first dispatch.
- Provider-reported usage is accounting evidence after a call, not authorization for a pre-call
  rewrite. No counting preflight or economizer-owned restore/resend is performed.

These rules avoid the failure mode where a compressor saves a few fresh tokens but forces a much
larger cached prefix to be billed as a cache write or uncached input.

## Configuration

`economizer.mode` accepts only `off`, `safe`, or `aggressive` (case-insensitive). An omitted setting
defaults to `safe`; malformed or unknown values fail configuration loading. The deprecated
`economizer.enabled` and `economizer.aggressive` fields are not accepted. `modules.economizer: false`
is an authoritative kill switch and makes the effective mode `off`.

Changes apply on the next request/turn. See [Settings](../SETTINGS.md) for the CLI and configuration
surface.
