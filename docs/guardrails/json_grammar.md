# Deterministic JSON fragment grammar

This document defines the byte-stable fixture grammar and oracle for the repetition-collapse detector. Inputs are UTF-8 bytes; offsets are an absolute byte offset from generation start; offsets and spans are measured from byte zero of the generated fixture body.

## Accepted JSON shapes

A fragment is a complete RFC 8259 JSON value, or a complete JSON value inside a Markdown fenced code block whose info string is exactly `json`. Accepted shapes are:

* top-level or nested arrays of primitives (`string`, `number`, `boolean`, or `null`);
* arrays of uniform-shape objects, each with the same required string discriminator key and primitive leaves; and
* objects with stable keys and compatible primitive leaf types repeated across array elements.

Shape identity is `(container path, kind, sorted keys, discriminator key, primitive types)`. Key order is insignificant; equal shape does not require equal serialized text.

The grammar excludes arbitrary heterogeneous objects, missing or extra keys, non-primitive leaves in uniform records, deeply nested comment-bearing fragments, and streaming partial tokens. JSON comments are not JSON. No JSONC dialect is enumerated as a sibling section; `//` and `/* ... */` comments, truncated strings/numbers/containers, and incomplete fences are out of scope.

## Canonical fixture envelope and oracle

Every fixture has one single-line header. Plain-text and source fixtures use `# shape:`; Markdown fixtures use `<!-- shape: ... -->`; a `.json` payload uses a sibling `.json.meta` file containing the unwrapped canonical line. These are the only envelope forms; `.meta`, `# shape:`, and `<!-- shape:` are normative.

The canonical fields and spellings are:

`shape:<description>; expected:<fire|no-fire>; expected_loop_start_offset:<integer>; expected_loop_span_bytes:<integer>; expected_repetitions:<integer>`

For `fire`, the offset identifies the first iteration boundary and the span is the byte length of one verbatim iteration; `expected_repetitions` records the number of iterations. The iteration boundary is derived from the first verbatim body occurrence and must be checked against the declared span. Detector threshold N has **default 4** repetitions. The long-span threshold M has default 60 bytes. Fire fixtures contain at least N verbatim iterations of a qualifying loop. A no-fire fixture uses `-1`, `-1`, and `0` for offset, span, and expected_repetitions respectively.

## Metrics

TP is fire on a fire-labelled fixture; FP is fire on a no-fire fixture; FN is no-fire on a fire-labelled fixture; TN is no-fire on a no-fire fixture. Report precision = **TP / (TP + FP)**, recall = **TP / (TP + FN)**, and specificity = **TN / (TN + FP)** separately; do not collapse them into accuracy.
