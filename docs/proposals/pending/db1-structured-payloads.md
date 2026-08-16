# Proposal: DB1 structured payloads are two changes, not a redesign

`docs/proposals/pending/db1-wire-capability-survey.md` measured what the rest of
DB1's migration needs and found 240 of 281 operations sitting in sources that
need "T3 — structured payloads", with the note that T3 needs a design rather
than an addition. This is that design, and the finding is that T3 is smaller
than its name: a counted reply, and a rule that a struct is its members.

## What the payloads actually are

Ninety-two structs are declared across DB1's headers. **Eighty-two contain no
pointer at all** — fixed `char` arrays and scalars. Of the ten that do, nine
carry `const char *` strings, which are values like any other. The tenth is
`db1_secret_backend_t`, a vtable of function pointers, and **no operation with a
linked caller takes it**.

The 109 T3 operations break down as:

| shape | count | what it is |
| --- | --- | --- |
| `T *out, int max` | 41 | a list of rows |
| `T *out` | 40 | one row |
| `T **out`, `char **out` | 24 | the same, allocated by the callee |
| `const T *` | 12 | one row, inbound |

Two operations take a `cJSON *` directly. A JSON tree is already a document, and
the wire already carries documents.

Median struct size is seven members; the largest is thirty-six.

## The design

### A struct is its members

Nothing about a struct needs to cross as a struct. The catalog already describes
an operation's fields as a list of `{name, type, required}`, and a struct is a
list of exactly that shape. So a struct parameter contributes its members to the
frame, and the generated code marshals them at the boundary — the domain still
sees the struct it has always seen.

This needs no frame change. It needs the member list in the catalog, which is
the same information the header already states, and which the generator can
render into the same counted fields it emits today.

`AIMEE_DB1_FIELDS_MAX` is derived from the widest request, so a thirty-six
member row widens it to thirty-six on its own. That constant already sizes a
pointer array rather than a payload buffer, so the cost is a few hundred bytes.

### A reply is counted, the same way a request is

This is the only frame change, and it is the one thing every remaining shape
waits on:

    reply: status(u32) | count(u32) | (len(u32) | bytes) * count

A single value becomes `count = 1`. A struct out-parameter becomes its members.
A list of rows becomes `rows * members`, with the row width implied by the
operation's declared member list rather than sent separately — an operation
knows how wide its rows are.

An allocated out-parameter (`T **out`, `char **out`) is not a wire shape at all:
the values arrive the same way, and the generated client allocates what the
caller is documented to free. The contract the callers already follow does not
move.

### The tiers were the wrong shape

The survey separated T2 ("a richer reply") from T3 ("structured payloads"), and
recommended T2 only when a family wanted it. That separation does not survive
contact with the design: **the counted reply is the foundation of both.** T2 is
not a small feature to be deferred; it is half of T3, and doing it alone would
unlock seven more sources on the way.

## What this does not solve

Three things stay outside the wire, and none of them are payload shapes:

- A return contract richer than success/failure still needs somewhere to put the
  distinction. A counted reply gives it one — a status field beside the values —
  but choosing what `db1_wfe_bind`'s -2 becomes is a contract decision per
  operation, not a frame feature.
- `delegation` remains coupled to `agent_jobs` for reasons recorded in
  `coupled_sources`, and needs an idempotency story before it crosses.
- `identity` remains on the auth path.

## Sequence

1. ~~**Counted reply.**~~ Done: the reply is `status | count | (len | bytes) *
   count`, and the format is named `db1-fields-v2` because the frame changed.
   Family 1's keyed blob is a different format and did not move.
1. **Superseded — kept for the sequence.** One frame change, both generated sides, family 1's keyed
   blob untouched. Unlocks the seven sources the survey attributed to T2, and is
   prerequisite for everything below.
2. **Struct flattening.** Member lists in the catalog; marshalling in the
   generator. No frame change.
3. **Rows.** A list is a struct repeated, once one row can cross.
4. **Migrate**, now by whole source, since that is the unit that can move.

Steps 1 and 2 are each a day's work of the kind already done twice — the integer
and optional-field changes have the same shape, and the same proof is available:
generate, diff the shipped output for byte-identity, and run the existing suites
against generated code.

## How to check this

`scripts/survey_db1_wire.py` reports readiness per whole source. If this design
is right, implementing steps 1 and 2 should move the sources that are reachable
in principle into sources reachable in practice — and those numbers should be
re-derived from the script rather than read from this document.
