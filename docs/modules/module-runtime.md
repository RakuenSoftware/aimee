# Module-runtime module

## Purpose and classification

Module-runtime is required core and the dependency root for module contracts. It owns contracts
that required Aimee execution must provide whether or not any optional module is selected. Its
extension ABI is implemented at `src/modules/module-runtime/extension.c`, with the canonical header
`src/modules/module-runtime/include/aimee/module-runtime/extension.h`. Its synchronous pre-LLM hook
registry is at
`src/modules/module-runtime/pre_llm_hook.c`, with its public contract at
`src/modules/module-runtime/include/aimee/module-runtime/pre_llm_hook.h`.

It ships in every build profile. The optional `plugin-loader` module defaults to disabled and
depends on module-runtime; module-runtime has no dependency on plugin-loader.

These ownership slices do not change configuration or activation semantics.

## Extension ABI and registries

The required extension contract defines the kind and permission taxonomies, their stable string
vocabulary, typed contribution records,
`plugin_ctx_t`, context lifecycle, and process-local registries. `plugin_ctx_create` installs the
typed registration callbacks, and `plugin_ctx_destroy` runs the registered shutdown callback.
Tool and hook callbacks write to bounded module-runtime registries instead of the legacy no-op
placeholders. Memory-provider and context-engine callbacks are deliberately unset by the dependency
root and are installed by plugin-loader only when it creates a dynamic-plugin context.
The canonical implementation, optional loader contract, and focused test consume it through
`src/modules/module-runtime/extension.c`,
`src/modules/plugin-loader/include/aimee/plugin-loader/plugin.h`, and
`src/tests/test_plugin.c`.

Module-runtime does not parse manifests, persist installed-plugin state, inspect plugin directories,
or call `dlopen`. Those optional responsibilities belong to plugin-loader, which depends on this
contract. The required side has no include or symbol dependency on plugin-loader.

Concurrent registry mutation is unsupported. Extensions register during single-threaded startup,
and consumers read the process-global typed registries only after initialization. Adding concurrent
registration requires synchronization or a snapshot API before changing that lifecycle contract.
Slash and CLI commands use the existing process-global command registries and the same 64-entry
capacity they had before this ownership move.

## Pre-LLM hook contract

The contract preserves the existing `plugin_chook_*` compatibility symbols:

- `plugin_chook_register_pre_llm` registers a callback and opaque caller value.
- `plugin_chook_run_pre_llm` runs callbacks in registration order and joins accepted output.
- `plugin_chook_apply_pre_llm` returns a new user-message buffer only when context was produced.
- `plugin_chook_reset` clears process-local state for tests.
- `plugin_chook_pre_llm_count` reports the registered callback count.

The retained names are compatibility vocabulary, not plugin-loader ownership. The canonical include
is `aimee/module-runtime/pre_llm_hook.h`.

## Dependencies and consumers

The implementation uses only the C runtime and required logging. Its production consumer is
`src/server/agent_runtime.c`; its focused test is `src/tests/test_plugin_c_hook.c`. The implementation
itself is `src/modules/module-runtime/pre_llm_hook.c`. It does not include or link plugin-loader,
plugin manifests, `plugin_t`, install state, or dynamic loading.

## Lifecycle and concurrency

The registry is process-local, fixed-capacity state. Registration order is execution order. The
current implementation provides no locking, unregister operation, or per-session registry, so
registration and reset must not race hook execution. These are current contract limits, not future
concurrency guarantees.

## Security and privacy

Callbacks receive only the current user message, an output buffer, and their opaque value. The
system prompt is not present in the callback type, is not passed by
`plugin_chook_apply_pre_llm`, and is not logged by this module. Accepted callback output is appended
to the user message as ephemeral context; callers remain responsible for treating that text as
untrusted extension output.

## Failure behavior and diagnostics

Null callbacks and registrations beyond the fixed capacity fail. A callback error or empty output
is skipped without aborting the remaining callbacks. Output is bounded by caller-provided buffers.
Registration and capacity failures use the existing module log messages; the ownership move adds no
new route, command, metric, or configuration key.

## Tests and compatibility

`unit-test-plugin-c-hook` covers empty state, single and multiple callbacks, empty and error results,
per-turn behavior, capacity, null registration, user-message delivery, reset, and count. Make and
CMake continue to compile the implementation unconditionally as required core. All exported symbol
names and signatures remain unchanged; only the owned source path, header path, object path, and
in-tree include spelling change.

## Migration status

The legacy `src/plugin_c_hook.c` and `src/headers/plugin_c_hook.h` paths are retired without a
forwarding header because the repository has no installed-header export for this contract. The
shared guard at `scripts/check_module_source_ownership.py` enforces canonical ownership for this and
the preceding plugin-loader move without duplicating a checker per module.

The legacy `plugin.c`/`plugin.h` mix is split by ownership. The unreferenced `plugin_ctx.c` and
`plugin_ctx.h` wrapper island was removed after call-site, build-manifest, and installed-header
inventory found no consumer; the required `plugin_ctx_create` and `plugin_ctx_destroy` symbols remain
in this module. The plugin-loader profile now omits the optional loader while preserving these
required contracts.
