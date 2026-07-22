# plugin-loader module

## Purpose and non-goals

`plugin-loader` is the optional, default-disabled implementation for manifest discovery, installed-state
management, and dynamic-library loading at `src/modules/plugin-loader/plugin_loader.c` and
`src/modules/plugin-loader/plugin.c`. It does not own extension types, typed registries, or the
pre-LLM hook contract; those remain required in
[module-runtime](module-runtime.md#purpose-and-non-goals). It does not provide runtime unload.

## Public contracts

`src/modules/plugin-loader/include/aimee/plugin-loader/plugin_loader.h` exports
`plugin_loader_set_install_prefix`, `plugin_loader_scan_dir`, and `plugin_loader_discover_all`.
`src/modules/plugin-loader/include/aimee/plugin-loader/plugin.h` exports manifest parsing,
installed-registry operations,
install/enable/remove, hook/tool aggregation, conflict checks, `plugin_manifest_parse`, and
`plugin_load_and_register`. It consumes the required `plugin_permission_name` and
`plugin_permission_from_str` vocabulary.
Loader types consume `aimee/module-runtime/extension.h` one way; module-runtime never includes this
module.

## Dependencies and consumers

- `audit`: records governed plugin-management actions through the core audit contract.
- `config`: supplies Aimee paths and loader-related environment/config state.
- `execution-policy`: guards plugin management and execution decisions.
- `memory`: supplies the memory-provider bridge installed on a dynamic extension context.
- `module-runtime`: owns the required extension ABI and registries populated by the loader.
- `response-composition`: supplies the context-engine bridge installed by the loader.

Consumers are guarded Runtime startup in `src/server/server_main.c`, guarded CLI/server/dashboard
management surfaces, and the focused plugin tests. A disabled build has no required-to-optional
compile or link edge.

## Providers and readiness

The in-tree `plugin-loader` manifest/dynamic-library implementation is the reference provider. Readiness is
compile-time selection plus successful startup discovery; individual malformed or unloadable
plugins do not make Runtime startup fail. There is no alternate loader provider or hot-unload
provider.

## Configuration and activation

- `runtime_toggle.supported`: `false`; loader selection is compile-time and process-lifetime.

Make enables it with `AIMEE_WITH_PLUGIN_LOADER=1`; CMake uses
`-DAIMEE_WITH_PLUGIN_LOADER=ON`. Both default off. `AIMEE_INSTALL_PREFIX` selects the bundled
prefix, and `AIMEE_ENABLE_PROJECT_PLUGINS` gates project-local discovery. Individual manifest
entries must also be enabled before loading.

## Surfaces

When selected, the module provides the `plugin` CLI command, plugin-management HTTP/OpenAPI routes,
dashboard endpoints, the GUI plugin drawer, and manifest-provided hook/tool registrations. When
omitted, the disabled profile has none of those surfaces and the GUI hides the drawer after capability probing.
The exact absence proof is in `docs/validation/core-modularization-slice-11.md`.

## Data and migrations

The module owns plugin manifests and `plugins/installed.json` under the configured Aimee directory.
Disabling it neither reads nor deletes that state; re-enabling reads it again. Discovery precedence
is bundled, user, then project, with later same-name entries replacing earlier ones. No schema or
data migration is introduced by the ownership split.

## Security and privacy

`AIMEE_ENABLE_PROJECT_PLUGINS` makes project discovery opt-in because project content is less trusted. Missing required environment
variables cause an entry to be skipped. Core execution-policy, audit, and required runtime
contracts remain authoritative; loader omission cannot weaken those core controls. Dynamic plugin
code shares the Runtime process and must therefore be treated as trusted executable code.

## Supported journeys

An enabled build discovers manifests, filters disabled or unsatisfied entries, creates a
module-runtime context, installs memory/context bridges, loads the library, registers contributions,
and invokes `on_init`. Operators can inspect and manage installed entries through the guarded CLI,
HTTP, dashboard, and GUI surfaces. A default build runs the unrelated core journeys without any
loader object or surface.

## Tests and failure behavior

`unit-test-plugin-loader` at `src/tests/test_plugin_loader.c` covers missing/empty directories,
parsing, precedence, capacity,
environment requirements, and project gating. `unit-test-plugin` covers manifests, persistence,
context registration, and loader lifecycle behavior in `src/tests/test_plugin.c`. Missing directories are empty results;
malformed manifests, missing environment, and individual load failures are logged and skipped.
Allocation failure reports an error while startup continues without discovered plugins.

## Operational diagnostics

`plugin-loader` startup logs report skipped manifests and load failures. The enabled/disabled profile gate checks
objects, symbols, CLI, routes, OpenAPI, dashboards, and GUI capability behavior. There is no
independent loader health endpoint and no runtime transition from absent to ready.

## Compatibility

Manifest formats, registry paths, discovery order, symbol signatures, and stored state remain
compatible with the former root-level implementation. No forwarding header exists for
`src/headers/plugin_loader.h`; repository consumers use the canonical include. Required
`plugin_*` ABI names live in module-runtime and are not evidence that this optional module is core.

## Extension and removal

New discovery sources, public functions, dependencies, settings, or surfaces must update the
descriptor, both build systems, absence/profile tests, and this document. The loader currently
keeps contexts and library handles for process lifetime and never calls `plugin_ctx_destroy` during
normal operation; an unload-capable extension must call shutdown before `dlclose`. The old
root-level sources and forwarding includes are forbidden by
`scripts/check_module_source_ownership.py`. Removing this optional module must continue to preserve
module-runtime and every unrelated core journey.
