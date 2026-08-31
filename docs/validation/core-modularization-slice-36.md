# Core modularization slice 36: complete plugin-loader ownership

## Scope

This slice marks the optional `plugin-loader` descriptor `ownership_complete: true`. Earlier slices
already moved manifest discovery, installed-state management, and dynamic loading into
`src/modules/plugin-loader`, established the canonical public headers, and proved the omitted
profile. This slice proves that the descriptor exhaustively covers the validator's module-local C
and private-header domain, requires the canonical document, and records the audited public headers,
direct tests, build and test membership, and export liveness. It changes metadata, validation,
documentation, and cleanup accounting only; no production code, public symbol, build membership,
configuration, storage, or runtime behavior changes.

This is the first optional, default-off module to take the latch. Slices 29 through 35 latched
required core modules. The latch is independent of the feature flag: the validator's domain is the
module root on disk, so `AIMEE_WITH_PLUGIN_LOADER` does not change what must be declared, and the
regression controls run regardless of the selected profile.

## Ownership domain

The completeness domain is every module-local file whose suffix matches the `sources` or
`private_headers` role, excluding `module.yaml` and everything under
`src/modules/plugin-loader/include/aimee/plugin-loader/`. The module root contains exactly
`plugin.c`, `plugin_loader.c`, `module.yaml`, and the two canonical public headers. There are no
private headers, so the declared source set already equals the actual set and the latch is exact on
the checked-in tree. The descriptor's `docs` field equals
`["docs/modules/plugin-loader.md"]`, as the latch requires.

Completeness is a file-ownership statement. It does not assert that every owned export has a
caller, that every declared test runs in every build system, or that the public-header inventory is
auto-verified; public headers and tests remain explicit audited claims.

## Build membership

Both build systems gate the module identically and symmetrically, and both compile both sources
when it is selected:

- Make defaults `AIMEE_WITH_PLUGIN_LOADER ?= 0`. Under `=1`, `CORE_SRCS` gains
  `modules/plugin-loader/plugin.c` and `modules/plugin-loader/plugin_loader.c`, and `CMD_SRCS`
  gains `cmd_plugin.c`. Under `=0`, `SERVER_SRCS` drops `server/server_plugin.c`. `C_FLAGS` always
  carries `-Imodules/plugin-loader/include` and defines `AIMEE_WITH_PLUGIN_LOADER`.
- CMake declares `option(AIMEE_WITH_PLUGIN_LOADER "Build the optional plugin manifest loader" OFF)`
  and defines the same macro either way. Under `ON`, `CORE_SRCS` gains both sources and `CMD_SRCS`
  gains `cmd_plugin.c`. Under `OFF`, `SERVER_SRCS` drops `server_plugin.c`.

There is therefore no source membership asymmetry to record for this module. The omitted profile is
already proven by the `plugin-loader-profiles` CI job and `scripts/check_plugin_loader_disabled.sh`,
whose deny pattern covers the plugin routes, CLI verbs, and the `plugin_loader_discover_all`,
`plugin_registry_*`, `plugin_manifest_parse`, and `plugin_load_and_register` symbols.

## Source liveness and ownership

Exports with tracked production consumers:

- `plugin_registry_load` and `plugin_discover_local`, `src/cmd_hooks.c`, `src/cmd_plugin.c`,
  `src/dashboard.c`, `src/modules/protocols/mcp/mcp_tools.c`, and
  `src/server/dashboard_server.c`.
- `plugin_registry_json` and `plugin_set_enabled`, `src/server/server_plugin.c`, with
  `plugin_set_enabled` also called by `src/cmd_plugin.c`.
- `plugin_install`, `plugin_remove`, and `plugin_manifest_parse`, `src/cmd_plugin.c`.
  `plugin_install` is additionally called by `src/tests/test_dashboard.c`.
- `plugin_collect_hooks`: `src/cmd_hooks.c`. `plugin_collect_tools`,
`src/modules/protocols/mcp/mcp_tools.c`.
- `plugin_loader_discover_all`: `src/server/server_main.c`, the live startup path.

Exports with no tracked production caller outside the module. The list is exhaustive; each entry
names every tracked caller:

- `plugin_registry_path`: called only from `plugin.c`. The two mentions in
  `src/tests/test_plugin.c` are comments about path caching, not calls.
- `plugin_registry_save`: called only from `plugin.c`, by the install, enable, and remove paths.
- `plugin_load_and_register`: called from `plugin.c` and `plugin_loader.c`.
- `plugin_tool_conflicts_with_builtin`: called from `plugin.c`, and externally only by
  `src/tests/test_plugin.c`.
- `plugin_registry_get`: no module-local caller; its only tracked caller is
  `src/tests/test_plugin.c`.

Exports with no caller in the tracked tree at all:

- `plugin_load_all_registered` is a second registration entry point that loads every enabled
  registry entry. The live startup path is `plugin_loader_discover_all`, which performs its own
  four-source merge and calls `plugin_load_and_register` directly, so this function is superseded.
  Its header comment still promises "Calls plugin_collect_delegate_backends() after registration";
  `plugin_collect_delegate_backends` existed in the pre-split `src/plugin.c` and was removed by the
  ABI and loader split in #1722, which left the comment behind. No such symbol exists in the current
  tree.
- `plugin_loader_set_install_prefix` sets the prefix used for bundled discovery. Its header
  documents "Call once at startup from main() before plugin_loader_discover_all()", and no tracked
  `main()` does, so in the tracked startup path bundled discovery takes the documented fallback
  chain: `$AIMEE_INSTALL_PREFIX`, then `./plugins/` relative to the working directory.

Both of the latter remain declared in public headers and shipped as external symbols in an enabled
build. Absence of a tracked-tree caller is liveness evidence, not proof that no downstream host
links and calls them. A downstream host can call the setter before discovery, in which case the
fallback chain does not apply. Removing or rewiring either is therefore an API and ABI
compatibility decision rather than a dead-code cleanup, and the same applies to privatizing the
five exports above.

Whole-tree tracked-file and symbol searches find no second manifest parser, installed-registry
implementation, or dynamic-library loader.

## Adjacent surfaces

`src/cmd_plugin.c`, `src/server/server_plugin.c`, `src/dashboard.c`,
`src/server/dashboard_server.c`, `src/modules/protocols/mcp/mcp_tools.c`, and
`src/modules/config/config_plugin.c` are loader consumers and feature-gated adapters that live
outside the module root, so they are outside the completeness domain. Being gated by this module's
feature flag does not make a command-layer or server-layer adapter part of the module's
implementation; treating feature gating as ownership would blur the layer boundary. Their lack of a
descriptor is broader layer-ownership debt tracked separately and does not weaken this latch.

## Test membership

`src/tests/test_plugin.c` and `src/tests/test_plugin_loader.c` are the module's declared direct
tests, covering manifest parsing, registry persistence, install/enable/remove round trips, hook and
tool aggregation, conflict checks, discovery precedence, capacity, required-environment filtering,
and project gating.

Build systems do not register tests uniformly. Make registers `unit-test-plugin` and
`unit-test-plugin-loader` in `src/tests/Rules.mk`; `src/tests/CMakeLists.txt` registers
`test_plugin_loader` as a CTest case but not `test_plugin`, which therefore runs only under Make.

> **Correction (slice 37).** As merged, this section and the verification note below claimed that
> neither plugin test was registered with CTest. That was wrong for `test_plugin_loader`, which
> `src/tests/CMakeLists.txt` registers with an explicit `add_executable` plus `add_test`. The audit
> read only the top-level `CMakeLists.txt` and missed the test subdirectory. The remaining gap is
> real but narrower than recorded: `test_plugin` alone lacks CTest registration. Slice 37 corrected
> the text and added `scripts/check_module_test_registration.py`, which pins per-test build-file
> registration to a reviewed baseline. No other finding in this document depended on the
> mistaken claim, and the ownership latch is unaffected.

This remaining single-test gap is pre-existing test-registration debt with a concrete verification
target (CTest must also execute `test_plugin` in an enabled profile) and is deferred to a
build-membership slice because it changes build inputs rather than file ownership.
`scripts/check_module_test_registration.py` pins the build-file registration of every
descriptor-declared test, bound to the declared source path rather than to a target name, so a
change to this gap surfaces as a baseline diff.

## Regression controls

The descriptor mutation suite removes `plugin.c` and `plugin_loader.c` individually; each omission
must fail `rule=ownership-complete` on `/sources`. It plants
`src/modules/plugin-loader/undeclared.c` and `undeclared.h`, which must fail the same rule on
`/sources` and `/private_headers`. It removes `docs/modules/plugin-loader.md` from the descriptor's
`docs` field, which must fail that rule on `/docs`. The latched-descriptor assertion introduced in
slice 35 now also covers `plugin-loader`, so silently clearing the latch fails directly instead of
only weakening the mutation assertions. The unmodified descriptor graph must pass. These controls
run independently of `AIMEE_WITH_PLUGIN_LOADER`. They cover the module-local source and
private-header domain and the canonical document; they do not police the public-header inventory,
which the validator checks only as declared paths.

## Verification

Run the locally available checks from the repository root; each command must exit zero:

```sh
python3 scripts/validate_module_descriptors.py --check-schema src/modules
python3 -m unittest scripts.tests.test_validate_module_descriptors
python3 scripts/check_module_docs.py
python3 scripts/check_cleanup_ledger.py
python3 scripts/check_module_header_layout.py
python3 -m unittest scripts.tests.test_check_module_header_layout
python3 scripts/check_module_source_ownership.py
python3 -m unittest scripts.tests.test_check_module_source_ownership
python3 -m unittest scripts.tests.test_check_module_docs \
  scripts.tests.test_check_cleanup_ledger scripts.tests.test_refactor_baselines
python3 scripts/refactor_baselines.py check
make -C src -j2
make -C src lint
make -C src -j2 AIMEE_WITH_PLUGIN_LOADER=1 build/obj/tests/unit-test-plugin \
  build/obj/tests/unit-test-plugin-loader
src/build/obj/tests/unit-test-plugin
src/build/obj/tests/unit-test-plugin-loader
```

The default `make -C src` build omits the module, which is the shipped default; the two test
binaries link the module objects directly, so they are built and run explicitly.

The local environment used for this slice does not provide `cmake`. In a CMake-capable environment,
and in the required pull-request CMake job, confirm both profiles still configure and that the
enabled profile compiles both sources:

```sh
cmake -S . -B build/cmake-slice36 -DAIMEE_WITH_UI=OFF -DAIMEE_WITH_PLUGIN_LOADER=ON
cmake --build build/cmake-slice36 --target aimee-core -j2
```

`test_plugin_loader` is a registered CTest case. Configuring registers it but does not build it, so
build the test target before invoking CTest:

```sh
cmake --build build/cmake-slice36 --target test_plugin_loader -j2
ctest --test-dir build/cmake-slice36 -R '^test_plugin_loader$' --output-on-failure
```

`test_plugin` has no CTest selector until the deferred test-registration slice lands. The
omitted-profile proof remains the `plugin-loader-profiles` CI job.

Technical-writer review, exact-final-diff roundtable approval, and every required pull-request
check, including Windows CMake, are required before merge.
