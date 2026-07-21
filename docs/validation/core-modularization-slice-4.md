# Core modularization slice 4: descriptor-v1 envelope

## Decision

This slice establishes the smallest stable descriptor envelope before production descriptors and
generators land. It adds no `src/modules/<id>/module.yaml`, generated build/profile artifact,
runtime registration, readiness claim, or source move.

The authoritative model is `scripts/validate_module_descriptors.py`. It deterministically emits
the committed Draft 2020-12 `src/modules/module.schema.json`; CI requires byte equality. The schema
is structural interchange, not a second handwritten registry. Its `$comment` is advisory and is
never a semantic input.

## Frozen v1 vocabulary

Descriptor files are strict UTF-8 JSON stored under the conventional `module.yaml` name. JSON is a
subset of YAML, so no YAML parser or runtime dependency is introduced. Every descriptor contains:

- `descriptor_version`, exactly integer `1`
- `id`, a lowercase, case-sensitive, NFC-normalized module ID of at most 64 UTF-8 bytes
- `dependencies`, a lexicographically sorted, unique array of canonical module IDs
- `runtime_toggle.supported`, a Boolean
- `enabled_by_default`, a Boolean required only for optional modules and forbidden for required
  modules

Required and optional classification comes only from
`tests/baselines/modules/canonical-inventory.yaml`. The validator embeds no taxonomy IDs or counts.
Required modules must declare `runtime_toggle.supported: false`; optional modules may support a
runtime toggle independently of their build/profile default. Dependencies must be canonical IDs
and may not name the containing module. Graph cycles remain deferred until production descriptors
provide a complete graph.

The v1 property vocabulary and semantics are frozen. Adding a property, changing a required key or
meaning, or tightening an identifier rule requires `descriptor_version: 2` and explicit
compatibility tests. Ordinary canonical-inventory membership changes do not change descriptor
vocabulary and therefore do not require a descriptor-version bump.

## Parser and failure behavior

The validator uses the standard-library JSON parser with duplicate-key rejection at every object
level. CRLF and LF are valid JSON whitespace; emitted schema uses LF. It rejects a UTF-8 BOM,
invalid UTF-8, comments, anchors, tags, block scalars, unquoted keys, YAML-only scalars, duplicate
keys, floating/exponent/non-finite numbers, invalid surrogate values, and trailing data. Inputs are
limited to 1 MiB, 32 nesting levels, 256 array items, and 64 UTF-8 bytes per module ID.

Failures include a stable `rule=<id>` and JSON pointer. Validation stops at the first failure.
Explicit roots with no `module.yaml` fail with `rule=no-descriptors-found`; `--allow-empty` exists
only for the named production-root probe while production descriptors are deferred.

## Verification and cleanup

Three positive fixtures cover a required module, an optional default-on module, and an optional
default-off module. Mutation tests cover parser, shape, identity, classification, selection,
runtime-toggle, dependency, duplicate-ID, discovery, schema-drift, and inventory-convergence
failures. Successful CI reports exactly three fixture descriptors and zero production descriptors.

Tree inspection at base `027573e43ef627b877ec2456a733fcf8b54f1fdc` found no prior
`module.schema.json`, `module.yaml`, or `validate_module_descriptors` artifact. No cleanup deletion
is claimed. The slice adds one authoritative model, one generated projection, and no wrapper or
parallel registry.

## Deferred

- production module descriptors and complete graph/cycle validation
- kinds, components, capabilities, providers/readiness, sources/headers/globs
- config ownership and compiled-read evidence
- stages, routes, commands, protocols, data, tests, docs, and compatibility aliases
- Make/CMake/profile/runtime/documentation generators and individual signed module docs
- path/glob policy, which has no v1 input field

Each deferred field lands with its first named consumer under a new descriptor version rather than
as unconsumed self-validating metadata.
