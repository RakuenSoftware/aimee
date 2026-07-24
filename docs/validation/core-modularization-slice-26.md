# Core modularization slice 26: canonical gateway core ownership

## Scope

This slice starts from `6d72e16119a0bb338aec56e4faad7db6b4042b3f` and moves the required-core
gateway orchestration seam into its canonical module without changing behavior:

- `src/gateway_pipeline.c` to `src/modules/gateway/gateway_pipeline.c`
- `src/gateway_policy.c` to `src/modules/gateway/gateway_policy.c`
- `src/gateway_delegate.c` to `src/modules/gateway/gateway_delegate.c`
- the three matching headers from `src/headers/` to
  `src/modules/gateway/include/aimee/gateway`

Consumers use `<aimee/gateway/gateway_pipeline.h>`,
`<aimee/gateway/gateway_policy.h>`, or `<aimee/gateway/gateway_delegate.h>`.
There are no forwarding headers or alternate implementations.

## Boundary

This is not a wholesale move of `src/gateway`. That tree still combines gateway context, pairing, and
session-key lifecycle, which require further audit, alongside the optional Telegram, webhook, ntfy,
STT/TTS, and delivery providers. Moving it here would collapse the required ingress seam and optional
delivery implementations.
The economizer-owned gateway mutation family also remains outside gateway ownership.

## Build and ownership

The Make build changes only the three source paths and adds `modules/gateway/include` as a public-header
root. Rules.mk changes only the corresponding object paths. CMake target membership remains unchanged
because it did not list the three implementation files before this slice. The CMake target names
`aimee-core`, `aimee-data`, `aimee-agent`, `aimee-git`, and `aimee-cmd` retain PUBLIC visibility for
shared header roots, while the CMake `aimee` executable target retains PRIVATE visibility; each receives
the canonical gateway include root.
All production translation units that include `router_advise.h` compile through those targets. CMake test
targets link `aimee-core`, whose PUBLIC include roots propagate the gateway contract. The ownership gate
also requires the three gateway implementation basenames to remain absent from CMake source membership.

`src/modules/gateway/module.yaml` owns exactly the three moved sources, three public headers,
`src/tests/test_gateway_pipeline.c`, `src/tests/test_gateway_policy.c`,
`src/tests/test_gateway_p4_delegate.c`, and `docs/modules/gateway.md`. Mixed ingress, response-governance,
stage-registry, and platform tests remain with their existing boundaries.

## Regression controls

The existing source-ownership registry now has three data entries for gateway. It rejects each legacy
source/header path, requires its canonical replacement, checks the Make source and focused-test object,
requires the corresponding flat object to be absent throughout Rules.mk, preserves CMake source absence,
requires canonical includes in representative consumers, and requires the module document to describe
the contract. The descriptor-derived header-layout gate rejects flat-header shadows, basename includes,
and source-root include exposure across every consumer.

## Compatibility

The moved files are byte-identical except for canonical include substitutions. Public symbols, prototypes,
target membership, link visibility, request and response mutation, stage order, configuration, storage,
and runtime behavior are unchanged.

## Public-surface baseline

The pre-slice refactor baseline in `tests/baselines/refactor/index.json` enumerated only
`src/headers/*.h`. Freezing after a canonical module move
would therefore remove the moved API from public-surface coverage. The existing baseline now also includes
`src/modules/*/include/**/*.h`, bringing the already-canonical audit and skills contracts and the new
gateway contracts under the same content and membership hash.
`scripts/tests/test_refactor_baselines.py` pins representative root, audit, skills, and gateway headers so
narrowing the surface fails CI.
The gateway entries are the three `public_headers` paths in `src/modules/gateway/module.yaml`, the
canonical ownership inventory.

## Verification

- moved-file comparison permitting only canonical include substitutions
- descriptor, header-layout, source-ownership, documentation, cleanup-ledger, and expanded
  public-surface/refactor-baseline gates
  plus their focused mutation suites
- build and execute `unit-test-gateway-pipeline` (`src/tests/test_gateway_pipeline.c`),
  `unit-test-gateway-policy` (`src/tests/test_gateway_policy.c`), and
  `unit-test-gateway-p4-delegate` (`src/tests/test_gateway_p4_delegate.c`)
- `make -C src lint`
- technical-writer review, exact-final-diff roundtable approval, and all 22 pull-request checks before merge
