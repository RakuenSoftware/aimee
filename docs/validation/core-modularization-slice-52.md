# Core modularization slice 52: declare delegates ownership

## Scope

This slice completes the `delegates` descriptor's `sources` declaration, adding the twenty-four
sources an earlier slice left undeclared, and expands `tests` from two to nineteen. It does not set
`ownership_complete`. It is the declaration half of the declaration-then-latch pair; the latch, its
mutation coverage, and the completeness audit follow in slice 53. It changes descriptor metadata, the
regenerated test-registration baseline, documentation, and cleanup accounting only; no production code,
public symbol, build membership, configuration, storage, or runtime behavior changes. No header is
moved and no include site is rewritten.

`delegates` is the seventh Class B module and the only one with a different shape.

## Why this module is different

Every other Class B module kept all its headers at the module root and declared them private.
`delegates` is the inverse and was already partially declared:

- It declares **twenty-one public headers** under `src/modules/delegates/include/aimee/delegates/`.
The canonical include tree the header-layout checker requires. This slice leaves them unchanged.
- It has **zero module-root headers**, so the `private_headers` completeness domain is empty. The
  descriptor therefore declares no `private_headers` field: an absent field is an empty declared set
  compared against an empty actual set, which is exact by construction. This was verified against the
  validator before writing, a descriptor with all twenty-seven sources, no `private_headers` field,
  and `ownership_complete: true` passes `validate_complete_ownership`, so slice 53's latch does not
  depend on an untested convention.
- It already declared **three of twenty-seven sources** (`aimee_ir_rescue.c`, `panel_provider.c`,
  `panel_roster.c`) and **two tests**. The earlier slice claimed the panel and IR-rescue pieces only.

So the completeness domain here is sources alone, and twenty-four of twenty-seven were undeclared.

## What the module owns

Twenty-seven module-root sources, now all declared: `aimee_ir_rescue.c`, the backend family
(`delegate_backend.c`, `delegate_backend_docker.c`, `delegate_backend_local.c`,
`delegate_backend_ssh.c`), `delegate_checkout.c`, `delegate_credential_retry.c`,
`delegate_credentials.c`, `delegate_depth.c`, `delegate_driver.c`, `delegate_economics.c`,
`delegate_ephemeral_ws.c`, `delegate_launch.c`, `delegate_openai.c`,
`delegate_patch_coordinator.c`, `delegate_plan.c`, `delegate_prompt.c`, `delegate_prompt_stdin.c`,
`delegate_role.c`, `delegate_routing.c`, `delegate_run_phases.c`, `delegate_sandbox_image.c`,
`delegate_source_authority.c`, `delegate_xml_fallback.c`, `gw_orch_delegates.c`, `panel_provider.c`,
and `panel_roster.c`. Zero module-root headers.

Every source is live, reached across the server compute path, the CLI delegate entry points, routing,
and the gateway orchestration seam.

## Build membership

Make compiles all twenty-seven sources. CMake compiles twenty-three, omitting four:
`aimee_ir_rescue.c`, `delegate_ephemeral_ws.c`, `delegate_sandbox_image.c`, and
`gw_orch_delegates.c`. The IR-rescue, ephemeral-workspace, sandbox-image, and gateway-orchestration
units that are server/kb-side. This is the same intentional thin-client profile boundary recorded for
gateway (slice 38), audit (slice 34), learning (slice 42), workspace (slice 44), vault (slice 46),
config (slice 48), and git (slice 50), evidenced by the green thin-client CMake jobs. Notably CMake
reaches far more of this module than of git (12 of 26) or vault (6 of 12), because the thin client
genuinely uses the delegate backend, driver, plan, and role machinery. The descriptor records
canonical source ownership, which both build systems agree on; it does not claim identical
build-product membership.

## Test membership

Classification is by which object each test target links as its subject, read from `src/tests/Rules.mk`
at this slice's base commit. Nineteen tests are declared:

- `test_aimee_ir_rescue.c` and `test_panel_provider.c` (already declared by the earlier slice).
- The backend family: `test_delegate_backend.c`, `test_delegate_backend_docker.c`,
  `test_delegate_backend_local.c`, `test_delegate_backend_ssh.c`.
- Like-named subjects: `test_delegate_credentials.c`, `test_delegate_driver.c`,
  `test_delegate_economics.c`, `test_delegate_ephemeral_ws.c`,
  `test_delegate_patch_coordinator.c`, `test_delegate_plan.c`, `test_delegate_role.c`,
  `test_delegate_sandbox_image.c`, `test_delegate_xml_fallback.c`.
- `test_gw_orch_delegates.c` → `gw_orch_delegates.c`.
- `test_delegate_context_shed.c`, `test_delegate_dispatch_reliability.c`, and
  `test_delegate_handoff.c` each link **only** `modules/delegates/delegate_prompt.o` and no other
  module object. `delegate_prompt.c` is their sole subject, and there is no `test_delegate_prompt.c`,
  so together these three are that source's coverage: context shedding, dispatch reliability, and
  handoff validation.

Six adjacent files are excluded because their subject is another module's source, the same
subject-over-name criterion applied to gateway, learning, workspace, vault, and git:

- `test_delegate_liveness.c` links only `server/liveness.o`, a server test.
- `test_delegate_token_budget.c` links only `server/agent_coord.o`, a server test.
- `test_delegate_ensemble.c` links `modules/roundtable/delegate_ensemble.o` plus four other roundtable
  objects and `server/` objects; it links `panel_roster.o` only as a dependency, a roundtable test.
- `test_gw_orch_workflows.c` and `test_gw_orchestration_seam.c` link no delegates object, a
  workflows test and a pipeline orchestration-seam test.
- `test_panel_ir_contract.c` links no module object and includes `<aimee/ir/panel_result.h>`, an IR
  contract test.

All nineteen declared tests are Make-registered; one, `test_delegate_plan.c`, is additionally
registered as a CTest case, so its row is `make: true, ctest: true` while the other eighteen are
`make: true, ctest: false`. `scripts/check_module_test_registration.py` now records those nineteen
delegates rows; that regeneration is the only reason the baseline file changes. That baseline is also
the drift detector for this classification: if a declared test's registration changes in
`Rules.mk` or `CMakeLists.txt`, the pinned baseline fails CI, so slice 53 cannot latch against a
silently moved partition.

## Why declare without latching

The latch asserts the descriptor exhaustively covers the module root. That is true once these
twenty-four sources are declared (verified directly against the validator, as noted above) so the
latch would pass. It is deferred because declaring the files and asserting completeness are distinct
claims, and the roundtable required the completeness audit to review declarations merged on their own
first rather than authored in the same change.

## Regression controls

The declaration is covered by the existing descriptor validation: every declared path must exist and
resolve within the module, and the regenerated test-registration baseline pins the nineteen delegates
tests' per-suite registration. The empty-domain guard from slice 39 does not apply, because the module
root is not empty. The latch mutation coverage (source removal, planted files, cleared latch) is
deferred to slice 53. There is no private-header removal case for this module because it has no
private headers.

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
make -C src -j2 build/obj/tests/unit-test-delegate-driver build/obj/tests/unit-test-delegate-plan
src/build/obj/tests/unit-test-delegate-driver
src/build/obj/tests/unit-test-delegate-plan
```

Slice 53 sets `ownership_complete: true`, adds the delegates latch mutation tests, and records the
completeness audit. Technical-writer review, exact-final-diff roundtable approval, and every required
pull-request check are required before merge.
