# Core modularization slice 6: canonical production descriptor graph

## Decision

This slice makes the canonical inventory's eighteen required and eight optional IDs the first
complete production descriptor graph. Descriptor v1 records classification-derived selection,
runtime-toggle support, and target public-contract dependencies. It does not claim current source
ownership, build inclusion, runtime registration, generated profiles, or readiness.

Classification comes only from the required and optional partitions in
`tests/baselines/modules/canonical-inventory.yaml`; v1 descriptors contain no classification field.
Every descriptor has a required, sorted `dependencies` array, including the empty array on the
dependency root `module-runtime`. Production validation requires exact inventory coverage and the
path `src/modules/<id>/module.yaml`. Other implementation files may coexist in that module
directory, but nested descriptors, path aliases, and case variants are forbidden.

## Selection and lifecycle

`runtime-web` and `control-web` are default-on and runtime-toggleable because each GUI has an
independent configured process lifecycle. `plugin-loader`, `governance`, `workflows`, `roundtable`,
`kb-synthesis`, and `benchmarks` are default-off and are not yet runtime-toggleable. Required
modules have no default-selection field and explicitly declare runtime toggling unsupported.
Future optional IDs default off and non-toggleable until an approved amendment says otherwise.

## Dependency boundary

The committed descriptors are the target public-contract graph. Required modules cannot depend on
optional modules. Validation uses lexicographically ordered DFS and reports a closed cycle at its
lexicographically smallest rotation, making diagnostics stable across platforms.

The direct `governance` to `delegates` edge is normative: governance owns delegation governance
identity and consumes the required delegate contract. `audit` intentionally does not depend on
`ir`; stage owners translate actions into audit-owned typed events. `skills` intentionally does
not depend directly on `learning`, `routing`, or `workspace`: those consumers use the skills
contract, while resource access is mediated by the declared `skills` to `tools` and `tools` to
`workspace` edges. Physical-edge enforcement must simplify or move current code to this graph;
widening it requires a separately reviewed amendment.

## Documentation follow-up boundary

Descriptor v2 and substantive individual module documentation are the immediate next
modularization slice. No intervening slice may add source/surface metadata or placeholder module
documentation. V2 will atomically migrate all descriptors and add `docs` as one repository-relative
document path, `sources` and `public_headers` as sorted unique arrays of repository-relative globs,
and `surfaces` as strict sorted route, command, protocol, and stage ID arrays. Mixed descriptor
versions will fail validation.

V2 is not acceptable until every descriptor resolves to a substantive `docs/modules/<id>.md` that
satisfies the approved module-document contract and cites real source/header globs and surface IDs,
or explicitly justifies an empty category. Empty READMEs, headings-only files, stubs, TODO-only
content, and invented references cannot satisfy that gate. V1 contains no speculative v2 logic.

## Verification and cleanup

The Python validator remains the single semantic authority and continues to generate the JSON
schema. CI runs production validation as the blocking graph gate and separately retains fixture and
mutation tests. The obsolete `--allow-empty` escape hatch is removed. This slice adds no shell
validator, parallel taxonomy, readiness registry, build generator, or documentation placeholder.
