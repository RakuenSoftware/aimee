# Core modularization slice 35: complete module-runtime ownership

## Scope

This slice marks the required `module-runtime` descriptor `ownership_complete: true`. Earlier slices
already moved the extension ABI and the pre-LLM hook registry into `src/modules/module-runtime`,
established the canonical public headers, and made this module the ownership-descriptor pilot. This
slice proves that the descriptor exhaustively covers the validator's module-local C and
private-header domain, requires the canonical document, and records the audited public headers,
direct test, build and test membership, and public-surface liveness. It changes metadata,
validation, documentation, and cleanup accounting only; no production code, public symbol, build
membership, configuration, storage, or runtime behavior changes.

## Ownership domain

The validator's completeness domain is every module-local file whose suffix matches the `sources` or
`private_headers` role, excluding `module.yaml` and everything under
`src/modules/module-runtime/include/aimee/module-runtime/`. The module root contains exactly
`extension.c`, `pre_llm_hook.c`, `module.yaml`, and the two canonical public headers. There are no
private headers, so the declared source set already equals the actual set and the latch is exact on
the checked-in tree. The descriptor's `docs` field equals `["docs/modules/module-runtime.md"]`, as
the latch requires.

Completeness is a file-ownership statement. It does not assert that every owned public facility has
a host-side caller, that every declared test runs in every build system, or that the public-header
inventory is auto-verified; public headers and tests remain explicit audited claims.

## Source liveness and ownership

Both descriptor-owned sources are compiled by both build systems, so this module has no source
membership asymmetry:

- Make's `CORE_SRCS` in `src/Makefile` lists `modules/module-runtime/extension.c` and
  `modules/module-runtime/pre_llm_hook.c`, and `C_FLAGS` adds `-Imodules/module-runtime/include`.
- CMake lists both sources in `CORE_SRCS` and adds the same include root.

- `extension.c` supplies the extension ABI. `plugin_ctx_create` and `plugin_ctx_destroy` are called
  by `src/modules/plugin-loader/plugin.c`; `plugin_kind_name` is called by `src/cmd_plugin.c` and
  the loader; `plugin_kind_from_str` and `plugin_permission_from_str` are called by the loader; and
  `plugin_permission_name` is called by the loader and by
  `src/modules/protocols/mcp/mcp_tools.c`.
- `pre_llm_hook.c` supplies the synchronous PRE_LLM_CALL registry. `plugin_chook_apply_pre_llm` is
  called once per request by `src/server/agent_runtime.c`, which appends any returned ephemeral
  context to the user message and never to the system prompt.

Whole-tree tracked-file and symbol searches find no second extension-context implementation, typed
registry, or pre-LLM hook registry. `src/headers/context_engine.h` documents registration through
`plugin_ctx_t->register_context_engine` and is a consumer-side contract, not a competing registry.

## Producer-facing surface and liveness findings

The module's registration entry points are producer-facing ABI: they exist for in-process extensions
loaded by plugin-loader, so the absence of a host-side caller is the expected state rather than dead
code. The audit records which side of that boundary each export sits on.

Reader side, with tracked host-side callers:

- `plugin_chook_apply_pre_llm`: `src/server/agent_runtime.c`.
- `plugin_kind_name`, `plugin_kind_from_str`, `plugin_permission_name`,
  `plugin_permission_from_str`, `plugin_ctx_create`, `plugin_ctx_destroy`, plugin-loader, the
  plugin CLI, and MCP tool projection as listed above.

Producer side, with no host-side caller in the tracked tree:

- `plugin_chook_register_pre_llm` has no host-side caller; its only tracked callers are in
  `src/tests/test_plugin_c_hook.c`. A host build therefore starts with an empty registry, and the
  per-request `plugin_chook_apply_pre_llm` call returns `NULL` until a loaded extension registers a
  hook through the exported API. This is a producer/consumer asymmetry, not proof that the code is
  unreachable: the export is part of the extension ABI and a dynamically loaded plugin can call it.
- The `plugin_ctx_t` `register_*` callbacks are installed by `plugin_ctx_create` but invoked only
  from a loaded extension's `register()` entry point.

Helper exports with no host-side caller: `plugin_chook_reset` is an explicit test hook and
`plugin_chook_pre_llm_count` is an introspection accessor; both are called only by
`src/tests/test_plugin_c_hook.c`. `plugin_chook_run_pre_llm` is different in kind: it is the
internal implementation that `plugin_chook_apply_pre_llm` runs on every request, and it is
additionally exported so the test can drive it directly with its own output buffer. It is on the
live production path even though no host-side source calls it by name.

Typed registry state, audited individually:

- `plugin_slash_commands` and `plugin_slash_command_count` are written by
  `src/modules/plugin-loader/plugin.c` from manifest data and are read only by
  `src/tests/test_plugin.c`. In production they are write-only.
- `plugin_delegate_backend_count`, `plugin_platform_adapter_count`, `plugin_context_engine_count`,
  and `plugin_memory_provider_count` are read by exactly one loader `LOG_INFO` registration summary.
- `plugin_memory_providers` and `plugin_context_engines` are never written by any tracked code. The
  loader installs `loader_register_memory_provider` and `loader_register_context_engine` bridges
  that route to `memory_provider_register` and `context_engine_register` instead, so the two counts
  reported by that log line are always zero.
- `plugin_tools`, `plugin_tool_count`, `plugin_hooks`, `plugin_hook_count`,
  `plugin_cli_subcommands`, `plugin_cli_subcommand_count`, `plugin_model_providers`, and
  `plugin_model_provider_count` have no non-test reader. MCP plugin-tool exposure does not read the
  global registry: `src/modules/protocols/mcp/mcp_tools.c` calls the loader's
  `plugin_collect_tools` over per-plugin manifest data.

These are focused candidates for privatization, activation, deletion, or bridge alignment. Every one
of them is either exported extension ABI or state written by the optional loader, so changing them
alters the extension contract and requires a separate compatibility decision. Their containing
sources remain live.

## Test membership

`src/tests/test_plugin_c_hook.c` is the module's declared direct test and covers empty state,
ordering, error and empty hook results, bounded capacity, reset, count, null and excess
registration rejection, the user-message argument, and per-turn application.

`src/tests/test_plugin.c` remains declared by plugin-loader. Its primary subject is loader
integration, and a test is never claimed by two descriptors; it is complementary coverage of
`extension.c`, context lifecycle, kind and permission conversion, and the tool, hook, and
slash-command registries, and is currently the only coverage of those extension surfaces.

Both build systems register the declared test. Make builds `unit-test-plugin-c-hook` from
`src/tests/Rules.mk`, and `src/tests/CMakeLists.txt` registers `test_plugin_c_hook` as a CTest case.

> **Correction (slice 37).** As merged, this section and the verification note below claimed that
> CMake registered no module-runtime test target and that the declared test executed only in the
> Make suite. That was wrong. The audit read only the top-level `CMakeLists.txt` and missed
> `src/tests/CMakeLists.txt`, which `add_subdirectory(src/tests)` pulls in and which registers
> `test_plugin_c_hook`. Slice 37 corrected the text and added
> `scripts/check_module_test_registration.py`, which derives per-test registration from the build
> files and pins it to `tests/baselines/refactor/module-test-registration.json`, so a future change
> in registration surfaces as a reviewed baseline diff instead of being rediscovered by hand. No other finding in this document depended on the mistaken claim, and the ownership latch
> is unaffected.

## Regression controls

The descriptor mutation suite removes `extension.c` and `pre_llm_hook.c` individually; each omission
must fail `rule=ownership-complete` on `/sources`. It plants
`src/modules/module-runtime/undeclared.c` and `undeclared.h`, which must fail the same rule on
`/sources` and `/private_headers`. It removes `docs/modules/module-runtime.md` from the descriptor's
`docs` field, which must fail that rule on `/docs`. A new assertion requires every latched
descriptor, including `module-runtime`, to still declare `ownership_complete: true`, so silently
clearing the latch fails directly instead of only weakening the mutation assertions. The unmodified
descriptor graph must pass. These controls cover the module-local source and private-header domain
and the canonical document; they do not police the public-header inventory, which the validator
checks only as declared paths.

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
make -C src -j2 build/obj/tests/unit-test-plugin-c-hook build/obj/tests/unit-test-plugin
src/build/obj/tests/unit-test-plugin-c-hook
src/build/obj/tests/unit-test-plugin
```

The local environment used for this slice does not provide `cmake`. In a CMake-capable environment,
and in the required pull-request CMake job, confirm that both module-runtime sources still compile
into the core library:

```sh
cmake -S . -B build/cmake-slice35 -DAIMEE_WITH_UI=OFF
cmake --build build/cmake-slice35 --target aimee-core -j2
```

The declared test is also a registered CTest case. Configuring registers it but does not build it,
so build the test target before invoking CTest:

```sh
cmake --build build/cmake-slice35 --target test_plugin_c_hook -j2
ctest --test-dir build/cmake-slice35 -R '^test_plugin_c_hook$' --output-on-failure
```

Technical-writer review, exact-final-diff roundtable approval, and every required pull-request
check, including Windows CMake, are required before merge.
