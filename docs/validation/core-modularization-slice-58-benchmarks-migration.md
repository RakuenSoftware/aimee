# Core modularization slice 58: migrate and latch `benchmarks` (first Class A migration)

## Scope

This slice migrates the first Class A module, `benchmarks`, from an empty descriptor to a declared and
latched module by relocating the cohesive eval/benchmark framework that lived in the non-descriptor
`src/modules/agent_eval/` directory into `src/modules/benchmarks/`. It moves four sources and two
headers, repoints the build systems, declares the descriptor's `sources`/`private_headers`/`docs`, and
sets `ownership_complete: true`. No production symbol, behaviour, storage, or configuration changes; the
`agent_eval_` API prefix and all function signatures are unchanged.

## Decision and authority

Two decompositions were possible: (A) rename the descriptor id `benchmarks` -> `agent_eval` and drop a
descriptor into the existing directory (zero file moves), or (B) move the directory into
`src/modules/benchmarks/`. The roundtable-approved architecture proposal
`docs/proposals/pending/memory-learning-and-inference-boundaries.md` is decisive: "Offline benchmark
runners, datasets, ablations, and regression suites belong to optional `benchmarks`. … There is no
mixed `agent-eval` module," and its binding check #6 runs
`check_module_names.sh --forbid memory-tier-b,agent-eval,evals,bare-synthesis`. The parent taxonomy
(`core-substrate-and-source-module-boundaries.md`) records that optional `evals` was renamed to
`benchmarks`. `agent-eval`/`evals` are therefore forbidden names; **Option B is mandated**. The
independent review (MiniMax-M3, delegate job 12215) reached Option A on blast-radius grounds but
flagged this exact proposal as the blocker; reading it resolves the direction to B.

## What moved

`git mv` relocated, preserving history:

- Sources: `agent_eval.c` (shared eval machinery), `agent_eval_baseline.c` (regression baselines),
  `agent_eval_benchmarks.c` (LoCoMo/LongMemEval runners), `agent_eval_memory_support.c` (memory-eval
  support).
- Headers: `agent_eval.h` (public contract, 16 external includers), `agent_eval_internal.h` (private
  seam shared across the four sources).

All four sources share `agent_eval_internal.h`; `agent_eval_benchmarks.c` depends on the machinery in
`agent_eval.c`, so the family is one indivisible module. `agent_eval_memory_support.c` is eval support
built into the module, not a memory test fixture, so it stays with `benchmarks` (the proposal's
"memory quality fixtures belong to memory test support" concerns test-tree fixtures, not this module
source).

## Build repointing

`-Imodules/agent_eval` -> `-Imodules/benchmarks` in `src/Makefile`; the four source paths and object
paths in `AGENT_SRCS`, the `../aimee-negation-eval` kb target, `src/tests/Rules.mk` link lists, and the
six `${AIMEE_SRC_DIR}/modules/agent_eval` references in `CMakeLists.txt` (source list + five
`target_include_directories`) all repoint to `modules/benchmarks`. The 16 external `#include
"agent_eval.h"` sites are unchanged — the flat include resolves through the swapped `-I` root, matching
the flat-layout convention used by `config`, `learning`, and the other latched modules whose module-root
headers are declared `private_headers`.

## Ownership domain

The module root `src/modules/benchmarks/` contains exactly the four declared sources and two declared
private headers (plus `module.yaml`), so the declared sets equal the actual sets and the latch is exact.
`docs` equals `["docs/modules/benchmarks.md"]`. The empty-domain guard from slice 39 no longer applies
because the root is no longer empty. `tests` is empty: the eval framework has no dedicated test. It is
a harness exercised indirectly by memory/server tests (e.g. `test_memory_retrieval_eval.c`, already
claimed by `memory` in slice 56) whose subjects are those modules, not the framework; the
test-registration baseline records no benchmarks test.

## Verification

Run from the repository root; each must exit zero:

```sh
python3 scripts/validate_module_descriptors.py --check-schema src/modules
python3 scripts/check_module_docs.py
python3 scripts/check_module_source_ownership.py
python3 scripts/check_module_header_layout.py
python3 scripts/check_module_test_registration.py
python3 scripts/check_module_inventory.py
python3 scripts/check_cleanup_ledger.py
python3 scripts/refactor_baselines.py check
make -C src ../aimee-server        # links the four relocated objects via AGENT_SRCS
```

Validated on the .253 aimee-test container: full Make build and the full CMake configure/build (the
relocated `agent_eval` sources compile at `modules/benchmarks/` and link into `aimee-server`; the 16
external includers resolve through `-Imodules/benchmarks`). Independent review (MiniMax-M3) and every
required pull-request check are required before merge.

## Where the programme stands

Nineteen of twenty-six descriptors are latched. Seven Class A modules remain: `control-web`,
`execution-policy`, `kb-synthesis`, `response-composition`, `routing`, `runtime-web`, `tools`, tracked
in `docs/validation/core-modularization-class-a-migration.md` with per-module decomposition in
`core-modularization-class-a-decomposition.md`.
