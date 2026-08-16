# Go config field descriptors: editable flat-scalar save residual

- **State:** PENDING — restored after rejection audit on 2026-08-15.
- **Archived parent:** [config-field-descriptor-table.md](../done/config-field-descriptor-table.md).
- **Corrective history:** PR #2655 rejected this live objective because its old wording assigned
  save generation to C. That was an ownership error, not evidence that descriptor-owned
  persistence was obsolete. The current plan makes Go the semantic owner and treats legacy C
  behavior only as compatibility evidence during migration.

## Current boundary

The old residual assumed that completing generic save meant extending
src/modules/config/config_save.c. The current repository has a Go-owned control-plane mutation
path:

- server-go/internal/api/config.go sends authenticated /v1/config mutations to
  config.Store.SetVersioned;
- server-go/internal/config/store.go validates those mutations, edits a yaml.Node, preserves
  unrelated keys, writes through a mode-0600 temporary file, syncs and closes it, then atomically
  renames it over the configured file; and
- Store.Values owns the safe editable projection returned to the control-plane client.

The generic Go writer therefore already exists. The remaining drift is in the metadata that decides
what that writer may expose and persist. The same editable flat scalar is currently repeated across
policyDefaults, configurableTypes, optional configurableIntBounds, branches in
validateKeyValue, and projection filtering in Values. Adding a field can still omit a default,
type, bound, special validator, or projection rule without a structural failure.

That is the surviving objective: make one Go descriptor registry authoritative for the eligible
flat-scalar contract and have the existing Go store consume it.

## Ownership and invariants

server-go/internal/config is the sole semantic owner for descriptor-defined fields:

- the descriptor decides whether a key is editable, its scalar kind, its default/absence behavior,
  its bounds and special validation, its sensitivity, and its persistence behavior;
- the Go config API and store decide whether a mutation is authorized and valid;
- the existing yaml.Node update path remains the only descriptor-owned persistence
  implementation; and
- legacy C loaders may consume the compatible file, but C tables and writers do not define the
  descriptor policy.

The following invariants are load-bearing:

1. Unknown, non-editable, or sensitive keys fail closed at the mutation boundary and never enter the
   editable projection.
2. An absent key and an explicitly persisted default retain the behavior recorded for that key
   before migration. The plan does not impose one blanket omission rule on every field.
3. A successful write preserves unrelated mappings, nested sections, comments where the YAML
   library can preserve them, and secrets byte-for-byte at the value level.
4. Validation happens before any temporary file is created. Invalid values and structural
   conflicts leave the file unchanged.
5. A descriptor-owned key cannot also have an independent Go default, type allowlist entry, bound,
   or validation branch.
6. The registry is deterministic and immutable after process initialization.

## Eligible scope

A field is eligible only when all of these are true:

- it is a dotted or top-level scalar key exposed through the Go editable config projection;
- its value has a scalar kind directly supported by the Go store;
- setting it requires no nested-object merge, allocation, cross-field derivation, coercion, or
  secret replacement; and
- the existing Go path can persist it with the generic yaml.Node leaf update.

The initial inventory is the existing configurableTypes set. trigger_rules remains a named
custom path because it is a nested sequence with optimistic concurrency and domain validation.
StringValue remains a read-only internal lookup and does not make a string key editable.

Nested configuration objects, bootstrap transport, environment-only exceptions, and wholesale
migration of the legacy C configuration subsystem are outside this residual. The separate
[config-authority-surface-residual.md](config-authority-surface-residual.md) continues to own the
cross-language inventory of durable knobs and environment exceptions.

## Plan

### 0. Freeze the eligibility and compatibility inventory

Add a checked-in inventory test for every currently editable Go flat scalar. For each key, record:

- scalar kind;
- default and whether absence is distinct from an explicitly written default;
- editable and sensitive flags;
- numeric bounds and any named validator;
- current accepted/rejected JSON input shapes; and
- the expected YAML node shape after a write.

The inventory fails on an editable key that lacks a descriptor and on a descriptor that is not
reachable through the editable projection. trigger_rules appears in the inventory as an explicit
custom exclusion, not as an unclassified exception.

### 1. Add the Go descriptor registry

Add a package-private descriptor type in server-go/internal/config, equivalent to:

~~~go
type FieldDescriptor struct {
    Key         string
    Kind        FieldKind
    Default     any
    Editable    bool
    Sensitive   bool
    Persistence PersistencePolicy
    IntBounds   *IntBounds
    Validate    func(any) error
}
~~~

The concrete implementation may split typed defaults or validators to preserve compile-time
safety, but it must keep one row authoritative. Registry construction rejects duplicate keys,
unsupported kinds, a default that does not match the kind or bounds, an editable sensitive field,
and an omission policy without a default.

### 2. Drive projection, defaults, and validation from descriptors

Replace direct reads of policyDefaults, configurableTypes, and configurableIntBounds with
descriptor lookup:

- Values iterates descriptors to construct the editable projection and inject defaults;
- Value resolves only descriptor-owned or explicitly custom keys;
- validateKeyValue dispatches scalar validation through the descriptor; and
- named field validators, including the derived autonomy.max_wall_secs floor, remain focused
  functions referenced by the owning row.

The custom trigger_rules validator and version contract remain unchanged. Unknown keys, structural
conflicts, non-integral JSON numbers for integer fields, and out-of-bound values keep their current
fail-closed behavior and diagnostic specificity.

### 3. Make descriptor save semantics explicit

Keep Store.SetVersioned as the write entry point and the existing yaml.Node writer as the mechanism.
Before changing the tree, resolve the descriptor and normalize only according to its declared kind
and persistence policy.

For every eligible key, the descriptor declares whether an explicit default is retained or omitted.
The migration first copies observed behavior; changing a key's policy later requires its own
compatibility decision. Removing a default-valued key must remove only that mapping leaf and must
not collapse a parent that still contains unrelated children.

No descriptor may read, render, log, or rewrite a sensitive value. The store continues to preserve
unknown and non-editable YAML without exposing it.

### 4. Prove compatibility, then delete duplication

Add focused fixtures covering every eligible Go scalar kind and the concrete failure classes that
motivated the original residual:

- absent, explicitly defaulted, and non-default values;
- integer JSON decoding, bounds, and named validators;
- unknown-key and sensitive-key non-disclosure;
- scalar-versus-mapping structural conflicts;
- unrelated and nested YAML preservation;
- a rejected write leaving content and version unchanged; and
- a Go-written descriptor key loading with the same value in the retained legacy consumer.

Legacy C output is a compatibility baseline, not policy. Where C and Go formatting differ, compare
parsed semantics and the stable Go serialization separately rather than requiring accidental
whitespace equality.

Only after the fixtures pass may the duplicate Go maps and covered switch branches be removed.
Any legacy C mutation adapter for a descriptor-owned key can be retired only after its callers use
the Go authority and the cross-language fixture remains green. Unmigrated C-only keys stay outside
the registry instead of creating two owners.

## Acceptance

These are TDD targets for the pending implementation; this corrective PR does not claim they exist
or pass yet:

    cd server-go && go test ./internal/config -run TestFieldDescriptorsCoverEditableFlatFields
    cd server-go && go test ./internal/config -run TestFieldDescriptorValidation
    cd server-go && go test ./internal/config -run TestDescriptorPersistenceCompatibility
    cd server-go && go test ./internal/config -run TestDescriptorProjectionExcludesSensitiveAndUnknown
    cd server-go && go test ./internal/config -run TestDescriptorWriterPreservesUnrelatedYAML

The retained config compatibility suite must also prove a Go-written eligible value is read with
the same semantics by the legacy consumer.

The implementation is complete when every eligible Go field is descriptor-covered; projection,
defaulting, validation, and persistence consume that descriptor; duplicate Go metadata is gone;
and no nested/custom section or secret has moved into the flat-scalar path.
