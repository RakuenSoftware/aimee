# Deterministic JSON fragment grammar

This corpus recognizes UTF-8 JSON values (RFC 8259), with no whitespace-dependent
semantics. A fragment is either a complete value or a JSON value fenced in a
Markdown code block whose info string is `json` (case-sensitive). Parsing is
performed on the bytes between the value's delimiters; offsets in fixtures are
absolute UTF-8 byte offsets from generation start.

## Accepted shapes

* A top-level array of primitives, or an array nested beneath objects/arrays.
  Primitives are strings, numbers, booleans, or null; nesting is bounded by the
  detector's configured maximum depth.
* An array of objects having one uniform key set. Each object must contain a
  required string discriminator (the same named key and string type in every
  element), and all remaining leaves must be primitive values. Key order does
  not affect shape.
* An object with stable keys repeated across array elements: every element has
  the same key set and compatible primitive leaf types.
* JSON fenced in a Markdown `json` code block.

Shape identity is the ordered tuple (container path, array/object kind, sorted
keys, discriminator key, primitive types). Equal identities are structural
repeats; textual equality is not required.

## Explicit exclusions

The grammar excludes arbitrary heterogeneous objects, objects with missing or
extra keys, non-primitive leaves in uniform records, and deeply nested
comment-bearing fragments. JSON comments are not JSON; no JSONC dialect is
enumerated here. Streaming partial tokens (truncated strings, numbers,
containers, or incomplete fences) are excluded and must not be classified.

## Metrics

For a labelled fixture, TP is a fire correctly identifying the labelled loop;
FP is fire on a no-fire fixture; FN is no-fire on a fire fixture; TN is no-fire
on a no-fire fixture. Report **precision = TP/(TP+FP)**, recall
`TP/(TP+FN)`, and specificity `TN/(TN+FP)` separately; do not substitute
accuracy for these metrics.

Fixture headers are part of the oracle. `expected_loop_start_offset` and
`expected_loop_span_bytes` are absolute byte counts from generation start; a
no-fire fixture uses `0` and `0`.
