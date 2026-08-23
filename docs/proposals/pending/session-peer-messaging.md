# Proposal: session peer messaging — one aimee session addresses another

- **State:** DRAFT — 2026-08-23; awaiting roundtable review. P1/P2 implemented in the `aimee`
  module (see [Implementation status](#implementation-status)).
- **Owner:** the **`aimee` module** (principal ref 31, outbound client ref 67), Go, sources under
  `server-go/modules/aimee/`, declared in `src/modules/aimee/module.yaml` and
  `src/modules/process-contracts.json`, documented in [`docs/modules/aimee.md`](../../modules/aimee.md).
  See [Ownership and language](#ownership-and-language) — this is the section a reviewer should read
  first, because two earlier drafts of this proposal got the placement wrong in two different ways.
- **Owns:** the verb by which one aimee session addresses another as a **peer** — the peer
  message envelope and its provenance, the inbox, cross-owner grants, the three interaction modes
  (send / ask / channel), and the loop-and-deadlock bounds that keep peer traffic from eating the
  appliance. It solely owns that state. It does **not** own the session directory — `db1`'s
  `server_sessions` already does, and a second directory would mean two tables disagreeing about
  which sessions exist.
- **Consumes (does not modify):** [`docs/dev/GO_REWRITE.md`](../../dev/GO_REWRITE.md) — the
  language-ownership decision and its non-negotiable contracts; the unified-presence registry
  (`src/headers/presence.h`, still C-owned) for turn arbitration, attachments, and the
  presence-event stream, **read only**, over `/v1`; the module event bus and its governance/audit
  tap ([`event-bus-wire-spec.md`](event-bus-wire-spec.md),
  [`event-bus-governance-and-capture.md`](event-bus-governance-and-capture.md)) via the Go bus
  client in `server-go/bus`; the thin-client↔Runtime mTLS transport
  ([`tiered-llm-p8-thinclient-mtls.md`](../done/tiered-llm-p8-thinclient-mtls.md)); the MCP/ACP
  protocol surfaces (`src/modules/protocols/`).
- **Does not consume:** the delegate subsystem. Delegation is hierarchical and owns its child's
  lifecycle; peering is lateral and owns nothing. See [Non-goals](#non-goals).

## Thesis

Every inter-agent path in aimee today is **hierarchical and outbound**. A session spawns a delegate
(`src/modules/delegates/`), drives a provider CLI (`src/server/cli_codex.c`,
`src/server/provider_cli_adapter.c`), or drives an external ACP agent (`src/server/cli_acp.c`).
An editor attaches *to* a session (`src/modules/protocols/acp/acp_server.c`). In every case the
relationship is parent→child or client→session.

There is no verb by which a session addresses another session **as a peer** — no way for the model
driving session A to say "ask B" where B is a running, independently-owned, equally-capable session
with its own transcript, workspace, and operator.

The missing piece is exactly one direction: **inbound peer delivery**. Nothing turns a message from
session B into an addressable item in session A with sender provenance and a route back. This
proposal adds that, and the verbs over it.

## Ownership and language

**This family is Go, and that is not a stylistic preference.**
[`docs/dev/GO_REWRITE.md`](../../dev/GO_REWRITE.md) is a standing decision:

> Aimee is being rewritten as Go-owned services. C is not an ownership boundary in the target
> architecture. […] a C process […] must not remain authoritative for state, scheduling, artifacts,
> credentials, policy, or recovery.

Peer messaging introduces new session control-plane state — a directory, inboxes, an authorization
table. Migration step 3 of that document is explicitly "Move the remaining DB1 API families and
session/auth control plane to Go one family at a time, maintaining one writer per family."
Implementing peer messaging in C would create exactly the C-owned state that step then has to
migrate.

**The single-writer rule is satisfied, not bent.** Contract 5 ("One authority during cutover") is
per family:

| State | Writer | This family's access |
|---|---|---|
| Peer inboxes, cross-owner grants | `aimee` (ref 31) | owns, sole writer |
| Session directory (`server_sessions`) | `db1` (ref 30) | **reads** over the bus, never writes |
| Turn lock, attachments, event ring | C presence registry | **reads**, never writes |

**Why co-location with the turn lock is unnecessary.** The plausible argument for C was that
acceptance A2 (a peer message must not preempt or race a turn in flight) needs to be atomic with the
turn lock. It does not, because **delivery is pull-based**: `Send` appends to an inbox and returns,
and the receiver drains it at a moment of its own choosing via `Take`. A message is never atomic
with a turn, so it does not need to live where the turn lock lives. The same property is why `Ask`
cannot wedge a peer that is busy — it waits on its *own* inbox, never on the peer's turn.

The only real coupling is the live announcement (D3), which is one outbound call across the seam,
not a reason to own state in C.

**Contract 1 (no silent truncation) is honored directly.** An over-long message body is
`ErrTooLong` — an explicit refusal — never a stored prefix. The bounded `Preview` on a live
notification is labelled an excerpt and is never the artifact; the inbox always holds the full text.

## Topology: the Runtime is the sole broker

Sessions live server-side. Per
[`remote-first-session-start.md`](remote-first-session-start.md), aimee runs as a thin client
against a remote aimee-server and *there is never a co-located server*; session state is
Runtime state, and a client is an **attachment** to it.

Therefore:

> **Peer routing is Runtime-local. It crosses machines because attachments are remote, not because
> Runtimes federate.**

Two agents running on different physical hosts converse because both their sessions are registered
in the same broker. There is no Runtime↔Runtime federation, no peer-to-peer socket between clients,
and no second message path to govern.

This is what makes the feature safe to build. A direct agent-to-agent channel would be a second,
ungoverned path; brokering through the Runtime keeps capture structural rather than inventoried
(`event-bus-governance-and-capture.md`).

## Why "Codex talks to Claude" needs no vendor-specific code

In aimee's model Codex and Claude are not sessions — they are **backends** driven by a session
(`cli_codex.c` spawns `codex app-server`; `cli_acp.c` drives an ACP agent; `provider_cli_adapter.c`
/ `AGENT_BACKEND_CLI_STDIO` is the generic spawn path). A peer message is addressed to a *session*
id, and which backend that session happens to be running is invisible to the sender.

So "Codex asks Claude to review this diff" is: session A (Codex-backed) → `Ask(B, …)` → broker →
session B (Claude-backed). Cross-vendor is a consequence of the design, not a feature of it. Adding
a third vendor adds a driver, not a peering path.

## Design

### D1 — Peer identity and the directory

A peer is addressed by **session id**, with a **label** as the ergonomic handle (`"reviewer"`,
`"codex-main"`). Models pick peers by role; ids stay the wire identity.

- `Register(sessionID, owner, surface)` makes a session addressable; `Unregister` removes it.
- `SetLabel` / `Lookup` resolve labels. Labels are **unique per owner**: a label already held by a
  different session of the same owner is refused, not silently reassigned, because a directory that
  lies about which session is "reviewer" is worse than one that refuses the collision.
- `Directory(owner)` lists peers with `label`, `surface`, `inbox`, `idle_ms`, and — enriched
  through the read-only `Live` hook across the C seam — `attachments`, `turn_in_flight`, `turn_id`,
  so a caller can avoid addressing a session with nothing live behind it.

**Visibility defaults to same-owner.** A session sees, and may address, peers sharing its owner
principal. Cross-owner peering requires an explicit grant (D5) and is off by default — on a
multi-user appliance, agent-to-agent reachability across principals is a capability, not a default.

### D2 — Delivery is pull-based, and the broker is the only path

There is no delivery-target plumbing and no push. `Send` stamps an envelope, appends it to the
receiver's inbox, and returns. The receiver drains with `Take` when it chooses — canonically at the
head of its next turn.

This is the load-bearing simplification. It removes the need for peer delivery to interlock with
turn arbitration at all, which is what lets the family be Go-owned while the turn lock is still C
(see [Ownership and language](#ownership-and-language)), and what makes `Ask` deadlock-free.

Timing, in full:

- receiver **idle** → the message waits until it drains;
- receiver **mid-turn** → the message waits; the turn is neither interrupted nor raced;
- receiver **detached** → its directory entry is kept alive by the non-empty inbox, so the message
  is there when it re-registers. Nothing is silently dropped.

### D3 — Peer traffic is announced live

Every delivery fires the `Notify` hook with the receiver's session id and the message. The Runtime
wires it to the receiver's presence-event stream, so **every attached surface sees peer traffic as
it happens** — the human watching session B sees "session A (reviewer) asked: …" arrive.

Agent-to-agent conversation a human cannot observe in real time is a trust failure; making the
announcement part of delivery rather than an audit query is what prevents it. The notification
carries the envelope and a bounded `Preview`; the inbox holds the authoritative text.

### D4 — The verbs

All share one envelope and one inbox; they differ only in how the caller waits.

#### `Send(from, to, text, opts) → Message`

Fire and forget, as described in D2. `opts.ConversationID` continues an exchange (empty opens one);
`opts.Hop` is the sender's current hop — 0 for an opener, N+1 when relaying something received at
hop N. A hop at the ceiling is refused with `ErrHopLimit`, which is what terminates a ping-pong.

#### `Ask(ctx, from, to, text) → (reply, askID, error)`

Blocking, the tool-call mental model. Sends with a fresh correlation id, then waits **on the asker's
own inbox** for the correlated reply, bounded by `ctx`. It never takes the peer's turn lock.

An ask that would close a cycle in the wait-for graph is **refused immediately** with `ErrCycle`
(D7). **On timeout the ask degrades to a send** — `ErrTimeout` comes back *with the question's id*,
the question stays in the peer's inbox, and a late reply lands in the asker's inbox still carrying
that correlation id. A slow peer costs latency, never a lost answer.

#### `Reply(from, to, text) → Message`

Answers a message taken from the inbox, routing back with its correlation and conversation ids at
`to.Hop + 1`, so a waiting `Ask` wakes and a ping-pong still burns the hop budget.

#### Channels

`ChannelJoin` / `ChannelLeave` / `ChannelSend` / `ChannelMembers` over a named membership set.
Deliberately **thin — addressing sugar over `Send`**, not a new transport: a channel owns membership
and fan-out and nothing else. No history, no ordering guarantee stronger than each recipient's own
inbox order, and every delivery is an ordinary peer message subject to the same authorization, hop
ceiling and inbox bound.

Three properties are load-bearing:

- **The sender must be a member.** A non-member writing to a channel would be a way to reach
  sessions that never agreed to hear from it. The sender is excluded from its own fan-out.
- **One `conversation_id` spans the fan-out, at `hop + 1`.** A cycle through a channel therefore
  terminates on the same budget as a direct exchange rather than escaping it.
- **`ChannelSend` reports PER RECIPIENT, not one status.** The interesting case is partial: a
  five-member channel where two deliveries were denied for want of a cross-owner grant and three
  landed. A single "sent" would hide precisely what the sender needs to know — the silent
  partial-success shape this design keeps running into. A member whose session has gone is reported
  as `no_peer` rather than skipped, because the sender asked to reach it and did not.

A session whose entry is removed is dropped from its channels, so membership can never name a
recipient that can never receive — the in-memory form of the orphan-row problem.

### D5 — Provenance, and the rule that a peer is not an operator

Every message carries an immutable envelope: `message_id`, `correlation_id`, `conversation_id`,
`from_session`, `from_owner`, `from_label`, `origin_session`, `hop`, `is_reply`, `sent_at`.

The envelope is **stamped by the broker, never by the sender** — `from_*` are read out of the
sender's own directory entry under the lock, so a session cannot forge who it is.

The message is rendered into the receiver's context as a **distinct role** — not a user turn, not
tool output. This is the single most important safety property in this proposal:

> **A peer is not an operator.** Peer content carries no authority. It cannot grant, widen, or
> borrow the receiver's capabilities; the receiver's `execution-policy` is evaluated against the
> receiver's own principal, unchanged by who is talking to it.

Concretely, that forbids: a peer message being treated as an approval; a peer message satisfying a
confirmation prompt; a peer's owner principal being substituted for the receiver's in any policy
check. A compromised or merely confused peer can waste a receiver's turn — it cannot escalate it.

**Cross-owner grants** are *directed*: `Grant(X, Y)` lets X address Y and says nothing about the
reverse, which needs its own grant. Recorded, revocable, and surfaced in the audit stream.

**`origin_session`** is the session that opened the conversation, and so the one the exchange is
charged to. `Send` stamps the sender; `Reply` propagates it, so the origin survives every hop and a
runaway ping-pong exhausts an allowance someone is watching rather than the appliance's.

### D6 — Governance

Peer sends are **action-class events on the module event bus**, published through the Go bus client
in `server-go/bus`, so the tap in `event-bus-governance-and-capture.md` captures every one and
applies a synchronous pre-delivery verdict through `execution-policy`. Because the broker is the
only path, there is no peer route that bypasses the tap — capture stays structural, and
`uncovered_enforcers` stays a mechanical descriptor check rather than a manual sweep.

*Not yet wired; see [Implementation status](#implementation-status). The `Notify` seam is where it
attaches.*

### D7 — Deadlock and loop control

Three hazards, each with a bound.

**Wait-for cycles.** A blocking `Ask` running on session A's worker while B asks A back would wedge
both. The broker maintains a **wait-for graph** — each session records the peer its in-flight ask is
blocked on — and an ask that would close a cycle is **refused immediately** with `ErrCycle`, naming
nothing to guess at. Detection walks the edges forward from the target; a walk longer than the
session count is itself treated as a cycle, because a false positive costs a fallback to `Send`
while a missed one costs two wedged sessions. The refusal is a normal error result; the asking model
falls back to `Send`, which never blocks and so can never cycle.

**Ping-pong.** Every message carries `hop` and `conversation_id`; hops are bounded per conversation
(`MaxHops`, default 16) and the cost is charged to `origin_session` (D5).

**Fan-out storms.** `ChannelSend` is bounded by membership size and shares the hop budget; a channel
message at hop *N* produces member messages at hop *N+1*, so a cycle through a channel terminates on
the same counter.

**Inbox pressure.** `InboxMax` (32) bounds one session's backlog. Overflow is refused with
`ErrInboxFull` **and counted** (`Dropped`), so a peer that stopped being heard is diagnosable rather
than invisible.

### D8 — Surfaces

**Bus stages** are the module's interface to other modules (`server-go/modules/aimee/module.go`).
Event kinds derive from the bus formula `4096 + principal_ref*256 + stage` rather than being written
by hand, so a module's identity and the kinds it answers on cannot drift apart:

| Stage | Kind | Operations |
|---|---|---|
| `peer-delivery` | 12033 | `OpSend`, `OpReply`, `OpCancelWait` |
| `peer-inbox` | 12034 | `OpInboxLen`, `OpInboxPeek`, `OpInboxTake` |
| `peer-grant` | 12035 | `OpGrant`, `OpRevoke`, `OpGrantExists` |
| `peer-channel` | 12036 | `OpChannelJoin`, `OpChannelLeave`, `OpChannelSend`, `OpChannelMembers` |

The wire is `db1-fields-v2`, reused rather than reinvented — one dialect in the tree beats two.

**No stage blocks, and that is a hard constraint rather than a preference.** The bus caps a module
at `moduleMaxInFlight` (16) and **refuses** the seventeenth invocation rather than queueing it, with
a generic `Internal` status that does not read as backpressure. A feature whose normal use is "wait
for another agent" must therefore never hold a handler slot while it waits: sixteen outstanding asks
would wedge the module, and the next caller's cheap inbox-length check would look like a crash.

So an ask is split. `OpSend` with `expect_reply` performs the cycle check and records the wait-for
edge — server-side, where that state lives — and returns immediately; the caller waits by polling
its own inbox. The edge carries an expiry (`DefaultWaitExpiry`) so a caller that dies mid-ask does
not leave behind an edge that makes every later ask along that path look like a cycle that no longer
exists.

**Outcomes come at three levels, not two.** `bus.ModuleStatus` says only whether the module could
understand the question. A *refusal* — hop ceiling, cycle, denied, inbox full — is a **successful**
invocation carrying a domain status, because collapsing those two would leave the governance tap
unable to tell "the module is broken" from "the module said no". And a question whose truthful
answer is negative — does this grant exist, is there mail — answers `StatusOK` with the "no" in a
field, because a tap seeing a steady rate of non-OK cannot tell working-as-designed from broken
either.

The test for the last two is whether the caller must do something differently. `denied` on a send is
a refusal; `grant_exists` returning false is an answer. One case sits on the line and is
deliberately a refusal: reading the inbox of a session that does not exist. "No such session" and
"no mail" are indistinguishable as a count, and answering `StatusOK` with zero rows is precisely how
a caller polling for its reply (see above) waits forever on a session that was torn down — the
hang-shaped failure again, one level down.

**HTTP edge** for thin clients, in `server-go/modules/aimee/peer/http.go`:

| Route | Verb |
|---|---|
| `GET /v1/sessions/peers` | the directory, scoped to the caller's principal |
| `POST /v1/sessions/{id}/peer` | send, or ask when `wait_ms > 0`; targets by `to` or `to_label` |
| `POST /v1/sessions/{id}/peer/reply` | reply to a message taken from the inbox |
| `GET /v1/sessions/{id}/inbox` | read pending messages, removing none |
| `POST /v1/sessions/{id}/inbox/take` | drain, removing what it returns |
| `POST /v1/peers/grants` | grant or revoke cross-owner peering |

**Authorization fails closed.** The handler takes `Authorize`, `Principal`, and `AdminAllowed`
hooks and has no defaults: absent `Authorize` every session route answers 503, because a peer
surface that authorizes by accident is worse than one that is switched off.

**Status codes carry the distinction a caller acts on:** 403 "you need a grant", 404 "no such peer",
409 "restructure the conversation" (cycle, label collision), 413 over-long body, 429 "you are over a
budget" (inbox full, hop ceiling). A timed-out ask is **200 with `status: "timeout"`** — it degraded
to a send, which is not a failure.

Still to build (P4): the same verbs mirrored as MCP tools on the gateway and over ACP, so an
**external** Codex or Claude session that reaches aimee over MCP — but is not itself an aimee-hosted
session — gets peer addressing without any change to that agent.

## Implementation status

The module is declared and served over the bus: `src/modules/aimee/module.yaml`, the `aimee`
component at principal ref 31 in `src/modules/process-contracts.json`, the inventory entry in
`tests/baselines/modules/canonical-inventory.yaml`, the dispatch case in
`server-go/cmd/aimee-module/main.go`, and `docs/modules/aimee.md`.

**"Done" below means BUILT AND TESTED, which is not the same as reachable.** The
distinction is not pedantic here: as deployed, the module has no session
directory and nothing registers a session, so no session can exist and every
session-scoped stage answers `no_directory`. The registry, delivery, inboxes,
channels and hop budget are all implemented and covered, and none of them can be
exercised by a real caller yet. The `/v1` routes are further back still --
mounted by nothing at all.

The third column says which, because the first two columns said "done" for both
and that is how this went unnoticed through a container validation.

| Slice | Built | Reachable as deployed |
|---|---|---|
| The `aimee` module itself — descriptor, contract, inventory, dispatch, docs | **done** | yes — the module runs and serves |
| D1 labels and lookup | **done** in-module; directory moves to `db1` (see below) | no — needs a session to label |
| D2 pull-based delivery, inbox lifetime | **done** | no — no session can exist |
| D3 live announcement (`Notify` seam) | **done** in the registry; Runtime wiring outstanding | no — nothing to announce, and no subscriber |
| D4 `Send` / `Ask` / `Reply` | **done** | no — no session can exist |
| D4 channels | **done** — `peer/channel.go`, stage `peer-channel`, per-recipient outcomes | no — no session can join |
| D5 envelope, provenance, grants | **done** | grants YES; envelope and provenance no |
| D6 bus tap / `execution-policy` verdict | **not started** — attaches at the `Notify` seam | no |
| D7 cycle refusal, hop budget, inbox bound, wait-edge expiry | **done** | no — no traffic to bound |
| D8 bus stages | **done** — 12033/12034/12035/12036 | advertised and routed; session stages answer `no_directory` |
| D8 `/v1` routes | **done** and MOUNTED BY NOTHING — no caller constructs `Registry.Handler`, so no client can reach a route | no |
| D8 MCP / ACP mirroring | not started (P4) | no |
| Directory read from `db1` | **seam only** — `DirectorySource` is defined and unimplemented | no — and this is what gates every "no" above |
| Durable inboxes | **not started** — needs the `postgres` generic storage wire | no |

60 tests green under `-race`. Verified by mutation, not only by passing:

- removing the cycle walk makes both cycle tests fail (they hang to their deadlines — the exact
  wedge the check prevents);
- un-advertising one declared stage fails three independent tests with the precise diagnostic
  "declares `peer-grant` (event 12035) … but never advertises it".

**Known gaps, stated rather than implied.** The largest one was not stated at
all until it was found by asking what CALLS this in production, rather than
whether the tests pass.

As deployed, the module cannot have a session. `DirectorySource` is unimplemented,
and nothing else populates the registry either -- `Register` has no caller outside
tests, and no bus op reaches it. So peer messaging is INERT in the shipped
artifact, and was inert throughout the container validation that reported fifteen
green checks.

Those checks could not have caught it. A module that correctly refuses an
unregistered sender and a module that can never have a sender both answer
`unknown_sender`. The channel row is worse than a refusal: "members of an absent
channel answers OK with none" is a SUCCESS, and the same success a working module
would print. The module now answers `no_directory` for session-scoped stages --
a fact about the module rather than about the caller's session -- and the probe
establishes which configuration it is talking to before asserting any refusal.

The remaining gaps, unchanged: inboxes do not survive a process restart; the
governance tap is unwired, so peer sends are not in the audit chain and A9 is
unmet; and the `/v1` routes are built and tested but constructed by no caller, so
no client can reach one.

## Merging with the db1 absorption

`aimee` is the home for everything aimee-server-specific, so db1's nineteen
families absorb into it rather than sitting beside it. That work is happening in
another tree; this one carries peer messaging at **principal ref 31, stages
1/2/3**. The agreed end state is **ref 30, stages 20/21/22/23** (kinds
11796/11797/11798/11799), because the ref is baked into every kind by
`4096 + ref*256 + stage`: at ref 30 four stages renumber, at ref 31 nineteen
kinds move and 461 C call sites move with them.

**Neither tree can do the renumber alone**, for mirrored reasons. This tree still
has db1 at ref 30, so taking that ref here means performing the absorption twice.
The other tree has no peer-messaging code, so declaring stages 20/21/22 there
would declare stages nothing serves — which both trees' contract tests correctly
refuse, since a declared-but-unadvertised stage routes and then answers
`CAPABILITY_ABSENT` from a module that is plainly running. It happens at merge,
in the first tree containing both, and the deployment validation is then repeated
against the real numbers.

Four hardcoded gates and two lists have to move in that one commit. Each fails
loudly rather than silently, which is the only reason this is a checklist and not
an incident:

| Gate | Merged value |
|---|---|
| `canonical-inventory.yaml` required | contains `aimee`, not `db1`; `principal_refs: aimee: 30` |
| `REQUIRED_COUNT` | **19** — `aimee` takes db1's slot rather than joining it |
| `OPTIONAL_COUNT` | 11 |
| `PROCESS_REQUIRED` | `aimee`, no `db1` |
| `GO_PROCESSES` | exactly one `aimee`, no `db1` |
| `src/modules/` | one `aimee/module.yaml` carrying both stage sets |

`REQUIRED_COUNT` is the one worth care: **20 is correct in this tree**, where db1
and aimee both exist as required modules, and **19 is correct merged**, where db1
is gone. A count that is right on both sides of a merge and wrong in the middle
is exactly the shape that gets "fixed" in the wrong direction.

Names that stay: `db1-fields-v2`, `server-go/db1`, `AIMEE_DB1_EVENT_*` and
`SchemaOwner = "db1"`. A name in a contract belongs to the contract, not to
whoever implements it — and `SchemaOwner` is not a label but the key recorded
against every applied migration, so renaming it would not rename those rows, it
would make them invisible.

## Phasing

- **P1 — Directory, send, inbox, provenance.** D1, D2, D3, `Send`, D5. ✅
- **P2 — Blocking ask.** `Ask`, the wait-for graph, `ErrCycle`, hop budget (D7). ✅
- **P3 — Channels.** `Channel*`, membership.
- **P4 — External agents and governance.** MCP/ACP mirroring (D8) and the bus tap (D6).

  **Deliberately sequenced after the merge, not merely unfinished.** Mirroring the
  verbs onto MCP means C call sites in the `protocols` module invoking these
  stages, and the stages renumber from 12033-12036 to 11796-11799 when the db1
  absorption lands. Building those call sites first means editing them twice, and
  the second edit is the kind that leaves one constant behind. The precondition
  that matters is already true: nothing on the peering path is vendor-aware, so
  the work is wiring rather than design.
- **P5 — Durability.** DB1-backed inboxes.

## Non-goals

- **Runtime↔Runtime federation.** Out of scope by the topology decision. If two Runtimes must ever
  exchange peer traffic it belongs to the Control-Plane edge and gets its own proposal.
- **Peer-to-peer sockets, shared memory, or a shared filesystem between peers.** Every message
  crosses the broker.
- **A peer acting inside another session's workspace.** Workspace single-writer leases stay
  per-session; a peer can *ask* B to change a file, and B decides under B's own policy.
- **Replacing delegation.** A delegate is a child whose lifecycle, budget, and result its parent
  owns. A peer is an independent session that owns itself. Both stay.
- **A general pub/sub bus for user data.** Channels carry peer messages, not application events;
  the module event bus is the event substrate.

## Risks

- **Cross-session prompt injection.** A peer message is attacker-influenced text entering another
  agent's context. Mitigated structurally by D5 (distinct role, no borrowed authority), not by
  filtering. The residual risk is a peer wasting a receiver's turn, which the hop budget bounds.
- **Context bloat.** Chatty peers consume receiver context. Mitigated by the hop budget and by
  coalescing an inbox drain into one context block rather than N turns.
- **Inboxes do not survive a restart.** A message outlives its receiver's *detachment* but not a
  process bounce. This is the one place the implementation is weaker than the design intends, and it
  is deliberate scoping: durable inboxes go through the `postgres` module's generic storage wire
  under owner `aimee`, with `peer_inbox` and `peer_grants` numbered 1 and 2 explicitly rather than
  derived from sorted filenames. No API changes when it lands.
- **Orphan inbox rows.** `db1` carries no cross-family foreign keys, so `peer_inbox` cannot
  reference `server_sessions` and nothing cascades when a session is deleted. Decided rather than
  left open: such a row is **undeliverable, not delivered** — nobody drained it, so recording it as
  delivered would put a falsehood in the audit trail. The sweep runs after
  `server_session_delete_expired` rather than on its own timer, so it cannot race ahead of the
  lifecycle that creates the orphans. Unimplemented until inboxes are durable.
- **Two directories until `db1` is wired.** The `DirectorySource` seam exists but is not connected,
  so the module's in-memory map is still answering "which sessions exist" alongside
  `server_sessions`. The interesting failure is the one row where they disagree, which is why this
  is a gap rather than a design choice.
- **Observability of long exchanges.** A 16-hop conversation across four sessions is hard to follow
  in four separate transcripts. `conversation_id` is the join key; a conversation view is worth its
  own follow-up.

## Acceptance

Marked ✅ where a test in `server-go/modules/aimee/` asserts it today.

The path in this line used to read `server-go/internal/peer`, which has not
existed since the code moved into the module. A citation to a directory that is
gone is the cheapest kind of stale, and it survived because nothing reads a
prose path.

**✅ means A TEST ASSERTS IT IN PROCESS. It does not mean a real caller can do
it.** As deployed the module has no session directory and nothing registers a
session, so every criterion below involving two sessions is asserted against a
registry the tests populate themselves and is NOT reachable in the shipped
artifact. That is not a caveat on one row; it applies to A1 through A6, A12 and
A13. A8 is the exception among the session-shaped ones -- grants are
owner-to-owner and work without a directory.

- **A1** ✅ Two sessions with the same owner discover each other by label, and a message appears with
  an envelope the receiver did not author and the sender could not forge.
  (`TestDirectoryAndProvenance`)
- **A2** ✅ A session mid-turn receives a peer message without the in-flight turn being preempted or
  raced; the message is drained when the receiver chooses. (`TestDeliveryDoesNotPreempt`)
- **A3** ✅ A message to a detached session is delivered on re-registration, not dropped.
  (`TestMessageSurvivesUnregister`)
- **A4** ✅ `Ask` returns the peer's reply; a timed-out ask returns `ErrTimeout` with the question id
  and the late reply arrives in the asker's inbox with the original correlation id.
  (`TestAskAndTimeout`, `TestHandlerAskTimeoutIsNotAnError`)
- **A5** ✅ Mutual asks are refused with `ErrCycle`. Neither session deadlocks; both remain able to
  serve their own operators. Driven as a deliberate race, and transitively (A→B→C→A).
  (`TestAskCycleRefused`, `TestAskCycleTransitive`)
- **A6** ✅ A ping-pong terminates at the hop ceiling and `origin_session` rides every hop.
  (`TestHopBudgetTerminatesPingPong`)
- **A7** A peer message cannot satisfy a confirmation, grant a capability, or cause an
  `execution-policy` decision to be evaluated against the sender's principal. **Not assertable
  here** — it binds the context-rendering and policy call sites, and lands with D6/P4.
- **A8** ✅ Cross-owner addressing is refused without a grant, grants are directed, and revocation
  takes effect. (`TestCrossOwnerGrants`, `TestHandlerGrants`)
- **A9** Every peer send appears in the event-bus capture with its verdict; no peer path exists that
  the tap does not see. **Pending D6.**
- **A10** A Codex-backed session and a Claude-backed session complete an ask/reply exchange with no
  vendor-specific code on the peering path. **Pending P4** (needs the MCP/ACP mirroring); nothing on
  the peering path is vendor-aware today, which is the precondition.
- **A11** ✅ Authorization fails closed: without an `Authorize` hook every session route refuses.
  (`TestHandlerFailsClosedWithoutAuthorize`, `TestHandlerRefusesUnauthorizedSession`)
- **A12** ✅ Over-long bodies are refused, never truncated; concurrent senders and drainers neither
  lose nor duplicate a message under `-race`. (`TestNoSilentTruncation`,
  `TestConcurrentSendAndDrain`)
- **A13** ✅ The three outcome levels stay distinguishable, and reading the inbox of a session that
  does not exist refuses rather than reporting an empty one — so a caller polling for a reply learns
  its session is gone instead of hanging. (`TestThreeOutcomeLevelsStayDistinct`,
  `TestUnknownSessionIsNotAnEmptyInbox`)
- **A14** ✅ A stage declared in `process-contracts.json` but not advertised fails the build.
  (`TestAdvertisedStagesMatchTheContract`, plus the two in `cmd/aimee-module`)
