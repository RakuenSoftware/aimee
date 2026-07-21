# Module-runtime module

## Purpose and classification

Module-runtime is required core and the dependency root for module contracts. It owns contracts
that required Aimee execution must provide whether or not any optional module is selected. Its first
physical implementation family is the synchronous pre-LLM hook registry at
`src/modules/module-runtime/pre_llm_hook.c`, with its public contract at
`src/modules/module-runtime/include/aimee/module-runtime/pre_llm_hook.h`.

This slice changes ownership and include paths only. It does not change configuration, activation,
symbol names, callback order, output assembly, or failure behavior.

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

The next contract slice classifies `plugin.c`, `plugin.h`, `plugin_ctx.c`, and `plugin_ctx.h` per
symbol. Required extension ABI and registries move here; optional manifest discovery, installation,
enable/disable, removal, and dynamic loading remain with plugin-loader.
