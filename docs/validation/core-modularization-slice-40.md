# Core modularization slice 40: declare governance ownership

## Scope

This slice declares the `governance` descriptor's `sources`, `private_headers`, `tests`, and `docs`
fields. It does not set `ownership_complete`. It is the first of the declaration-then-latch pair the
scoping roundtable required for the nine descriptors that have module-local files no descriptor
declares: declaring a file is an ownership claim, and that claim is reviewed here on its own so the
follow-up latch slice does not bless declarations written in the same change. It changes descriptor
metadata, the test-registration baseline, documentation, and cleanup accounting only; no production
code, public symbol, build membership, configuration, storage, or runtime behavior changes.

`governance` is the smallest module in this class (one source and one private header) and it is the
first module in the whole series to carry a private header. The nine already-latched modules have
none, so the validator's private-header handling has never been exercised against a real declared
module. Doing governance first confirms it before the larger declarations depend on it.

## What the module owns today

The descriptor now declares what lives under `src/modules/governance/`:

- `src/modules/governance/gw_stage_governance.c`: the response-governance stage. It wraps
  `gateway_policy_police_parsed_response` in a togglable response-pipeline stage, collapsing the four
  inline police call sites into one path that config can disable. It exports two functions:
  `gw_response_governance_enabled` (the `AIMEE_STAGE_GOVERNANCE` env fallback, default-on) and
  `gw_response_run_governance` (runs the stage over a parsed response when enabled).
- `src/modules/governance/gw_stage_governance.h`: the private header for those two exports. It is not
  under `include/aimee/governance/`; consumers include it as `gw_stage_governance.h` via the
  `-Imodules/governance` search path, which is what makes it a private header rather than a public one.
- `src/tests/test_response_governance_stage.c`: the direct test, covering the env-toggle parsing, the
  enabled drop count, the disabled no-op, and the NULL-parsed guard.
- `docs/modules/governance.md`: the canonical document.

This is narrower than the governance plane the canonical document describes. The OIDC, organizational
identity, policy-distribution, and console surfaces named there are distributed across
`src/kb/auth_oidc.c`, DB2 identity tables, management-token code, and the console, and are not
module-local. The descriptor claims only what is actually under the module root; the rest is future
migration, which is exactly why this slice declares but does not latch.

## Source liveness and build membership

- `gw_stage_governance.c` is compiled by Make in `DATA_SRCS` and carries the `-Imodules/governance`
  include path. It has tracked production callers: `gw_response_run_governance` is called by
  `src/server/anthropic_http.c` (three sites) and `src/server/openai_chat.c`, and
  `gw_response_governance_enabled` is read by the governance-enabled resolvers in those same two
  files. `src/modules/delegates/gw_orch_delegates.c` references the pair in a comment describing the
  seam. The source is live production code, not a test-only or dead island.
- The private header is included by `src/server/anthropic_http.c`, `src/server/openai_chat.c`, and the
  direct test.
- CMake compiles neither this module's source under any target of its own; as with gateway, CMake
  builds only the thin client plus the unit-test suite. CMake does add `${AIMEE_SRC_DIR}/modules/governance`
  to several targets' include paths, but no CMake target lists `gw_stage_governance.c` as a source.
  This is the same intentional profile boundary recorded for gateway in
  `docs/validation/core-modularization-slice-38.md`, not drift, and it is orthogonal to declaring
  ownership.

## Test membership

Make registers `unit-test-response-governance-stage` from `src/tests/test_response_governance_stage.c`
in `src/tests/Rules.mk`; the same source object is also linked into the two
`unit-test-anthropic-http*-p2c` integration targets, which is complementary coverage, not a second
owner. CTest does not register the governance test, consistent with the module sitting outside the
thin-client profile. `scripts/check_module_test_registration.py` now records the governance row
(`make: true`, `ctest: false`); its baseline is regenerated in this slice, and that regeneration is
the only reason the baseline file changes.

`src/tests/support/ir_ingress_stubs.c` defines weak `gw_response_governance_enabled` and
`gw_response_run_governance` symbols so integration binaries that pull the response path but link no
policing graph stay inert. Those are deliberate test stubs, recorded so a future reader does not read
them as a second implementation.

## Why declare without latching

The latch asserts that the descriptor exhaustively covers the module root. That assertion is true here
today (the module root holds exactly the one source and one header now declared) so the latch would
pass. The roundtable's point is not that the assertion is false; it is that declaring the files and
asserting completeness are two distinct claims, and collapsing them into one change means the
completeness audit reviews declarations it just authored. Keeping them separate means the follow-up
latch slice reviews declarations that were reviewed and merged on their own first. For a one-source
module the gap is small; the separation is kept anyway because the same pattern governs the eight
larger Class B modules behind governance, and doing it consistently is the point.

The validator accepts a declared-but-unlatched descriptor: it validates that each declared path exists
and is within the module, but enforces set equality only when `ownership_complete` is true. So this
slice leaves `governance` flagged as migration debt, correctly, while pinning what it owns.

## Regression controls

The declaration is covered by the existing descriptor validation: every declared path must exist and
resolve within the module, and the regenerated test-registration baseline pins the governance test's
per-suite registration. The empty-domain guard added in slice 39 does not apply, because the module
root is not empty. The latch-specific mutation coverage (source removal, planted files, cleared latch)
is deferred to the latch slice, where `ownership_complete` is set and those mutations become
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
python3 scripts/refactor_baselines.py check
make -C src -j2
make -C src lint
make -C src -j2 build/obj/tests/unit-test-response-governance-stage
src/build/obj/tests/unit-test-response-governance-stage
```

The follow-up slice sets `ownership_complete: true`, adds the governance latch mutation tests, and
records the completeness audit. Technical-writer review, exact-final-diff roundtable approval, and
every required pull-request check are required before merge.
