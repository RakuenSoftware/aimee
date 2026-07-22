# Core modularization slice 20: governance capability ownership

## Scope

The slice starts from `46eeb6dc475bb3ade8a029207679bf461b03eedc` and implements only binding
check 6's governance ownership artifact, deterministic checker, focused mutation tests, CI invocation,
baseline instructions, validation record, and cleanup accounting. It changes no production source,
descriptor, build graph, package, profile, runtime toggle, route, configuration, schema, or GUI behavior.

## Contract

`tests/baselines/modules/governance-ownership.yaml` is strict JSON-compatible YAML and has exact semantic
equality with the fenced block parsed directly from the approved proposal. It assigns thirteen organizational capabilities to optional
`governance`, pins six forbidden core shadows, eleven required dependencies, and the forbidden
core-to-governance edge. Canonical module classification and live dependency edges remain sourced from
`canonical-inventory.yaml` and `src/modules/*/module.yaml`; they are not duplicated as new authority.

`scripts/check_capability_ownership.py` treats the proposal block as authority and rejects unknown keys, aliases, duplicate keys/entries, missing or
extra capabilities, unknown owners, core shadows, normative-list drift, missing governance dependency
edges, and required-core dependencies on governance. It rejects non-JSON constants, YAML anchors/aliases,
ambient root redirection, and descriptor path escapes. It resolves the repository independently of the
current working directory, emits stable diagnostics, and uses only the Python standard library.

## Dependency and security boundary

This gate makes `governance` the sole optional owner of OIDC/SSO federation, organizational roles/policy,
governance approvals/records/posture/attestation, governance identity/fleet records, and artifact
signing/trust. It forbids governance from shadowing core `execution-policy`, audit ledger, vault custody,
gateway authentication, or protocol authentication. Every required descriptor remains forbidden from
depending on governance.

The artifact does not establish broad source/header/test/docs/build/config/route/data ownership and does
not claim that distributed OIDC or response-policy code has moved. Those remain later descriptor,
profile, and source slices.

## Completed local verification

- `python3 -I -S scripts/check_capability_ownership.py`
- `cd /tmp && python3 -I -S "$WORKTREE/scripts/check_capability_ownership.py"`
- `python3 -I scripts/tests/test_check_capability_ownership.py -v`
- `python3 -I -S scripts/validate_module_descriptors.py --check-schema src/modules`
- `python3 -I -S scripts/check_cleanup_ledger.py`
- `make -C src lint`

## Close-time gates

- technical-writer review and exact final-diff roundtable approval
- feature-branch pull-request CI
