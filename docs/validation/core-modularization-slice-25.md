# Core modularization slice 25: canonical skills ownership

## Scope

This slice starts from `65a7d6b8b9d72ef88aec69e64de11cadace81bf5` and resolves the documented
singular/plural ownership mismatch for required core `skills`. It moves three implementation files and
two public headers without changing production logic, public symbols, target membership, runtime
behavior, configuration, routes, storage, filesystem data, or the external `SKILL.md` convention.

The physical moves are:

- `src/modules/skill/skill.c` to `src/modules/skills/skill.c`
- `src/modules/skill/skill_review.c` to `src/modules/skills/skill_review.c`
- `src/modules/skill/skill_rollback.c` to `src/modules/skills/skill_rollback.c`
- `src/modules/skill/skill.h` to
  `src/modules/skills/include/aimee/skills/skill.h`
- `src/modules/skill/skill_review.h` to
  `src/modules/skills/include/aimee/skills/skill_review.h`

All consumers now include `<aimee/skills/skill.h>` or
`<aimee/skills/skill_review.h>`. There are no forwarding headers or relative compatibility includes.
The `skill_*` ABI and all CLI/API/config/storage behavior remain unchanged.

## Build and descriptor ownership

Make replaces its three `modules/skill/` sources and `-Imodules/skill` root with the plural source
paths and `-Imodules/skills/include`. CMake replaces the same source paths and every former singular
include-root exposure with `${AIMEE_SRC_DIR}/modules/skills/include`. `src/tests/Rules.mk` changes only
the corresponding object paths. Target names, source membership, link membership, and visibility are
unchanged.

`src/modules/skills/module.yaml` declares exactly three sources, two canonical public headers,
`test_skill.c`, `test_skill_review.c`, and `docs/modules/skills.md`. `test_skill.c` directly covers the
resolver, management, injection, lifecycle, rollback, and telemetry implementation; the focused review
test covers `skill_review.c` and management poison checks. No separate rollback executable exists;
`skill_rollback.c` is linked into the focused skill targets.

## Reusable regression boundaries

The existing ownership gates retain separate responsibilities:

- descriptor validation checks canonical file existence, role boundaries, and duplicate claims;
- the descriptor-derived header-layout check rejects flat shadows, bare includes, and source-root
  include exposure;
- `validate_legacy_module_root` in the existing source-ownership checker rejects the retired
  `src/modules/skill` directory and exact `modules/skill/` references in the three build inventories.

The generic legacy-root helper takes the module, legacy root, and build-file set as parameters so later
path-only module moves can reuse it without another checker.

## Retired background curator

The historical removal disposition remains byte-identical and continues to record the paths that were
deleted at the time:

- `src/modules/skill/skill_curator.c`
- `src/modules/skill/skill_curator.h`

The absence checker additionally rejects resurrection at the canonical paths that the retired feature
would use after this move:

- `src/modules/skills/skill_curator.c`
- `src/modules/skills/include/aimee/skills/skill_curator.h`

These are separate from the five live skills files. Focused mutations create an empty file at each new
retired path and require the existing `deleted-file` failure rule.

## Why skills precedes benchmarks

Skills is a required-core, five-file path mismatch with direct focused tests and no profile-selection
change. The alternative `src/modules/agent_eval` move spans 4,321 implementation lines and also has to
resolve the documented contradiction between an optional `benchmarks` descriptor and unconditional
build/runtime surfaces. That larger optionality slice remains deferred; this slice does not imply that
`agent_eval` is dead or already isolated.

## Verification

- moved-file byte comparison, permitting only the three canonical include substitutions
- zero retired live paths, forwarding headers, or bare/relative skill-header includes
- zero exact `modules/skill/` references in `CMakeLists.txt`, `src/Makefile`, or `src/tests/Rules.mk`
- byte-identical `docs/audit/dispositions/background-skill-curator.yaml`
- descriptor, header-layout, source-ownership, background-curator-absence, documentation, cleanup, and
  refactor-baseline checkers and their focused failure-mode suites
- build and execute `unit-test-skill` and `unit-test-skill-review`
- `make -C src lint`

Technical-writer review, exact-final-diff roundtable approval, and all 22 pull-request checks are
mandatory before squash merge into `feature/core-modularization`.
