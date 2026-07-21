# Plugin-loader module

## Purpose and non-goals

Plugin-loader discovers plugin manifests in bundled, user, and project directories, merges
same-named entries by precedence, checks required environment variables, and asks the existing
plugin runtime to load enabled entries. Its owned implementation is
`src/modules/plugin-loader/plugin_loader.c`; its public discovery header lives at
`src/modules/plugin-loader/include/aimee/plugin-loader/plugin_loader.h` and is included as
`aimee/plugin-loader/plugin_loader.h`.

This module does not own the core extension ABI, extension registries, pre-LLM hook contract,
plugin manifest persistence, or plugin management commands. Those responsibilities are still
mixed in legacy files and are the subject of the next core ABI split.

## Classification and link profile

The descriptor at `src/modules/plugin-loader/module.yaml` records the target classification as
optional and disabled by default. That metadata does not describe the current binaries. The link
profile remains unconditional: Make and CMake still list the discovery source in their core source
lists. There is no build-time, link-time, or runtime-removal guarantee in this slice.

The current change establishes a new physical owner. The link profile remains unconditional until
the contract-split slice proves plugin-loader is not required for the core link closure.

## Public contracts

The discovery header exports three functions:

- `plugin_loader_set_install_prefix` selects the bundled-plugin prefix.
- `plugin_loader_scan_dir` parses plugin subdirectories into caller-owned `plugin_t` storage.
- `plugin_loader_discover_all` discovers, filters, and registers plugins without making individual
  plugin failures fatal to server startup.

The header currently retains a transitional `plugin.h` include because `plugin_t`, manifest
parsing, and load registration are still declared by the mixed legacy contract. That allowance
expires with the next contract-split slice. Any new symbol pulled from `plugin.h` into
`plugin_loader.c` or its header requires reopening this ownership contract.

## Dependencies and consumers

The implementation consumes configuration, Aimee home-path resolution, logging, manifest parsing,
and load-registration contracts. Direct consumers of the relocated header are the implementation
itself, Aimee Runtime startup in `src/server/server_main.c`, and the focused loader test in
`src/tests/test_plugin_loader.c`.

The required server currently invokes discovery during composition. This is migration debt, not
evidence that an optional module is absent from required code.

## Configuration and activation

This source move adds no settings and changes no defaults. `AIMEE_INSTALL_PREFIX` remains the
fallback bundled-plugin prefix. Project-local discovery remains gated by
`AIMEE_ENABLE_PROJECT_PLUGINS`; a missing or `0` value skips that source. User and bundled discovery
retain their existing behavior. Individual entries must already be enabled before they are loaded.

## Discovery order and data

Discovery visits bundled plugins, then user plugins, then project plugins. Later sources replace an
earlier same-named entry. Plugin manifests and the in-memory `plugin_t` representation remain owned
by the mixed legacy plugin contract; this module does not introduce a new persistence format or
migration.

## Security and privacy

Project discovery stays opt-in because project content is less trusted than bundled or user-owned
content. Missing required environment variables cause an entry to be skipped. Dynamic-loading,
permission, execution-policy, and audit enforcement are not redesigned here. OIDC and SSHSIG module
documentation attestation are separate governance hardening and do not gate this physical move.

## Failure behavior and diagnostics

An absent discovery directory is an empty result. An allocation failure reports an error and lets
startup continue without discovered plugins. A malformed manifest is logged and skipped. Missing
required environment variables and load failures are logged per plugin; discovery continues and
summarizes load failures for the caller.

## Tests

`unit-test-plugin-loader` covers empty and missing directories, manifest discovery, precedence,
capacity limits, required-environment handling, and project-plugin gating. The ownership checker
also proves the old source and header are gone, the exact canonical files exist, Make and CMake each
compile one canonical source, the Make test graph uses its canonical object, and every header
consumer uses the canonical include.

## Compatibility and migration gap

Symbols, function signatures, configuration, discovery order, and error handling are unchanged.
This slice adds no installed public-header export. There is no forwarding header at
`src/headers/plugin_loader.h`; any future export or compatibility shim needs an explicit contract
decision.

The migration gap is deliberate and bounded:

1. `plugin.c` and `plugin.h` still mix manifest/install/loading functions with required extension
   ABI and registries.
2. `plugin_ctx.c` and `plugin_ctx.h` remain ABI-adjacent pending per-symbol classification.
3. `plugin_c_hook.c` and `plugin_c_hook.h` remain required by agent runtime and must move with the
   required extension contract, not under an optional owner.
4. Required consumers still link the loader discovery unit unconditionally.

The next slice performs the per-symbol audit and core ABI split: required ABI, registries, and the
required pre-LLM hook contract move to `module-runtime`; manifest discovery, installation,
enable/disable, removal, and dynamic loading remain candidates for plugin-loader. It then updates
consumers to core registry contracts and proves the required link closure excludes plugin-loader.

## Extension and removal rules

New discovery sources, public functions, or dependencies require updating the module descriptor,
this document, `scripts/check_plugin_loader_ownership.py`, and focused tests together. Removal from
required binaries is not complete until the contract split and build-profile proof land. Ordinary
module documentation and physical source extraction use normal review and CI; signed descriptor-v2
attestations remain a separate governance program.
