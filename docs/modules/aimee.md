# aimee module

The core module for functionality specific to **aimee-server**, served as a bus
process. Principal ref `31`; outbound client ref `67` (`aimee-db1`). Runtime Go,
sources under `server-go/modules/aimee/`.

## Purpose and non-goals

`aimee` is where aimee-server-specific domain logic lives, as distinct from the
generic `postgres` module (which owns PostgreSQL and carries nothing
domain-specific) and the `control-plane` module (which takes the aimee-kb side).
It was created after the Go-first ruling in `docs/dev/GO_REWRITE.md`, so it has
no C implementation to migrate from.

Today it carries **session peer messaging**: the verb by which one aimee session
addresses another as a peer rather than as a delegate or a client.

Non-goals: it does not own the session directory (`db1` does), it does not own
turn arbitration (the C presence registry does), and it is not a general
pub/sub bus: the module event bus is the event substrate.

## Public contracts

Stages, whose event kinds derive from the bus formula
`4096 + principal_ref*256 + stage` rather than being written by hand:

- `peer-delivery` (stage 1, kind 12033): `OpSend`, `OpReply`, `OpCancelWait`.
- `peer-inbox` (stage 2, kind 12034): `OpInboxLen`, `OpInboxPeek`, `OpInboxTake`.
- `peer-grant` (stage 3, kind 12035): `OpGrant`, `OpRevoke`, `OpGrantExists`.
- `peer-channel` (stage 4, kind 12036): `OpChannelJoin`, `OpChannelLeave`,
  `OpChannelSend`, `OpChannelMembers`.

The wire is `db1-fields-v2`, reused rather than reinvented:
`op(u32) | field_count(u32) | (len(u32) | bytes) * field_count` for requests and
the same shape with a leading `status(u32)` for responses.

## Dependencies and consumers

- `audit`: peer sends are action-class events; the tap records each with its
  verdict.
- `config`: hop ceiling and inbox bounds.
- `db1`: the session directory (`server_sessions`) and, once durable, the
  `peer_inbox` and `peer_grants` families.
- `execution-policy`: the pre-delivery verdict on an action-class send.
- `module-runtime`: process lifecycle, admission, and the stage table.

Consumers are the session/turn layer when a model invokes a peer verb, the `/v1`
surface for thin clients, and `protocols` for MCP -- which is no longer future
work: `peer_send` and `peer_inbox` ship in the MCP tool table, and an external
agent addresses another aimee session through them.

## Providers and readiness

The module is ready as soon as its stages are advertised: `Stages()` is static
and its handlers hold no external connection. `DirectorySource` is the one
optional provider: when absent the registry falls back to its own in-memory
view, which exists for tests and for bringing a process up before `db1` is
reachable, and is never a second source of truth in production.

`DirectorySource` answers one question only: **who exists**. Labels are this
module's own state, so there is no second answer to reconcile for them; what is
not local is whether the session behind a label still exists, and a label match
is confirmed against the directory before it is handed out.

**Existence has three answers, not two:** present, definitely absent, and
could-not-say. db1 distinguishes them (`StatusOK`, `StatusMissing`, a failure)
and the Go caller side preserves the status word, so this module preserves it
too: `ErrNoPeer` for a definite absence, `ErrDirectoryUnavailable` for a
directory that did not answer, and `StatusUnavailable` on the wire.

That is the one refusal a caller should retry on. Every other status here is a
fact about the request; this one is a fact about the moment. Reporting it as
`no_peer` would turn a momentary outage into a permanent conclusion, since a
caller told "no such peer" correctly stops asking, and would let the
undeliverable sweep destroy mail on a transient fault.

`Owner` is the single decision point, and there is no separate `Exists`: two
functions answering nearly the same question is how they come to disagree, and
they did. One short-circuited on a local entry while the other let the
directory's denial outrank one. The second is right, because under the
undeliverable rule mail held for a departed session is exactly what that session
leaves behind rather than evidence it is there.

The negative is the case that matters. A caller told "no such peer" correctly
stops asking, so a wrong negative is silent and terminal: no error is raised, no
retry happens, and nothing ever revisits it. An authoritative negative is a
fact; a fallback negative is a guess, and the two are worth telling apart.

## Configuration and activation

- `runtime_toggle.supported`: `false`. The module is either admitted at startup
  or absent; there is no live enable/disable, because a session mid-ask would
  have no defined outcome if delivery vanished underneath it.

The module is **required** rather than optional, so it carries no
`enabled_by_default` key at all. It was briefly optional, which was actively
wrong: an optional module with `enabled_by_default` false is declared and never
spawned, which is how peer messaging came to be green in every test and absent
from `server.modules`.

Bounds are compile-time defaults in `peer`: `SessionsMax`, `GrantsMax`,
`InboxMax`, `ChannelsMax`, `ChannelMembersMax`, `DefaultMaxHops`,
`MaxTextBytes`, `DefaultWaitExpiry`.

Every collection the module owns is bounded, and the first two were not: the C
registry capped both the session table and the grant table, the Go port dropped
both caps, and the bounds comment went on asserting a property the code no
longer had. An unbounded grant table is also an unbounded authorization surface.
Both refuse at the ceiling rather than evicting, since evicting would discard an
inbox somebody is waiting on and choose the victim by map order.

## Surfaces

**The MCP tools are the surface that matters, and this section used to omit
them.** They are how a model reaches another session; everything below is how
other processes do.

`peer_send` and `peer_inbox` live in the server's MCP table (`peer_client` in
`src/peer_client` speaks db1-fields-v2 to this module's stages) and are folded
into one `peer` family, `command=send|inbox`, which sits in `MCP_CORE_TOOLS`.

The floor placement is load-bearing rather than a nicety. Tools outside it are
hidden from the initial `tools/list` and reachable only through
`find_tools` -> `describe_tool` -> `call_tool`, and these shipped that way once:
served, working, and never shown to a client. Every other capability withheld
from the floor has a fallback an agent reaches for instead; this one has none.
There is no clumsier way for a session to reach another, so the alternative to
being shown the tool is not coordinating at all.

The sender is the CALLING session, taken from the call rather than accepted as
an argument. A `from` parameter would let any caller claim to be any session,
putting the forgery a layer above the provenance stamping that exists to stop
exactly that.

Both tools are also NATIVE (`core`), so aimee's own agents get them. A live-model
run confirmed the identity half of that: a native chat turn's session reaches the
directory, appearing in `server_sessions` as `driver|chat` via
`chat_session_register`, so what a native caller sends as is addressable. What has
never been observed is a MODEL choosing to emit the call -- that run stopped at
provider credential provisioning, not at anything here. See
docs/validation/aimee-module-on-a-clean-container.md.

Bus stages are the module's interface to other modules. A `/v1` HTTP edge in
`peer/http.go` serves thin clients directly: `GET /v1/sessions/peers`,
`POST /v1/sessions/{id}/peer`, `GET /v1/sessions/{id}/inbox`,
`POST /v1/sessions/{id}/inbox/take`, and `POST /v1/peers/grants`.

`POST /v1/sessions/{id}/label` sets a session's addressable handle, and this
module is its only writer.

That route was briefly removed on the reading that a label is db1's
`server_sessions.title`. The reasoning was right and the premise was wrong: that
column has **no writer at all**. The session insert stores the literal empty
string and no statement updates it, so deferring to db1 would have left peer
addressing with nobody able to set a name.

A label is not a session title. A title is a display name for a session; a label
is the handle peer messaging is addressed by, and it is per-session state of
exactly the kind this module already owns beside inboxes. Matching them by
column shape rather than by meaning was the original error.

That edge fails closed: without an `Authorize` hook every session route answers
503, because a peer surface that authorizes by accident is worse than one that
is switched off.

## Data and migrations

Inboxes and grants are the module's own state. Durable storage arrives through
the `postgres` module's generic wire under owner `aimee`, with `peer_inbox`
numbered version 1 and `peer_grants` version 2: explicit numbers rather than
values derived from sorted filenames, so a file whose name later sorts into the
middle cannot renumber the history and invalidate every recorded checksum.

**A migration owner is a schema namespace, not a module, and `aimee` is chosen
here permanently.** Nothing keyed on it may be renamed: a recorded migration is
a fact about a database ("these statements ran, with this checksum, at this
time"), and that fact does not stop being true because the module that applied
it was reorganised. Renaming an owner does not move its history, it orphans it,
and the cost grows with every version.

So this module will legitimately own **two** namespaces once the db1 absorption
lands: `db1`, carrying the store's existing 21 versions, and `aimee`, carrying
these two. That is not a tidy-up waiting to happen. Collapsing them would
re-run 21 migrations against a database that already has them, and the first
`CREATE TABLE` would fail. The engine keys version, gap checks, checksums and
the advisory lock on the owner, so the two sequences advance independently.

**Length checks on these tables must use `octet_length`, never `length`.**
`length()` counts characters and a byte buffer holds bytes; at the same number
they disagree the moment a value is non-ASCII, and the check passes while the
buffer overflows. `text`, `from_owner` and `from_label` are all candidates, and
`text` most of all: a peer message body is the most likely thing in this system
to carry multi-byte characters. The cheap detection rule needs no C header and
no wire declaration: a `length()`-checked `TEXT` column with no ASCII-restricting
regex is suspect on its own.

The same confusion has a Go form, already fixed here: `Preview` bounds a
notification excerpt in BYTES but cuts at a rune boundary, because slicing a
string at a byte offset splits whatever character straddles it and puts invalid
UTF-8 into a live notification.

**A test database must be created UTF8 with `TEMPLATE template0`.** Under
`SQL_ASCII`: which is what `initdb` gives you by default in a bare container -
`char_length()` and `octet_length()` are the same function, so an assertion
about the difference between them passes whether or not the schema is right.
That is unfalsifiable on precisely the property it exists to prove. Check the
encoding before trusting any `octet_length` assertion here.

`db1` carries no cross-family foreign keys, so `peer_inbox` cannot reference
`server_sessions` and nothing cascades when a session is deleted. The rule is
therefore explicit: **an inbox row whose session no longer exists is
undeliverable, not delivered.** The message was never drained, so recording it
as delivered would put a falsehood in the audit trail. The sweep runs after
`server_session_delete_expired` rather than on a timer of its own, so it cannot
race ahead of the lifecycle that creates the orphans.

**The sweep must be one transaction, and it must not read a closed one as an
ordinary refusal.** It deletes the orphaned rows and records each as
undeliverable, and those two are the same fact told twice: mail that went
nowhere, and the record saying so. SQLSTATE `25P01` means the transaction is
already gone and everything written through that handle with it, which is a
different thing from a constraint failure leaving the transaction live with one
statement wrong. Code that collapses them logs "could not record that one,
carrying on" and then commits nothing: rows deleted, nothing recorded, no error
raised. Mail destroyed while the audit trail says it was accounted for.

## Security and privacy

**A peer is not an operator.** Peer content carries no authority: it cannot
grant, widen, or borrow a receiver's capabilities, and a receiver's
`execution-policy` decision is always evaluated against the receiver's own
principal. A message must never be rendered as a user turn or satisfy a
confirmation.

Provenance is unforgeable because `FromSession`, `FromOwner` and `FromLabel` are
read from the sender's own directory entry under the registry lock, never taken
from an argument. Same-owner peering is implicit; crossing owners requires a
directed grant, and a grant says nothing about the reverse direction.

## Supported journeys

A model in session A lists its peers, addresses one by label, and sends. The
message waits in B's inbox until B drains it at the head of its next turn, so a
turn in flight is neither interrupted nor raced.

For an answer, A sends with `expect_reply`, which records the wait-for edge and
returns immediately; A then polls its own inbox for the correlated reply. A
Codex-backed session and a Claude-backed session converse this way with no
vendor-specific code, because a peer is addressed by session id and its backend
is invisible to the sender.

## Tests and failure behavior

`server-go/modules/aimee/module_test.go` pins the stage advertisement against
`process-contracts.json`; `wire_test.go` covers framing, NUL rejection, row
division and status mapping; `peer/peer_test.go` and `peer/http_test.go` cover
the registry and the HTTP edge under `-race`.

Failure behavior distinguishes three levels deliberately. A malformed frame is
`ModuleStatusInvalidRequest`: the module could not understand the question. A
refusal (`hop_limit`, `cycle`, `denied`, `inbox_full`, `no_peer`) is a
**successful** invocation carrying a domain status, because collapsing those two
would leave the tap unable to tell "the module is broken" from "the module said
no". And a question whose truthful answer is negative: does this grant exist,
is there mail: answers `StatusOK` with the "no" in a field, because a tap
seeing a steady rate of non-OK cannot tell working-as-designed from broken.

The test for which of the last two applies is whether the caller must do
something differently. `denied` on a send is a refusal; `grant_exists` returning
false is an answer. One case sits on the line and is deliberately a refusal:
reading the inbox of a session that does not exist, since "no such session" and
"no mail" are indistinguishable as a count, and answering `StatusOK` with zero
rows is how a caller polling for a reply waits forever on a session that was
torn down.

## Operational diagnostics

`OpInboxLen` reports both depth and a monotonic `dropped` count, so an inbox
that overflowed is diagnosable rather than silently lossy. `OpChannelSend`
answers `StatusOK` for the FAN-OUT and carries a per-recipient outcome in each
row, so a partial delivery is visible rather than collapsed into one word.
`OpInboxTake` leads its reply with the number of messages REMAINING, because
rows alone cannot distinguish a complete drain from a capped one and a caller
that assumes the former simply stops asking.

Every status names one fact, and an unrecognised one names itself
`unknown_status` rather than borrowing a real name. The default used to return
`bad_request`, which hid four errors that were never mapped at all: two of them
capacity (`at_capacity`), where a caller told its request was malformed goes on
correcting arguments that were never wrong while the table stays full. A test
asserts every sentinel error maps to a status of its own, and asserts the count,
since Go cannot enumerate package-level vars and the list would otherwise go
stale. Every refusal has a
distinct status name (`Status.String`) rather than collapsing into
`bad_request`.

The bus caps a module at `moduleMaxInFlight` (16) and refuses the seventeenth
invocation with a generic `Internal` status that does not read as backpressure.
No handler here blocks, specifically so that ceiling is never reached by waiting.

## Compatibility

### Every guard here has been made to fail

Each check in this module was mutation-verified: the property it asserts was
broken deliberately and the guard watched to fire. That is recorded because a
guard nobody has seen fail is a guard nobody has evidence for, and this module
found three of its own that could not have failed -- a probe that could not
detect an inert module, a record satisfied by prose quoting it, and an
experiment that passed because nothing reached the function it was testing.

Two of them were audited late, after a peer found a test of their own passing for
a reason it did not state. Their credential-stripping test passed with the strip
DELETED, because an unrelated path rule dropped the credential anyway. So the
audit here was not "did I write a mutation" but "does each CLAUSE matter": the
admission tests fail without directory admission, and the sender remap fails
separately from it, so "I do not know you" and "there is no such peer" are held
apart by something rather than by coincidence.

### A caller can name what to retry

Twenty-one statuses cross this wire and every one of them is a named constant, so
a caller can test for any single one. That is not the same as being able to
classify them, and the question a caller actually has is not "what does this
status mean" but "do I try again".

That was answered by a doc comment on one constant claiming it was the only
retryable status. The claim was false for four others -- an inbox drains, a
timeout may be answered next time, a full channel loses a member, a full table
frees a slot -- so a caller believing it would loop forever on an unreachable
store and give up on an inbox that clears in seconds.

`Status.Retryable()` is the answer that comment was pretending to be. The line is
the one this module draws everywhere: a decision about the REQUEST is permanent
and the caller must change something or stop; a fact about the MOMENT is not.
It is exhaustive rather than defaulting, and a test asserts every declared status
is classified, because an unrecognised status returning "do not retry" is safe
for the caller and wrong for the work -- it silently drops what would have
succeeded.

### A refusal is not an outage

The directory has FOUR outcomes, not three, and the fourth was found on the last
pass of the night. `directory_refused` is db1 understanding a request and
declining it -- the module asked for something the store will not accept.

It is distinct from `unavailable`, which means retry; from `bad_request`, which
blames a caller whose request was fine; and from `unclassified`, which means an
error the module could not name. This one is named, the caller cannot fix it, and
retrying it is pointless.

It exists because a refusal was reaching callers as `unavailable`. `Registry.Owner`
wrapped anything that was not absence into "did not answer", with `%v` rather
than `%w`, so the chain broke and a permanent defect in a request THIS MODULE
built arrived as a transient condition. A caller obeying that status retries
forever.

The isolated tests were no help: the directory's own mapping was correct, and the
registry's own wrapping was reasonable. Only running the two together shows what
a caller receives, which is the argument for testing across a seam rather than on
both sides of it.

### The wire has ONE boolean grammar, and it is peerwire's

`peerwire.Btoa` writes `"1"`/`"0"`; `Atob` reads those and `"true"`/`"false"`.
The C client in `src/peer_client` accepted only the two words `Btoa` never
writes, so it rejected every message row this module has ever sent -- while
`Atob`'s leniency accepted the client's REQUESTS, so the send direction worked
and only replies broke. Half a conversation working is worse than none: at the
caller it reads as this module failing.

Both suites stayed green. The C fixture spelled the cell `"false"`, having been
written from the same misreading as the code it checked, and `cwire_test.go`
pinned the status numbers and the row width but nothing about what is IN a cell.
`TestCClientSpeaksTheSameBooleanGrammar` now asserts that against `Btoa`'s real
output, in both directions.

The lesson is not about booleans. Two sides agreeing on a frame's SHAPE and
disagreeing about a cell's spelling produces no error anywhere -- the reply is
well-formed, the count is right, and the value is unreadable.

### The earlier "daemon-originated send wedges the module" reading was wrong

An earlier revision of this document reported a send that entered the handler
and never returned, and pointed at the bus host. That was a defect in the
diagnostic, not in the module: the timing harness printed `0.00s` for every call
because `bc` was absent, and the "only one bus failure was logged" claim came
from a log captured before the runs it was applied to. The handler was returning
normally the whole time; the client was rejecting its reply.

Recorded because the wrong reading was confident and specific, and the thing
that corrected it was making the client NAME its failure rather than summarise
five of them.

### The module as deployed has no session directory

`aimee-module` builds the capability with `NoDirectory{}`, and that is accurate
rather than provisional. There is no `DirectorySource` yet -- it needs db1's
session family, which arrives with the absorption -- and nothing else populates
the registry: `Register` has no caller outside tests, and no bus op reaches it.
No session can exist, so **peer messaging is inert in this configuration** and
the session-scoped stages answer `no_directory`.

The grant stage is still served, because grants are owner-to-owner and need no
session directory. Refusing them would be a second wrong answer and would hide
that the module is otherwise healthy.

`no_directory` is deliberately not `unavailable`. Unavailable is the one status
a caller should retry on -- a fact about this moment. This one is a fact about
how the module was built, and a caller retrying against it never stops.

The reason this is written down rather than left to the code is that it was
invisible for a whole validation cycle. The construction site passed `nil`, and
`nil` meant "answer existence from the registry's own map". Every session-scoped
call then refused with `unknown_sender` or `no_peer`: answers about the CALLER'S
session, from a module that could not know about any session. Fifteen checks in
the container run passed against exactly that, and had to -- a correct refusal
and a module that can never do anything produce the same word. The channel case
was worse than a refusal: `members` answered OK with none, a healthy-looking
reply that read as the feature working.

So `nil` is now refused at construction. The two real configurations are named:
`LocalDirectory{}` claims the registry's own map is the truth, which is what the
tests and an in-process host use; `NoDirectory{}` states there is none. They
answer the same call oppositely, which is why one of them has to be said out
loud.

Message rows are a fixed width and new cells **append**, so a reader built
against an older width never has its field numbering shift underneath it.

The ENCODER is pinned against the struct, because that side has no wire to be
strict about: a decoder cannot object to a field the encoder never wrote, so a
field added to `peer.Message` and not carried would arrive zero-valued and look
exactly like one the sender left empty. A test asserts the field count matches
the row width. `SendOptions` is deliberately richer than the wire (`WaitExpiry`
is not carried, since bus callers take the default) and that exclusion is
asserted by name rather than assumed, so the next field added has to be
classified rather than silently dropped. A list
reply carries no row count: the caller divides by the row width and refuses a
remainder rather than accepting a short final row.

Stage ids and event kinds are stable once declared; a stage declared in
`process-contracts.json` but not advertised by `Stages()` is never available and
every call to it returns `CAPABILITY_ABSENT` while the module runs normally.

## Extension and removal

New capabilities join as new stages with the next free stage id, appended to both
`Stages()` and the component's `stages` in `process-contracts.json`: the two are
compared by test, so neither can drift. New Go files must be added to
`go_sources` in `src/modules/aimee/module.yaml`; an undeclared file owning state
is the defect this module's own history illustrates.

Removing a stage means removing it from both places in the same change. The
module can be removed entirely only when no session depends on peer addressing,
since a caller blocked on `expect_reply` resolves through its own inbox rather
than through a live connection.
