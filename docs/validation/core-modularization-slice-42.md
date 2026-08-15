# Core modularization slice 42: declare learning ownership

## Scope

This slice declares the `learning` descriptor's `sources`, `private_headers`, `tests`, and `docs`
fields. It does not set `ownership_complete`. It is the declaration half of the declaration-then-latch
pair; the latch, its mutation coverage, and the completeness audit follow in slice 43. It changes
descriptor metadata, the regenerated test-registration baseline, documentation, and cleanup accounting
only; no production code, public symbol, build membership, configuration, storage, or runtime behavior
changes. No header is moved and no include site is rewritten.

`learning` is the second Class B module, after governance. It is the first module in the series with
more than one source and the first whose module-root headers include a de-facto public contract, so
its declaration settles two questions governance did not raise.

## What the module owns

The descriptor now declares what lives under `src/modules/learning/`:

- Four sources: `learning_bundle.c`, `learning_evidence.c`, `learning_implicit.c`,
  `learning_router.c`.
- Four module-root headers: `learning.h`, `learning_bundle.h`, `learning_evidence.h`,
  `learning_implicit.h`. `learning_router.c` has no paired header; its API is declared in `learning.h`.
- Two direct tests: `src/tests/test_learning_bundle.c` and `src/tests/test_learning_metrics.c`.
- `docs/modules/learning.md`.

DB2 persistence for learning (`src/modules/db2/c/db2_learning.h`, `src/modules/db2/c/learning_synth_ops.c`) and the KB
synthesis lane remain outside the module root and are physical-ownership debt the document already
records; the descriptor claims only what is module-local.

## Headers: all private, one a de-facto public contract

No `src/modules/learning/include/aimee/learning/` directory exists, so all four headers are at the
module root and are declared in `private_headers`. `check_module_header_layout.py` constrains only
*declared* `public_headers` — they must live under the canonical include tree — and retired include
spellings; it neither requires a module to declare a public header nor objects to headers at the module
root. The completeness latch treats `public_headers` as an explicit audited claim outside set-equality.
So the private-header declaration is accurate to the layout that exists and is accepted by every check.

`learning.h` is nonetheless the module's public API in practice: it is included repository-wide via the
`-Imodules/learning` search path as `"learning.h"`, including by `src/headers/aimee.h`, `src/modules/db2/c/db2_learning.h`,
`kb/kb_mining.c`, `kb/kb_service.c`, and tests. Declaring it as a `public_header` today would require it
to sit under the canonical include tree, which the layout checker enforces, so that path forces a
file move and roughly six repository-wide include rewrites into this slice. The declare-then-latch split
exists precisely to keep such a refactor out of an ownership-metadata change. The relocation is
therefore deferred to a dedicated header-layout slice, and the de-facto-public status is recorded here
and in the module document so it is visible to a reader of `module.yaml`, who would otherwise see only
the private label. The slice-decision roundtable confirmed this scoping.

## Source liveness and build membership

Every declared source has tracked production consumers:

- `learning_router.c` — the router and proposal API (`learning_router_enabled`,
  `learning_router_record_signal`, `learning_list_proposals`, `learning_accept_proposal`,
  `learning_metrics_commit_ratio`, and the JSON marshalling), called by `src/cmd_learning.c`,
  `src/cmd_rules.c`, `src/modules/db2/c/kb_service_backend_agent.c`, and the config layer.
- `learning_bundle.c` — called by `src/modules/db2/c/artifacts.c`, `src/modules/db2/c/learning_synth_ops.c`,
  `src/kb/kb_learning_synth.c`, `src/kb/kb_learning_version.c`, `src/kb/kb_curator_drain.c`,
  `src/kb/kb_service_workers.c`, `src/modules/config/config_learning.c`, and
  `src/modules/delegates/delegate_prompt.c`.
- `learning_evidence.c` — called by `src/modules/db2/c/kb_service_backend_agent.c` and `src/kb/kb_service_agent.c`.
- `learning_implicit.c` — called by `src/modules/db2/c/kb_service_backend_agent.c`, `src/dogfood.c`, and
  `src/server/openai_chat.c`.

Make's `DATA_SRCS` compiles all four sources and carries the `-Imodules/learning` include path. CMake
compiles `learning_evidence.c`, `learning_implicit.c`, and `learning_router.c` but not
`learning_bundle.c`: `learning_bundle.c`'s callers are entirely server/kb/db2/config-side, so the thin
`aimee` client CMake builds does not pull it into its dependency closure while the other three are
reachable. This is the same intentional thin-client profile boundary recorded for gateway (slice 38)
and the audit CMake asymmetry (slice 34), not source-list drift. The descriptor records canonical
source ownership, which both build systems agree on; it does not claim identical build-product
membership.

## Test membership

Make registers four `unit-test-learning-*` targets. Only two are the module's:

- `test_learning_bundle.c` — the evidence-bundle builder, `learning_bundle.c`. Learning-owned.
- `test_learning_metrics.c` — the router metrics through the public learning API. Learning-owned.

The other two are KB tests carrying the `learning` name: `test_learning_synth.c` exercises
`kb/kb_learning_synth.c` and links `learning_bundle.o` only as a dependency, and
`test_learning_version.c` exercises `kb/kb_learning_version.c`. A shared name prefix is not ownership,
the same criterion applied to gateway's delivery-binary `test_gateway_*` tests. `learning_implicit_replay.c`
is a replay harness rather than a registered `unit-test-learning-*` target and is likewise not claimed.

CTest registers none of the four, consistent with learning being outside the thin-client profile.
`scripts/check_module_test_registration.py` now records the two learning rows (`make: true`,
`ctest: false`); that regeneration is the only reason the baseline file changes.

## Why declare without latching

The latch asserts the descriptor exhaustively covers the module root. That is true today — the module
root holds exactly these four sources and four headers — so the latch would pass. It is deferred anyway
because declaring the files and asserting completeness are distinct claims, and the roundtable required
the completeness audit to review declarations merged on their own first rather than authored in the same
change. The validator accepts a declared-but-unlatched descriptor: it checks each declared path exists
and resolves within the module, and enforces set-equality only when `ownership_complete` is true.

## Regression controls

The declaration is covered by the existing descriptor validation: every declared path must exist and
resolve within the module, and the regenerated test-registration baseline pins the two learning tests'
per-suite registration. The empty-domain guard from slice 39 does not apply, because the module root is
not empty. The latch mutation coverage — source removal, private-header removal, planted files, cleared
latch — is deferred to slice 43, where `ownership_complete` is set and those mutations become
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
make -C src -j2 build/obj/tests/unit-test-learning-bundle build/obj/tests/unit-test-learning-metrics
src/build/obj/tests/unit-test-learning-bundle
src/build/obj/tests/unit-test-learning-metrics
```

Slice 43 sets `ownership_complete: true`, adds the learning latch mutation tests, and records the
completeness audit. Technical-writer review, exact-final-diff roundtable approval, and every required
pull-request check are required before merge.
