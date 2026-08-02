# module-runtime module

## Purpose and non-goals

Module-runtime is required core and the dependency root for module contracts. It owns the
synchronous in-process PRE_LLM_CALL hook registry in
`src/modules/module-runtime/pre_llm_hook.c`: a bounded, process-local set of callbacks that agent
execution runs against the user message before a model call. It does not parse manifests, persist
state, scan directories, dynamically load libraries, or provide any general "extension" object
model; those responsibilities are out of scope and belong to first-class modules, not to this
registry.

## Public contracts

`src/modules/module-runtime/include/aimee/module-runtime/pre_llm_hook.h` exports
`plugin_chook_register_pre_llm`, `plugin_chook_run_pre_llm`, `plugin_chook_apply_pre_llm`,
`plugin_chook_reset`, and `plugin_chook_pre_llm_count`, plus the `plugin_chook_pre_llm_fn` callback
type. The `plugin_chook_*` spelling is retained compatibility vocabulary for the pre-LLM hook and
does not imply any relationship to a plugin subsystem. The header is the module's only public
contract; the module owns no other exported surface.

## Dependencies and consumers

The descriptor declares no module dependencies. The implementation uses C runtime allocation and
the shared logging primitive. The sole production consumer is `src/server/agent_runtime.c`, which
calls `plugin_chook_apply_pre_llm` per turn; the focused test consumer is
`src/tests/test_plugin_c_hook.c`.

Consumption is asymmetric by design. The host tree consumes the registry only as a reader.
`agent_runtime.c` applies registered hooks, and no host-side source calls
`plugin_chook_register_pre_llm`. The registration entry point is producer-facing ABI for in-process
callers, so an unfired hook path in a plain host build is the expected state, not a defect.

As an ownership-descriptor module, `module.yaml` declares this module's single implementation
source, single public header, focused pre-LLM hook test, and canonical module document. The
descriptor validator rejects symlinked paths before claiming them and rejects duplicate normalized
lexical paths within or across descriptors, then emits a deterministic declared-ownership report.
Build inputs are maintained by Make and CMake; descriptor-driven build generation remains a later
step, so these ownership fields are documentation and validation only.

The descriptor sets `ownership_complete: true`. That latch exhaustively checks the module-local C
and private-header files (the module has no private headers) and requires this canonical
document. The public-header and test entries are explicit audited claims. Completeness is a
statement about file ownership only: it does not assert that every owned public facility has a
host-side caller, nor that every declared test runs in every build system. The source liveness,
build and test membership, adjacent-boundary, and public-surface audit is recorded in
`docs/validation/core-modularization-slice-35.md`.

## Providers and readiness

`module-runtime` is its own required reference implementation and has no replaceable provider.
The registry is ready after process initialization. Registration is single-threaded startup work;
concurrent mutation is not supported.

## Configuration and activation

- `runtime_toggle.supported`: `false`; module-runtime is present in every profile and exposes no
  enable switch.

It owns no configuration keys and is never omitted from a build.

## Surfaces

The module exposes `C` headers and symbols only. It owns no CLI command, HTTP route, protocol
listener, dashboard, static asset, metric namespace, or background job.

## Data and migrations

All `module-runtime` state is fixed-capacity, process-local registry state. The module owns no
file, database table, schema, migration, or durable compatibility record.

## Security and privacy

`plugin_chook_apply_pre_llm` callbacks receive the current user message, an output buffer, and
opaque caller data; the system prompt is not in the callback type. Accepted output becomes
untrusted ephemeral context. Callers remain responsible for authorization and for preventing
sensitive data from being exposed to registered callbacks.

## Supported journeys

Required startup exposes the pre-LLM hook registry even when every optional module is omitted.
Agent execution then runs the registered `plugin_chook_*` pre-LLM callbacks in order, applying
their bounded output as ephemeral context for the turn.

## Tests and failure behavior

`unit-test-plugin-c-hook` (`src/tests/test_plugin_c_hook.c`) covers empty state, ordering,
error/empty results, bounded capacity, reset, count, and per-turn message application. Null or
excess registrations fail; a failing callback is skipped and later callbacks still run. Output is
bounded by the caller-provided buffer.

Both build systems register the declared test. Make builds `unit-test-plugin-c-hook` from
`src/tests/Rules.mk`, and `src/tests/CMakeLists.txt` registers `test_plugin_c_hook` as a CTest
case. `scripts/check_module_test_registration.py` derives that from the build files, bound to the
declared source path rather than to a target name, and pins it to
`tests/baselines/refactor/module-test-registration.json`, so a change in build-file registration
fails until the baseline is regenerated and re-reviewed.

## Operational diagnostics

Registration and capacity failures use existing `module-runtime` log messages. There is no
module-specific health route or metric. Diagnosis relies on startup logs and the focused unit test.

## Compatibility

Exported names and signatures for the pre-LLM hook are unchanged from the former root-level
contract. The legacy `src/plugin_c_hook.c` and `src/headers/plugin_c_hook.h` paths are retired
without forwarding headers because no installed-header manifest exported them.
`scripts/check_module_source_ownership.py` rejects their reappearance.

## Extension and removal

New callback kinds must update `pre_llm_hook.h`, `pre_llm_hook.c`, the `agent_runtime.c` consumer,
`src/tests/test_plugin_c_hook.c`, `module.yaml`, and this document together. Removing module-runtime
is impossible without breaking the required module architecture and the core agent round trip.
