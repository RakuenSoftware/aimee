# Provider-neutral cache-aware economizer: off-only v1 implementation

- **State:** APPROVED
- **Review status:** CONVERGED (`converged=1`, zero blockers)
- **Date:** 2026-07-21
- **Normative gate:** `provider-neutral-economizer-safety-spec.md`
- **Scope:** Remove all production economizer behavior while preserving the reviewed no-economizer
  provider request graph byte-for-byte.

## Executive decision

V1 does not economize and does not observe. It removes live Headroom-, RTK-, folding-, condensation-,
compression-, cache-planning-, recall-, and rescue-style behavior from production. It also ships no
usage adapter, shadow simulator, economizer telemetry, storage, or savings reporting.

This is the only current design that satisfies the requested rule:

> If Aimee cannot prove that an intervention strictly lowers both the user's charge and provider cost
> for the individual complete task, it does not intervene.

OpenAI and Anthropic report what happened after a request. They do not expose the authoritative
untouched counterfactual, future cache residency/reuse/eviction, concurrent writers, later recall or
rescue, or proof that changed model-visible content produces the same completed task. A token
reduction or favorable cohort is therefore not a no-regret cost proof.

## Why token compressors can cost more

### OpenAI GPT-5.6

OpenAI caching requires exact prefix matches. Current GPT-5.6 documentation prices cache writes at
1.25 times ordinary input and discounts cache reads. A compressor can reduce nominal tokens while
changing a reusable prefix, turning a discounted read into ordinary input or a premium write.
Returned `cached_tokens` and `cache_write_tokens` arrive after dispatch; they are not a query for the
cache state or counterfactual cost of the request that was not sent. `prompt_cache_key` influences
matching/routing but is not a readable cache handle.

An addon cannot discover the decisive provider breakpoint or future reuse through the API. It can
only guess from request structure, historical observations, and provider documentation.

### Anthropic Claude

Anthropic also requires exact-prefix matching and separately accounts for uncached input, cache
creation, and cache reads. Creation can have different five-minute and one-hour prices, and server
tools can add cache behavior. A changed prefix can forfeit a read or create another write.

Rehydration compounds the problem: the user first pays for the reduced representation, then pays
storage/lookup/latency and possibly another provider turn to recover omitted context. One recall can
erase a session's nominal savings.

### Shared limitation

Headroom, RTK, and similar addons generally see the request, their own local state, and post-response
usage. They do not receive all of:

- pre-dispatch authoritative cache residency and exact matched prefix for both alternatives;
- future reads, eviction, concurrent writers, and TTL refreshes;
- exact account-contract prices, credits, platform modifiers, and late billing adjustments;
- complete later retries, follow-on calls, recall, rehydration, and rescue; and
- per-task proof of model-visible outcome equivalence.

Without those inputs, request-token savings cannot prove lower dollars per completed task. V1 stays
off rather than encode a probabilistic exception.

## Current implementation audit

The current production tree contains live economizer behavior. The implementation must regenerate
this inventory from the final source and build artifacts, but the known starting points are:

| Surface | Current behavior | Required change |
|---|---|---|
| `src/modules/config/config.h` | `economizer_tier` defaults to `ECON_TIER_SAFE`; safe/aggressive enable concrete levers | Replace with the sole production tag `ECON_V1_OFF` |
| `src/posix/agent_runtime.c` | Calls `context_reduce()` and substitutes reduced messages for provider request construction | Remove the production call edge and reducer state |
| `src/posix/agent_runtime.c` | Anthropic fallback can call `build_fold_view()` and dispatch folded history | Remove the economizer-owned fold edge |
| `src/modules/economizer/context_reduce.c` | Orchestrates body compression and history folding using estimated economics | Exclude the object and public symbols from the production link |
| `src/modules/economizer/gateway_mutate_wire.c` | Aggressive mode replaces gateway messages with compressed messages | Remove the live gateway mutation edge |
| `src/modules/economizer/gateway_mutate_wire.c` | A 4xx can restore pristine messages and signal a resend | Remove the economizer-created resend path |
| `src/server/anthropic_http.c` and `openai_chat.c` | Call `gw_economizer_measure()`, write reduction ledgers, invoke gateway mutation, and process resend actions on multiple sync/streaming paths | Remove every measurement, ledger, mutation, and resend call edge |
| `src/posix/agent_tools.c` and `agent_tools_anchored.c` | `tool_condense_apply()` changes tool output before model dispatch | Remove production condensation calls |
| `src/posix/agent_tools_dispatch.c` | `agent_compress_tool_result()` can replace full model-bound tool results | Remove economizer-owned callers; classify independent hard limits as baseline or typed errors |
| `src/modules/economizer/tool_condense.c` | Condensation is enabled by safe/aggressive tiers | Exclude it from the production link |
| Anthropic cache/config path | Economizer tier can add or remove a cache breakpoint | Remove tier coupling; exact client fields pass through, otherwise the reviewed baseline uses provider defaults |
| `src/server/server.c` and `aimee_backend_anthropic.c` | `server_sync_economizer_runtime()` changes Anthropic cache enablement at startup/reload | Remove runtime synchronization and the economizer cache setter edge |
| `src/server/server_main.c` | Logs the active economizer tier at startup | Remove the log and tier lookup |
| `src/server/server_http_identity.c` | Captures bearer/session identity for gateway mutation state | Remove economizer-specific capture/accessors after proving no non-economizer consumer |
| `src/server/agent_logging.c`, `gw_mutate_stats.c` | Persist reduction ledgers and mutation counters | Remove economizer writers, sinks, and production objects |
| CLI/server routes, auth capabilities, headers, generated OpenAPI/help | Expose `economizer.stats` and economizer control/telemetry symbols | Remove every route, handler, capability, declaration, generated surface, and help entry; ordinary baseline route-not-found applies |

Unrelated session maintenance is not silently reclassified. Each potentially model-visible compactor
or context-limit path must be identified as either part of the content-addressed no-economizer
baseline or an economizer-owned denied path. Ambiguous paths fail the release gate.

## Target runtime

```text
ingress
  -> reviewed no-economizer planner
  -> aimee_v1_serialize_once
  -> pinned production transport
  -> provider
```

There is no economizer branch before, during, or after this path. The economizer creates no provider
call, local call, thread/task, row, counter, metric, log, trace, modeled value, or report.

### Baseline identity

`aimee_v1_dispatch_baseline` is the only provider dispatcher. It owns the request, calls
`aimee_v1_serialize_once` exactly once, and transfers directly to the pinned transport. The
content-addressed baseline includes:

- canonical plaintext provider bytes and ordered cache-sensitive headers before TLS;
- full client parameters and unknown provider extensions;
- model, endpoint, tier, region, and account/project routing;
- serializer, SDK, transport, proxy, TLS/HTTP version, connection reuse/0-RTT, framing/compression,
  timeout, retry and context-limit policies;
- provider and auxiliary call count, follow-on behavior, results, and errors;
- provider response status, ordered headers, raw body/stream-event bytes, terminal event selection,
  and every usage/cache/tier field returned to the client, including OpenAI cached/write details and
  Anthropic cache creation/read/TTL details; and
- byte-exact request/response goldens plus full-process network traces.

Randomized TLS ciphertext, packet coalescing, and connection timing need not be byte-identical. Their
policy and the canonical plaintext request must be identical to the reference build.

### Cache behavior

V1 owns no cache policy.

- Client keys, markers, breakpoints, modes, TTLs, and OpenAI `prompt_cache_key` pass through exactly.
- When client cache intent is absent, Aimee synthesizes nothing; provider-default behavior applies.
- Contradictory client intent is not repaired, normalized, or replaced.
- Existing provider cache benefits are baseline behavior and are not Aimee savings.

Presence/value matrices, not decoded defaults, prove absent-in/absent-out and exact passthrough for
each API shape.

## Mechanical build closure

The production release implements the normative fixed names and artifacts:

- sole tag `ECON_V1_OFF=0x56310000` and static `ECON_V1_COUNT=1`;
- `aimee_v1_dispatch_baseline`, `aimee_v1_serialize_once`, and `aimee_v1_assert_build`;
- compiled `AIMEE_ECON_V1_BASELINE_BUILD_SHA256`;
- signed `aimee-economizer-v1-denied-symbols.json` with literal full symbol/object names; and
- signed `aimee-economizer-v1-config-map.json` covering every configuration source.

CI compares the denied artifact to pre-LTO IR call graphs and post-LTO link/import maps. It rejects
mutator/observer/accounting/shadow/reporting symbols and objects, alternate/weak aliases, interposable
imports, dynamic loaders, and economizer extension points. Production disables preload/symbol
interposition and contains no in-process native plugin/MCP loader or economizer FFI/JIT import.

Manifest generation is deny-by-default, not name-only. A pinned compiler AST/IR inventory enumerates
every source file, translation unit, function, global initializer, provider-request/response buffer
write, cJSON mutation, serializer/transport call edge, config reader, route, worker, and storage/
logging sink. Each entry is classified in the signed root manifest as baseline-required or denied;
an unclassified, new, or renamed entry fails CI. The manifest is generated fresh and diffed against
both the source inventory and prior approved release. The pinned pre-LTO IR scan is authoritative for
semantic call edges and inlining inputs; the post-LTO scan corroborates the final package and cannot
override a pre-LTO failure.

The baseline, denied-symbol, and config artifacts use canonical SHA-256 digests and Ed25519 release
signatures. Only the public verification key is embedded; the external release authority keeps the
private key outside the build pipeline. A signed root manifest pins every artifact, compiler/linker/
scanner binary and rule corpus, build flag, and package digest, and an independent reproducible job
verifies it. Startup verifies them before traffic. Failure latches a typed process-lifetime build or
config error and every request fails before serialization; reload/admin/plugin paths cannot reset it.

Any compiler, linker, SDK, serializer, transport, retry, config, or relevant build-flag change
requires regenerated goldens/traces, a new baseline digest, and roundtable approval of the release
manifest.

`aimee_v1_assert_build` also verifies the packaged link/import-map digest, absence of preload and
interposition state, and signed runtime package digest before accepting traffic. It does not try to
reconstruct pre-LTO semantics; those are release-gated by the authoritative signed IR result.

## Configuration and migration

The compatibility input may still accept historical values, but the production runtime has one tag:

```yaml
economizer: off
```

The canonical parser maps absent, `off`, `observe`, `safe`, `aggressive`, recognized legacy aliases,
and either module-toggle value to `ECON_V1_OFF` without an economizer warning, log, diagnostic,
metric, or other side effect. Unknown keys/types/sources/integers or IO, JSON, schema, signature,
and mapping failures latch `AIMEE_ECON_CONFIG_INVALID`; no partial configuration applies.

All finite signed sources are read once before traffic; conflicts/races fail, and reload is disabled
for this setting. The immutable signed map is verified at startup and only the sole off tag remains in
read-only memory. There is no per-request config read/recheck, runtime default arm, boolean coercion,
observe mode, or mutating variant.

`/v1/economizer/stats` and every economizer status/reporting route are removed from the production
route table and API schema. Requests receive the baseline router's ordinary route-not-found behavior.

## Implementation sequence

### Slice 0: freeze the reference

1. Build the current tree with the economizer explicitly off.
2. Enumerate every production dispatch entrypoint, serializer, transport, retry/context-limit path,
   auxiliary call, response/stream forwarding path, usage/cache field, config source, and cache field.
3. Capture byte-exact request goldens, transport-policy fixtures, results/errors, and network traces
   across the provider/API matrix.
4. Produce `AIMEE_ECON_V1_BASELINE_BUILD_SHA256` and sign the canonical reference manifest.
5. If any baseline path is ambiguous or non-reproducible, stop; do not implement an alternate path.
6. Reproduce the complete build and `AIMEE_ECON_V1_BASELINE_BUILD_SHA256` on an independent host from
   the pinned toolchain and source; byte/digest mismatch blocks the release.

### Slice 1: remove live behavior

1. Replace the tier resolver with the sole off tag and canonical signed migration map.
2. Remove delegate `context_reduce()` and Anthropic `build_fold_view()` call edges.
3. Remove every `gw_economizer_measure()`, reduction-ledger, gateway-mutation, and economizer-specific
   4xx restore/resend edge from all OpenAI and Anthropic sync/streaming paths.
4. Remove model-bound `tool_condense_apply()` and economizer-owned
   `agent_compress_tool_result()` calls.
5. Remove economizer cache-marker enablement and preserve exact client/default baseline behavior.
6. Remove startup/reload Anthropic cache synchronization, the economizer cache setter edge, tier
   startup logging, and economizer-specific request-identity capture.
7. Remove usage/ledger, shadow, telemetry, diagnostics, counters, background work, storage, and
   savings fields, sinks, and production objects.
8. Remove economizer CLI/server routes, handler/auth capability, headers, generated OpenAPI/help, and
   stats/status schemas; verify baseline route-not-found behavior.

### Slice 2: close the production link

1. Generate the literal denied-symbol/object manifest from every removed reducer, fold, compressor,
   condenser, cache planner/mutator, observer, adapter, store, reporter, recall, rescue, resend,
   logger, metric, and worker. Generate a deny-by-default AST/IR inventory of all files/functions/
   initializers and provider-buffer mutations; require explicit signed baseline-or-denied
   classification and diff it against source and the prior approved release.
2. Exclude those objects from the production target; test code may link them only in a separately
   named non-production binary. A packaging allow-list contains only approved production artifacts;
   a negative fixture attempts to package each test/denied binary and must fail.
3. Make the pinned pre-LTO AST/IR call/data-flow result the authoritative release gate. Add
   corroborating post-LTO link/import exact scans, weak/alias/interposition checks, `.init_array`,
   constructor/destructor, `atexit`, TLS-initializer, string-probe, and production-package inspection.
4. Remove in-process dynamic/native economizer extension surfaces. Out-of-process plugins run under
   a separate UID/security domain without ptrace, `/proc`, process-memory, descriptor, shared-memory,
   or network-namespace access to Aimee. Package and integration tests assert the separate UID,
   namespaces, mount/proc visibility, ptrace policy, descriptors, IPC/shared memory, and denied
   connection attempts.
5. Implement startup package/link/import digest, preload/interposition, signed-manifest, and
   process-lifetime latch verification.

### Slice 3: prove parity

1. Re-run all reference byte, header, cache-presence, transport, call-count, trace, result, error,
   response-body/header/stream, usage/cache-field, retry, context-limit, and config fixtures against
   the exact production package.
2. Prove no economizer filesystem/database/network/thread/log/trace/metric/API-schema side effect.
3. Prove every current safe/aggressive configuration reaches only the off path before traffic with
   no warning, log, diagnostic, metric, or other migration side effect.
4. Run build, unit, integration, sanitizer, package, and documentation checks.
5. Execute signed `aimee-economizer-v1-cutover.json`: remove old nodes from discovery, pin each
   in-flight connection/request/retry to its old node until completion, wait for zero old in-flight
   work, stop and attest every old process, then admit only nodes whose package digest matches v1.
   Load balancers reject mixed digests; client retries begin only after the cutover epoch is complete.
6. Submit the content-addressed release artifacts for final roundtable approval before enabling the
   release in production.

There are no later v1 slices for observation or live intervention.

## Test matrix

### Provider and API identity

- OpenAI Responses and Chat Completions, synchronous/streaming, tools, parallel tools, reasoning,
  structured output, metadata, service tiers, errors, retries, and context limits;
- Anthropic Messages, synchronous/streaming, tool use/results, system blocks, cache-control blocks,
  errors, retries, and context limits;
- every other supported provider/platform/API entrypoint in the generated closed manifest; and
- full feature interactions involving client cache fields and provider extensions.

For every cell, canonical provider plaintext, ordered cache-sensitive headers, transport policy,
provider/auxiliary calls, result, and error match the reference.

### Mutation and link closure

- no `context_reduce`, `build_fold_view`, `context_compress_view`, `tool_condense_apply`, gateway
  mutator/resend, reducer, observer, shadow, or reporting symbol/object in the production package;
- no weak/alternate alias, LTO-inlined call edge, unresolved/interposable import, native dynamic
  loader, FFI/JIT entry, or in-process plugin/MCP path around the denied manifest;
- addition of a dispatch entrypoint, config source, or suspicious mutator symbol fails CI; and
- production startup rejects any manifest/build digest or signature mismatch before traffic.

### Configuration and upgrade

- Cartesian sources: file, database, environment, CLI, admin/reload, compatibility/provider alias,
  module toggle, debug/test, plugin/MCP input, recognized legacy, unknown key/type/source/integer, and
  malformed/unsigned artifacts;
- every recognized value resolves to the sole off tag; every invalid source fails before traffic;
- explicit fixtures cover missing/invalid/expired-or-not-yet-valid signature metadata, unknown or
  rotated verification key, truncated/partial map, IO error, schema/version mismatch, conflicting
  sources, concurrent startup writes, restart, and unavailable external release authority. Runtime
  uses only local signed artifacts and never contacts that authority;
- latches survive reload/admin/environment changes for the process lifetime; and
- current safe/aggressive installations cannot dispatch through any former live path.

### No data or reporting

- no economizer rows, files, keys, schemas, roles, counters, metrics, logs, traces, timers, workers,
  usage projections, shadow values, billing fields, or savings fields;
- no economizer status route, handler, header, or schema remains;
- no future-version principal can read/import/promote v1 data because none exists; and
- no user/platform metering dimension changes relative to the reference.

## Acceptance gates

1. The normative off-only safety spec has converged.
2. All signed artifact digests match compiled constants.
3. The root manifest pins exact compiler/linker/scanner binaries, versions, flags, rule corpus, and
   authoritative pre-LTO result; an independent host reproduces the baseline and package digests.
4. Denied-symbol/call/data-flow/object/initializer scans and production-package allow-list inspection
   are clean; negative test binaries cannot be packaged.
5. Dispatch/config manifests are generated by deny-by-default source/AST inventory plus explicit
   allow-list. They have no wildcard, default, unclassified, or untested entry.
6. Provider request and response, usage/cache fields, transport policy, call graph, result, and error
   parity pass every fixture.
7. Cache intent passes through exactly and no default cache plan is synthesized.
8. Safe/aggressive/legacy upgrades resolve silently off before traffic.
9. No economizer data, observation, accounting, shadow, side effect, or savings report exists.
10. Build/config failure latches stop traffic before serialization and cannot be bypassed.
11. Plugin sandbox integration tests and signed zero-in-flight cutover/attestation pass; no mixed
    serving digest or cross-version retry is possible.
12. Production build, tests, sanitizers, package inspection, docs links, and `git diff --check` pass.

Failure of any gate prevents release. There is no degraded observation mode or alternate request.

## Future work is a different safety version

No live cache policy or transform is scheduled here. A future proposal begins with no inherited
authority and must prove for each task:

```text
candidate_user_charge_upper_bound < baseline_user_charge_lower_bound
AND
candidate_provider_cost_upper_bound < baseline_provider_cost_lower_bound
```

It must cover authoritative account billing, client/default cache intent, future write/read/eviction,
all retries/follow-ons/recalls/rescues, and task-outcome equivalence. Expected values, token estimates,
public pricing, post-response counters, and cohorts cannot authorize dispatch.

## Primary sources checked 2026-07-21

- OpenAI prompt caching: <https://developers.openai.com/api/docs/guides/prompt-caching>
- OpenAI GPT-5.6: <https://developers.openai.com/api/docs/models/gpt-5.6-sol>
- Anthropic Messages usage: <https://platform.claude.com/docs/en/api/messages/create>
- Anthropic prompt caching: <https://platform.claude.com/docs/en/build-with-claude/prompt-caching>
- Anthropic tool use and caching: <https://platform.claude.com/docs/en/agents-and-tools/tool-use/tool-use-with-prompt-caching>
- Anthropic pricing: <https://platform.claude.com/docs/en/about-claude/pricing>

## Review record

Earlier revisions proposed safe live transforms and then observation-only shadow accounting.
Roundtable review continued to find unavailable counterfactual authority and unnecessary request,
metering, storage, security, and reporting surfaces. The current revision removes all v1 economizer
behavior. The safety gate converged first; after verification-detail remediation, the final two-round
implementation review converged on 2026-07-22 with zero blockers.
