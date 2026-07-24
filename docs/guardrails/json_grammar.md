# Deterministic JSON fragment grammar

This document defines the byte-stable fixture grammar and oracle for the repetition-collapse detector. Inputs are UTF-8 bytes; offsets are an absolute byte offset from the start of the fixture file (byte zero of the file, including the header line). Offsets and spans are measured in UTF-8 bytes.

## Accepted JSON shapes

A fragment is a complete RFC 8259 JSON value. Markdown fencing is a wrapper: when the info string is exactly `json`, its inner content must independently satisfy one of the accepted shapes below. The accepted shapes are:

1. **top-level or nested arrays of primitives** — `string`, `number`, `boolean`, or `null` leaves in a flat array, or arrays of such arrays;
2. **arrays of uniform-shape objects with a required string discriminator and primitive leaves** — every element shares the same key set, including a string-valued discriminator key whose value is distinct per element; remaining leaves are primitives;
3. **objects with stable keys and compatible primitive leaf types repeated across array elements** — every element shares the same key set, leaves are primitives, and the set need not include a discriminator key; and
4. **JSON fenced inside markdown code blocks** — a ` ``` json ... ``` ` wrapper whose inner content independently satisfies one of shapes 1–3.

For every uniform object array in shape 2, the discriminator is the lexicographically first key common to every element whose value is a string in every element **and whose value is distinct across every element**. If no such key exists, the shape is excluded from shape 2 (it may still satisfy shape 3 if the leaves are primitives).

Shape identity is `(container path, kind, sorted keys, discriminator key, primitive types)`. Key order is insignificant; equal shape does not require equal serialized text.

The grammar excludes arbitrary heterogeneous objects, missing or extra keys, non-primitive leaves in uniform records, deeply nested comment-bearing fragments, and streaming partial tokens. JSON comments are not JSON. JSONC is excluded from this corpus and no JSONC dialect is enumerated as a sibling section; `//` and `/* ... */` comments, truncated strings/numbers/containers, and incomplete fences are out of scope.

## Canonical fixture envelope and oracle

Every fixture has one single-line header. Plain-text and source fixtures use `# shape:`; Markdown fixtures use `<!-- shape: ... -->`; a `.json` payload uses a sibling `.json.meta` file containing the unwrapped canonical line. These are the only envelope forms; `.meta`, `# shape:`, and `<!-- shape:` are normative.

The required fields and spellings are:

`shape:<description>; expected:<fire|no-fire>; expected_loop_start_offset:<integer>; expected_loop_span_bytes:<integer>; expected_repetitions:<integer>`

For `fire`, the offset identifies the first iteration boundary and the span is the byte length of one complete verbatim iteration, including every line terminator (including the final trailing newline) that belongs to that period; `expected_repetitions` records the number of iterations. The loop period begins at that iteration boundary. **The iteration boundary is the byte offset of the first iteration of the repeating period — the first occurrence after any non-repeating ramp or prefix, not the first verbatim body occurrence anywhere in the file.** Detector threshold N has **default 4** repetitions. The long-span threshold M has default 60 bytes. Fire fixtures contain at least N verbatim iterations of a qualifying loop. A no-fire fixture uses `-1`, `-1`, and `0` for offset, span, and expected_repetitions respectively.

### Authoritative scope of the `no-fire` label

The formal `no-fire` label is authoritative for every fixture in `tests/fixtures/collapse_legit/`, including:

* JSON fragments that obey any of the accepted shapes 1–4 above;
* non-JSON structural patterns explicitly enumerated in the request: markdown tables, enumerated/ordered lists, ASCII art boxes, repeated code boilerplate (e.g. import blocks, test stubs), fenced code blocks of non-JSON content, and repeated short lines; and
* any further `tests/fixtures/collapse_legit/fp_risk/` sub-corpus entries.

A detector MUST honour the no-fire label on every fixture in `tests/fixtures/collapse_legit/` (including the `fp_risk/` sub-corpus). The `fp_risk/` sub-corpus is a higher-priority regression set rather than an advisory escape hatch: a fire on these inputs is a precision regression and is reported as a FP.

### Interleaved-loop fixtures (optional fields)

A fire fixture whose structure has non-repeating connective tissue **between** iterations (e.g. a repeated loop body with a varying connector marker between every pair of iterations, rather than `period * N` as a contiguous block) declares two additional fields:

* `connective_tissue: yes` — marks the fixture as interleaved; the standard contiguous `period * N` byte-window check does not apply; and
* `expected_loop_iteration_offsets: a,b,c,d` — a comma-separated list of absolute byte offsets, one per iteration, equal in count to `expected_repetitions`; the first entry must equal `expected_loop_start_offset`.

The oracle for an interleaved fixture asserts:

1. every iteration at the listed offset matches the verbatim period `payload[expected_loop_start_offset:expected_loop_start_offset + expected_loop_span_bytes]`;
2. the bytes between consecutive iteration offsets form `expected_repetitions - 1` segments that are pairwise distinct (the connective tissue is genuinely non-repeating); and
3. `expected_loop_iteration_offsets[0] == expected_loop_start_offset`.

A fixture that claims `connective_tissue: yes` without listing per-iteration offsets, or whose listed offsets do not satisfy the rules above, is malformed.

### Consistency rule for `expected_repetitions`

For `fire` fixtures, `expected_repetitions >= N` (default 4). For contiguous fixtures (no `connective_tissue: yes`), the body length from `expected_loop_start_offset` onward must be `>= expected_repetitions * expected_loop_span_bytes`. A fixture that violates either constraint is malformed.

## Metrics

For a fixture $i$, let $y_i$ be the fixture's expected label (`fire` or `no-fire`) and $\hat{y}_i$ be the detector's verdict. Count:

* TP = $|\{i : y_i = \text{fire} \land \hat{y}_i = \text{fire}\}|$
* FP = $|\{i : y_i = \text{no-fire} \land \hat{y}_i = \text{fire}\}|$
* FN = $|\{i : y_i = \text{fire} \land \hat{y}_i = \text{no-fire}\}|$
* TN = $|\{i : y_i = \text{no-fire} \land \hat{y}_i = \text{no-fire}\}|$

Report precision = **TP / (TP + FP)**, recall = **TP / (TP + FN)**, and specificity = **TN / (TN + FP)** separately; do not collapse them into accuracy. Precision, recall, and specificity are reported over distinct fixtures; do not weight by file size.

## Corpus coverage requirements

The corpus under `tests/fixtures/collapse_legit/json/` must contain at least one fixture for each accepted JSON shape listed above (shapes 1–4). The accompanying oracle test (`tests/test_collapse_corpus.py::test_required_json_shape_categories_present`) parses each JSON payload (or fenced JSON body) and asserts the structural invariants of the category it claims before counting that fixture toward required coverage. A fixture whose payload does not satisfy the documented invariant for its category is not counted, and the test fails.
