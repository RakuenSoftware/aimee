# Core modularization slice 48: declare config ownership

## Scope

This slice declares the `config` descriptor's `sources`, `private_headers`, `tests`, and `docs`
fields. It does not set `ownership_complete`. It is the declaration half of the declaration-then-latch
pair; the latch, its mutation coverage, and the completeness audit follow in slice 49. It changes
descriptor metadata, the regenerated test-registration baseline, documentation, and cleanup accounting
only; no production code, public symbol, build membership, configuration, storage, or runtime behavior
changes. No header is moved and no include site is rewritten.

`config` is the fifth Class B module. It is required core and one of the most depended-upon modules in
the graph, but structurally cleaner than vault: no orphan tests, no cross-module test ambiguity.

## What the module owns

The module root `src/modules/config/` contains, excluding `module.yaml`:

- Fifteen sources: `config.c`, `config_charter.c`, `config_database.c`, `config_fields.c`,
  `config_kb_curator.c`, `config_kb_maintenance.c`, `config_learning.c`, `config_memory.c`,
  `config_mode.c`, `config_plugin.c`, `config_save.c`, `config_sections.c`, `config_server_api.c`,
  `config_skills.c`, `config_trigger.c`.
- Seven headers: `config.h`, `config_database.h`, `config_fields.h`, `config_internal.h`,
  `config_learning.h`, `config_memory.h`, `config_sections.h`.

Six headers pair with a like-named source. `config_internal.h` has no paired source — it is the
internal seam header. The nine sources without a paired header (`config_charter.c`,
`config_kb_curator.c`, `config_kb_maintenance.c`, `config_mode.c`, `config_plugin.c`, `config_save.c`,
`config_server_api.c`, `config_skills.c`, `config_trigger.c`) are section parsers and facades that
declare through `config.h`, `config_sections.h`, and `config_internal.h`. No
`src/modules/config/include/aimee/config/` directory exists, so every header is at the module root and
is declared in `private_headers`.

`config_fields.h` is declared private today, matching the layout, but it is functionally a
cross-cutting contract: the get/set allowlist registry it declares is reached across module boundaries
by `src/cmd_data.c` and `src/server/server_config.c`. The slice-decision roundtable noted it is a
likely public-header candidate whose promotion — moving it under a canonical include tree — belongs to
a future header-layout slice, not this ownership-declaration slice, which moves nothing.

## Source liveness

Every declared source is live. `config.c` is the parse and snapshot core (the double-buffer/seqlock
live-config mechanism lives here). Each `config_*.c` is a section parser or facade reached by the
config load path and by cmd/server consumers: charter, the database section, the `config_fields`
get/set allowlist registry, kb-curator, kb-maintenance, learning, memory, operating-mode
(engineer/novel), the plugin section, save, the section dispatcher, the server HTTP API section,
skills, and trigger.

## Build membership

Make's `DATA_SRCS` compiles all fifteen sources and carries the `-Imodules/config` include path. CMake
compiles twelve and omits three — `config_fields.c`, `config_mode.c`, `config_server_api.c` — whose
callers are cmd/server/TLS-side: `config_fields` is reached by `cmd_data.c` and `server/server_config.c`,
`config_mode` by `aimee_tls.c` and `cli_agent_keys.c`, and `config_server_api` by `server_api.c`,
`server_main.c`, and the provider adapter. The thin `aimee` client's link closure does not reach these
three, and the required Windows and Linux CMake jobs build the thin client green from the twelve-source
set — the standing evidence that this is an intentional thin-client profile boundary, the same one
recorded for gateway (slice 38), audit (slice 34), learning (slice 42), workspace (slice 44), and vault
(slice 46), not source-list drift. The descriptor records canonical source ownership, which both build
systems agree on; it does not claim identical build-product membership.

## Test membership

Four `test_config*.c` files back four `unit-test-config*` targets, and all four link the core object
bundle (`TEST_CORE_OBJS`) plus their test object; none singles out another module's object. All four
are config-owned by subject:

- `test_config.c` — the config core.
- `test_config_surface.c` — a characterization net for `legacy_config_read`'s parse surface, auto-derived
  from `config.c`.
- `test_config_snapshot.c` — the live config snapshot double-buffer/seqlock in `config.c`: init/get,
  reload, validate-or-keep, and a concurrent torn-read stress.
- `test_config_economizer.c` — config's parsing and resolution of the `economizer: off|safe|aggressive`
  setting, a `config_fields`/`config.c` concern. The `economizer` in the filename is the setting being
  parsed, not the module under test; the subject and the `config` name agree, so this is the mirror
  image of the earlier subject-over-name calls where the two diverged.

Adjacent tests such as `test_cmd_config.c` (the cmd config command) and the frontend setup/settings
tests exercise the cmd and UI layers and are not claimed. Of the four, only `test_config.c` is
registered with CTest (`src/tests/CMakeLists.txt`); the other three run under Make alone, consistent
with their subjects sitting outside the portable thin-client test set. `scripts/check_module_test_registration.py`
now records four config rows — `test_config.c` as `make: true`, `ctest: true` and the other three as
`make: true`, `ctest: false`; that regeneration is the only reason the baseline file changes.

## Why declare without latching

The latch asserts the descriptor exhaustively covers the module root. That is true today — the module
root holds exactly these fifteen sources and seven headers — so the latch would pass. It is deferred
because declaring the files and asserting completeness are distinct claims, and the roundtable required
the completeness audit to review declarations merged on their own first rather than authored in the
same change. The validator accepts a declared-but-unlatched descriptor: it checks each declared path
exists and resolves within the module, and enforces set-equality only when `ownership_complete` is
true.

## Regression controls

The declaration is covered by the existing descriptor validation: every declared path must exist and
resolve within the module, and the regenerated test-registration baseline pins the four config tests'
per-suite registration. The empty-domain guard from slice 39 does not apply, because the module root is
not empty. The latch mutation coverage — source removal, private-header removal, planted files, cleared
latch — is deferred to slice 49, where `ownership_complete` is set and those mutations become
meaningful.

## Verification

Run from the repository root; each must exit zero:

```sh
python3 scripts/validate_module_descriptors.py --check-schema src/modules
python3 -m unittest scripts.tests.test_validate_module_descriptors
python3 scripts/check_module_docs.py
python3 scripts/check_cleanup_ledger.py
python3 scripts/check_module_test_registration.py
python3 -m unittest scripts.tests.test_check_module_test_registration
python3 scripts/check_module_source_ownership.py
python3 scripts/check_module_header_layout.py
python3 scripts/refactor_baselines.py check
make -C src -j2
make -C src lint
make -C src -j2 build/obj/tests/unit-test-config build/obj/tests/unit-test-config-snapshot
src/build/obj/tests/unit-test-config
src/build/obj/tests/unit-test-config-snapshot
```

The required pull-request CMake jobs build the thin client from the twelve-source subset and are the
standing evidence for the thin-client boundary above.

Slice 49 sets `ownership_complete: true`, adds the config latch mutation tests, and records the
completeness audit. Technical-writer review, exact-final-diff roundtable approval, and every required
pull-request check are required before merge.
