# Deterministic JSON fragment grammar

This document defines the byte-stable fixture grammar and oracle for the repetition-collapse detector. Inputs are UTF-8 bytes; offsets are an absolute byte offset from the start of the fixture file (byte zero of the file, including the header line). Offsets and spans are measured in UTF-8 bytes.

## Accepted JSON shapes

A fragment is a complete RFC 8259 JSON value. Markdown fencing is a wrapper: when the info string is exactly `json`, its inner content must independently satisfy one of the accepted shapes below. Accepted shapes are:

* top-level or nested arrays of primitives (`string`, `number`, `boolean`, or `null`);
* arrays of uniform-shape objects, each with the same required string discriminator key and primitive leaves; and
* objects with stable keys and compatible primitive leaf types repeated across array elements.

For every uniform object array, the discriminator is deterministically the lexicographically first key common to every element whose value is a string in every element. If no such key exists, the shape is excluded.

Shape identity is `(container path, kind, sorted keys, discriminator key, primitive types)`. Key order is insignificant; equal shape does not require equal serialized text.

The grammar excludes arbitrary heterogeneous objects, missing or extra keys, non-primitive leaves in uniform records, deeply nested comment-bearing fragments, and streaming partial tokens. JSON comments are not JSON. JSONC is excluded from this corpus and no JSONC dialect is enumerated as a sibling section; `//` and `/* ... */` comments, truncated strings/numbers/containers, and incomplete fences are out of scope.

## Canonical fixture envelope and oracle

Every fixture has one single-line header. Plain-text and source fixtures use `# shape:`; Markdown fixtures use `<!-- shape: ... -->`; a `.json` payload uses a sibling `.json.meta` file containing the unwrapped canonical line. These are the only envelope forms; `.meta`, `# shape:`, and `<!-- shape:` are normative.

The canonical fields and spellings are:

`shape:<description>; expected:<fire|no-fire>; expected_loop_start_offset:<integer>; expected_loop_span_bytes:<integer>; expected_repetitions:<integer>`

For `fire`, the offset identifies the first iteration boundary and the span is the byte length of one verbatim iteration; `expected_repetitions` records the number of iterations. The loop period begins at that iteration boundary. **The iteration boundary is the byte offset of the first iteration of the repeating period — the first occurrence after any non-repeating ramp or prefix, not the first verbatim body occurrence anywhere in the file.** Detector threshold N has **default 4** repetitions. The long-span threshold M has default 60 bytes. Fire fixtures contain at least N verbatim iterations of a qualifying loop. A no-fire fixture uses `-1`, `-1`, and `0` for offset, span, and expected_repetitions respectively.

### Consistency rule for `expected_repetitions`

For `fire` fixtures, `expected_repetitions >= N` (default 4), and the body length from `expected_loop_start_offset` onward must be `>= expected_repetitions * expected_loop_span_bytes`. A fixture that violates either constraint is malformed.

### Scope of the `no-fire` label

The formal `no-fire` label applies to fragments that obey the accepted JSON shapes above (the JSON profile) and to the non-JSON structural patterns explicitly enumerated in the corpus: markdown tables, enumerated/ordered lists, ASCII art boxes, and JSON fenced inside markdown. Fixtures outside this scope — including free-form prose near-verbatim repeats and non-JSON fenced code blocks — are advisory **false-positive-risk** cases and live in a separate `tests/fixtures/collapse_legit/fp_risk/` sub-corpus. A detector is not required to honour the no-fire label on `fp_risk/` inputs; it is encouraged to use them as a regression set to surface unintended fires.

## Metrics

TP is fire on a fire-labelled fixture; FP is fire on a no-fire fixture; FN is no-fire on a fire-labelled fixture; TN is no-fire on a no-fire fixture. Report precision = **TP / (TP + FP)**, recall = **TP / (TP + FN)**, and specificity = **TN / (TN + FP)** separately; do not collapse them into accuracy.
