# Plugin-loader module

## Purpose and non-goals

Plugin-loader discovers plugin manifests in bundled, user, and project directories, merges
same-named entries by precedence, checks required environment variables, and asks the existing
plugin runtime to load enabled entries. Its owned implementation is
`src/modules/plugin-loader/plugin_loader.c`; its public discovery header lives at
`src/modules/plugin-loader/include/aimee/plugin-loader/plugin_loader.h` and is included as
`aimee/plugin-loader/plugin_loader.h`.

This module does not own the core extension ABI, extension registries, or pre-LLM hook contract.
It does own optional plugin manifest persistence, management commands, and dynamic loading. The
manifest implementation is `src/modules/plugin-loader/plugin.c`; its canonical contract is
`src/modules/plugin-loader/include/aimee/plugin-loader/plugin.h`, included as
`aimee/plugin-loader/plugin.h`.

## Classification and link profile

The descriptor at `src/modules/plugin-loader/module.yaml` classifies the module as optional and
default-disabled. Make selects it with `AIMEE_WITH_PLUGIN_LOADER=1` (or omits it with `0`); CMake
uses `-DAIMEE_WITH_PLUGIN_LOADER=ON|OFF`. Both default to disabled.

The disabled profile omits the loader sources and the plugin-management command and server handler
translation units. Startup discovery, manifest hook/tool aggregation, CLI/HTTP routes, and dashboard
surfaces are compiled out, including their generated OpenAPI entries. The web GUI also hides its
plugin drawer when its capability probe gets the absent-route response. The required extension ABI
remains available from module-runtime. See `docs/validation/core-modularization-slice-11.md` for the
complete disabled-surface proof.

## Public contracts

The discovery header exports three functions:

- `plugin_loader_set_install_prefix` selects the bundled-plugin prefix.
- `plugin_loader_scan_dir` parses plugin subdirectories into caller-owned `plugin_t` storage.
- `plugin_loader_discover_all` discovers, filters, and registers plugins without making individual
  plugin failures fatal to server startup.

`plugin_load_and_register` is documented under the optional contract in
`aimee/plugin-loader/plugin.h`; it is the only entry point that installs bridges and invokes
`on_init`, and it does not invoke `on_shutdown` in this slice.

The discovery header includes the canonical loader contract for `plugin_t`, manifest parsing, and
load registration. That contract depends one-way on `aimee/module-runtime/extension.h`;
module-runtime never includes plugin-loader.

## Dependencies and consumers

The implementation consumes configuration, Aimee home-path resolution, logging, manifest parsing,
load-registration contracts, and the memory-provider/context-engine registration bridges it
installs on dynamic-plugin contexts. Direct consumers of the relocated header are the implementation
itself, Aimee Runtime startup in `src/server/server_main.c`, and the focused loader test in
`src/tests/test_plugin_loader.c`. The server include and discovery call are both guarded by
`AIMEE_WITH_PLUGIN_LOADER`, so the disabled profile has no required-to-optional compile or link edge.

The descriptor therefore declares the required `memory` and `response-composition` dependencies in
addition to `module-runtime`, configuration, execution policy, and audit.

## Configuration and activation

This source move adds no settings and changes no defaults. `AIMEE_INSTALL_PREFIX` remains the
fallback bundled-plugin prefix. Project-local discovery remains gated by
`AIMEE_ENABLE_PROJECT_PLUGINS`; a missing or `0` value skips that source. User and bundled discovery
retain their existing behavior. Individual entries must already be enabled before they are loaded.

## Discovery order and data

Discovery visits bundled plugins, then user plugins, then project plugins. Later sources replace an
earlier same-named entry. Plugin manifests and the in-memory `plugin_t` representation are owned
here; this split does not introduce a new persistence format or migration.

## Security and privacy

Project discovery stays opt-in because project content is less trusted than bundled or user-owned
content. Missing required environment variables cause an entry to be skipped. Dynamic-loading,
permission, execution-policy, and audit enforcement are not redesigned here. The management entry
points and their existing server/dashboard authorization guards are unchanged by the source move.
OIDC and SSHSIG module documentation attestation are separate governance hardening and do not gate
this physical move.

## Failure behavior and diagnostics

An absent discovery directory is an empty result. An allocation failure reports an error and lets
startup continue without discovered plugins. A malformed manifest is logged and skipped. Missing
required environment variables and load failures are logged per plugin; discovery continues and
summarizes load failures for the caller.

## Tests

`unit-test-plugin-loader` covers empty and missing directories, manifest discovery, precedence,
capacity limits, required-environment handling, and project-plugin gating.
`scripts/check_module_source_ownership.py` separately proves the old source and header are gone,
the exact canonical files exist, Make and CMake each compile one canonical source, the Make test
graph uses its canonical object, and every header consumer uses the canonical include.

## Compatibility and migration gap

Symbols, function signatures, configuration, discovery order, and error handling are unchanged.
This slice adds no installed public-header export; there is no forwarding header at
`src/headers/plugin_loader.h`. Any future export or compatibility shim needs an explicit contract
decision.

The contract split is complete: `plugin_manifest_parse`, `plugin_load_and_register`, registry
persistence, install/enable/remove, manifest hook/tool aggregation, and dynamic loading live here.
Canonical consumers include `src/modules/plugin-loader/plugin.c`,
`src/modules/plugin-loader/include/aimee/plugin-loader/plugin_loader.h`, and
`src/tests/test_plugin.c`.

The required runtime co-locates `plugin_permission_name` and `plugin_permission_from_str` with the
permission enum. Plugin-loader consumes that stable vocabulary when reading and writing manifests.
For dynamic libraries, plugin-loader sets identity and memory/context bridge callbacks before it
invokes `register()`. If registration succeeds, plugin-loader invokes the plugin's `on_init`
against the registered context. `on_shutdown` is the registered shutdown callback that
`plugin_ctx_destroy` would invoke; in the current build, plugin-loader does not call
`plugin_ctx_destroy` during normal operation, and plugin contexts and library handles remain alive
for the process lifetime. No plugin unload path exists in this slice, so bridge code is never
unloaded while a plugin can call it. A future unload-capable slice must invoke `on_shutdown` before
destroying the context and calling `dlclose` on the handle.

Disabling the loader does not read, migrate, or delete plugin manifests or the
`plugins/installed.json` registry under the configured Aimee directory. That state remains dormant
and is read again if a later build enables the loader.

## Extension and removal rules

New discovery sources, public functions, or dependencies require updating the module descriptor,
this document, `scripts/check_module_source_ownership.py`, and focused tests together. Removal from
required binaries is not complete until the contract split and build-profile proof land. Ordinary
module documentation and physical source extraction use normal review and CI; signed descriptor-v2
attestations remain a separate governance program.
