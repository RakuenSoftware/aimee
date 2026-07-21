# Core modularization slice 9: required pre-LLM hook ownership

## Decision

The synchronous pre-LLM hook registry is required runtime behavior, not optional plugin loading.
This slice moves its source and public contract into `module-runtime`, preserves all
`plugin_chook_*` symbols and behavior, and changes no configuration or activation semantics.

## Exact ownership

- `src/plugin_c_hook.c` moves to `src/modules/module-runtime/pre_llm_hook.c`.
- `src/headers/plugin_c_hook.h` moves to
  `src/modules/module-runtime/include/aimee/module-runtime/pre_llm_hook.h`.
- `src/server/agent_runtime.c`, the implementation, and `src/tests/test_plugin_c_hook.c` use
  `aimee/module-runtime/pre_llm_hook.h`.
- Make, CMake, and the focused Make test graph use the canonical source and object paths.

## Guard consolidation

The plugin-loader-specific ownership checker becomes the data-driven
`scripts/check_module_source_ownership.py`. One contract table now enforces both landed physical
moves. This avoids one script and mutation suite per module while keeping each legacy path,
canonical path, build input, test object, consumer, and document explicit.

## Verification

- ownership checker and mutation suite: pass
- `unit-test-plugin-c-hook` build and execution: pass
- `make -C src lint`: pass
- `make -C src all -j2`: pass
- CMake configure/build: validation-pending locally and authoritative in CI

## Deferred next slice

No `plugin.c`, `plugin.h`, `plugin_ctx.c`, or `plugin_ctx.h` symbol moves here. The next slice audits
those symbols and moves required ABI/registries to `module-runtime` while leaving optional manifest,
installation, lifecycle, and dynamic-loading behavior under `plugin-loader`.
