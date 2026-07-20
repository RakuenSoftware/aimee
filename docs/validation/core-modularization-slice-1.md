# Core modularization slice 1: canonical inventory bootstrap

## Decision

Slice 1 implements only the pre-child taxonomy gate from acceptance id 8 of
`module-runtime-source-ownership-and-build.md`. The canonical inventory is the sole build/runtime
list of module IDs and classifications. Its checker validates structure, counts, uniqueness,
disjointness, ID syntax, and Git's required classification without reading source directories,
descriptors, profiles, or proposal state.

The existing `src/modules/git/` directory predates this effort. It is historical baseline, not
evidence that the Git migration slice has begun and not a descriptor or readiness claim. Binding
Git source to the new module contract is re-decided only after `git-core-contract.md` is accepted.

## Included

- `tests/baselines/modules/canonical-inventory.yaml`
- a CWD-independent, safe-JSON parser and shell entrypoint
- mutation-focused failure tests without a second hard-coded module list
- the `module-inventory` pull-request workflow for `feature/core-modularization`

## Deferred

- `git-core-contract.md` and `check_proposal_ordering.sh`
- module descriptors, descriptor schema, runtime loading, and generated profiles
- descriptor/profile equality and readiness checks
- Git source changes or memory ingest-boundary changes
- Make/CMake integration and source relocation

The dedicated proposal-ordering checker is the next governance prerequisite and must land before
any Git descriptor, build/profile registration, readiness, or source-migration slice. Branch
protection for the `module-inventory` job is an external repository-administration step; this
program independently refuses to merge a slice until the job is green.
