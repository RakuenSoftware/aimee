# Core modularization slice 34: complete audit ownership

## Scope

This slice marks the required `audit` descriptor `ownership_complete: true`. Slices 23 and 24 already
moved the implementation and public contracts into `src/modules/audit` and assigned the four direct
tests. This slice proves that the descriptor exhaustively covers the validator's module-local C and
private-header domain, requires the canonical document, and records the audited public headers and direct
tests. It changes metadata, validation, documentation, and cleanup accounting only; no production code,
public symbol, build membership, configuration, storage, or runtime behavior changes.

## Source liveness and ownership

Make's `CORE_SRCS` compiles all four descriptor-owned sources:

- `audit_action.c`: governed-action hashes and bounded command previews are called by
  `modules/guardrails/guardrails_action_audit.c`; key provisioning is called by `cmd_hooks.c` and
  `server/server_main.c`.
- `audit_ledger.c`: `audit_ledger_read` is called by `trajectory_export.c` and `server/server_state.c`.
- `audit_worm.c`: append, read, verify, checkpoint, seal, and metric paths are called by guardrails,
  `cmd_audit.c`, server state/management, and KB vault operations.
- `audit_worm_chain.c`: the SQLite store consumes its canonical hashing and checkpoint primitives, while
  `db2/kb_audit_worm.c` consumes `audit_worm_row_hash` for byte-identical PostgreSQL rows.

Make derives `KB_CORE_SRCS` from `CORE_SRCS` but removes `audit_worm.c`, the SQLite store, while retaining
the engine-independent chain for the separate DB2 PostgreSQL implementation. CMake's `CORE_SRCS` contains
only `audit_action.c` and `audit_ledger.c`; that list feeds `aimee-core`, `SERVER_CORE_SRCS`, and
`KB_CORE_SRCS`. CMake therefore does not currently ship either WORM source. This pre-existing target
asymmetry is documented rather than silently normalized in an ownership-only slice: the descriptor owns
canonical implementations, while build projections decide which product compiles them.

Whole-tree tracked-file and symbol searches classify adjacent audit-named files as consumers,
server/KB storage composition, or separate code-intelligence, token-accounting, memory-harness, and
integration-test boundaries. None implements a second governed-action hash, legacy ledger reader,
SQLite WORM store, or canonical chain algorithm.

## DRY and dead-code findings

No complete module source is self-tested-only. Exact caller searches identify these public declarations
without an external shipping caller:

- `audit_hmac_sha256_testonly` is an explicit unit-test hook;
- `audit_worm_init_at`, `audit_worm_count`, and `audit_worm_close` are test/embedding or introspection
  contracts used by `test_audit_worm.c`;
- `audit_worm_verify_file` currently has only `test_audit_worm.c` callers;
- `audit_worm_verify_chain` is called inside `audit_worm.c` and directly by its test;
- `audit_worm_hex32`, `audit_worm_ckpt_mac`, and `audit_worm_chain_key_load` are module-internal helpers,
  with direct vector coverage for the first two.

These are focused public-surface privatization, activation, or deletion candidates. Their containing
sources remain live, and changing linkage or removing intended embedding/verification contracts requires
a separate compatibility decision.

## Regression controls

The descriptor mutation suite removes `audit_action.c`, `audit_worm.c`, and `audit_worm_chain.c`
individually; each omission must fail `rule=ownership-complete` on `/sources`. It also plants
`src/modules/audit/undeclared.c` and `undeclared.h`, which must fail the same rule on `/sources` and
`/private_headers`, and removes `docs/modules/audit.md` from the descriptor's `docs` field, which must
fail it on `/docs`. The unmodified 26-descriptor graph must pass.

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
make -C src -j2 build/obj/tests/unit-test-audit-worm \
  build/obj/tests/unit-test-audit-worm-chain
src/build/obj/tests/unit-test-audit-worm
src/build/obj/tests/unit-test-audit-worm-chain
```

The local environment used for this slice does not provide `cmake`. In a CMake-capable environment, and
in the required pull-request CMake job, configure and execute the two registered direct tests with:

```sh
cmake -S . -B build/cmake-slice34 -DAIMEE_WITH_UI=OFF
cmake --build build/cmake-slice34 --target test_audit_action test_audit_ledger -j2
ctest --test-dir build/cmake-slice34 \
  -R '^(test_audit_action|test_audit_ledger)$' --output-on-failure
```

Technical-writer review, exact-final-diff roundtable approval, and every required pull-request check,
including Windows CMake, are required before merge.
