# Core modularization slice 10: extension ABI and plugin contract split

## Decision

The legacy plugin contract mixed a required extension ABI with optional plugin discovery,
persistence, management, and dynamic loading. This slice separates those owners without claiming
that plugin-loader is absent from current product link profiles.

## Required module-runtime ownership

- `src/modules/module-runtime/extension.c` owns extension kind conversion, typed registries,
  bounded registration callbacks, `plugin_ctx_create`, and `plugin_ctx_destroy`.
- `src/modules/module-runtime/include/aimee/module-runtime/extension.h` owns the public types and
  declarations and is included as `aimee/module-runtime/extension.h`.
- Module-runtime has no include edge to `aimee/plugin-loader/`.

## Optional plugin-loader ownership

- `src/modules/plugin-loader/plugin.c` owns `plugin_t` manifest parsing, installed-state
  persistence, install/enable/remove, project discovery, manifest hook/tool aggregation, builtin
  conflict checks, and `plugin_load_and_register` dynamic loading.
- `src/modules/plugin-loader/include/aimee/plugin-loader/plugin.h` owns that contract and depends
  one-way on the required extension ABI.
- All former `plugin.h` consumers now use `aimee/plugin-loader/plugin.h`.
- Plugin-loader installs memory-provider and context-engine bridges on contexts it creates, so the
  module-runtime dependency root has no direct symbol edge to those implementations.

## Removed self-contained wrapper island

`src/plugin_ctx.c` and `src/headers/plugin_ctx.h` exported convenience wrappers around the actual
context ABI. Repository-wide source inventory found no call site outside those two files, Make and
CMake did not install the header, and no installed-header manifest exported it. The implementation
also stored the extended name in one process-global buffer. Moving it would preserve an unused,
duplicate interface, so the wrapper layer and its `plugin_ctx_*_ex`/accessor symbols are removed.
The live `plugin_ctx_create` and
`plugin_ctx_destroy` ABI remains required in module-runtime.

The inventory was reproduced with a whole-tree definition/reference scan over `src/**/*.{c,h}` and
the Make/CMake source lists. It found no caller for `plugin_ctx_create_ex`,
`plugin_ctx_destroy_ex`, `plugin_ctx_name`, `plugin_ctx_source_path`, `plugin_ctx_kind`, either
setter, or any `plugin_ctx_register_*` wrapper. The ownership checker now rejects every deleted
symbol and either retired include spelling if it reappears.

## Retained symbol inventory

| Owner | Retained symbols |
| --- | --- |
| module-runtime | `plugin_kind_name`, `plugin_kind_from_str`, `plugin_permission_name`, `plugin_permission_from_str`, `plugin_ctx_create`, `plugin_ctx_destroy`, typed `plugin_*` registries and counts |
| plugin-loader | `plugin_registry_*`, `plugin_manifest_parse`, `plugin_install`, `plugin_set_enabled`, `plugin_remove`, `plugin_discover_local`, `plugin_collect_hooks`, `plugin_collect_tools`, `plugin_tool_conflicts_with_builtin`, `plugin_load_and_register`, `plugin_load_all_registered` |

The loader contract transitively exposes the required extension types by including the canonical
module-runtime header. The reverse include is forbidden mechanically.

## Build and compatibility

Make, CMake, and focused test objects use both canonical source owners. Existing manifest formats,
registry paths, retained API symbol names, discovery order, loading behavior, and defaults are
unchanged. `unit-test-plugin` now exercises the required context lifecycle and typed registration
surface as well as optional manifest behavior.

Plugin-loader is still linked unconditionally because startup composition, dashboards, MCP tools,
and CLI commands consume installed-plugin state. A later slice must make those surfaces conditional
or route required status through module-runtime before it can prove plugin-loader omission from the
object and symbol closure.

## Verification

Verified locally: source-ownership checker and mutation suite; `unit-test-plugin` and
`unit-test-plugin-loader`; lint; and a clean parallel Make build. CMake is unavailable in the local
environment, so configure/build validation remains pending in the required `windows-cmake` CI job.
The exact diff returns to roundtable after every material correction.
