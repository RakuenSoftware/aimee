# module-runtime module

## Purpose and non-goals

Module-runtime is required core and the dependency root for module contracts. It owns the extension
types and bounded in-process registries in `src/modules/module-runtime/extension.c`, plus the
synchronous pre-LLM hook registry in `src/modules/module-runtime/pre_llm_hook.c`. It does not parse
plugin manifests, persist plugin state, scan directories, or dynamically load libraries; those
responsibilities belong to [plugin-loader](plugin-loader.md#purpose-and-non-goals).

## Public contracts

`src/modules/module-runtime/include/aimee/module-runtime/extension.h` exports `plugin_ctx_create`,
`plugin_ctx_destroy`, kind/permission conversion, and the typed `plugin_*_register` and
`plugin_*_count` APIs. The canonical
`src/modules/module-runtime/include/aimee/module-runtime/pre_llm_hook.h` exports
`plugin_chook_register_pre_llm`,
`plugin_chook_run_pre_llm`, `plugin_chook_apply_pre_llm`, `plugin_chook_reset`, and
`plugin_chook_pre_llm_count`. The `plugin_*` spelling is retained compatibility vocabulary; it does
not transfer ownership to plugin-loader.
The optional contract consumer is
`src/modules/plugin-loader/include/aimee/plugin-loader/plugin.h`.

## Dependencies and consumers

The descriptor declares no module dependencies. The implementation uses C runtime allocation and
the shared logging primitive. Production consumers include `src/server/agent_runtime.c` and the
optional plugin-loader contract; focused consumers are `src/tests/test_plugin.c` and
`src/tests/test_plugin_c_hook.c`.

As the ownership-descriptor pilot, `module.yaml` also declares this module's two implementation
sources, two public headers, focused C-hook test, and canonical module document. The descriptor
validator rejects symlinked paths before claiming them and rejects duplicate normalized lexical
paths within or across descriptors, then emits a deterministic declared-ownership report. Build
inputs are still maintained by Make and CMake in this slice; descriptor-driven build generation
remains a later step, so these ownership fields are documentation and validation only.

## Providers and readiness

`module-runtime` is its own required reference implementation and has no replaceable provider.
Registries are ready after process initialization. Registration is single-threaded startup work;
concurrent mutation and unload are not supported.

## Configuration and activation

- `runtime_toggle.supported`: `false`; module-runtime is present in every profile and exposes no
  enable switch.

It owns no configuration keys. The optional loader may populate runtime registries, but disabling
that loader does not disable this module.

## Surfaces

The module exposes `C` headers and symbols only. It owns no CLI command, HTTP route, protocol
listener, dashboard, static asset, metric namespace, or background job. Runtime/plugin loading
management surfaces are documented by [plugin-loader](plugin-loader.md#surfaces).

## Data and migrations

All `module-runtime` state is fixed-capacity, process-local registry state. The module owns no file, database table,
schema, migration, or durable compatibility record.

## Security and privacy

`plugin_chook_apply_pre_llm` callbacks receive the current user message, an output buffer, and opaque caller data; the
system prompt is not in the callback type. Accepted output becomes untrusted ephemeral context.
Callers remain responsible for authorization and for preventing sensitive data from being exposed
to extensions.

## Supported journeys

Required startup exposes the typed extension API and registries even when every optional module is
omitted. Agent execution can then run registered `plugin_chook_*` pre-LLM callbacks in order.
Callers create `plugin_ctx_t` instances when needed; selected plugin-loader creates such a context
and populates these required registries rather than creating a parallel registry implementation.

## Tests and failure behavior

`unit-test-plugin` covers context lifecycle and typed registration. `unit-test-plugin-c-hook` covers
empty state, ordering, error/empty results, bounded capacity, reset, count, and per-turn message
application. Null or excess registrations fail; a failing callback is skipped and later callbacks
still run. Output is bounded by the caller-provided buffer.

## Operational diagnostics

Registration and capacity failures use existing `module-runtime` log messages. There is no module-specific
health route or metric. Diagnosis therefore relies on startup logs, focused unit tests, and the
plugin-loader profile absence check.

## Compatibility

Exported names and signatures remain unchanged from the former root-level plugin contract. The
legacy `src/plugin_c_hook.c`, `src/headers/plugin_c_hook.h`, `src/plugin_ctx.c`, and
`src/headers/plugin_ctx.h` paths are retired without forwarding headers because no installed-header
manifest exported them. `scripts/check_module_source_ownership.py` rejects their reappearance.

## Extension and removal

New registry kinds or callbacks must update the canonical headers, implementation, consumers,
tests, descriptor, and this document together. The removed `plugin_ctx_*_ex` wrapper island had no
consumer outside itself and must not be recreated. A future unload path must synchronize registry
access and invoke shutdown before library unload. Removing module-runtime is impossible without
breaking the required module architecture and core agent round trip.
