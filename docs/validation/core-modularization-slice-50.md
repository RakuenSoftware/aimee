# Core modularization slice 50: declare git ownership

## Scope

This slice declares the `git` descriptor's `sources`, `private_headers`, `tests`, and `docs` fields.
It does not set `ownership_complete`. It is the declaration half of the declaration-then-latch pair;
the latch, its mutation coverage, and the completeness audit follow in slice 51. It changes descriptor
metadata, the regenerated test-registration baseline, documentation, and cleanup accounting only; no
production code, public symbol, build membership, configuration, storage, or runtime behavior changes.
No header is moved, no include site is rewritten, and neither the `git-core-contract` proposal nor its
approval evidence is touched.

`git` is the sixth Class B module and the largest by file count so far: twenty-six sources, eighteen
module-root headers, and fourteen direct tests carved out of sixteen `test_git*`/`test_forge*` files.

## The git-core-contract is orthogonal

`git` has a dedicated governance mechanism, `docs/proposals/done/git-core-contract.md` with
roundtable-approval evidence in `docs/validation/roundtable/git-core-contract.json`, enforced by
`check_git_core_contract.py` in the `git-core-contract` CI job. That contract bounds git's *core
capability*, what git may and may not do as a submit-only module against memory's
`repository-record.ingest.v1` contract. It is not the descriptor's file-ownership latch. The check
reads `module.yaml` only to validate the contract, and this ownership-declaration slice does not touch
the proposal or its evidence. `check_git_core_contract.py --require-status roundtable-approved` passes
both before and after this declaration.

## What the module owns

The module root `src/modules/git/` contains, excluding `module.yaml`, twenty-six sources and eighteen
headers. No `src/modules/git/include/aimee/git/` directory exists, so every header is at the module
root and is declared in `private_headers`.

- Sources: the forge-credential broker (`forge_credentials.c`), credential injection
  (`git_cred_inject.c`), the forge vault (`git_forge_vault.c`), host-cred and host-resolve
  (`git_host_cred.c`, `git_host_resolve.c`), the three OAuth backends (`git_oauth_device.c`,
  `git_oauth_gh.c`, `git_oauth_github.c`), git ops (`git_ops.c`), org-repos (`git_org_repos.c`), the
  PR API and CI grader (`git_pr_api.c`, `git_pr_ci_grade.c`), project (`git_project.c`), ssh-agent
  (`git_ssh_agent.c`), the eight-file verify family (`git_verify.c`, `git_verify_config.c`,
  `git_verify_hook.c`, `git_verify_jobs.c`, `git_verify_ops.c`, `git_verify_select.c`,
  `git_verify_state.c`, `git_verify_step.c`), and the four MCP git tools (`mcp_git_branch.c`,
  `mcp_git_pr.c`, `mcp_git_query.c`, `mcp_git_write.c`).
- Headers: sixteen pair with a like-named source. Two have no paired source, `git_verify_internal.h`
  (the verify-family seam) and `mcp_git.h` (the shared MCP-git header). The sources without a paired
  header (`git_pr_ci_grade.c`, the `git_verify_config/hook/ops/state/step.c` family, and the four
  `mcp_git_*.c`) declare through `git_verify.h`, `git_verify_internal.h`, and `mcp_git.h`.

Every source is live, reached across the server, kb, cmd, and MCP layers by the forge-credential,
credential-injection, OAuth, git-operation, org-repo, PR, verify, and MCP-git-tool paths.

## Build membership

Make's `DATA_SRCS` compiles all twenty-six sources and carries the `-Imodules/git` include path. CMake
compiles twelve: the eight `git_verify_*` sources and the four `mcp_git_*` tools, the git verify
surface and the MCP git tools the thin `aimee` client reaches. It omits the fourteen credential, OAuth,
ops, forge-vault, host, org-repos, and PR-API sources, which are the server/kb-side git machinery. The
required Windows and Linux CMake jobs build the thin client green from the twelve-source set. The
standing evidence that this is an intentional thin-client profile boundary, the same one recorded for
gateway (slice 38), audit (slice 34), learning (slice 42), workspace (slice 44), vault (slice 46), and
config (slice 48), not source-list drift. The descriptor records canonical source ownership, which
both build systems agree on; it does not claim identical build-product membership.

## Test membership

Fourteen tests are declared, each driving a git-module source. Eleven run under Make `unit-test-*`
targets: `test_forge_credentials.c`, `test_git_cred_inject.c`, `test_git_forge_vault.c`,
`test_git_host_resolve.c`, `test_git_ops.c`, `test_git_pr_ci_grade.c`, `test_git_project.c`,
`test_git_ssh_agent.c`, `test_git_verify_contract.c`, `test_git_verify_select.c`, and
`test_mcp_git.c`.

Three are CTest-only, registered in `src/tests/CMakeLists.txt` but built by no Make `unit-test-*`
target: `test_git_oauth_device.c`, `test_git_oauth_gh.c`, and `test_git_org_repos.c`. Their subjects
(`git_oauth_device.c`, `git_oauth_gh.c`, `git_org_repos.c`) are git-module sources, so they are
declared; their registration rows are `make: false, ctest: true`, the inverse of the usual pattern and
the same asymmetry audit records for `test_audit_action` and `test_audit_ledger`. Classification is by
subject membership in the module, not by which build system drives the test.

Two adjacent tests are excluded:

- `test_forge_app_token.c` exercises `src/forge_app_token.c`, a root-level source that is not a
  git-module file. `forge_credentials.c`. The git-module forge-credential broker, tested by
  `test_forge_credentials.c`, is a different file. A shared `forge` name prefix is not ownership.
- `test_forge_credentials_live.c` backs the `forge-cred-live` integration harness, which requires
  `AIMEE_TEST_FORGE_REPO`, `AIMEE_TEST_FORGE_TOKEN`, and a running forge. Its subject
  `forge_credentials.c` already has a unit test (`test_forge_credentials.c`), so unlike vault's tpm2
  harness (claimed because it was the *only* exerciser of its source) this harness is supplementary
  integration coverage, not sole coverage, and is not claimed.

`scripts/check_module_test_registration.py` now records fourteen git rows, eleven `make: true,
ctest: false` and three `make: false, ctest: true`; that regeneration is the only reason the baseline
file changes.

## Why declare without latching

The latch asserts the descriptor exhaustively covers the module root. That is true today. The module
root holds exactly these twenty-six sources and eighteen headers, so the latch would pass. It is
deferred because declaring the files and asserting completeness are distinct claims, and the roundtable
required the completeness audit to review declarations merged on their own first rather than authored
in the same change. The validator accepts a declared-but-unlatched descriptor: it checks each declared
path exists and resolves within the module, and enforces set-equality only when `ownership_complete`
is true.

## Regression controls

The declaration is covered by the existing descriptor validation: every declared path must exist and
resolve within the module, and the regenerated test-registration baseline pins the fourteen git tests'
per-suite registration, including the three CTest-only rows. The empty-domain guard from slice 39 does
not apply, because the module root is not empty. The latch mutation coverage, source removal,
private-header removal, planted files, cleared latch, is deferred to slice 51, where
`ownership_complete` is set and those mutations become meaningful.

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
python3 scripts/check_git_core_contract.py --require-status roundtable-approved
python3 scripts/check_module_header_layout.py
python3 scripts/refactor_baselines.py check
make -C src -j2
make -C src lint
make -C src -j2 build/obj/tests/unit-test-git-ops build/obj/tests/unit-test-mcp-git
src/build/obj/tests/unit-test-git-ops
src/build/obj/tests/unit-test-mcp-git
```

The three CTest-only git tests build under a CMake-capable environment, which is unavailable here; the
required pull-request CMake jobs cover them and build the thin client from the twelve-source subset.

Slice 51 sets `ownership_complete: true`, adds the git latch mutation tests, and records the
completeness audit. Technical-writer review, exact-final-diff roundtable approval, and every required
pull-request check are required before merge.
