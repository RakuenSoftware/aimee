# Proposal: session_state wants the keyed-blob wire, not a flattened row

- **State:** OPEN — the shape is the question, and choosing it wrong loses
  guardrail state silently.

`session_state` was one of six sources in the `sessions` family. The other five
are what that family's doc describes — "server and webchat session rows" — and
they have migrated. This one is the hook's guardrail state for a session, which
is a different thing that happened to have "session" in its name, so it now sits
in a reserved family of its own rather than holding five ready sources back.

## Why it does not fit the fields wire

`session_state_t` is not a row. It is a scalar record plus five collections:

    char seen_paths[MAX_SEEN_PATHS][MAX_SEEN_LEN];
    worktree_mapping_t worktrees[MAX_WORKTREES];
    tdd_write_t tdd_writes[MAX_TDD_WRITES];
    char read_paths[MAX_READ_PATHS][MAX_SEEN_LEN];
    file_read_hash_t file_hashes[MAX_FILE_HASHES];
    ap_session_hit_t ap_hits[MAX_AP_SESSION_HITS];

and `db1_session_state_save` writes it as a scalar upsert plus a delete-and-
reinsert of each child table, inside one transaction.

Flattening that onto `db1-fields-v2` means several hundred cells per call, most
of them empty most of the time, with the arity fixed at the declared maxima. The
wire would carry it. The cost is that every future field added to any of those
six structs is a silent change to the frame's width, and the failure mode when
somebody forgets is that a guardrail's memory of what it has seen comes back
short — which reads as "this session has not touched that file yet".

## What the survey says, and why it is not the answer

The wire survey reports `guardrail_state 5 / 5` — every operation fits. That
count is right about the SIGNATURES and wrong about the shape: it inspects the
parameters, and `const session_state_t *in` is one parameter whether the struct
holds three members or three hundred. The same over-count has appeared before
in this migration, always in the flattering direction, and it is why the survey
is a starting point rather than a verdict.

## The shape that fits

`db1-keyed-blob-v1` already exists and `economizer_state` already uses it:
state_load and state_save carry an opaque document under a key. That is what
this is — a per-session document that only the guardrail interprets.

The work is a serialisation of `session_state_t` and its inverse, with a test
that round-trips a fully-populated struct member by member. That test is the
whole point: a flattening that silently drops the last element of one array
would otherwise pass everything else.

Where that serialisation should live is the open question. It is not storage,
so by the rule the rest of this migration has followed it belongs outside the
module -- but the module has to write the child tables from it, which means the
module needs to read it too. `delegate_learning` hit the same shape and was
resolved by moving the conversion to the caller and passing the converted value
across; whether that works here depends on whether the child tables are worth
keeping as tables at all, or whether the document should simply be stored whole.

That last question is a schema decision, not a wire one, which is why this is
written down rather than answered.
