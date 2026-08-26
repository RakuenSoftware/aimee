# Core modularization slice 38: complete gateway ownership

## Scope

This slice marks the required `gateway` descriptor `ownership_complete: true`. Earlier slices moved
the canonical request-pipeline seam, the tool-policing policy, and the delegate-loop entry point into
`src/modules/gateway` and established the canonical public headers. This slice proves that the
descriptor exhaustively covers the validator's module-local C and private-header domain, requires the
canonical document, and records the audited public headers, direct tests, build and test membership,
adjacent surfaces, and export liveness. It changes metadata, validation, documentation, and cleanup
accounting only; no production code, public symbol, build membership, configuration, storage, or
runtime behavior changes.

## Ownership domain

The completeness domain is every module-local file whose suffix matches the `sources` or
`private_headers` role, excluding `module.yaml` and everything under
`src/modules/gateway/include/aimee/gateway/`. The module root contains exactly `gateway_delegate.c`,
`gateway_pipeline.c`, `gateway_policy.c`, `module.yaml`, and the three canonical public headers.
There are no private headers, so the declared source set already equals the actual set and the latch
is exact on the checked-in tree. The descriptor's `docs` field equals `["docs/modules/gateway.md"]`,
as the latch requires.

Completeness is a file-ownership statement. It does not assert that every owned export has a caller,
that every declared test runs in every build system, or that the public-header inventory is
auto-verified; public headers and tests remain explicit audited claims. In particular, it is not a
cross-build-system completeness claim.

## Build membership

Membership is asymmetric, and the asymmetry is policy rather than drift:

- Make compiles all three sources. They appear in `DATA_SRCS` in `src/Makefile`, and `C_FLAGS`
  carries `-Imodules/gateway/include`.
- No CMake target compiles any of them. CMake sets `AIMEE_GATEWAY_INCLUDE_DIR` and adds it to several
  targets' include paths, but an exact search for the three filenames across every CMake file in the
  repository returns nothing.

`CMakeLists.txt` states the reason directly: CMake builds only the `aimee` thin client plus the
unit-test suite, and the `aimee-server`, `aimee-kb`, `aimee-gateway`, and `aimee-webchat` targets were
removed because CMake's source lists are not maintained for the full server build. They had drifted
roughly 188 files behind `src/Makefile`, and no CI gate ever built them. Gateway policy is a
server-side concern, so its absence from the thin-client profile follows from that documented scope.
This is therefore recorded as an intentional profile boundary, not as ordinary source-list drift, and
it is not a blocker for the ownership latch: the validator's domain is the module root on disk, not
whichever profile a build selects.

One structural consequence is worth recording, and is an inference rather than a reproduced result.
CMake's `aimee-agent` static library includes `src/posix/agent_runtime.c`, which calls
`gateway_delegate_run_request_pipeline` and `gateway_delegate_tool_shape`, symbols no CMake target
compiles. A static archive may contain unresolved references, so this is not in itself a build
failure, and CI is green, which shows that no currently registered CTest binary links a consumer that
pulls that object and leaves those symbols unresolved. A future test that does pull it would fail to
link unless another linked object or library supplies the gateway symbols. `cmake` is unavailable in
the environment used for this slice, so this was derived from the source lists plus green CI and was
not reproduced locally. The gap is a latent build-boundary one, distinct from the intentional
profile boundary above; the two should not be collapsed into "CMake absence is harmless".

## Source liveness and ownership

Exports with tracked production consumers:

- `gw_pipeline_run_request`: `src/server/anthropic_http.c`, `src/server/openai_chat.c`.
- `gateway_delegate_run_request_pipeline` and `gateway_delegate_tool_shape`,
`src/posix/agent_runtime.c`.
- `gateway_policy_apply_request`: `src/server/anthropic_http.c`, `src/server/openai_chat.c`, and
  module-locally from `gateway_delegate.c`.
- `gateway_policy_pin_model`: `src/server/anthropic_http.c`.
- `gateway_prevent_subagents_enabled`: `src/server/anthropic_http.c`, plus module-local callers in
  `gateway_policy.c` and `gateway_delegate.c`.
- `gateway_policy_set_delegates_available_provider`: `src/server/server_compute.c`.
- `gateway_policy_police_parsed_response`: `src/modules/governance/gw_stage_governance.c` and
  `src/posix/agent_runtime.c`.
- `gateway_policy_strip_tools`: called module-locally from `gateway_policy.c`; externally only by
  `src/tests/test_gateway_policy.c`.

The single export with no tracked-tree caller outside tests:

- `gateway_policy_is_denied_tool`: its only tracked caller is `src/tests/test_gateway_policy.c`. It
  is a small predicate over the same gate `gateway_policy_strip_tools` applies, so it is a
  privatization or deletion candidate rather than a missing-feature signal. It remains a declared
  public export, so absence of a tracked-tree caller is liveness evidence, not proof that no
  downstream consumer links it.

Two further observations:

- `src/tests/test_anthropic_http.c` defines its own `gateway_policy_pin_model`. That test binary
  therefore links a local stub instead of `gateway_policy.c`, and its passing does not exercise the
  module's implementation. This is deliberate test isolation, recorded so a future reader does not
  count it as coverage.
- `.claude/hooks/block_subagent.py` and its README reference `gateway_policy_strip_tools` by its
  pre-move path `src/gateway_policy.c`. These are developer-tooling comments, not build inputs; the
  stale path is noted for a later tooling pass.

Whole-tree tracked-file and symbol searches find no second request-pipeline runner, tool-policing
implementation, or delegate-loop pipeline entry point.

## Adjacent surfaces

`src/gateway/` is the `aimee-gateway` delivery binary: `gateway_main.c`, `gateway_ctx.c`,
`delivery_router.c`, `platform_telegram.c`, `platform_ntfy.c`, `platform_webhook.c`, `pairing.c`,
`session_key.c`, `stt.c`, and `tts.c`. It is a distinct ownership surface from this module despite the
shared gateway name; `GATEWAY_SRCS` in `src/Makefile` refers to that directory. No descriptor owns it, which is broader
layer-ownership debt of the same kind recorded for `cmd_plugin.c` in slice 36, and it does not weaken
this latch. The gateway-mutation family remains owned by economizer under
`src/modules/economizer/gateway_mutate*.c`.

Consumers that live outside the module root, `src/server/anthropic_http.c`,
`src/server/openai_chat.c`, `src/posix/agent_runtime.c`, `src/server/server_compute.c`, and
`src/modules/governance/gw_stage_governance.c`, are callers, not implementation, and are outside the
completeness domain.

## Test membership

The three declared tests are `src/tests/test_gateway_pipeline.c`, `src/tests/test_gateway_policy.c`,
and `src/tests/test_gateway_p4_delegate.c`. Make registers all three as `unit-test-gateway-pipeline`,
`unit-test-gateway-policy`, and `unit-test-gateway-p4-delegate` in `src/tests/Rules.mk`. CTest
registers none of them. That is an audited current condition rather than a derived one: it is
consistent with the module sitting outside CMake's thin-client profile, but CMake does own a
unit-test suite, so exclusion from the production profile does not by itself establish that these
tests belong outside that suite. No CMake policy statement covers the question either way.

`scripts/check_module_test_registration.py`, added by slice 37, pins that per-test registration to
`tests/baselines/refactor/module-test-registration.json`, so any change to it surfaces as a reviewed
baseline diff.

The remaining `unit-test-gateway*` targets, `test_gateway.c`, `test_gateway_telegram.c`,
`test_gateway_ntfy_webhook.c`, `test_gateway_stt_pairing.c`, `test_gateway_mutate.c`, and
`test_gateway_mutate_wire.c`, exercise the `src/gateway` delivery binary or the economizer mutation
family, not this module, and are correctly unclaimed. A shared filename prefix is not ownership.

## Regression controls

The descriptor mutation suite removes `gateway_delegate.c`, `gateway_pipeline.c`, and
`gateway_policy.c` individually; each omission must fail `rule=ownership-complete` on `/sources`. It
plants `src/modules/gateway/undeclared.c` and `undeclared.h`, which must fail the same rule on
`/sources` and `/private_headers`. It removes `docs/modules/gateway.md` from the descriptor's `docs`
field, which must fail that rule on `/docs`. The latched-descriptor assertion introduced in slice 35
now also covers `gateway`, so silently clearing the latch fails directly. The unmodified descriptor
graph must pass. These controls cover the module-local source and private-header domain and the
canonical document; they do not police the public-header inventory, which the validator checks only
as declared paths, and they are independent of any build profile.

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
python3 scripts/check_module_test_registration.py
python3 -m unittest scripts.tests.test_check_module_test_registration
python3 -m unittest scripts.tests.test_check_module_docs \
  scripts.tests.test_check_cleanup_ledger scripts.tests.test_refactor_baselines
python3 scripts/refactor_baselines.py check
make -C src -j2
make -C src lint
make -C src -j2 build/obj/tests/unit-test-gateway-pipeline \
  build/obj/tests/unit-test-gateway-policy build/obj/tests/unit-test-gateway-p4-delegate
src/build/obj/tests/unit-test-gateway-pipeline
src/build/obj/tests/unit-test-gateway-policy
src/build/obj/tests/unit-test-gateway-p4-delegate
```

`cmake` is unavailable in the environment used for this slice. There is no CMake target that compiles
this module's sources and no CTest case that runs its tests, so there is no module-specific CMake
command to add; the required pull-request CMake jobs continue to cover the thin-client profile they
own.

Technical-writer review, exact-final-diff roundtable approval, and every required pull-request check,
including Windows CMake, are required before merge.
