# Deterministic JSON fragment grammar

This corpus is the pre-detector oracle for the collapse detector. Every
fixture is a UTF-8 text file checked in under `tests/fixtures/`. Generator
output is not relied upon; the byte sequence on disk is the source of
truth.

## Byte semantics

* All offsets are absolute byte offsets from the start of the checked-in
  file. Byte 0 is the first byte of the file.
* `expected_loop_start_offset` is the byte offset of the first byte of the
  repeated region. For `fire` fixtures this is the byte immediately after
  the envelope ends, i.e. the offset of the first byte of the first loop
  iteration in the file.
* `expected_loop_span_bytes` is the length, in bytes, of **one loop
  period** (the unit that repeats verbatim). It is NOT the length of the
  entire repeated region. The number of repetitions is declared in the
  fixture header.
* For `no-fire` fixtures both fields are `-1`.

## Metadata envelope

Every fixture has a metadata header that declares its shape, expected
detector outcome, and oracle bytes. Two envelope forms are accepted, both
deterministic and machine-parseable:

1. **Sibling `.meta` file.** A file with the same stem plus `.meta`
   extension holds the header. Used when the payload must remain a
   standalone parseable value (e.g. `.json`). The payload file contains
   no header bytes.
2. **Inline header line.** For non-parseable text payloads (`.txt`,
   `.md`, `.py`), the header is the first line of the file, beginning
   with `# shape:`, terminated by a single LF (byte `0x0A`). The payload
   begins at the byte immediately after that LF.

The envelope extraction step is deterministic: given a fixture path the
loader first looks for `<path>.meta`; if present it parses that file as
the header and treats the entire payload file as the body. Otherwise it
reads the first line of the payload file up to (and including) the first
LF as the header, and the payload begins at `first_lf_offset + 1`.

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

## Detector oracle and metrics

Each fixture header declares `fire` or `no-fire`, an absolute
`expected_loop_start_offset`, and `expected_loop_span_bytes`. As above,
the span is the length of one loop period, not the full repeated region.

A `fire` fixture is correct when the detector reports the declared loop
period starting at the declared offset (within byte tolerance). A
`no-fire` fixture is correct when the detector reports no loop.

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
