# Core modularization slice 33: complete skills ownership

## Scope

This slice marks the required `skills` descriptor `ownership_complete: true`. Slice 25 already moved
the implementation and public contracts from the retired singular root into `src/modules/skills`; this
slice proves that the descriptor exhaustively covers the validator's module-local C and private-header
domain, requires the canonical module document, and records the audited public headers and direct tests.
It changes metadata, validation, documentation, and cleanup accounting only. Production code, public
symbols, build inputs, configuration, stored skill data, and runtime behavior do not change.

Skills depend on required core services and cannot be disabled as a module. Individual dispatch and
lifecycle policies remain configuration, but the resolver, validation, injection, review, management,
and rollback contracts remain available to every profile.

## Source liveness and ownership

`src/Makefile` and the root `CMakeLists.txt` both compile all three descriptor-owned sources:

- `skill.c` implements resolution, loading, validation, injection, management, telemetry, lifecycle,
  and capability coverage. Production consumers include `cmd_skill.c`, `cmd_agent_delegate.c`,
  `session_briefing.c`, `server/server_skill.c`, `server/server_mcp_skill.c`, and the guardrails
  orchestrator.
- `skill_rollback.c` implements `skill_rollback_snapshot`, called by the `aimee skill rollback` path in
  `cmd_skill.c`.
- `skill_review.c` implements `skill_review_should_fire`, called by the live review-nudge path in
  `server/server.c`.

The public contracts live only under `src/modules/skills/include/aimee/skills`. Tracked-file, directory,
legacy-include, and build-input searches find no retired `src/skill` implementation, flat forwarding
header, or alternate resolver/lifecycle implementation. `cmd_skill.c`, `server/server_skill*.c`,
`modules/protocols/mcp/mcp_skill_tools.*`, and `modules/config/config_skills.c` are user, protocol, job,
or configuration boundaries that consume the module; they do not implement a second skill engine.

The descriptor-owned `test_skill.c` exercises the resolver, management, validation, injection,
lifecycle, rollback, and telemetry contracts. `test_skill_review.c` directly covers the review predicate
and poison-check path. Wider server, protocol, delegate, session, and guardrails tests remain owned by
their respective composition boundaries.

## DRY and dead-code findings

No complete source file is self-tested-only, and no duplicate canonical skills implementation was
found. Exact symbol searches do identify four public declarations without a caller outside the module
or its direct tests:

- `skill_trigger_matches_content` and `skill_name_is_valid` are used internally and exposed for direct
  unit testing, so their public exposure is a privatization or test-contract candidate;
- `skill_record_activation` is exercised directly but is not wired to a shipping activation caller; and
- `skill_metrics` exposes the associated activation counter only to the unit test.

The latter pair is also an activation-or-deletion candidate: self-contained implementation and tests do
not establish a live feature. Removing or wiring these symbols requires a focused telemetry/API slice;
doing so here would mix behavior with an ownership-metadata change.

## Regression controls

Production descriptor mutations remove each of the three skills sources in turn, plant an undeclared C
source and private header, and remove the canonical skills document. Each mutation must fail with the
stable `ownership-complete` rule. Existing header-layout and source-ownership checks continue to reject
the retired singular root, flat headers, basename includes, and legacy build spellings.

## Verification

Run these commands from the repository root; each must exit zero:

```sh
python3 scripts/validate_module_descriptors.py --check-schema src/modules
python3 -m unittest scripts.tests.test_validate_module_descriptors
python3 scripts/check_module_docs.py
python3 scripts/check_cleanup_ledger.py
python3 scripts/check_module_header_layout.py
python3 -m unittest scripts.tests.test_check_module_header_layout
python3 scripts/check_module_source_ownership.py
python3 -m unittest scripts.tests.test_check_module_source_ownership
python3 -m unittest scripts.tests.test_check_module_docs \
  scripts.tests.test_check_cleanup_ledger scripts.tests.test_refactor_baselines
python3 scripts/refactor_baselines.py check
make -C src -j2
make -C src lint
make -C src -j2 build/obj/tests/unit-test-skill build/obj/tests/unit-test-skill-review
src/build/obj/tests/unit-test-skill
src/build/obj/tests/unit-test-skill-review
```

The focused binaries must report `All skill tests passed.` and `PASS`, respectively. Technical-writer
review, exact-final-diff roundtable approval, and all required pull-request checks are also required.
