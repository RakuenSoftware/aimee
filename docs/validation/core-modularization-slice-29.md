# Core modularization slice 29: complete roundtable ownership

## Scope

This slice makes `src/modules/roundtable/module.yaml` the complete checked inventory for the
roundtable implementation root. It extends descriptor v1 with `private_headers` and the opt-in
`ownership_complete` latch, then enables that latch for roundtable.

It does not generate Make or CMake fragments, change target membership, or omit roundtable objects.
Those behaviors require the composition/provider seams and generated build selection planned for the
next slice.

## Completeness contract

The descriptor validator uses one role-extension policy for declared entries. Completeness scanning
covers the two owner-local implementation roles:

- `sources`: `.c`, `.cpp`, `.S`, and `.s`;
- `private_headers`: `.h` and `.hpp` below the module root but outside
  `include/aimee/<module>/`;
- `public_headers`: `.h` and `.hpp` only below `include/aimee/<module>/`; these are boundary-checked
  when declared but are not part of completeness scanning in this slice.

When `ownership_complete` is true, the declared `sources` and `private_headers` must exactly equal the
matching owner-local files. Missing declarations, stale declarations, symlinks, non-files,
cross-role/cross-descriptor duplicates, and public headers presented as private all fail with a stable
rule and JSON pointer. The `docs` field must contain exactly the canonical module document. Public
headers and tests remain explicit declarations rather than set-equality checks; integration tests can
span multiple owners.

Descriptors without the latch retain the migration-compatible declared-file validation from earlier
slices. They are not complete inventories and must not be consumed as authoritative generated-build
inputs.

## Roundtable inventory

The roundtable descriptor declares all ten owner-local translation units and eleven private headers.
It declares the direct ensemble, chair, preset, seat-resolution, pipeline capture/chunk/evaluation, and
verification tests, plus `docs/modules/roundtable.md`. It intentionally declares no public header:
roundtable headers have not yet moved to a canonical `include/aimee/roundtable/` surface.

## Verification

`scripts/tests/test_validate_module_descriptors.py` covers the production inventory and planted
failures for:

- omitted declared sources and private headers;
- undeclared new sources and private headers;
- invalid types, nonexistent paths, duplicates, and cross-role/cross-descriptor claims;
- public-header leakage into `private_headers`;
- a missing canonical module document;
- schema drift between the Python authority and `src/modules/module.schema.json`.

`scripts/validate_module_descriptors.py --check-schema src/modules` validates the complete production
graph and generated schema together.
