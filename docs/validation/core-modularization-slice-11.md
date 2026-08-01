# Core modularization slice 11: optional plugin-loader build profile

## Outcome

`plugin-loader` is now physically optional. Make enables it with
`AIMEE_WITH_PLUGIN_LOADER=1` and omits it with `0`; CMake uses
`-DAIMEE_WITH_PLUGIN_LOADER=ON|OFF`. Both default to disabled, matching the optional-module policy
and module descriptor.

The required `module-runtime` extension ABI and its in-process typed registries remain in every
profile. Disabling plugin-loader removes manifest parsing and persistence, discovery, install and
enable/disable management, dynamic loading, manifest hooks, and manifest-provided MCP tools.

## Surface behavior

When disabled, startup does not scan plugin directories, the `plugin` CLI command is absent, plugin
HTTP operations and routes are absent, plugin dashboard endpoints are absent, and the web GUI hides
its plugin drawer after capability probing. The server's generated OpenAPI document omits the same
routes. Plugin manifests and the configured Aimee directory's `plugins/installed.json` registry are
neither read nor deleted. Re-enabling the module reads that state again.

This is a compile-time selection because loader contexts and dynamic-library handles live for the
process lifetime. Runtime enable/disable of the loader itself remains unsupported. This slice adds
no repository adapter, provider abstraction, or replacement persistence layer.

## Verification

The profile CI job first builds the default-disabled `aimee`, `aimee-server`, `aimee-kb`, and
`aimee-gateway`, then `scripts/check_plugin_loader_disabled.sh` verifies:

- no plugin-loader, plugin-management CLI, or plugin-management server object exists;
- no loader management route, OpenAPI path, operation, or loader symbol string appears in shipped
  binaries;
- required `module-runtime/extension.o` remains present; and
- the required extension runtime has no unresolved loader or dynamic-loading dependency.

CMake also configures and builds the thin client with the loader disabled. The same job then switches
the Make profile to explicit opt-in, builds all binaries, runs `unit-test-plugin` and
`unit-test-plugin-loader`, and proves the loader objects and management/OpenAPI surfaces returned.
