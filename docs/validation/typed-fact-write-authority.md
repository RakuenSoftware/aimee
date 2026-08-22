# Validating the typed-fact write-authority fixes on a clean container

Two authority gaps in the typed-fact layer were fixed on `worktree-fact-authority-gaps`.
Both are about the same thing: a write that says it speaks for the user, without
anything having checked whether it does. This is the record of running the fixed
build against a real deployment rather than the test shim.

## What was used

- **Host**: Proxmox `pvetest` at 192.168.1.252, `pve-manager/9.2.6`.
- **Container**: LXC **CT 9078** `aimee-fact-authority`, created for this, Debian 13
  standard template, 4 cores / 6GB / 24GB disk, unprivileged, DHCP (192.168.0.194).
- **Build**: `worktree-fact-authority-gaps` off `testing` at `8c7c0b540a`
  (`pre-merge-safety-2700-g8c7c0b540a`, printed by every binary), built locally and
  carried in. Nothing was compiled in the container.
- **Installed**: `aimee`, `aimee-server`, `aimee-kb` into `/usr/local/bin`; PostgreSQL
  17.11 with pgvector from Debian packages.
- **Topology**: the default local one — `aimee-server` reaching `aimee-kb` over plain
  loopback HTTP, which is what `install.sh` and `scripts/aimee-local-stack-e2e.sh
  --mode full` produce.

Scripts are in `scripts/validation/fact-authority/`.

## The two gaps

**Gap 1 — declared authority.** The retraction path took `authority` from the request
body. `{"authority":"user"}` bought the right to delete a user-stated Class-A fact and
to override an `immutable` relation. Worse, `db2_typed_fact_ingress` — reached by the
`get_context_block` MCP tool, whose `query` the *model* composes — passed a hardcoded
`FACT_AUTHORITY_USER`, so an agent could retract the user's facts by writing "forget my
email" into a query nobody asked for.

**Gap 2 — outranked supersede.** On a FUNCTIONAL (single-valued) relation, a new object
contradicts the old one, so the commit path applies the relation's
`correction_behavior`. It did that without checking who was writing, so an ordinary
Class-B model write silently superseded the user's Class-A fact. No retraction endpoint
involved — just a normal write.

## What was checked, and what it found

### Gap 2 on the real engine — the guard is SQL, so the shim proves nothing

The guard is a CASE-rank comparison inside the UPDATE and its probe. `make unit-tests`
runs against the sqlite shim, which *translates* DB2's SQL rather than executing it as
Postgres would, so a guard that is correct under the shim and wrong under libpq stays
green to deployment. The repo has a mode for exactly this (`AIMEE_TEST_PG=1` +
`db2-test-template`); it had not been run for these tests.

Both typed-fact suites pass against real PostgreSQL 17.11:

    unit-test-fact-lifecycle     PASS
    unit-test-fact-ingest        PASS

**Negative control.** Reverting the guard, rebuilding against libpq and re-running gives

    test_fact_lifecycle.c:262: main: Assertion `strcmp(cur, "acme") == 0' failed.

— the model's `globex` had replaced the user's `acme`. So the assertions genuinely run
in postgres mode (they are not silently skipped, as some shim tests are), and they fail
exactly when the guard is absent. Restoring it makes them pass again.

The suite asserts the surviving **target**, not a row count, so "the user's value
stands" cannot be confused with "both values are current" — which is the other way this
could have been wrong, since an outranked write must be *dropped* rather than inserted
alongside.

### Gap 1 through the real stack

Every probe pairs the Class-A escalation attempt with a Class-B control, so a refusal
can be told apart from a broken route or an unreachable service:

| caller | transport | Class A (user-stated) | Class B (model) |
|---|---|---|---|
| agent's bearer | TCP → server | refused at the capability layer | refused (same) |
| person at terminal | UDS → server | `retracted: 1` | `retracted: 1` |
| owner bearer | loopback → kb | `retracted: 1` | `retracted: 1` |
| owner bearer | **remote** → kb | **`retracted: 0`, fact stands** | `retracted: 1` |

The last row is the fix: the same credential, the same request body asking for `user`,
refused because the peer is not local. The Class-B control succeeding in that same run
is what makes the 0 meaningful.

The TCP row is a pre-existing defence (write-tier grants), not this change — worth
recording because it means an agent on the network never reaches the retraction path at
all; the fix matters for the paths that *do* get through.
### `get_context_block` no longer speaks as the user

With `aimee-module-memory` placed in kb (below) the retraction scan actually
fires, so this scenario is finally conclusive. The agent's own query carries the
retraction cue, and both classes go through the identical query:

    user-stated (Class A) before: A current
    after the agent's query:      A current      <- the user's fact stands

    model-authored (Class B) before: B current
    after the same query:            B gone      <- the path is demonstrably live

The Class-B row being retracted is what makes the Class-A row surviving mean
anything. On the first pass, before the module was placed, BOTH survived and the
result proved nothing at all.

### Provenance is stamped from identity, and fails closed

Gap 2's prerequisite: `memories.provenance_category` is what the drain reads later to
decide whether facts mined from a note may be Class A. It defaulted to `user_stated` for
every row, so anything the model chose to remember became a source of permanent facts
outranking the user's own.

    loopback + owner bearer, asking "user":   user_stated
    loopback, asking nothing:                 agent_message
    REMOTE peer, asking "user":               agent_message
    no provenance supplied at all:            agent_message

The last line is the column default, now `agent_message`. The `SET DEFAULT` migration
applies to future inserts only; existing rows keep what they were stored with, which is
the right behaviour for an upgrade — pre-existing Class-A facts are exactly what gap 2's
guard is there to protect.

### memory.delete destroys only for an attested person

A third instance of the same shape, found while sweeping for the pattern:
`memory_delete_command` hardcoded `MEMORY_AUTHORITY_USER`, so any caller clearing
`CAP_MEMORY_ADMIN` hard-destroyed the row and its provenance. The capability is
the right answer to "may this caller delete" and the wrong one to "is this caller
the user" — it sits inside `CAPS_AUTHENTICATED`, so a bearer clears it, and the
destroy is irreversible with an audit event carrying only the id.

The authority now comes from the connection's attestation, the same rule
`facts.retract` and `memory.store` use. A caller that clears the capability but
is not attested as a person still deletes — it retires the row, which
`memory.fact_history` can still read — so this narrows the blast radius, not the
feature. The response reports which happened as `destroyed`.

    UDS, kernel-attested uid:  {"deleted":true,"destroyed":true}   row gone
    TCP bearer, no grant:      refused at the write-tier gate      row untouched

**The TCP line is not evidence of a retire.** Over TCP the request is refused
several layers above the authority decision, by the per-user write-tier grant
gate, and reaching that decision needs a KB-signed identity token this container
has no tenancy to issue. So on a default deployment this change is defence in
depth; it becomes load-bearing the moment an operator issues a grant. The retire
path itself is covered by `test_mcp_memory_gate.c`, which asserts the mapping for
all six `attested_transport_t` values — including `ATTEST_NONE`, the value a
missed hop collapses to — with a negative control confirming the test fails when
the mapping is broken.

### Gap 2 through the REAL commit path, and the placement fix that unblocked it

`aimee-module-memory` is a required module serving 5889-5894, but its
`placements` were `["server"]` while the caller
(`src/kb/kb_module_stage_adapters.c`) runs in kb. aimee-kb called three stages
nothing served there -- `EXTRACT_INDEX` (extraction + retraction scan), `WRITE`
(the typed-fact write gate) and `EMBED` -- each failing as TRANSPORT, so all of
them silently did nothing. `memory` is now placed in `["server", "kb"]`.

The module is stateless: its stages are pure decisions plus one outbound HTTP
call, and "storage, graph, and lifecycle units likewise remain C". So there is no
sqlite-on-server / postgres-on-kb split to reconcile -- the generated kb grant is
byte-identical to the server one and requests nothing. Verified on the container
rather than on paper: built from `server-go/cmd/aimee-module`, installed with its
generated grant, attached to kb's bus, after which the scan stopped answering
"no answer" and the drain ran for the first time.

That made the only production route to the commit gate reachable:
`memory.store` -> `memory_facts` job -> drain -> pattern extractor -> the module's
write gate -> `db2_fact_commit`. Choosing the relation for this took three
attempts, each of which produced a green-looking result that proved nothing:

- `city` is not FUNCTIONAL, so two values legitimately accumulate and no
  correction ever applies;
- `has_hostname` is functional, but the extractor turns "my X is Y" into rel_type
  X verbatim, so that text yields `hostname` -- a different, novel relation, which
  §5 forces to Class C whatever the authority;
- `age` is functional, seed, and extractable -- but produced NO row, because it
  declares tail `NODE_SCALAR` while the extractor classifies a bare number as
  `NODE_OTHER`, so the gate rejected it before the guard was ever consulted.

That last one is a real defect, not just an awkward test: the pattern path had no
kind fixup, so it silently dropped every seed relation whose declared kinds
disagreed with a guess made from the value's spelling. The LLM path in
`kb_memory_facts.c` already repairs exactly this and carries a comment saying why;
only the pattern path was left without it. Fixed in `db2_fact_ingest_text`, with a
unit test that fails without it.

With `age` finally reachable, gap 2 on the real path:

    the user stated:  user/age = 30   class=A  [current]
    a model note says "my age is 41"  (provenance = agent_message)
    after the drain:  30  class=A  [current]        <- alone

    positive control (no prior fact):  ctl@example.com  class=B   <- drain ran
    user-provenance note "my age is 52":  52  class=A  [current]

The control is what makes this evidence: it commits through the same gate on the
same drain, so "41 is absent" is the guard dropping an outranked write rather
than nothing having happened. The last line closes the provenance loop in the
READ direction -- the drain took USER authority from the row's recorded
provenance, not from the hardcoded constant it used before.

## What this did not cover

- **The mTLS/distributed topology.** The asserted-caller path
  (`X-Aimee-Caller-Subject` -> `KB_PRIN_HOST` actor) is wired and unit-tested, but
  this run used the plain-loopback deployment, so the host-actor branch of
  `kb_memory_request_authority()` was not exercised against a live mTLS listener.

- **`memory.delete`'s retire path.** Over TCP the request is refused above the
  authority decision by the per-user write-tier grant gate, and reaching it needs
  a KB-signed identity token this container has no tenancy to issue. The mapping
  is covered for all six attestation values in `test_mcp_memory_gate.c`.

### The whole model, live, against a real LLM

The synthesis endpoint is Qwen3.8-27B on the appliance's llama-server. It binds
127.0.0.1:8762 there, which is why no external probe finds it;
`scripts/validation/fact-authority/llm-tunnel.sh` forwards it onto the
workstation's LAN address for the container.

Two configuration traps cost a wrong diagnosis each, and both are worth naming
because each produced a plausible-looking failure:

- `SYNTHESIS_ENDPOINT` takes a BASE url. `config_synth_chat_endpoint_normalize`
  appends `/v1` itself and `provider_client` then appends `/chat/completions`, so
  a value ending in `/v1/chat/completions` becomes
  `.../v1/chat/completions/v1/chat/completions` and every call fails as
  `provider HTTP -1` -- a transport error that reads like the endpoint is down.
- `start-kb.sh` had no `pkill`, the same trap as `start-server.sh`, so a changed
  endpoint appeared to be ignored while the ORIGINAL kb kept running with the old
  environment. Both scripts restart properly now.

With those fixed the `memory_facts` jobs reach `done`, and the resulting edges
show the entire authority model in one table:

    user | age       | 52              | A   pattern extractor, user_stated note
    user | email     | ctl@example.com | B   pattern extractor, agent_message note
    user | has_email | ctl@example.com | C   REAL LLM extraction, novel rel_type
    user | city      | Berlin          | A   the user's, still current
    user | city      | Paris           | B   model's, alongside -- city is not functional

The third row is the LLM's own extraction, which chose a relation the seed does
not define; §5 forces a novel rel_type to Class C whatever the authority, so it
cannot outrank anything. The LLM path commits at `FACT_AUTHORITY_MODEL`
unconditionally (`kb_memory_facts.c`), which is the right asymmetry: the pattern
extractor reproduces what the user actually wrote, while an LLM's reading of the
same note is inference.

### RERANK, and the bug that was hiding it

Chasing a live chat turn as far as the memory module's `RERANK` stage found a
real defect, not just configuration.

`aimee_ir_apply_request_stages()` inserts the persona onto the first user message
BEFORE running the stage list, and `ir_stage_memory` then took its query from
`aimee_ir_last_user_text()` -- the whole message, persona included, up to 16384
bytes. So on any turn delivering a persona, which is the opening turn of every
session, the "query" put to recall was thousands of characters of persona with
the real question buried at the end.

Measured on the box: the same question recalls **1 row** asked plainly and
**0 rows** asked the way the stage asked it (5773 chars). Recall returned empty,
the block assembled empty, and `ingress_preinject_build` returned NULL before it
ever reached the confidence call. Memory pre-injection was silently dead on the
first turn of every session -- with no error logged anywhere, because "recall
found nothing" is not an error.

The caller now captures the user's query before the persona is prepended and
hands it to the stage through the transform's `ud`, which was unused. A caller
that supplies none still falls back to the message, so nothing else changes.

The fix is visible in the prompt that actually goes upstream, read through a
capture proxy rather than inferred from token counts: the system message grows
2186 -> 2498 chars and gains `recommended (memory previews)` and
`aimee-context`, both previously absent.

And that makes RERANK observable, because a failed confidence call does not
degrade the envelope, it DELETES it:

    LOG_WARN("memory", "rerank confidence unavailable; omitting pre-injection envelope");

So the envelope's presence is proof the module answered. Stopping and restarting
the server-side module around the same turn:

    module RUNNING    envelope PRESENT
    module STOPPED    envelope ABSENT   + exactly 1 "rerank confidence unavailable"
    module RESTARTED  envelope PRESENT

RERANK is exercised by a live chat turn, and the module answering it is
load-bearing for the whole injection rather than for one confidence line.

The gates that had to be satisfied first, each of which looked like the last:
a chat provider; `/v1/messages` rather than `/v1/chat/completions` (pre-injection
hooks the Anthropic-native ingress); `ingress_preinject_anthropic_enabled` ON
(opt-in, no env override); an ACTIVE REPOSITORY, without which
`ingress_preinject_build` refuses to broaden to global recall; the memory SCOPED
to that project; and `code_context_mode` not `on`, whose strict mode zeroes both
the preview and facts layers.

### RETRIEVE: the kb was deciding PII locally

The mirror of the placement gap, found by asking who actually calls each stage.

`RETRIEVE` is the §7 PII recall gate, and its only callers -- `fact_recall`,
`fact_ingest`, `rel_types_store` -- are db2 code that runs in the **kb**. But the
kb registered no provider for it, so `memory_pii_turn_requests_sensitive()` and
the sensitivity batch silently fell back to the hardcoded cue list in
`pii_classifier_primitives.c`. Meanwhile aimee-**server** registered the
module-backed providers and calls `RETRIEVE` -- but nothing in the server invokes
the gate. The capability was wired on one side and consumed on the other, so the
module never decided anything.

`docs/modules/memory.md` names this exactly: "a registered provider that is
authoritative and never falls back to the local implementation, because a silent
fallback lets a broken module look healthy."

The kb now registers both providers. Failure stays fail-closed by construction: a
turn classifier that errors reads as "did not ask for sensitive data" (withhold),
and a sensitivity batch that errors makes `fact_recall` abandon its candidates
rather than inject them.

Verified live by cycling the kb-side module around a recall of a PII-classed
relation:

    module RUNNING    facts: - age: 52 / - has_email: ctl@example.com
    module STOPPED    facts: (none)
    module RESTARTED  facts: - age: 52 / - has_email: ctl@example.com

Identical output in all three would have meant the kb was still deciding
locally. It is not.

- **The module's other stages.** aimee-server calls four memory stages --
  EXTRACT_INDEX, WRITE, RETRIEVE (the PII recall gate) and RERANK (the ingress
  confidence tier). All four are now exercised: EXTRACT_INDEX and WRITE are the
  same code paths proven on the kb side, RETRIEVE by the PII cycling above, and
  RERANK by the envelope cycling under "the persona was the query". EMBED (5891)
  is NOT exercised -- it needs an embedder, and none is configured here.

  An attempt to call them directly over the bus was abandoned. It needs a second
  principal granted `request=` for those events, and the grant written for it was
  refused -- which does not merely skip that module: the whole module endpoint
  fails and the daemon exits ("obs_bus: module endpoint failed", then "server:
  shut down"). One malformed file in `modules.d` takes the daemon with it, which
  is worth knowing independently of this work. The grants were removed and both
  daemons restored, with the full e2e suite re-run afterwards to confirm the
  restore rather than assume it.

## The exploratory pass

Everything above is a targeted probe: each one answers a question that was asked
before it was written, so it can only find what was already suspected. The
exploratory pass (`explore.sh`, `explore-cli.sh`) instead walks the surfaces a
user actually touches and reports whatever comes back. It found four things the
targeted probes could not.

### The CLI surface had never been touched

Every `aimee memory *` command answered `aimee-server unavailable / endpoint:
none configured`. The HTTP surface underneath was green throughout, so nothing
in the earlier work noticed that the layer a person actually types into had
never once been run. It needs `AIMEE_API_ENDPOINT=unix:/root/aimee-http.sock`;
with that set, store / list / search / stats all work. Calling the work "e2e
tested" while the entry point was untested was overclaiming, which is the
specific thing an exploratory pass exists to catch.

### `aimee status` reported a store that was not there

`status` said `DB2 schema not ready` / `store: unavailable` while memory
store, list and search demonstrably worked against that same database. A false
negative, and an expensive one -- it points an operator at the database when the
database is fine.

The cause is the `postgres` module. Its manifest places it in kb
(`serve: [11265]`), the kb answers `AIMEE_POSTGRES_EVENT_HEALTH` through it, and
it was never started; the working store paths go through the kb's own libpq
connection and never consult the probe. `install-postgres-module.sh` starts it,
and two things had to be right:

- the grant must exist **before** the kb starts, or the attach is denied;
- the module is a separate process with its own environment, so it needs its own
  `AIMEE_DB2_URL`. Without it the probe answers `AIMEE_DB2_URL is unset` and the
  kb reports the same "schema not ready" -- the deployment fault and the database
  fault are indistinguishable from the operator's side.

With the module up, `store: ok`.

### `relations.schema_list` can only ever return empty

It returns `{"rows":[]}` against a graph holding 20 relation types.
`db2_relation_schema_list` (`entity_edges.c`) selects from
`memory_relation_schema`, and that table is **never written**: across the tree,
the only code that names it is that one SELECT. The surface is therefore dead by
construction rather than empty by circumstance.

It is not a hazard to the authority work and not on this charter's path: the only
consumer (`cmd_memory_vector.c`) omits an informational `schema_rules` array when
the list is empty, and nothing enforces anything from it. The kind constraints
that *are* enforced -- including the kind fixup added on the pattern path -- come
from the compiled `rel_types_seed_lookup` table, which is populated. Recorded
because a reader of that surface would reasonably conclude the graph has no
relation schema at all.

### `memory.entity_profile` cannot see the typed-fact graph

`entity_profile` 404s for `user` and `Dana` while returning a card for
`deployment runbook` -- even though `user` and `Dana` are both sources of live
`entity_edges` rows and `deployment runbook` has `relation_count: 0`.

`db2_memory_entity_profile_stats` (`memory_relations.c`) counts mentions from
`memory_entities` and relations from `memory_relations`. The typed-fact layer
writes to `entity_edges`. The two graph representations are disjoint, so a
profile reflects the older one only: an entity known **only** through typed facts
has no profile, and one that has a profile reports zero relations while holding
several.

Left unfixed deliberately. Reconciling two graph tables is a data-model decision,
not a defect repair, and it does not touch the authority guarantees this change
is about.

### `aimee kb grant set` does not exist

Reaching the authority derivation over TCP needs a write-tier grant. The server
says so itself, in the 403 body (`server_http.c:1685`):

> an operator issues one with `aimee kb grant set` (see docs/UPGRADING.md)

That command cannot run. `aimee kb grant list` answers `'grant' is not a
subcommand of 'kb'`.

Every other piece of it is present and unit-tested: the flag marshaller
(`cli_v1_routes_b.c:1610`), the outcome renderers (`cli_v1_routes_c.c:2377+`),
the print-table entry (`cli_v1_routes_d.c:51`), and the KB endpoints
(`kb_http_grants.c`: `POST /v1/write-tier-grants/set`, `/revoke`, `GET
/v1/write-tier-grants`). What is missing is the connection: there are no
`{"kb", "grant ...", "kb.grant.*"}` rows in `cli_dispatch_defs_data.h`, and
**no handler anywhere resolves the method `kb.grant.set`** -- the KB exposes the
capability under REST paths that nothing maps those methods onto.

So an operator who hits the 403 is sent to a command that cannot dispatch. Not
repaired here: wiring it spans the dispatch table, a method-to-REST mapping, and
an admin credential path for the call, which is feature completion rather than
the defect repair this charter covers.

## What the transport test does and does not prove

`test-transport-authority.sh` sends the *same* retraction body -- including
`"authority":"user"` -- over both transports:

    TCP  127.0.0.1:8740   ATTEST_TCP_BEARER     not a person -> MODEL
    UDS  aimee-http.sock  ATTEST_UDS_PEERCRED   a person     -> USER

Result: TCP retracted nothing, UDS retracted the Class A fact. The security
property holds end to end -- **a network caller cannot retract a Class A fact by
asserting authority in the body, and a real person can.**

The reason it holds is worth stating precisely, because two earlier versions of
this test passed for reasons that proved nothing:

1. with no bearer, TCP was refused at the **authentication** wall;
2. with a bearer, TCP was refused at the **write-tier capability** wall
   (`server_http.c:1673`) -- over the network a bearer is read/query only.

Neither reached `server_attested_memory_authority`. The third wall, the
derivation itself, is therefore *not* what stops the TCP caller here; it is
covered by the unit matrix over `attested_transport_t` and, live, by the UDS
positive control. Reaching it over TCP requires the write-tier grant, which is
blocked by the missing `kb grant set` command above.

That is a stronger posture than the derivation alone (the network path is refused
twice before authority is even considered), but it is not the same claim, and the
test now fails loudly rather than passing green if either wall answers first.

## Re-verification against merged `testing`

Everything above was proven on this branch alone. `testing` then merged in a
config-module extraction and the P6 epistemic-kind work, so the whole suite was
re-run against binaries built from the merged tree. Four things had to be fixed
before any of it meant anything again, and each one had been passing green while
proving nothing.

### The daemons no longer start on their own

Config became a process module. Both daemons now refuse to run without it:

    aimee-kb: config module unavailable: config module unavailable

`install-config-module.sh` installs the grant (principal_ref 2, serves 4609) on
both buses and starts an instance on each. The ordering is awkward enough to
deserve its own script: the grant must exist **before** the daemon starts (a
daemon reads `modules.d` once), and the module can only attach **after**, once
the daemon has created the bus socket -- so the start scripts launch it in the
background while their daemon is still coming up.

### A migration that its own trigger refuses

`schema.sql:1995` creates `entity_edges_semantic_guard`, which enforces that a
row with `edge_class='semantic'` may only be written inside an open
`fact_graph_commits` row. The one-shot P6 block at `schema.sql:15289` then runs:

    UPDATE entity_edges SET epistemic_kind='world_fact';

with no commit open. Every semantic edge trips the guard, the `DO` block raises,
and because the apply is one transaction the **entire schema rolls back**:

    aimee: db2_init: schema apply failed: ERROR: semantic facts must be changed
    through fact_mutation
    aimee-kb: DB2 not ready (...); retry 13/24 in 5s

aimee-kb never becomes ready. The server's knowledge calls then fail
(`TCP connect failed: 127.0.0.1:8741`) and every memory write answers
`failed to store memory` -- three layers away from the cause.

It is invisible on an empty database, because the UPDATE touches no rows. It
fires on any database that already holds semantic facts: every real deployment,
every upgrade. CI against a fresh template will not catch it.

The UPDATE also looks redundant -- the `ALTER TABLE ... ADD COLUMN IF NOT EXISTS
epistemic_kind TEXT NOT NULL DEFAULT 'world_fact'` immediately above it already
gives every existing row that value. (The sibling UPDATE on `memories` is not
redundant; it also computes `expiry_days_migration_override`.) So dropping the
`entity_edges` UPDATE appears to be the fix, but that is a schema change under
the frozen-boundary rules and belongs to whoever owns P6.
`unblock-p6-migration.sh` documents it and works around it for this container;
it does not fix it.

### The suite was judging liveness by a column that no longer means that

Retraction now sets `lifecycle_state='invalidated'` and `invalidated_at`. Only a
supersession sets `superseded_at`. Every test here judged "current vs gone" by
`superseded_at='' AND suppressed=0`, which is true of an invalidated row -- so a
**successful** retraction read as a blocked one, and `seed-facts.sh` reported
rows as freshly seeded when they were the previous run's invalidated ones.

### The seed was protecting nothing, silently

Three compounding failures, all invisible because the seed's writes were
redirected to `/dev/null`:

- `entity_edges` carries more than one write guard --
  `entity_edges_semantic_guard` and `semantic_evidence_event_guard` ("semantic
  assertion mutation committed without its evidence event"). Both refuse a raw
  INSERT or DELETE of a semantic edge. Suspending one leaves the other, so the
  seed now suspends every user trigger on the table and restores them
  immediately. That is the right call here specifically: the premise of the test
  is rows written by an earlier build.
- `authority_rank`, not `confidence_class`, is what retraction gates on:
  `if (actor->rank < rows[i].authority_rank) continue`. A row seeded `'A'`
  without rank 30 looks Class A and is retractable by anyone. The tests were
  seeding exactly that.
- and `lifecycle_state` has to be `persistent`, or the selector skips the row.

`seed-facts.sh` now asserts it left two live rows behind, because a seed that
silently did nothing is how all of the above survived a full run.

### Results after the corrections

Against binaries built from merged `testing`, on real PostgreSQL, with the
memory module on both buses and the postgres and config modules attached:

| probe | result |
|---|---|
| `test-retract` (kb, loopback owner bearer = a person) | PASS -- retracts Class A, control retracts too |
| `test-server-retract` (agent over TCP) | PASS -- refused; UDS person retracts both |
| `test-transport-authority` (same body, both transports) | PASS -- TCP 1->1, UDS 1->0 |
| `test-drain-supersede` (gap 2) | PASS -- Class A `age=30` survives and stands ALONE; positive control commits a Class B fact, so the drain demonstrably ran |
| `test-provenance` | PASS -- `user_stated` vs `agent_message` vs column default |
| `test-memory-delete` | PASS -- TCP refused above the authority decision, UDS person destroys |
| `test-context-block` | **FAILS LOUDLY** -- see below |

`test-retract` was also relabelled. It called the loopback-owner-bearer leg "the
attack" and expected refusal, which became wrong once the policy settled that an
owner bearer on loopback IS the operator. Left alone it would report a security
failure every time the system behaved correctly.

### EXTRACT_INDEX (5889) is not answering, and it was hiding behind a green test

`test-context-block` asks the kb to serve a block for the query "please forget
my email" and checks that the agent's own words do not retract the user's fact.
It passed. It was passing for the wrong reason:

    WARN memory: retraction scan gave no answer; not retracting this turn

`db2_typed_fact_ingress` runs the §4 correction pre-scan through the memory
module's EXTRACT_INDEX stage. When that stage does not answer it returns without
reaching the authority decision at all, so both facts survive no matter what the
authority rules say. The test now counts those warnings across the run and fails
if any appeared -- and it does fail, which is the honest result.

The same stage backs pattern extraction, which reports the same thing:

    WARN kb.memory.facts: pattern extraction gave no answer for memory 28..35

The module is attached and its grant serves 5889 (`serve=5889,...,5894`), and
the Go module implements both halves on that stage (`handleScanTurn` when the
request carries the scan magic, `handleExtract` otherwise). So this is a
protocol-level mismatch between the C caller and the module, not a missing
placement or a denied grant. Both consumers fail closed, which is why nothing
looked broken: turns that say "forget my X" quietly never retract, and pattern
extraction quietly contributes nothing. The LLM drain lane is unaffected, which
is why `test-drain-supersede` still has a working positive control.

Not diagnosed further here. It is a real functional gap, it is not an authority
defect, and the authority conclusions above do not rest on it.

## Correction: authority is the account, not the transport

The fix recorded above derived "is this caller a person" from
`attested_transport_t`. That was wrong, and the sections above that reason in
terms of UDS-vs-TCP should be read with this correction in front of them.

Authentication happens **once, at message receipt**. The channel (mTLS), the
session (bearer) and the account (OIDC or PAM) are each verified there, and a
request that fails any of them never reaches a handler. By the time a surface
asks who the caller is, identity is already proven. Re-deriving it per surface
spends work to re-learn something already established -- and, worse, arrived at a
different answer.

What the transport table got wrong:

- **An mTLS client certificate names one enrolled machine.** That is *narrower*
  than a person, not weaker: one person may connect several thin clients to the
  same server. It was classed with shared bearers as "not a person".
- **An OIDC subject is the same account whichever socket carried it.** The same
  person was the user over UDS and an anonymous agent over TCP.
- **aimee-kb already derived this from the authenticated principal**, so a single
  caller could be a person to one daemon and an agent to the other.
- `vault_capability.c:255` had it right the whole time, placing a verified client
  cert with UDS/webchat precisely because holding `cert:<CN>` "makes the grant
  expressible per client, which a bearer -- carrying no principal at all --
  cannot be."

`server_attested_is_person(transport)` is now
`server_account_is_person(account)`, with `server_request_account()` as the one
place every surface below ingress asks who the caller is. It reads the account
ingress already carried on the request: a thread-local read, no verification
repeated. `facts_retract_command` and `memory_delete_command` take the account
rather than the transport enum. On the kb, `kb_memory_request_authority` accepts
any authenticated principal -- OIDC, PAM host account, verified cert, or the
single-org owner -- where `KB_PRIN_CERT` had been excluded for the same reason
`ATTEST_MTLS_CLIENT` was.

The account is verify-then-trust at every entry: a kernel-verified UDS peer uid
resolved to a host account, a verified KB-signed identity token's subject, an
enrolled client certificate's grant, or a proxy stamp honoured only from the
root UDS hop or a caller presenting the vaulted ingress secret. A plain
authorized TCP client cannot choose its own. An empty account is a bare bearer:
it authorizes the call but names nobody, so it still acts with model authority.

The vault's transport checks are deliberately untouched. Those gate channel
confidentiality (D2b: never mint a server credential over plaintext), which is a
different contract from who the caller is.

### One behavioural widening, stated plainly

The kb's owner case previously required loopback. That qualifier was the same
transport reasoning and is gone. A bearer reaching the kb over the network still
has to pass the write-tier capability gate before it can write at all -- as every
run above shows, that gate refuses first -- so this does not open the destructive
path. It is still a widening, and it is called out rather than buried.

### What is proven live, and what is not

Re-run against the account-based build on CT 9078, real PostgreSQL, both daemons
and all modules attached: `test-retract`, `test-server-retract`,
`test-transport-authority`, `test-memory-delete` and `test-provenance` all pass
unchanged, so the refactor is not a regression on the paths that were already
covered.

**The mTLS and OIDC paths are proven by unit assertion only, not live.** This
container has TLS disabled (`tls_port=8743 set but TLS cert/key not loadable`,
and the mTLS ramp self-test refuses without a reachable DB1 pki module) and no
IdP, so no request has actually arrived here bearing `cert:<CN>` or
`oidc:<sub>`. What the live runs demonstrate is the account-vs-no-account split
using a PAM host account on one side and a bare bearer on the other. The claim
that a cert or OIDC caller is now treated as that account rests on
`server_account_is_person` being account-keyed plus the unit matrix over all
four account forms -- which is a much smaller inferential step than the transport
table required, but it is not the same as having run one.

## mTLS and OIDC, proven live

The correction above was first reported with these two paths covered by unit
assertion only. That was a limitation to remove, not to report, and removing it
found a defect that made one of them unusable in production.

### mTLS

`test-mtls-authority.sh`. Legs 2 and 3 are the whole test: same port, same TLS,
same bearer, same body, and the ONLY difference is whether a client certificate
is presented.

| leg | request | result |
|---|---|---|
| 1 | certificate from an unrelated CA | refused at the handshake (curl rc=56) |
| 2 | TLS + bearer, **no** certificate | 403 (caps), alice `A current` |
| 3 | TLS + the same bearer + trusted certificate | 200 `retracted:1`, alice `A gone` |
| 4 | the same certificate, Class-B fact | 200 `retracted:1` |

Under the transport table legs 2 and 3 were both MODEL. Leg 1 stops this being
read as "any certificate is accepted"; leg 4 stops it being read as "the
endpoint retracts for anyone".

Four things had to be built, and each failed in a way that named the wrong
component:

- **db1.** `server_tls_init_default()` runs the mTLS ramp self-test at startup,
  and that test IS db1 stage 19 (db1-pki). Without the module the ramp refuses
  and the operator is told `tls_port=8743 set but TLS cert/key not loadable` --
  which blames a certificate that is fine. It also needs its own
  `AIMEE_DB1_PATH`, and refuses outright rather than guessing, which is right.
- **The client certificate must be issued by aimee.** Verification is two gates:
  the chain against `mtls_client_ca`, then the serial against the durable roster
  (`pki_cert_check` then `db1_pki_cert_check`). An openssl-made cert clears the
  first and fails the second as "no longer valid (revoked, expired, or
  unrecognized)" -- which reads like a revocation rather than a cert that was
  never aimee's.
- **The server identity must NOT be hand-made.** An existing `server.crt` is
  authoritative and its key is sealed into the Vault on first load, with the
  on-disk copy erased. Regenerating it orphans the vaulted key
  (`Vault-held server TLS key does not match ...`) and TLS silently stops
  serving. Removing the cert lets pki generate and vault its own.
- **The certificate must be enrolled.** A verified cert still stops at the
  write-tier wall until its serial is bound to a grant
  (`db1_remote_client_tier`). `enroll-client-cert.sh` writes the row the wizard
  flow would have left behind.

### OIDC, and a defect that made it unusable

`test-oidc-authority.sh`, every leg over plain TCP so transport is constant:

| leg | request | result |
|---|---|---|
| 1 | JWT signed by a key **absent** from the JWKS | `unauthorized`, alice `A current` |
| 2 | no credential | `unauthorized`, alice `A current` |
| 3 | the real OIDC bearer | `retracted:1`, alice `A gone` |
| 4 | the same bearer, Class-B fact | `retracted:1` |

No network IdP is needed: kb verifies an RS256 bearer against a JWKS **file**,
so an issuer is a keypair, a JWKS document and a signer (`make-oidc-idp.sh`).

Getting there surfaced a real defect. Every OIDC bearer was rejected as
`unauthorized`, with a correct signature, a matching JWKS modulus, and matching
iss/aud/sub/exp. The cause was `char auth_val[512]` on the kb's plain-HTTP
listener (`kb_http_conn.c`), and `header_value()` **silently truncating** to fit:
a valid credential became a mangled one, which then failed verification and was
reported as a rejected credential.

Measured on the box, same key and same JWKS both times:

    338-byte token (plus 7 for "Bearer ")  ->  {"status":"ok"}
    509-byte token (plus 7 for "Bearer ")  ->  {"error":"unauthorized"}

A 2048-bit RS256 signature is 342 base64url characters on its own, and ordinary
claims (`nbf`, `azp`, `scope`, `email`, `name`) push a routine IdP token past 700
bytes. **OIDC bearer auth over that listener was unusable for essentially every
real token**, and it failed as an authentication error, so it looked like a bad
credential rather than a request the server never read in full. aimee-server has
always sized its own bearer buffer for exactly this
(`server_http_identity.c`, `tl_bearer[4097]`); the kb side had not, and the TLS
front end already answered 400 for the same condition while the plain path could
not, because `header_value` had no way to report it.

Fixed: the buffer matches the server's, and an over-long Authorization header is
now a 400 that says so rather than a 401 that misattributes it.

### What that leaves

Both account forms the design names -- OIDC and PAM host accounts -- and the
machine identity that accompanies them are now exercised against running
daemons, not asserted. The one account form still covered by unit assertion
alone is a **PAM host account presented to aimee-server over TCP**: that path
needs the management JWKS plane (signed envelope, manifest, and a hash-pinned
trust bundle cached in DB1) to verify a kb-issued identity token, which is a
provisioning exercise in its own right and is not what this charter changed.

## The two remaining gaps, closed

Two things were still stood in for. Both are now driven for real.

### Enrollment is now the real flow, not a seeded row

`enroll-client-cert.sh` wrote the `remote_client_grants` row by hand. That
skipped the step that makes enrollment mean anything: possession of the
client-generated private key, proved by a CSR. `enroll-first-user.sh` drives what
the product actually defines:

1. `POST /v1/deploy/apply` as a webchat user -> `server_http_first_user_bootstrap`
   claims the first user and returns an **enrollment-only** bearer, with no tier
   yet active
2. a keypair and CSR generated client-side; the private key never leaves
3. `POST /v1/cert/sign` presenting that bearer -> `pki_sign_csr` issues the
   certificate **and** `server_http_first_user_bind_cert` binds its serial to the
   grant, activating `full` only at that point

Two things gate step 1 and neither is a default: `AIMEE_DEPLOY_ENABLED`, and a
`webuser:` principal, which `vault_principal_resolve` grants only for an
`X-Aimee-Webuser` header arriving over the root-owned UDS. The same header over
TCP is a spoof and is refused a principal outright.

The grant the flow produced:

    principal      tier  cert_serial
    webuser:alice  full  7F07650580CFD3402FA2D0AE859A65E3

`webuser:alice` comes from the enrollment itself. Under the seeded version the
principal was whatever name the script chose, which is exactly the difference
between testing the mechanism and asserting it. `test-mtls-authority.sh` then
passes unchanged using that certificate.

The deploy half does go on to attempt `docker compose up -d`, which fails here.
That is not a workaround: the enrollment is claimed **before** the deploy starts,
deliberately, so a stack can never come up without a usable remote owner.

### An account over TCP to aimee-server

This needed the management trust chain, which the server does not accept in any
simplified form: it reads a root-owned, single-link, non-writable trust bundle
pinning an Ed25519 manifest key, then loads a **signed publication envelope**
from DB1 and validates it against that bundle.

The tree already has the rig for it. `write-tier-enforce-live provision` builds
the chain with the production functions -- real Ed25519 manifest signing, the
real envelope encoder, a real DB1 row -- so a change that breaks the real
publication path breaks this too. `provision-mgmt-trust.sh` drives it and records
the three variables the server then needs (`AIMEE_SERVER_ID` as the token
audience, `AIMEE_SERVER_TEAM_ID`, and the bundle path).

Building that rig first required a Makefile fix. The Makefile already documents
this failure and lists the auxiliary drivers that need the core archive as an
order-only prerequisite -- and `write-tier-enforce-live` and `identity-mint-live`
were not on the list. So on a clean tree the one driver that can prove a minted
token reaches a live server could not itself be built.

`test-account-tcp-authority.sh`, every leg plain TCP:

| leg | request | result |
|---|---|---|
| 1 | server bearer only, no identity token | 403, alice `A current` |
| 3 | identity token minted for a **different** audience | 403, alice `A current` |
| 2 | identity token for the account, correct audience | 200 `retracted:1`, alice `A gone` |
| 4 | the same account, Class-B fact | 200 `retracted:1` |

This is the case the transport table got exactly backwards: the same account,
over TCP, was MODEL purely because of the socket. Leg 3 stops leg 2 being read as
"any token is accepted"; leg 4 stops it being read as "it retracts for anyone".

Two things about how a caller presents one, neither obvious:

- The identity token goes in `Authorization` and the server bearer in
  `x-api-key`. Over TCP the server runs two independent checks on the same
  request: `server_http_authorize` wants a credential equal to the configured
  bearer, and `server_http_resolve_write_tier` reads the identity token out of
  `Authorization`. The token alone gets a 401 from the bearer check before the
  account is ever resolved -- a refusal that proves nothing.
- **`aimee.api.mtls` must be `off` for this probe**, and that is a real behaviour
  worth recording rather than a test convenience. `server_http_effective_conn_caps`
  gives a caller presenting no client certificate `CAPS_READ_ONLY` whenever mTLS
  is in optional mode, whatever its token says -- "optional-mode bearer fallback
  is deliberately weaker than a client cert". Only with mTLS off does the token's
  per-user tier reach the route gate. The first run of this test was refused for
  precisely that reason and looked like a broken account path.

  So the two probes want opposite postures: `test-mtls-authority.sh` needs
  `optional` because it is about the certificate; this one needs `off` because it
  is about the identity token ALONE, and with a certificate present the
  certificate would supply the account instead. `set-mtls-mode.sh` switches
  between them, and each test refuses to run under the wrong one rather than
  reporting a result that cannot mean what it appears to.

### Coverage now

Every account form the design names is exercised against running daemons:

| account | proven by |
|---|---|
| PAM host account, local | `test-retract.sh`, `test-server-retract.sh` |
| OIDC bearer | `test-oidc-authority.sh` |
| enrolled mTLS client certificate | `test-mtls-authority.sh` |
| KB-issued identity token over TCP | `test-account-tcp-authority.sh` |
| no account (bare bearer) | the negative leg of all four |

Nothing in the authority path is now covered by assertion alone.

## The P6 migration, fixed rather than worked around

`unblock-p6-migration.sh` is gone. The migration itself is fixed:

    UPDATE entity_edges SET epistemic_kind='world_fact'
      WHERE epistemic_kind IS DISTINCT FROM 'world_fact';

The `ALTER TABLE ... ADD COLUMN ... DEFAULT 'world_fact'` immediately above
already gives every existing row that value, so on the ordinary upgrade path the
predicate matches zero rows and the statement is a no-op -- the guard never
fires and the schema applies. The predicate is kept rather than the statement
deleted so a database carrying other values is still corrected, and if those rows
are semantic it is still refused loudly, which is what the guard is for.

`test-p6-migration.sh` reproduces the exact broken condition and is falsifiable
in the way that matters: it clears the one-shot marker, restarts the kb, and then
checks that **the marker came back**. A kb that started because the block was
SKIPPED looks identical to one that started because the block SUCCEEDED, and
skipping is precisely what the workaround did.

    semantic edges in the store: 8
    PASS: aimee-kb became ready with semantic facts present
    PASS: the one-shot block RAN to completion (it set the marker itself)
    epistemic_kind on semantic edges: world_fact|8

## Gap 2, re-examined against the merged tree

Re-running the drain probe after the merge showed the model's contradicting
write minting **Class A** and superseding the user's fact. Two separate things
were behind that, and only one of them was a defect.

### The defect: a note's authority did not reach its recorded actor

`db2_fact_actor_capture_memory` derived the actor from the REQUEST alone. The
drain reads that row, not `provenance_category`. So a person storing a note whose
provenance is `agent_message` recorded a **USER** actor, and the drain then minted
Class-A facts from model-composed text:

    memory  provenance      actor_role  authority_rank
    50      agent_message   user        30
    49      agent_message   user        30
    44      agent_message   (none)      (none)     <- older row, fell back to MODEL

Row 44 is why this passed before: memories queued before the actor table existed
fall back to MODEL, so the earlier green was partly luck.

Two derivations of one fact -- `provenance_category` from the write's authority,
the actor row from the request -- could disagree about the same memory. They no
longer can: the write's own authority now CAPS the captured actor. A caller's
identity may lower the recorded authority, never raise it above what the note
claims. A person storing agent-composed text is still storing agent-composed
text.

This was load-bearing. At Class A the note's actor had rank 30, which satisfies
`may_replace` (`actor->rank >= priors[i].authority_rank`, 30 >= 30) and genuinely
superseded the user's value. With the cap the note mints Class B and cannot.

### Not a defect: my original guard is now dead code

The merge wrapped the old `db2_entity_edge_upsert_semantic` body -- including the
class-rank guard added for gap 2 -- in `#if 0 /* superseded by fact_mutation;
retained temporarily for blame continuity */`, leaving that function a
compatibility shim that delegates to `db2_fact_mutation_assert`. The guard is
therefore unreachable.

That is fine, and the replacement is better. `db2_fact_mutation_assert` runs an
authority-rank check over the functional-relation priors and, when the incoming
actor cannot outrank the incumbent, QUARANTINES the write -- inserting it as a
`candidate` rather than dropping it, so the proposal stays on record for review
instead of vanishing. The original guard simply discarded it.

### The test was wrong a third time

`show()` judged currency by `superseded_at`/`invalidated_at`/`suppressed`, none
of which a quarantined row sets. A `candidate` therefore read as a live fact
sitting beside the user's value -- reporting a breach of gap 2 that had not
happened. Currency is `lifecycle_state IN ('persistent','promoted')`, the
product's own definition in `db2_fact_current_count`, and the test now prints the
lifecycle rather than collapsing it. It also asserts and exits non-zero instead
of only printing:

    30  class=A  persistent  [current]
    41  class=B  candidate   [not current]
    PASS: the user's Class-A value is the only current one

The expectation was reworded to match the design: the requirement is that the
user's value is the only CURRENT one, not that the model's write disappears.
Asserting absence would fail on correct behaviour.

## Two test-hygiene fixes found by re-running

- `test-oidc-authority.sh` now mints its own tokens. kb applies a hard token-age
  ceiling on `iat` (`AIMEE_KB_OIDC_MAX_TOKEN_AGE`, default 900s) independently of
  `exp`, so a token minted at setup was refused once the run happened more than
  fifteen minutes later -- with the same `unauthorized` a forged token gets, so
  the suite read as a broken account path when nothing was broken.
- The Authorization-truncation defect was swept for other instances, as a
  pattern rather than a single site. Every other Authorization buffer in the tree
  is already sized for long tokens (`server_http_*`: `KB_IDENTITY_TOKEN_WIRE_MAX`
  = 4096 or 4105; `kb_tls_serve.c`: `KB_TLS_AUTH_MAX` = 8192), and the Go
  listeners use `Header.Get` with no fixed buffer at all. `kb_http_conn.c` was
  the only production instance.

## `aimee kb grant set`: not a missing feature, a removed one

The earlier entry recorded that this command does not dispatch and left it as
feature completion. That was the wrong conclusion, and building it would have
been worse than leaving it.

`v1_route_requires_uds` keeps the reason:

> "The family this guarded, /v1/grants/write-tier, is gone: aimee-server no
> longer proxies write-tier grant administration. That is an operator action
> against aimee-kb, where the DB layer's admin-or-team-lead RLS check is the
> authority. Proxying it meant aimee-server needed an administrative identity on
> aimee-kb, which is precisely what a single-tenant data-plane service should
> not hold."

So the CLI marshaller, renderers and print table are leftovers of a deliberately
removed path, and re-wiring them would reintroduce the anti-pattern the removal
existed to fix. What survived the removal instead was every artifact that tells
an operator to use it:

- the server's 403 body (`server_http.c`)
- the route comment in `server_http_routes.c`, which described a UDS-only family
  and was followed by unrelated GET routes, reading as if they were below it
- `docs/UPGRADING.md`'s operator procedure
- `docs/QUICKSTART.md`'s "grant administration is local-socket only"

All four now say where grants are actually administered: aimee-kb's own
`/v1/write-tier-grants` routes, by a principal with admin or team-lead authority
in the target team.

### And the refusal was blaming the database

Running it (`test-grant-admin.sh`) rather than reading it produced:

    {"error":"grant administration requires the postgres backend"}

on a kb running Postgres. The log named the real code:

    WARN kb.grants: tenant scope refused for team 7 (rc=-104)

`DB2_ERR_TENANT_DENIED` -- "team not in principal memberships". An ordinary
authorization refusal, reported as a deployment fault.

`map_db_failure` mapped `rc < -1` to "requires the postgres backend", which
sweeps up EVERY tenancy code. Only `DB2_ERR_TENANT_REQUIRES_PG` (-100) means the
backend is wrong; -101..-104 are unauthenticated, no connection, scope-open
failure, and not-a-member. The function's own comment set out to keep these
apart:

> "Reporting them alike would have an operator debugging their credentials when
> the backend is simply wrong."

which is exactly the confusion it produced, in the other direction: an operator
whose credential is simply not a member of the team goes looking at the
database. Each code now has its own answer, and a membership refusal says so.

The unit test agreed with the collapse because it asserted on `stub_rc = -42`, a
value the tenancy layer cannot return. It uses the real codes now, and covers
the DENIED case that was wrong.
