# Deterministic JSON fragment grammar

This corpus is the pre-detector oracle for the collapse detector. Every
fixture is a UTF-8 text file checked in under `tests/fixtures/`. Generator
output is not relied upon; the byte sequence on disk is the source of
truth.

## Byte semantics

* All offsets are absolute byte offsets from the start of the
  fixture file. For inline envelopes the header occupies the first
  `lf_offset + 1` bytes (one line terminated by LF), and the loop
  period is found below that. For sibling `.meta` files the payload
  file is offset 0 and the `.meta` file holds the header separately.
* `expected_loop_start_offset` is the absolute byte offset of the first
  byte of the first loop period measured from byte 0 of the fixture file,
  including any header bytes. For inline envelopes the header occupies
  bytes `[0, first_lf_offset + 1)` and the body begins at that boundary;
  when the body begins with the loop period, the offset equals the header
  length including its trailing LF. For `.meta` envelopes the payload file
  starts at offset 0.
* `expected_loop_span_bytes` is the length, in bytes, of **one loop
  period** (the unit that repeats verbatim). It is NOT the length of
  the entire repeated region.
* `expected_repetitions` is the exact count of verbatim iteration
  occurrences in the body. For `no-fire` fixtures it is `0`.
* For `no-fire` fixtures both `expected_loop_start_offset` and
  `expected_loop_span_bytes` are `-1`.

## Metadata envelope

Every fixture has a metadata header that declares its shape, expected
detector outcome, and oracle bytes. The envelope extraction step is
deterministic: given a fixture path the loader first looks for
`<path>.meta`; if present it parses that file as the header and treats
the entire payload file as the body. Otherwise it reads the first line
of the payload file up to (and including) the first LF as the header,
and the body begins at `first_lf_offset + 1`.

Envelope forms:

1. **Sibling `.meta` file.** A file with the same stem plus `.meta`
   extension holds the header. Used when the payload must remain a
   standalone parseable value (e.g. `.json`). The header text is the
   raw bytes of the `.meta` file. The `.meta` file is itself a
   single-line LF-terminated record for consistency with the inline
   envelope forms (one canonical header line, terminated by a single
   LF, byte `0x0A`); the loader strips the trailing LF before parsing
   so a `.meta` file with or without a trailing newline parses
   identically.
2. **Inline `# shape:` header line** for `.txt` and `.py` payloads. The
   header is the first line of the file, beginning with `# shape:` and
   terminated by a single LF (byte `0x0A`).
3. **Inline `<!-- shape: ... -->` HTML-comment header** for `.md`
   payloads. The header is an HTML comment in a Markdown comment block
   (`<!-- ... -->`) that occupies the first line of the file. The
   comment must start with `<!-- shape:` and end with ` -->`, separated
   by a single LF (byte `0x0A`) from the start of the Markdown body.
   Using `<!-- shape: ... -->` rather than `# shape:` keeps the
   surrounding Markdown parseable by tools that would otherwise treat a
   leading `# shape:` line as a Markdown ATX heading.

### Canonical header grammar

The wrapper absorbs the leading `shape:` literal from inline envelopes
(`<!-- shape: <text>; ... -->` and `# shape: <text>; ...`); to apply a
single anchored grammar the loader rebuilds the canonical form by
prepending `shape: ` to the wrapper-stripped body when needed. Sibling
`.meta` files already contain the literal `shape:` field. The canonical
grammar is:

```
shape: <text>; expected: fire|no-fire; expected_loop_start_offset: <int>; expected_loop_span_bytes: <int>; expected_repetitions: <int>
```

Field-level rules:

* Fields appear in the exact order: `shape`, `expected`,
  `expected_loop_start_offset`, `expected_loop_span_bytes`,
  `expected_repetitions`.
* Fields are separated by exactly `; ` (semicolon and one space). No
  other separator is permitted.
* `<text>` may not contain `;`. It is described by the leading pattern
  `[^;]+`.
* `<int>` matches `-?[0-9]+`. For no-fire fixtures the offset and span
  integers are `-1`; `expected_repetitions` is `0`.
* `expected:` accepts only the literals `fire` or `no-fire`.

Envelope integrity rules (these are mandatory — the corpus test rejects
violations):

* The header occupies exactly one line, terminated by a single LF. No
  internal LF, CR, or CRLF is permitted.
* No field may appear more than once.
* No field outside the canonical five is permitted.
* The wrapper forms (`# shape:` and `<!-- shape: ... -->`) are required
  for inline envelopes and carry no leading or trailing whitespace
  beyond what is specified above.
* A sibling `.meta` file is itself a single-line record: its raw
  content MUST end with exactly one LF (byte `0x0A`) or with no
  trailing newline at all; both forms are equivalent under the
  loader's LF-strip step. CRLF (`0x0D 0x0A`), any embedded CR, and
  any embedded LF inside the record are rejected. The corpus test
  enforces this via `test_meta_files_terminate_with_lf`, which asserts
  an LF terminator on the checked-in `.meta` files; the no-newline
  form is documented here as an allowed parser-side equivalence but
  the canonical checked-in form is LF-terminated.

These rules are enforced by an anchored regex match against the header
content (between the wrapper bounds for `.md`, and starting at `# shape:`
/ start-of-file for the others). A header that fails the anchored match
is treated as malformed and the fixture is rejected.

## Loop period oracle

For a `fire` fixture the loop period oracle is defined as follows:

* The first iteration starts at `expected_loop_start_offset` and covers
  bytes `[off, off + expected_loop_span_bytes)`. Call this the **period
  slice** `P[0]`. The body at that range must equal `P[0]` verbatim.
* Subsequent iterations are found by walking the body forward from
  `off + sp` in order. A loop period boundary is a position `p >= off`
  such that `body[p : p + sp] == P[0]` AND `p` is reachable from the
  previous iteration boundary by stepping over zero or more non-empty
  connective bytes (any byte sequence that is not equal to `P[0]`).
  The connective bytes between two consecutive iterations are
  `body[p_k + sp : p_{k+1}]`.
* The body must contain **exactly `expected_repetitions` iteration
  boundaries**, no more and no fewer. This excludes accepting
  non-contiguous substring matches (e.g. an `item\nvalue\n` substring
  appearing incidentally inside connective tissue) as iterations.
* Between any two consecutive iteration boundaries the connective must
  be non-empty AND must not contain `P[0]` as a substring starting at
  any byte inside the connective. This rules out accidental false
  matches where one iteration is "hidden" inside the connective of
  another.
* For **contiguous** loops the connective between consecutive
  iterations is empty; an iteration boundary is therefore at
  `off + k * sp` for `k = 0 .. N - 1`.
* For **interleaved** loops the connective is non-empty and typically
  varies between iterations (e.g. varying row numbers, varying list
  markers). Distinct connective slices are required when the fixture
  shape description declares the iteration is "interleaved".
* For **nested** loops the inner-period slices are contained entirely
  inside the outer period; the outer iteration is the unit that
  repeats, and the inner sub-loop's iterations are not separately
  counted against `expected_repetitions`.

A `fire` fixture is correct when the body matches this oracle. A
`no-fire` fixture is correct when no period slice repeated
`expected_repetitions` times (with `>= 2`) exists in the body at any
walk-forward iteration boundary.

## Accepted grammar (JSON)

The accepted JSON fragment is one complete JSON value, or a JSON value
fenced inside a Markdown code block whose info string is `json`. JSON
strings use standard escapes; numbers, `true`, `false`, and `null` are
primitive leaves. Arrays may be top-level or nested and may contain
primitives. An object array is accepted when every element has the same
key set, contains a required string discriminator (declared by the
fixture), and all remaining leaves are primitives. Objects may therefore
repeat stable keys across array elements. Duplicate keys are not
accepted. Markdown fences must be paired and closed.

## Sibling dialect: JSONC

JSONC (JSON with `//` and `/* */` comments) is **not** part of the
accepted grammar. A JSONC dialect, if needed, must be enumerated as a
separate sibling section with its own delimiters and is out of scope for
this corpus. Comments inside JSON fragments are otherwise an exclusion
trigger.

## Explicit exclusions

* Arbitrary heterogeneous objects, including arrays that mix unrelated
  object shapes, are excluded.
* Deeply nested comment-bearing fragments are excluded. JSONC is
  enumerated above as a sibling dialect only.
* Streaming partial tokens, truncated values, and incomplete escapes are
  excluded.
* `.json` fixtures in this corpus must contain only standard JSON once
  the envelope is stripped; metadata lives in a sibling `.meta` file.
  Inline `//` comments inside `.json` fixtures are rejected.
* Markdown-fenced payloads must use the `json` info string; non-`json`
  fences are out of grammar (legitimate repeats of non-JSON code live
  under `tests/fixtures/collapse_legit/`, not under the accepted-grammar
  JSON shapes).

## Detector oracle and metrics

Each fixture header declares `fire` or `no-fire`, an absolute
`expected_loop_start_offset`, `expected_loop_span_bytes`, and an
`expected_repetitions` count. The span is the length of one loop
period, not the full repeated region.

A `fire` fixture is correct when the detector reports a period of
`expected_loop_span_bytes` bytes starting within byte tolerance of
`expected_loop_start_offset`, with the body yielding exactly
`expected_repetitions` verbatim iteration boundaries as defined under
the loop period oracle above. A `no-fire` fixture is correct when
the detector reports no loop of at least two verbatim iterations.

Let `fire` mean "the fixture contains a genuine collapse pattern" and
`no-fire` mean "the fixture contains only legitimate structural repeats
the detector must suppress". Then:

* **TP** = `fire` fixture, detector reports the loop.
* **FP** = `no-fire` fixture, detector reports a loop.
* **FN** = `fire` fixture, detector reports no loop.
* **TN** = `no-fire` fixture, detector reports no loop.

Precision is `TP / (TP + FP)`. Recall is `TP / (TP + FN)`. Specificity is
`TN / (TN + FP)`. Recall and specificity are reported as separate
metrics; they are not collapsed into accuracy.

All offsets refer to bytes produced by the checked-in fixture itself, not
an external generator or path. The metadata envelope is part of the byte
sequence of `.txt` / `.md` / `.py` fixtures but is excluded from the
parsed payload; for `.json` fixtures the envelope lives in a sibling
`.meta` file and the payload file is pure JSON.
