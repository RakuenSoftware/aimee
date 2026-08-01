# Provider-neutral economizer safety specification

- **State:** APPROVED
- **Review status:** CONVERGED (`converged=1`, zero blockers)
- **Effective:** NOT YET
- **Version:** `aimee-economizer-safety-v1`
- **Date:** 2026-07-21
- **Scope:** Normative v1 behavior for all provider, platform, endpoint, model, and API shapes.

## Decision

V1 is off-only. It performs no live economization and no observation, accounting, shadow simulation,
telemetry, diagnostics, storage, or savings reporting.

The governing requirement is:

> Wherever Aimee intervenes, the user's charge and the authoritative provider cost must both be
> strictly lower than they would have been without the economizer. If that cannot be proven for the
> individual request and its complete task lifecycle, Aimee does not intervene.

Current OpenAI and Anthropic APIs do not expose an authoritative pre-dispatch counterfactual for the
exact untouched request, future cache reuse, eviction, concurrent writers, later retries/recall, or
model-visible outcome equivalence. Token estimates, returned cache counters, provider documentation,
public prices, and cohort evidence cannot prove the required inequality for an individual future
task. Even a passive runtime observer would add code, storage, metering, security, and reporting
surfaces without making live intervention provable. V1 therefore contains none.

## Exact baseline

The baseline is the request graph produced by the reviewed no-economizer reference build:

- the same provider-bound plaintext bytes and ordered cache-sensitive headers before TLS;
- the same complete client parameter envelope and unknown provider extensions;
- the same model, endpoint, account/project routing, service tier, region, and API shape;
- the client's exact cache keys, markers, breakpoints, modes, and TTLs when supplied;
- provider-default cache behavior only when the client supplied no cache intent;
- the same serializer, SDK/transport policy, compression/framing, proxy, TLS/HTTP version, timeout,
  retry policy and representation, connection reuse/0-RTT policy, request count, follow-on calls,
  context-limit behavior, result, and error; and
- no Aimee-created cache plan, summary, condensation, fold, compression, recall, rehydration, rescue,
  alternate request, or retry.

Provider-bound identity means the canonical plaintext request and transport policy, not randomized
TLS ciphertext or packet coalescing. Observation is absent, so it cannot affect either.

When client cache intent is absent, Aimee is forbidden to synthesize a default cache key, marker,
breakpoint, mode, TTL, or plan. Independent field-presence tests prove absent-in/absent-out parity.
Contradictory client intent is passed through unchanged so the provider produces the same success or
error as the reference build.

“Absent” means that the ingress parser's independent presence bit is false, not that a decoded value
equals a default or zero. The same presence bit and raw value enter the pinned serializer in the
reference and v1 builds. Serializer omission/default rules are therefore part of the byte-exact
golden, not an Aimee interpretation.

## Threat model

In scope are tenant/client inputs, malformed configuration, every named configuration source,
recognized legacy values, concurrent config attempts before startup closure, untrusted
out-of-process plugins/MCP servers, application bugs, build/link drift, aliases, LTO, packaging
mistakes, rolling upgrades, and process restarts.

The OS/kernel/hypervisor, pinned compiler/linker/scanner binaries, release-signing authority,
production secret store, and provider/TLS implementation are trusted platform roots. Compromise of a
private signing key, compiler binary, kernel, root account, hypervisor, provider, or a same-UID process
with `ptrace`/`process_vm_writev` capability is a platform-security incident outside this economizer
spec. Production out-of-process plugins run under a different UID/security domain with ptrace,
`/proc/<pid>`, process-memory, file-descriptor, shared-memory, and network-namespace access to the
Aimee process denied. Tests verify that sandbox boundary; the spec does not claim safety after root or
trusted-root compromise.

## Normative invariants

1. **No economizer request path.** Production dispatch has no branch that can reduce, rewrite,
   summarize, fold, condense, compress, cache-plan, recall, rehydrate, rescue, or resend for the
   economizer.
2. **No economizer observer.** No request, response, usage object, buffer, header, event, or terminal
   result is delivered to economizer code.
3. **No economizer side effect.** From process start through shutdown, the economizer makes no
   provider, network, health, discovery, pricing, tokenizer, metering, telemetry, diagnostics,
   storage, background, retry, keepalive, plugin, MCP, or admin call and creates no thread or task.
4. **No economizer data.** It writes no row, counter, log, trace, metric, cache key, identifier,
   modeled value, billed value, or cross-tenant/cross-version artifact.
5. **No savings or status surface.** No runtime or user-facing API emits economizer status,
   `saved_tokens`, `saved_dollars`, modeled savings, hypothetical delta, cache benefit, or equivalent.
6. **Client/default cache passthrough.** V1 neither adds nor removes cache behavior. Existing cache
   benefits belong to the baseline and are not attributed to Aimee.
7. **No configuration escape.** File, database, environment, CLI, admin/reload, compatibility,
   provider alias, module toggle, debug/test flag, plugin, MCP, tool registration, FFI, JIT, dynamic
   loading, and migration cannot enable economizer code.
8. **Future versions inherit no authority.** A later observer, accounting adapter, shadow store,
   cache planner, or transform requires a new separately reviewed safety version. V1 artifacts and
   compatibility values cannot authorize it.

## Mechanical v1 contract

These names and constants are normative.

### Production build and dispatch

- `aimee_v1_dispatch_baseline` is the only production provider dispatcher.
- `aimee_v1_serialize_once` is its only serializer and transfers the request directly to the pinned
  production transport. There is no economizer callback or hook before, between, or after them.
- `AIMEE_ECON_V1_BASELINE_BUILD_SHA256` content-addresses the reference binary, compiler/link flags,
  serializer, SDK, transport, retry policy, config map, link/import maps, network-trace fixtures, and
  byte-exact goldens.
- `aimee-economizer-v1-denied-symbols.json` is canonical JSON containing the literal full symbol and
  object-file names of every reducer, folder, compressor, condenser, cache mutator/planner, observer,
  accounting adapter, shadow writer/reader, reporter, recall, rescue, resend, economizer logger,
  economizer metric, and economizer background worker.
- The denied-symbol artifact and baseline manifest have canonical SHA-256 digests and Ed25519
  signatures. Only the verification public key is embedded in the binary; the signing private key is
  held by the external trusted release authority and is unavailable to the build pipeline. CI performs exact literal comparisons
  against pre-LTO IR call graphs plus post-LTO linker/import maps, rejects weak/alternate aliases and
  interposable imports, and links production with symbol interposition and preload disabled.
- The production target contains no in-process `dlopen`, native plugin/MCP loader, economizer FFI/JIT
  import, or request/response economizer extension point. Out-of-process plugins cannot access the
  dispatcher or transport.
- A canonical root manifest-of-manifests pins every artifact digest, verification public key,
  compiler/linker/scanner binary digest, scanner rule-corpus digest, build flag, and package digest.
  The external release authority signs its digest, and an independently reproducible verification
  job checks the final package against it.
- `aimee_v1_assert_build` verifies the compiled baseline and denied-symbol manifest digests once
  before accepting traffic. Failure latches `AIMEE_ECON_BUILD_INVALID` for the process lifetime;
  every request returns that typed error before serialization until restart with a valid build.

Any compiler, linker, SDK, serializer, transport, retry, config, manifest, or relevant build-flag
change requires regenerated goldens/traces, new content digests, and roundtable approval of the
release manifest. Without approval the production build contains no economizer and exposes only the
existing baseline path.

### Configuration

The only production tag is `ECON_V1_OFF=0x56310000`; `ECON_V1_COUNT=1` is statically asserted. There
is no observe, safe, aggressive, unknown, or mutating runtime variant. Exhaustive switches compile
with switch-enum warnings as errors and forbid a default arm.

`aimee_econ_v1_parse` is the only parser. The signed, content-addressed
`aimee-economizer-v1-config-map.json` enumerates every configuration source. Absent, `off`, `observe`,
`safe`, `aggressive`, all recognized legacy aliases, and either module-toggle value map to
`ECON_V1_OFF` without an economizer log, warning, diagnostic, metric, or other side effect.
Unknown keys, types, sources, or integers and all IO, JSON, schema, signature, or mapping failures
latch `AIMEE_ECON_CONFIG_INVALID` before traffic. No partially parsed value is applied. Configuration
source cardinality is the finite signed map; all sources are read once before traffic, conflicts or
races are typed errors, and reload is disabled for this value. The canonical map is signature-
verified at startup and the resulting sole off tag is copied into read-only memory. There is no
per-request config read, hash, timing branch, or fault-sensitive recheck.

### No compatibility surface

`/v1/economizer/stats` and every economizer status/reporting route are absent from the production
route table and OpenAPI/schema output. A request receives the ordinary route-not-found behavior from
the content-addressed baseline router; no economizer handler or header runs. All economizer fields,
stores, keys, roles, and interfaces are absent from the production schema and binary.

## Acceptance

V1 may ship only when all checks below pass against the exact release artifact:

1. The signed reference-build, config-map, and denied-symbol digests match their compiled constants.
   The independently verified root manifest-of-manifests pins the release public key, scanner/toolchain
   binaries and rules, every child artifact, and final package.
2. Pinned pre-LTO call-graph and post-LTO link/import scans find zero denied symbols, objects, aliases,
   interposable imports, dynamic loaders, and economizer extension points. Source/package inventory
   additionally proves all economizer implementation files are excluded; suspicious new request
   mutation requires explicit manifest classification rather than name-only acceptance.
3. Every production dispatch entrypoint reaches only `aimee_v1_dispatch_baseline` and
   `aimee_v1_serialize_once`; the manifest has no wildcard or default entry.
4. Byte-exact ordered-header/body goldens and transport-policy fixtures match the no-economizer
   reference for OpenAI Responses, Chat Completions, Anthropic Messages, all other supported provider
   shapes, streaming, tool continuations, batch/background paths, errors, context limits, and retries.
5. Paired full-process syscall/channel traces cover sockets including DNS/UNIX/loopback, files,
   database, shared memory/rings, `io_uring`, BPF, process/thread creation, scheduling/priority, and
   IPC. They prove the economizer side-effect set is empty and baseline provider calls, auxiliary
   calls, timing class, retries, results, and errors are unchanged.
6. Cache-field presence/value matrices prove exact client passthrough and no synthesized default plan
   for every provider shape, including OpenAI `prompt_cache_key` and Anthropic cache-control blocks.
7. The generated Cartesian config test visits every signed source/map entry and invariant-7 surface,
   including concurrent/partial reads, precedence conflicts, IO failure, malformed/unsigned maps, and
   reload attempts. Every case resolves once to the sole immutable off tag or returns the typed pre-
   traffic config error.
8. Build/config latch tests prove invalid artifacts stop every request before serialization and cannot
   be reset by reload, admin, environment, plugin, MCP, or compatibility paths.
9. Filesystem, database, network, thread/task, log, trace, metric, route-table, and API-schema tests
   prove the production economizer creates no data, side effect, endpoint, handler, or header.
10. Upgrade tests prove current `safe` and `aggressive` installations become off before traffic and
    cannot invoke `context_reduce`, `build_fold_view`, gateway mutation/resend,
    `tool_condense_apply`, or economizer-owned `agent_compress_tool_result` paths.
11. A separately built future-version test principal cannot read, import, or promote any v1 runtime
    economizer data because no such data, key, schema, role, or interface exists. Build/config/
    manifest artifacts are inert release evidence only; their schemas contain no authorization field,
    and every future-version authority parser must reject their artifact types.
12. `git diff --check`, documentation link checks, production build, unit tests, integration tests,
    and sanitizer tests pass for the exact content-addressed release.
13. Deployment drains every older economizer-capable process before routing traffic to v1. Mixed-
    version serving is forbidden; fleet attestation proves all serving package digests equal the
    approved v1 root manifest before traffic resumes.

Failure of any check prevents release. There is no degraded observation mode and no runtime fallback
that creates an alternate request.

## Current provider limitations

The following facts motivate the off-only decision but authorize no runtime behavior:

- OpenAI prompt caching uses exact-prefix matching. GPT-5.6 cache writes are documented at 1.25x
  ordinary input; returned read/write counters arrive only after the request and do not expose the
  untouched counterfactual or future residency.
- Anthropic caching likewise depends on exact prefixes and separately billed reads and TTL-specific
  writes. Returned uncached/read/write buckets describe only the representation that was sent.
- Neither API gives Headroom, RTK, or a generic addon the authoritative future breakpoint/residency,
  reuse, eviction, recall, complete-task cost, and outcome-equivalence facts needed for a strict
  per-task no-regret proof.

Official sources checked on 2026-07-21:

- https://developers.openai.com/api/docs/guides/prompt-caching
- https://developers.openai.com/api/docs/models/gpt-5.6-sol
- https://platform.claude.com/docs/en/api/messages/create
- https://platform.claude.com/docs/en/build-with-claude/prompt-caching
- https://platform.claude.com/docs/en/agents-and-tools/tool-use/tool-use-with-prompt-caching
- https://platform.claude.com/docs/en/about-claude/pricing

## Requirements for any future version

A future proposal must begin from zero authority and obtain separate roundtable approval. At minimum
it must define a closed provider/platform/model/API scenario set and prove, for each individual task:

```text
candidate_user_charge_upper_bound < baseline_user_charge_lower_bound
AND
candidate_provider_cost_upper_bound < baseline_provider_cost_lower_bound
```

The proof must use authoritative account billing, complete request/retry/cache/recall/rescue lifecycle
costs, exact client/default cache intent, future-write/read settlement, and model-visible outcome
equivalence. Expected reuse, averages, token estimates, public prices, or shadow cohorts are not
authorization. If providers never expose sufficient evidence, Aimee remains off.

## Review record

Earlier drafts permitted live safe-mode transforms, then observation-only accounting and shadow.
Repeated roundtable review found that both designs retained unprovable cost authority or unnecessary
request, metering, storage, security, and reporting surfaces. V1 was reduced to a mechanically closed
off-only build. The final two-round safety review converged on 2026-07-22 with zero blockers.
