# Proposal: session_state wants the keyed-blob wire, not a flattened row

- **State:** RESOLVED against its own recommendation. The flattened row was
  taken, because the objection to it was fixable and got fixed. The family is
  migrated and served.

`session_state` was one of six sources in the `sessions` family. The other five
are what that family's doc describes ("server and webchat session rows") and
they have migrated. This one is the hook's guardrail state for a session, which
is a different thing that happened to have "session" in its name, so it sits in
a family of its own rather than holding five ready sources back.

## Why it looked like it did not fit the fields wire

`session_state_t` is a scalar record plus five collections:

    char seen_paths[MAX_SEEN_PATHS][MAX_SEEN_LEN];
    worktree_mapping_t worktrees[MAX_WORKTREES];
    tdd_write_t tdd_writes[MAX_TDD_WRITES];
    char read_paths[MAX_READ_PATHS][MAX_SEEN_LEN];
    file_read_hash_t file_hashes[MAX_FILE_HASHES];
    ap_session_hit_t ap_hits[MAX_AP_SESSION_HITS];

and `db1_session_state_save` writes it as a scalar upsert plus a delete-and-
reinsert of each child table, inside one transaction.

Flattening that onto `db1-fields-v2` means several hundred cells per call (386,
as declared), most of them empty most of the time, with the arity fixed at the
declared maxima. The wire carries it. The stated cost was that every future
field added to any of those six structs is a silent change to the frame's
width, and the failure mode when somebody forgets is that a guardrail's memory
of what it has seen comes back short, which reads as "this session has not
touched that file yet".

## Two claims in this doc were wrong

**The transaction was not a cross-call transaction.** This family was also
written up as blocked on `db1-transactions-across-the-boundary`. It is not: the
begin and the commit are both inside `db1_session_state_save`, nine lines apart
from the top of the function and 218 lines apart from each other, with no
return to the caller between them. One call, one transaction, which is exactly
the case the boundary already serves. The blocker was asserted from the shape of
the problem rather than checked against the code, and checking it took minutes.

**The silent-width fear was real, and is now structural rather than remembered.**
The objection above is the correct objection, and it is the reason this doc
recommended the blob. But "somebody forgets" is a property of the generator, not
of the wire, and the generator can be made incapable of it. It now is: declared
struct members are validated against the members actually parsed out of the
header, exactly and in order, for all 63 carried structs. Adding a field to
`file_read_hash_t` and not declaring it does not produce a short frame. It
fails the build with the member named. The failure mode the blob was chosen to
avoid cannot occur on either wire now.

With that gone, the blob's remaining advantage is frame size, and its cost is a
hand-written serialiser and its inverse. The one piece of code in the design
whose bugs are silent, since a flattening that drops the last element of one
array passes every test that does not specifically fill that slot.

## What the survey said, and why it was still not a verdict

The wire survey reported `guardrail_state 5 / 5`, every operation fits. That
count was right about the SIGNATURES and uninformative about the shape: it
inspects parameters, and `const session_state_t *in` is one parameter whether
the struct holds three members or three hundred. The survey remains a starting
point rather than a verdict; it happened to agree with the outcome here.

## What was built

Seven operations over a 386-cell row, on `db1-fields-v2`. `uint64` was added to
the generator for `file_read_hash_t.content_hash`, since a hash is not a signed
quantity and the decimal-text wire had no unsigned 64-bit kind.

The round-trip test the doc asked for exists, and fills the *ends* of every
collection, `seen_paths[63]`, `read_paths[63]`, `worktrees[15]`,
`tdd_writes[7]`, `file_hashes[63]`, `ap_hits[31]`, because a truncation that
loses one trailing element is precisely what a test filling the first two slots
of each array would miss. `content_hash` is stored as `UINT64_MAX - i`, every
value above `INT64_MAX`, so a signed round-trip anywhere on the path lands
negative and fails loudly.

One finding from writing it: the domain's load of `session_state_file_hashes`
has no `ORDER BY`, unlike `seen_paths` which orders by `seq`. Rows come back in
SQLite's order (lexical by path, as it happens: `/hashed/0, /hashed/1,
/hashed/10 … /hashed/9`). That predates and is unaffected by the migration, so
it was left alone and the test asserts the set rather than the positions. If any
caller does depend on hash order, it depended on an accident before this change
and still does, worth a look by whoever owns the guardrail, which is why it is
recorded here.
