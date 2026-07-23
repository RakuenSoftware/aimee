# The aimee Economizer

The economizer controls whether aimee changes provider-bound context to reduce its size. Its first
rule is that declining to economize is better than making a request more expensive. The public
surface is deliberately small:

```yaml
economizer:
  mode: safe             # off | safe | aggressive (default: safe)
```

```sh
aimee config set economizer.mode safe
aimee config get economizer.mode
```

The same setting is available on the web **Settings** page and through the authenticated
`POST /v1/config/set` operation. Changes are hot and apply to the next request or agent turn.

## Mode summary

| Mode | Fresh local tool result | Existing history | Native Anthropic history | Lossy |
| --- | --- | --- | --- | --- |
| `off` | Unchanged by the economizer | Unchanged | Unchanged | No |
| `safe` (default) | Strict JSON whitespace may be removed before first dispatch | Unchanged | Unchanged | No |
| `aggressive` | Includes `safe` | Eligible tool-result bodies and older context may be reduced on supported OpenAI-family paths | Unchanged | **Yes** |

`safe` is the normal choice. Select `off` when the economizer must do absolutely no work on the
request. Select `aggressive` only when lower context size is more important than preserving every
detail and the effect on provider cache reuse is acceptable.

## What `safe` guarantees

Safe has one live transform: deterministic whitespace compaction of a newly produced local tool
result when that result is one complete, strict JSON value.

It runs immediately after local tool execution and before the result is added to the conversation.
The provider therefore never received the whitespace-heavy version. There is no previously cached
provider prefix containing those original bytes to invalidate.

The compactor:

- removes only RFC 8259 whitespace outside JSON strings;
- retains every other input byte in its original order;
- never changes whitespace or escapes inside strings;
- accepts arrays, objects, strings, numbers, booleans, and `null`;
- requires valid UTF-8, valid JSON syntax, and unique decoded object keys;
- accepts at most 16 MiB, 64 levels of nesting, and 65,536 members in one object;
- emits a result only when it is shorter than the input; and
- returns the original result unchanged on every rejection or internal failure.

This is JSON-semantic preservation, not byte preservation: insignificant whitespace is the only
textual difference. Safe does not promise that a provider tokenizer will report fewer tokens for
every compacted result, or that the bill for an individual request will decrease. It avoids the
more dangerous operation: rewriting content that has already participated in a cache prefix.

Safe does **not** summarize, truncate, fold history, condense command output, call another model,
rehydrate content, call a token-counting endpoint, infer a cache breakpoint, or alter
provider/client cache controls.

Non-JSON text, malformed JSON, duplicate keys, invalid Unicode, oversized/deep input, and JSON
that cannot be made shorter pass through unchanged. Duplicate keys are rejected because parsers can
disagree about which value wins; silently normalizing them would not be a safe transform.

### Safe example

This fresh tool result:

```json
{
  "status": "ok",
  "items": [1, 2, 3],
  "message": "spaces inside this string stay"
}
```

is presented on first dispatch as:

```json
{"status":"ok","items":[1,2,3],"message":"spaces inside this string stay"}
```

If the same text were already part of conversation history, safe would leave it alone.

## What `aggressive` does

Aggressive includes safe compaction and opts into aimee's existing lossy context reducer on
supported OpenAI-family routes. Depending on the request path and conversation shape, it may:

- shorten older tool-result bodies while retaining configured head and tail excerpts; and
- fold eligible older history while retaining a recent-message tail.

The reducer works on a copied view. A failed allocation, unsupported structure, parsing failure, or
internal error bypasses the transform. The gateway applies a candidate only when its local token
estimate is strictly smaller and a structural check finds no orphaned tool-call/tool-result pairs.
Requests without a resolvable session identity bypass gateway mutation. These checks reduce risk;
they do not make aggressive lossless or prove provider-side savings.

The exact effect depends on the seam:

| Request path | Aggressive behavior |
| --- | --- |
| aimee agent loop to an OpenAI-compatible Chat Completions endpoint | Tool-result compression and eligible older-history folding |
| aimee agent loop to an OpenAI Responses/ChatGPT endpoint | Tool-result compression; history folding is not applied on that path |
| OpenAI-family gateway egress | Compress-only candidate; applies only after shrink and structure gates |
| Native Anthropic/Claude egress | No history or gateway-body reduction |

Aggressive can omit details the model later needs. It can also replace a prefix the provider would
otherwise have read from cache. Use it for workloads where lossy reduction is an explicit tradeoff,
not as a guaranteed cost-saving switch.

The older command-aware spill-and-recall helper is not called by production request paths. See
[Tool-output condensation](tool-output-condensation.md) for its retired status; aggressive uses the
general context reducer instead.

## Provider cache and pricing realities

Both OpenAI and Anthropic reward stable prefixes. A smaller serialized request is not automatically
a cheaper request: changing an already cached prefix can turn discounted cache reads into cache
writes or uncached input.

### OpenAI and GPT-5.6

OpenAI documents implicit and explicit prompt caching for GPT-5.6. Explicit cache breakpoints are a
client/provider contract; aimee does not discover or move them. As documented in July 2026, OpenAI
explicitly bills GPT-5.6 cache writes at 1.25 times the uncached input rate, while cached input is
discounted. The same documentation tells clients to track `cached_tokens` and
`cache_write_tokens`. Requests above 272,000 input tokens use long-context pricing for the whole
request: twice the standard input price and 1.5 times the standard output price. These are OpenAI's
GPT-5.6 terms, not an Anthropic pricing attribution. See OpenAI's current
[GPT-5.6 model page](https://developers.openai.com/api/docs/models/gpt-5.6-sol) and
[latest-model guide](https://developers.openai.com/api/docs/guides/latest-model) before making a
pricing decision; provider terms can change.

Avoiding the 272K boundary can be valuable in principle, but aimee does not claim that it has done
so. The API does not provide a mandatory, exact pre-dispatch token count or future cache outcome.
Provider usage fields arrive after dispatch and are accounting evidence, not permission to rewrite a
request beforehand.

### Anthropic and Claude

Anthropic caching also depends on matching prefixes and cache-breakpoint placement. Its documented
hierarchy considers tools, system content, and messages, so changing tool definitions or earlier
content can invalidate later cached content. Anthropic offers automatic caching and explicit
`cache_control`; aimee preserves the caller's placement and native Anthropic history in every mode.
See Anthropic's current
[prompt-caching guide](https://platform.claude.com/docs/en/build-with-claude/prompt-caching) and
[tool-use caching guide](https://platform.claude.com/docs/en/agents-and-tools/tool-use/tool-use-with-prompt-caching).

As documented in July 2026, Anthropic's default ephemeral cache lasts five minutes and is refreshed
without another write charge when it is read. A one-hour TTL is also available. Anthropic lists the
general prompt-cache multipliers as 1.25 times base input for a five-minute write, 2 times for a
one-hour write, and 0.1 times for a cache read; actual per-token prices are model-specific and other
pricing modifiers can stack. Responses distinguish `cache_read_input_tokens` from
`cache_creation_input_tokens`, with five-minute and one-hour creation detail. Check the current
model table in the prompt-caching guide rather than embedding a particular Claude model's dollar
rate in policy.

Anthropic's token-counting endpoint is free but documented as an estimate that can differ from
actual input usage. The economizer does not add a counting preflight. See
[Token counting](https://platform.claude.com/docs/en/build-with-claude/token-counting).

## Request and retry behavior

When `safe` or `aggressive` is active, aimee freezes each selected provider request body into an
immutable, exact-length in-memory snapshot before sending it. Ordinary HTTP retries for that request
reuse the same bytes. This prevents retry serialization drift; it does not add or change provider
cache directives.

If the snapshot cannot be created, aimee does not send an unfenced request under an enabled mode.
The request fails locally. `off` does not allocate the economizer snapshot.

Model fallback is a new request constructed for the fallback model and receives its own snapshot.
The normal agent-loop context-overflow recovery is independent of the economizer and may remove old
messages before a new attempt. The economizer itself performs no restore-and-resend cycle and never
rehydrates reduced material.

## Configuration surfaces

### YAML

```yaml
economizer:
  mode: safe
```

Only `off`, `safe`, and `aggressive` are accepted, case-insensitively. An omitted setting defaults
to `safe`. Unknown values, malformed sections, and legacy `economizer.enabled` or
`economizer.aggressive` fields fail configuration loading rather than silently selecting a mode.

An administrator can enforce a separate module-level hard kill:

```yaml
modules:
  economizer: false
```

That switch forces the effective mode to `off`, regardless of `economizer.mode`.

### CLI

```sh
aimee config set economizer.mode off
aimee config set economizer.mode safe
aimee config set economizer.mode aggressive
aimee config get economizer.mode
aimee config show
```

### HTTP API

The authenticated local/control-plane configuration operation accepts the same scalar value:

```http
POST /v1/config/set
Content-Type: application/json

{"key":"economizer.mode","value":"safe"}
```

Read it with `POST /v1/config/get` and `{"key":"economizer.mode"}`. Normal aimee transport
authentication and remote-write policy still apply; do not expose the management API merely to set
this option.

### Web Settings

Open **Settings**, search for `economizer.mode`, select the mode, and save. The field is generated
from the same typed configuration schema used by the CLI and API.

## Choosing a mode

- Use `safe` for the default operational posture. It changes only fresh strict JSON before first
  provider exposure and fails open to the original tool result.
- Use `off` for comparison testing, exact economizer pass-through, or a policy that prohibits even
  JSON whitespace normalization.
- Use `aggressive` only with explicit acceptance of lost detail and possible cache churn. Evaluate
  it against the actual provider, route, conversation distribution, and cache metrics.

For cost experiments, compare provider-reported uncached input, cache-write input, cache-read input,
output, and billed cost across representative multi-turn sessions. Comparing only serialized bytes
or total input tokens can incorrectly label a cache-busting transform as a saving. For GPT-5.6,
OpenAI directs clients to track `cached_tokens` and `cache_write_tokens`; Anthropic reports
`cache_creation_input_tokens` and `cache_read_input_tokens`. Treat those post-call values as
measurements, never as a precondition the economizer can know exactly.

## Troubleshooting

**A formatted JSON result did not shrink.** It may not be one complete strict JSON value, may have
duplicate keys or invalid Unicode, may exceed a safety limit, or may already be compact. Passthrough
is expected.

**Aggressive did not change a request.** The request may be on native Anthropic, lack a gateway
session identity, contain no eligible old tool results/history, fail the structure check, or produce
no estimated reduction. Aggressive is opportunistic.

**Aggressive did not reduce Claude history.** That is intentional. Native Anthropic history and the
gateway body are excluded from aggressive's lossy reducers to preserve the exact-prefix cache
contract. Aggressive still includes safe, so a newly produced strict-JSON local tool result may have
insignificant whitespace removed before its first Claude dispatch.

**The configured value is safe/aggressive but behavior is off.** Check `modules.economizer`. An
explicit `false` is authoritative.

**Costs rose despite a smaller prompt.** Inspect cache-read and cache-creation/uncached usage, route
selection, and whether the request crossed a provider pricing tier. Aggressive offers no billing
guarantee; return to `safe` or `off` when it is not a measured net improvement.

## Validation coverage

The shipped tests cover mode parsing/defaults and the hard kill; strict JSON syntax, Unicode,
duplicate keys, depth/size behavior, determinism, and no-gain passthrough; OpenAI-versus-Anthropic
route gates; gateway identity and structural bypasses; immutable wire snapshots; and live-surface
activation. The implementation remains intentionally independent of undocumented provider cache
breakpoints or exact preflight token counts.
