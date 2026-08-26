# Core modularization slice 24: audit test ownership

## Scope

This slice starts from `913fbdbcf94e8defc45df2d220582fe95846e6d5` and resolves issue #1753 by
assigning one canonical owner to the tests that directly exercise the required `audit` module. It
changes ownership metadata and documentation only. No production source, public header, test source,
build registration, runtime behavior, storage behavior, configuration, route, schema, or module
dependency changes.

`src/modules/audit/module.yaml` now claims exactly these four existing tests:

- `src/tests/test_audit_action.c`
- `src/tests/test_audit_ledger.c`
- `src/tests/test_audit_worm.c`
- `src/tests/test_audit_worm_chain.c`

The first two directly exercise `audit_action.c` and `audit_ledger.c`. The WORM-chain test pins the
engine-independent serialization, hash, and checkpoint-MAC primitives, while the WORM test exercises
the SQLite store's append, read, checkpoint, seal, tamper, paging, and verification behavior. The
store consumes the chain, but the two tests protect distinct contracts.

## Complete filename audit

The on-disk inventory command

```text
find src/tests -maxdepth 1 -type f -name '*audit*.c' -print | sort
```

returned these 14 files, all represented in `docs/modules/audit.md`:

1. `src/tests/test_audit_action.c`: audit-owned action primitive
2. `src/tests/test_audit_action_log.c`: core log writer
3. `src/tests/test_audit_ledger.c`: audit-owned ledger reader
4. `src/tests/test_audit_worm.c`: audit-owned SQLite WORM store
5. `src/tests/test_audit_worm_chain.c`: audit-owned shared chain primitive
6. `src/tests/test_code_audit.c`: independent CLI code-audit feature
7. `src/tests/test_code_audit_graph.c`: independent code-audit graph feature
8. `src/tests/test_db2_code_audit.c`: DB2 code-audit assembly
9. `src/tests/test_harness_memory_audit.c`: memory interception logging
10. `src/tests/test_kb_audit_worm.c`: mixed KB/store integration
11. `src/tests/test_kb_audit_worm_pg.c`: mixed KB/PostgreSQL integration
12. `src/tests/test_token_audit.c`: DB1 token accounting and agent ingress
13. `src/tests/test_token_audit_load.c`: DB1 token/ingress concurrency
14. `src/tests/test_vault_audit.c`: vault/server/log integration

Only items 1, 3, 4, and 5 are assigned to the audit descriptor. The other filenames use “audit” in a
different subsystem or cross a storage/runtime integration boundary; they are not dual-claimed.

## Existing enforcement and build evidence

No new checker is needed. `scripts/validate_module_descriptors.py` already requires each test claim to
be a normalized, repository-relative, existing regular file below `src/tests`, with the `test_`
convention and an allowed test extension. It also rejects duplicates within a descriptor, cross-role
claims, and duplicate ownership across descriptors. Its existing failure-mode suite covers those
rules.

Build registration is unchanged:

- `src/tests/CMakeLists.txt` registers `test_audit_action` and `test_audit_ledger` through
  `aimee_add_test`.
- `src/tests/Rules.mk` registers `unit-test-audit-worm` and `unit-test-audit-worm-chain`, and both are
  members of the Make `unit-tests` set.

The local environment does not provide `cmake` or `ctest`. The action and ledger executables therefore
remain mandatory CMake/CTest pull-request checks rather than claimed local results. The two focused
Make targets are run locally and the full Make and CMake suites remain pull-request gates.

## Aimee code-index observation

Code intelligence was attempted before using build and source evidence:

```text
$ aimee index scan --force
indexing project: aimee
ingested 3443 file(s) from /home/virant/dev/aimee (61 batches)
$ aimee index overview
No indexed projects.
$ aimee index find audit_worm_row_hash
No matches.
```

The scan resolved the linked checkout rather than this isolated worktree and did not expose a project
or symbols afterward. These results mean index evidence was unavailable; they are not evidence that
the audit APIs or tests are dead. Direct calls, compile/link inputs, runtime boundaries, and storage
boundaries supplied the ownership evidence for this slice.

## Completed local verification

- `python3 -I -S scripts/validate_module_descriptors.py --check-schema src/modules`
- `python3 -I -S scripts/validate_module_descriptors.py --emit-ownership src/modules`
- `python3 -I scripts/tests/test_validate_module_descriptors.py -v`
- Make targets and binaries for `unit-test-audit-worm` and `unit-test-audit-worm-chain`
- `python3 -I -S scripts/check_cleanup_ledger.py` and its nine failure-mode tests
- `python3 -I -S scripts/refactor_baselines.py` and its seven failure-mode tests
- the existing module-documentation, module-header-layout, and source-ownership checks
- `git diff --name-status 913fbdbcf94e8defc45df2d220582fe95846e6d5`

The descriptor validator passed for all 26 descriptors, and all 32 descriptor failure-mode tests
passed. Its ownership report contains exactly the four audit test paths. Both focused Make binaries
built and passed. The 14-file inventory matched the 14 documentation rows. The cleanup-ledger and
refactor-baseline checks passed without drift, so no baseline freeze or index update was required.
The staged name-status diff contains only the audit descriptor, audit documentation, this validation
record, and cleanup-ledger metadata.

Technical-writer job 7234 approved the four-file artifact without edits. Close issue #1753 only after
exact-final-diff roundtable approval, all 22 pull-request checks pass, and the slice merges.
