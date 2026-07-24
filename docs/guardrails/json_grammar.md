# Deterministic JSON fragment grammar

This corpus uses UTF-8 JSON text. Offsets and lengths are measured in bytes from byte zero of the generated fixture file, including its header.

## Accepted grammar

The accepted fragment is one complete JSON value, or a JSON value fenced in a Markdown code block whose info string is `json`. JSON strings use standard escapes; numbers, `true`, `false`, and `null` are primitive leaves. Arrays may be top-level or nested and may contain primitives. An object array is accepted when every element has the same key set, contains a required string discriminator (declared by the fixture), and all remaining leaves are primitives. Objects may therefore repeat stable keys across array elements. Duplicate keys are not accepted. Markdown fences must be paired and closed.

## Explicit exclusions

* Arbitrary heterogeneous objects, including arrays mixing unrelated object shapes, are excluded.
* Deeply nested comment-bearing fragments are excluded. No JSONC dialect is defined; JSONC requires a separately enumerated sibling grammar.
* Streaming partial tokens, truncated values, and incomplete escapes are excluded.

## Detector oracle and metrics

Each fixture header declares `fire` or `no-fire`, an absolute `expected_loop_start_offset`, and `expected_loop_span_bytes`. A no-fire fixture uses `-1` for both offset and span. `fire` means the declared repeated region must be reported; `no-fire` means legitimate repetition must be suppressed.

TP is a fire fixture correctly reported, FP is a no-fire fixture reported, FN is a fire fixture missed, and TN is a no-fire fixture suppressed. Precision is `TP/(TP+FP)`. Report recall `TP/(TP+FN)` and specificity `TN/(TN+FP)` separately; do not collapse them into accuracy.

All offsets refer to bytes produced by the checked-in fixture itself, not an external generator or path. Headers are part of that byte sequence.
