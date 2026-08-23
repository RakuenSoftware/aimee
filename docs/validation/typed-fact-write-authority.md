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

### EXTRACT_INDEX (5889): a wrong diagnosis, corrected

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

**This diagnosis was wrong, and the correction is worth more than the original
entry.** It read: "the module is attached and its grant serves 5889, and the Go
module implements both halves on that stage, so this is a protocol-level
mismatch between the C caller and the module, not a missing placement or a
denied grant."

Every clause of that was inference. The grant did serve 5889 and the Go module
did implement the stage, so I concluded the fault must be between them. What I
never checked was whether the module was ATTACHED at the moment of the call --
"the module is attached" was assumed from a `state: RUNNING` line, and a running
process is not an attached one.

Once obs_bus started naming its own failures (see below), one line settled it:

    WARN obs_bus: module stage call failed: event=5889 stage=1 result=capability_absent

Not a protocol mismatch. The kb-side memory module was not on the bus at all.
The cause was ordinary and mundane: `test-p6-migration.sh` restarts aimee-kb to
apply the schema, every module attached to that bus dies with it, and nothing
brought them back -- so the rest of the run executed with memory and postgres
detached. `run-suite.sh` now re-attaches after any daemon restart and counts
`capability_absent` across the run, because a probe whose stage never ran still
prints PASS.

With the modules attached, `test-context-block` passes: the retraction pre-scan
answers, the agent's own words do not retract the user's fact, and the run is
about the authority decision rather than about a stage that was never reached.

The general lesson is the one this whole record keeps arriving at: a component
that "should" work by construction is not evidence, and a symptom shared by four
different causes ("gave no answer") is worth one line of instrumentation before
it is worth a theory.

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

## The diagnostic pattern, and its last instance

Five separate times this branch was slowed by a message that named the wrong
cause. They are one shape, not five incidents:

| reported | actually |
|---|---|
| `unauthorized` | an Authorization header truncated at 512 bytes |
| "retraction scan gave no answer" | a module call that never reached the module |
| "requires the postgres backend" | the caller is not a member of that team |
| "failed to store memory" | aimee-kb never finished starting |
| "TLS cert/key not loadable" | the mTLS ramp could not reach DB1 |

The first four are fixed above. The last one is `server_tls_init_default()`,
which has three unrelated failure paths and returned `-1` for all of them:

- the mTLS ramp self-test, which IS DB1 stage 19 (db1-pki)
- aimee's client CA
- the server certificate and its Vault-held key

Only the third has anything to do with a certificate, and the message named a
certificate for all three. During this branch's own validation that sent me to
inspect a certificate that was fine, while the real fault was a module that was
not running. Each cause now has its own code and its own sentence, and the ramp
failure points at db1 rather than at the cert.

### What is and is not proven for this one

`unit-test-server-tls-init-cause` pins the mapping: the three causes are
mutually distinct, the ramp cause does not mention a certificate and does name
db1, the identity cause still says "certificate", and an out-of-range code
reports "unknown" rather than inventing a specific fault.

`test_server_management_tls` already exercised one of these paths and asserted a
bare `-1`; its own comment said it "fail[s] at the stubbed client-CA step", so it
now asserts exactly that. A test that could not tell the three apart was part of
how they stayed collapsed.

The LIVE reproduction is not available on this container. `test-tls-failure-cause.sh`
stages it, and its second leg (db1 present, TLS must come up) passes, but the
first leg SKIPS: with db1's state already established here the ramp completes
even with no db1 module attached and the binary moved aside, so the failure
branch cannot be provoked. That is reported as a skip rather than a pass, which
would claim coverage that does not exist, or a failure, which would blame the
fix. Two earlier attempts at that leg were themselves wrong and are worth
recording:

- launching aimee-server by hand instead of through `start-server.sh` left it
  without the environment it needs, so it exited immediately and the ABSENCE of
  a "TLS DISABLED" line read as success while nothing had run at all
- moving the db1 binary aside did not remove db1: `install-db1-module.sh`
  refuses on a missing binary BEFORE it reaches its own `pkill`, so a module
  started by an earlier run stayed attached and answered the ramp perfectly well

## The two graph surfaces

Both were recorded earlier as data-model judgement calls. They turned out to be
different from each other: one was a defect with a small correct fix, the other
is genuinely a decision, and the attempt to treat it as a defect was wrong.

### `memory.entity_profile` could not see typed facts — fixed

`db2_memory_entity_profile_stats` counted relations from `memory_relations`
only. The typed-fact layer writes `entity_edges`. Because the "found" test is
`(mentions > 0 OR relations > 0)`, an entity known ONLY through typed facts had
no profile at all:

    user   -> "entity profile not found"    (2 live entity_edges, 0 mentions)
    Dana   -> "entity profile not found"    (live entity_edges, 0 mentions)
    deployment runbook -> profile, relation_count 0   (1 mention, no typed facts)

The relation count now sums both tables, with currency defined as the product
defines it (`db2_fact_current_count`: persistent or promoted, not superseded,
invalidated or suppressed) so a quarantined candidate is not counted as a
relation the entity has. Verified against ground truth rather than by the call
succeeding:

    entity: user  (live typed-fact edges: 2, memory_entities mentions: 0)
    {"status":"ok","profile":{"entity":"user","mention_count":0,"relation_count":2,...}}

The probe deliberately picks an entity with typed-fact edges and NO mentions,
because an entity with mentions would have been found either way and would prove
nothing.

### `relations.schema_list` is empty for a reason — and my fix was wrong

I tried to make this surface truthful by serving the compiled seed ontology when
`memory_relation_schema` is empty, on the reasoning that the seed is what the
enforcement path actually uses. That was wrong, and the test caught it.

`memory_relation_schema.relation_id` is a `memory_relation_kind_t`: the
CODE-GRAPH ontology (`depends_on`, `implements`, `fixes`, `calls`, `tests`,
`authored_by`). The typed-fact seed is a different vocabulary entirely
(`works_for`, `has_email`, `lives_in`). Feeding seed relations through that
struct mapped every one of them through `memory_ontology_relation_from_text`,
which returns `REL_OTHER` (99) for a name it does not know — so the surface
reported seventeen rows all saying "other".

That is worse than empty, because it looks like an answer. Reverted.

The finding stands as originally recorded: nothing in the tree writes
`memory_relation_schema`, so this surface can only ever be empty, and the
code-graph kind constraints it was meant to describe are unenforced. Whether to
populate the table or retire the surface is a data-model decision about an
ontology this branch does not touch, and it is left as one. The probe asserts
only that the surface is not serving the WRONG ontology, which is the failure I
introduced and would not want reintroduced.

## Running the suite

`run-suite.sh` sequences every probe under the configuration each one needs and
prints one summary. Until it existed, that knowledge lived in scattered comments
and in whoever last ran them, and getting it wrong produces a green run that
proves nothing:

- two probes need OPPOSITE mTLS postures, and each refuses the wrong one
- several destroy the rows they act on, so a re-seed belongs between them
- a probe that restarts a daemon orphans every module on that bus

The last of those is what it was built to catch. `test-p6-migration.sh` restarts
aimee-kb to apply the schema; the kb-side memory and postgres modules died with
it and nothing brought them back, so every probe after it ran with those stages
missing. They still printed PASS, because each consumer of a missing stage
reports "no answer" and carries on.

So the runner does two things beyond sequencing. It re-attaches modules after any
restart, and it counts `capability_absent` across the run:

    WARNING: N module stage call(s) were refused as capability_absent
             A probe whose stage never ran can still print PASS, so treat the
             results above as unproven until this is zero.

A skipped probe is reported as a skip and never folded into the pass count.

Current state, on CT 9078 against real PostgreSQL, both daemons and all modules
attached:

    pass 13   fail 0   skip 1

The skip is the TLS ramp-failure branch, which cannot be staged on this box.
Everything else is exercised against running daemons.

## The widening, measured and then closed

The account correction removed the loopback qualifier from the kb's owner case,
and that was recorded at the time as "one behavioural widening, stated plainly".
Stating it was not enough. Measured from a peer that is genuinely not the
container:

    alice before: A current
    remote peer, owner bearer, authority=user -> {"status":"ok","retracted":1}
    alice after:  A gone

A remote holder of the install bearer could destroy a user-stated Class-A fact.
An earlier run of the same probe answered `retracted: 0` and looked reassuring;
it was vacuous, because alice was not there. The probe now seeds first and
carries a loopback control, so neither outcome can be read off an empty table.

The qualifier is back for `KB_PRIN_OWNER` alone, and the reason is a distinction
the account model makes rather than an exception to it. An account NAMES someone:

    OIDC   an issuer-scoped subject -- the same person over any socket
    HOST   a local host account PAM accepted
    CERT   a verified peer, naming one enrolled machine
    OWNER  a SHARED install credential, naming nobody in particular

The first three identify a principal, so no transport qualifier applies. OWNER
is one bearer for the whole install and cannot say WHICH person is acting;
loopback is what makes it stand for "the operator at this machine" rather than
"whoever holds the token". A shared credential is precisely the case where there
is no account to decide with.

    remote peer  -> retracted 0, alice A current
    loopback     -> retracted 1, alice A gone

This also honours the decision recorded earlier in the work, which was
specifically "owner bearer ON LOOPBACK counts".

## Four probes that existed and were not being run

`run-suite.sh` reported 13 pass while four probes in the same directory were not
in it at all:

    test-retrieve-live       RETRIEVE, by cycling the kb-side module
    test-rerank-live         RERANK, by cycling the envelope
    test-chat-memory-stages  a real chat turn carrying the envelope
    test-retract-remote      the non-loopback owner bearer

A green summary that silently omits probes is the same failure as a probe that
passes without running its stage, one level up. The first three are now in the
runner, and SKIP rather than fail when the capture proxy is not up, since a
missing provider is a staging fact rather than a defect.

`test-retract-remote` is deliberately NOT in the runner: it must run from a peer
that is not the container, and running it from inside would make every call
loopback and prove the opposite of what it asks. The host invokes it.

With the provider and proxy up:

    pass 16   fail 0   skip 1

RERANK is worth one note. Without the capture proxy it reports NO-CAPTURE for all
three legs while still showing the warning delta, so the stage is exercised but
the envelope cannot be observed. With the proxy:

    module RUNNING    envelope PRESENT
    module STOPPED    envelope ABSENT + 1 "rerank confidence unavailable"
    module RESTARTED  envelope PRESENT

## EMBED (5891): the last unexercised stage, partly closed

This was the one memory-module stage never run. `aimee status` reported
"BLOCKED: no embedder configured -- memory and KB search cannot embed"
throughout, and the llama-server serving Qwen answers 501 for `/v1/embeddings`,
so the call path had never once executed.

`stub-embedder.py` is a deterministic embedding service, in the same spirit as
the JWKS file that made OIDC testable: the PATH is real, the vectors are not.

    embed requests seen by the endpoint: 31  (200s: 31)
    further requests while the embedder was down: 0

So the kb resolves a configured embedder, posts to it, and receives vectors, and
the control ties those calls to the store rather than to a health probe or a
retry.

**What is deliberately NOT claimed.** Persistence and recall. Memory embeddings
are not written synchronously by the store path, and this run produced no rows I
could attribute with confidence, so the probe asserts only what its evidence
supports and says so in its own output. Retrieval QUALITY is further out of
reach: the stub's vectors are deterministic but meaningless, and any ranking
judgement from them would be worthless.

Two things learned that are worth keeping:

- The two endpoints take DIFFERENT body shapes. `/embed_batch` takes a JSON array
  of strings; `/embed` takes **the raw text**, not JSON
  (`memory_core_scope_embed.c`: "the polarity rides in the query string because
  the body is the raw text itself"). A stub that assumes JSON on both answers 400
  to every single-text call.
- Those 400s then trip the embedding circuit breaker, which keeps refusing for a
  while: `WARN memory: embedding dependency unavailable; retry after 1237 ms`.
  So a later, CORRECT attempt fails for a reason that has nothing to do with it.
  The probe therefore starts the embedder BEFORE the kb, and the first version of
  it -- which took a baseline with the embedder down -- was tripping the breaker
  itself and then measuring the result.

## Suite

    pass 17   fail 0   skip 1

### EMBED persistence: a correction, and an open observation

The entry above said persistence was "not claimed" because the run "produced no
rows I could attribute with confidence". That was true by accident. I counted
`memory_vectors`, which does not exist; the query errored, the count read as
zero, and I recorded a limitation on the strength of a typo. The table is
`PGVEC_MEMORY_TABLE` in `pgvec_transport.h`, i.e. `memory_embeddings`.

Checking the right table changes the picture and raises a better question.

Vectors DO persist. `memory_embeddings` holds `halfvec(384)` rows, and `kb_meta`
records:

    schema_embedder_serving_id | aimee-e2e-stub-v1-dim384
    schema_embedding_dim       | 384

That serving identity is this stub, so those rows were produced by it: the write
path works and the vector space agrees end to end.

But a NEW store does not add one. Measured at 5, 10, 15, 20, 30 and 40 seconds
after storing a memory, the count did not move, while the endpoint took 15
successful embed calls in the same window and `vector_index_ops` sat at the same
16. The existing rows look like the product of a bulk path that ran when the
embedder first became available -- `db2_init` records the serving identity and
dim at that point -- rather than of the store.

This is left as an observation, not a verdict. It may be correct: a queue this
probe does not wait long enough for, or a sync deliberately kept off the store
hot path. Calling it a defect on this evidence would be the same kind of guess
as the "protocol-level mismatch" claim corrected earlier in this document. The
probe prints the counts so the next person starts from the measurement rather
than from my inference.

What IS established: the call path (15 requests, 15 × 200, none once the
embedder is stopped) and the write path plus vector space (rows exist, produced
by this stub, at the recorded dimension).

## The clean-install path, which had never been run

Every result above came from one container that accumulated state across a long
session: schema applied, certificates issued, grants written, an enrollment
claimed, modules attached, facts seeded. That makes "it works" a claim about a
hand-built deployment. Given how many ordering traps this record already
documents, assuming a fresh bring-up would be smooth was exactly the kind of
inference that keeps having to be retracted here.

So a second container was created from nothing (`pct create`), three times, and
the whole path run against it. Three defects fell out, all in the tooling, all
invisible on a box that had run before.

### aimee-kb would not start at all

    ERROR obs_bus: module grant policy is invalid: .../modules.d/kb
    aimee-kb: module bus failed to start

`install-postgres-module.sh` wrote a grant naming
`/usr/local/libexec/aimee-modules/aimee-module-postgres`, and created that binary
**after** the `grants`-mode exit. On a fresh box the file therefore did not
exist; `parse_grant_file` resolves `executable` with `realpath()`, an
unresolvable path makes the grant invalid, and ONE invalid grant fails the whole
module endpoint. The kb refused to start.

This is the failure mode documented much earlier in this record -- "one bad file
in modules.d takes the daemon with it" -- reached for real, from the one
direction that had never been tried. The binary is now ensured before the grant
names it, in both the install script and at deploy-time unpack.

### Half the suite could not be prepared

`deploy-all.sh` copied scripts from a hand-maintained allowlist of names, and the
list had drifted: `make-mtls-certs`, `enroll-first-user`, `provision-mgmt-trust`,
`make-oidc-idp`, `set-mtls-mode`, `test-embed-stage`, `stub-embedder.py`,
`run-suite` and others were never added. On the old container it did not matter,
because they had been copied by hand. On a fresh one the preparation steps
reported "No such file or directory".

A list maintained by hand is a list that will be wrong, and its being wrong is
invisible until someone starts from nothing. It now copies whatever is staged.

`write-tier-enforce-live` was likewise never shipped, so the account-over-TCP
probe failed with "not installed" -- a deployment gap reporting as a test
failure.

### The detector counted its own warm-up

With those fixed the suite ran, and the `capability_absent` detector fired. It
was right to look and wrong about the cause, twice over:

- `grep -c` PRINTS `0` and EXITS non-zero when nothing matches, so `|| echo 0`
  appended a second line and the arithmetic saw `"0\n0"`.
- The readiness poll IS a module stage call: `aimee status` asks event 11265, so
  its first attempt before the module attaches is itself a `capability_absent`.
  The detector was counting the suite's own warm-up as evidence of a broken run.

Both fixed: bring-up now WAITS for a stage to answer rather than sleeping a
guessed interval, and re-baselines the count afterwards, so what is measured is
stages missing *while probes run*.

### Result on a container built from nothing

    suite                      pass 17   fail 0   skip 1
    capability_absent during probes: 0
    non-loopback probe (host)  PASS
    explore.sh                 flagged 0
    explore-cli.sh             flagged 0

`prepare-suite.sh` now collects the steps a bare deployment does not provide --
API bearer, management trust chain, mTLS identity, first-user enrollment, OIDC
issuer, chat provider, capture proxy, seeded facts -- and makes their ORDER
explicit, since several depend on each other and on a restart in between.

### One hypothesis disproved

I expected the TLS ramp-failure branch to become stageable on a fresh box, on
the theory that db1 state was what let the ramp succeed without the module. It
skips there too. So the ramp genuinely does not require the db1 module in this
configuration, and the earlier explanation was wrong. The branch remains
unstaged, now for a reason that has been tested rather than assumed.

## Final round: the last read surface, and three probes that lied

### `relations.schema_list` — settled, and not by populating the table

This was the one item left standing as "a data-model decision, recorded rather
than guessed at". It is now decided, and the answer was neither of the two
obvious ones.

`memory_relation_schema` has a `CREATE TABLE`, an index, and **no writer
anywhere in the tree**. Nothing inserts a row. Meanwhile
`memory_ontology_validate()` — the function that actually decides whether a
triple is allowed — never reads that table. It enforces a static C table in
`memory_episodes.c`.

So the surface was served from one place and enforced from another, and the
place it was served from was permanently empty. Every deployment answered
`{"rows":[]}`, and the CLI consumer omits the section entirely when the list is
empty, so the emptiness was invisible: a reader would conclude the graph has no
relation schema at all.

Two fixes were considered and both are wrong:

1. **Serve the typed-fact seed through it.** Tried earlier and reverted.
   `memory_relation_schema` is keyed by `memory_relation_kind_t`, the code-graph
   ontology (`depends_on`, `implements`, `fixes`, `calls`, `tests`) — a
   different vocabulary from the typed-fact seed (`works_for`, `has_email`,
   `lives_in`). Every seed relation mapped to `REL_OTHER(99)`, producing
   seventeen rows all saying "other": worse than empty, because it looks like an
   answer.

2. **Seed the table from the code-graph ontology.** Also wrong, just slower to
   hurt. The validator would still not read it, so this creates a second source
   of truth for the same question, free to drift from the one that decides.

The fix serves the surface from the enforcing table itself, via a new
`memory_ontology_rules()` accessor, and **deletes** `db2_relation_schema_list()`
so there is only one answer. The rows now carry the integer codes and their
names:

    {"relation_id":11,"subject_kind":0,"object_kind":0,
     "relation":"co_edited","subject":"file","object":"file"}

Measured live: 18 rules, none `REL_OTHER(99)`, every row resolving to a named
relation. `test-graph-surfaces.sh` was upgraded from recording the emptiness to
asserting against it.

A unit test in `test_db2_node_kind_text_support.c` now ties the published set to
the enforced one: every advertised triple must pass `memory_ontology_validate()`,
the sentinel must not be published, and the list must be non-empty — an empty
list being the exact symptom of the defect.

The CLI cannot reach this surface (`aimee memory ontology list` is not in the
`/v1` route map, and the thin client only addresses `/v1` routes). That is
existing, lint-enforced design rather than a regression, and the surface is
reachable by the path the product itself uses — the kb action endpoint.

### Three probes that reported failures the product did not have

All three had the same shape: **a probe that cannot tell its own setup failing
from the thing it is testing failing.** That shape has now cost time three times
in this branch, so each fix names it.

**1. `test-embed-persist.sh` stored a fixed key.** It stored
`persist-probe-up` with fixed content, which works exactly once. On the second
run the key and text already exist, the store is a no-op, no memory row appears,
so no embedding rows appear — and the probe reported "nothing persisted with the
embedder reachable" on a system where embedding was working perfectly.

This false failure was expensive because the kb log was full of `embedding HTTP
request failed` lines that looked like corroboration. They were the probe's own
leg-2 control, which kills the embedder on purpose. A direct `curl` to the stub
then returned an empty body — taken after leg 2 had already killed it, so that
measurement was of a closed port, not a bad reply.

The key is now unique per run. Re-measured: 60 → 82 rows (+22: one `memory` row
and its `unit` rows), with the embedder-down control at 0 growth.

`check-stub-embedder.sh` was added so the stub's own contract is proven before
anything is concluded from an EMBED result: `/health` names a `serving_id`,
`/embed` returns a bare JSON array of 384 floats for a RAW TEXT body, and
`/embed_batch` returns one vector per input string. A probe that cannot
distinguish "wrong reply" from "nothing listening" cannot diagnose anything, and
both were reached inside one run.

**2. `enroll-first-user.sh` read success as failure.** The enrollment bearer is
returned only *until the identity is paired* — after that the standing grant
lives on the mTLS certificate, so there is nothing to hand back. A container
that has enrolled once answers `{"state":"paired","tier":"full"}` with no
bearer, and the script called that "no enrollment bearer in the response",
reporting the finished state as a broken one.

Chasing it did surface the real precondition. The route collapses several
distinct causes into one 500, but `first_user_bootstrap_locked()` logs which one
fired, and here it was `mtls=0`: enrollment requires
`config_server_api_mtls() > 0`. The script now accepts the paired state, and on
a genuine failure prints the server's own `first_user` log line and names the
mTLS precondition.

**3. `run-suite.sh` discarded the seed's exit status.** `seed()` was
`bash seed-facts.sh >/dev/null 2>&1`. `seed-facts.sh` already asserts that two
live rows landed — the runner just was not listening. When a seed failed, the
next probe found nothing to retract and reported *its own* assertion failing,
pointing at the authority code rather than the setup. That produced one
unexplained intermittent failure of "same body, both transports" before being
tracked down. A failed seed now prints the reason and adds `seed-facts` to the
failure list, on the same reasoning as the `capability_absent` counter: a probe
that ran against an unseeded store proves nothing.

### Final state

Both containers, from the rebuilt binaries:

    CT 9078 (long-running)     suite  pass 17   fail 0   skip 1
    CT 9079 (built from bare)  suite  pass 17   fail 0   skip 1
    9079, two consecutive runs        pass 17   fail 0   skip 1  (stable)
    non-loopback retraction (host)    PASS on both, with loopback control
    explore.sh                        flagged 0 on both
    explore-cli.sh                    flagged 0 on both
    capability_absent during probes:  0

Tree:

    make -C src lint          64 checks, all passed
    make -C src unit-tests    all passed (incl. ASan/UBSan variants)

The one SKIP is the TLS ramp-failure branch, which is unreachable in this
configuration: removing `aimee-module-db1` makes `db1.grant` invalid, and the
server refuses to start before the ramp can run. That is a tested reason, not an
assumed one.

### Does the same defect repeat? A scan, and what it found

The `relations.schema_list` defect is a shape, not a one-off: **a read surface
served from a table nothing writes.** A shape can repeat where nothing calls it,
so it was searched for rather than assumed unique.

Method: every `CREATE TABLE` in `schema.sql`, cross-referenced against writers
(`INSERT INTO` / `UPDATE` / `COPY` / `DELETE FROM` in C, Go and SQL, including
the SECURITY DEFINER functions inside `schema.sql` itself) and readers
(`SELECT ... FROM`). `src/schema_data.h` has to be excluded from both sides — it
embeds the entire schema as one C string literal, so it matches every table name
and turns the whole scan into false positives.

Nine tables came back writerless-but-read. Seven are read only from tests
(`kb_vault_rewrap_operation`, `kb_vault_rewrap_dek_stage`,
`kb_vault_witness_emit_cursor`) or from nothing at all once the schema blob is
excluded (`derivation_policy_versions`, `kb_management_action_intent`,
`kb_management_status_key`). Those are not surfaces.

Two have real production readers, and **neither is the same defect**, for a
reason worth stating rather than waving at:

**`memory_scenes` / `memory_scene_members`** back `aimee memory scene list|show`
and a retrieval boost in `memory_core_search_b.c`. Nothing writes them — and the
reason is explicit: `memory_cluster_scenes()` and `memory_assign_scene()` in
`memory_episodes.c` are stubs that `return 0`. The whole feature is gated behind
`config_memory_scenes_enabled()`, which reads a config key that is absent by
default and therefore returns 0. So this is an **unimplemented feature that is
switched off**, whose read surface is correspondingly and correctly empty.

**`stopwords`** is read by `memory_core_search.c` to filter search terms. With
zero rows the filter passes every term through. The column comment calls these
"promoted" stopwords — learned data an optional process would populate. Absent
data here degrades the ranking; it does not produce a wrong answer, and no
surface anywhere claims stopwords are in effect.

What made `relations.schema_list` a defect was not that its table was empty. It
was that **the system enforced a rule set and published a different, empty one**
— the surface contradicted the behaviour. Neither of these two contradicts
anything: one is a feature that does not run, the other is optional data whose
absence is honest. Fixing them would mean implementing scene clustering and a
stopword-promotion process, which are features, not repairs.

So: one instance of the shape, found and fixed. The scan is recorded because
"I checked" is a claim, and a claim of that kind should carry its method.

## Re-run on request: a false PASS in my own suite

Asked to run the full e2e again, I redeployed from a clean build (binaries
hash-verified identical on both containers, so "is the box running what I built"
is answered rather than assumed) and ran the whole sequence. `same body, both
transports` failed. Chasing it properly overturned an earlier diagnosis of mine
and then found something worse than the failure.

### The earlier diagnosis was wrong

I had previously seen this probe fail once, attributed it to a failed seed, and
added a seed-failure detector. This time the detector did not fire — so the seed
was fine and that explanation had been wrong. It was a guess that fitted the one
data point I had.

### What was actually happening

`aimee.api.mtls` is **global**, and two probes need opposite postures:
`test-mtls-authority.sh` needs `optional`, `test-account-tcp-authority.sh` needs
`off`. `same body, both transports` runs BEFORE either of them and simply
inherited whatever the last thing to touch the setting had left.

    prepare-suite leaves mTLS on  -> TCP leg gets 401 "a valid client
                                     certificate is required"  -> FAIL
    a previous suite left it off  -> TCP leg gets as far as the write-tier
                                     gate                      -> PASS

So the probe's result depended on what the *previous run* left behind. That is
not measuring the system. `run-suite.sh` now normalises the posture at bring-up
instead of inheriting it.

### The worse finding: the PASS was false

Fixing the ordering exposed the real problem. "It did not retract" is only
evidence about authority if the request actually REACHED the authority
derivation, and over TCP there are two walls in front of it:

- **mTLS** — in `optional`/`required` mode a caller with no client certificate is
  capped by `server_http_effective_conn_caps` before the route gate.
- **write-tier** — with mTLS off, the per-subject tier reaches the gate. A plain
  API bearer carries **no subject**, so it can never hold a write-tier grant, and
  the gate refuses it permanently.

The second is structural: this leg **cannot** reach the authority decision with a
plain bearer. Measured directly, the refusal is `permission_error` naming the
write-tier grant — the request never got near the authority code. Yet the probe
printed:

    PASS: TCP caller could not retract by assertion

which is true as a sentence and false as evidence. It is exactly the failure
class this suite exists to catch, in the suite itself.

The probe now classifies WHICH wall stopped it and reports accordingly:

    TCP refusal wall: write-tier
    PASS (NARROW): the TCP caller did not retract, but it was stopped at the
          write-tier wall IN FRONT OF the authority derivation, so this leg
          shows defence in depth and NOT that authority is derived from the
          connection.

### Gap 1 over TCP is still proven — by the probe that clears the walls

The claim is not lost, it just belongs somewhere else.
`test-account-tcp-authority.sh` presents a KB-issued identity token whose subject
holds a write-tier grant, so it clears both walls and reaches the decision.
Re-verified this run:

    PASS: a bearer with no account retracted nothing
    PASS: a token for another audience was refused
    PASS: the account retracted a Class-A fact OVER TCP
    PASS: the Class-B control retracted, so the endpoint is working

The third line is what proves the request reaches the authority derivation over
TCP; the first is the gap-1 negative with a working endpoint behind it. That is a
real proof, and it is why the overall conclusion is unchanged even though one
probe was overstating what it showed.

### A subsystem that was silently down

The sweep also turned up a line that had not appeared before:

    ERROR coord_dispatcher: failed to persist boot claim owner; dispatcher not started

`server_coord_dispatcher_init()` is called once from `server.c`, calls
`db1_runtime_state_set()`, and has **no retry** — so when it fails the dispatcher
is down for the whole process lifetime behind that single line.

Tested rather than assumed (`test-coord-dispatcher-boot.sh`): restarting with the
modules already attached starts the dispatcher every time. It is a race in MY
bring-up — `imms.sh` attaches modules after `start-server.sh`, so the server can
initialise before the db1 module is on the bus — and not a product defect. But a
suite that runs against a deployment with a subsystem silently down is measuring
the wrong box, so `run-suite.sh` now detects the line, restarts once with modules
attached, and fails the run if it persists.

### An observation error worth recording

Two SSH invocations hit their own client timeout while the suite was still
running. The runs did not die with the connections: several orphaned
`run-suite.sh` processes piled up, each restarting daemons under the others,
leaving duplicate `aimee-server` and `aimee-kb` processes on the container. The
observing side corrupted the thing being observed.

`run-suite-detached.sh` now starts the run with `setsid` writing to
`/root/suite.out`, and refuses to start if one is already in progress, so a
timeout on the caller can no longer damage the run.

A related trap, recorded because it produced a wrong reading for a minute:
`pgrep -cf /usr/local/bin/aimee-server` counts the shell running the pgrep when
that pattern appears in its own command line, so a clean container reported
"2 servers". Confirming with `pgrep -af` showed one real daemon and the counting
shell.

## The re-run found an unfixed gap 1, hidden by my own probe

The most important thing this re-run produced: **gap 1 was still open in the
get_context_block path**, and the probe named "agent query must not retract" had
been reporting PASS over it on every previous run.

### How it stayed hidden

`test-context-block.sh` seeds a Class-A row at `authority_rank 30` (a genuine
user fact), has the agent issue a model-composed query "please forget my email",
and prints the state before and after. Its ONLY assertion was that the retraction
scan had fired:

    PASS: the scan answered, so the surviving facts reflect an authority decision

It never asserted that the user's fact survived. So its own output read

    user-stated (Class A) before: A current
    after the agent's query:      A gone

and it still exited 0. The probe written to catch the sharpest form of gap 1 was
blind to gap 1 happening in front of it.

### The defect

Measured directly by row id, to rule out a formatted-string misreading:

    before:  871 A rank=30 life=persistent   inval=no
    after:   871 A rank=30 life=invalidated  inval=YES

`kb_handle_memory_context_block()` passes `FACT_AUTHORITY_MODEL` **structurally**,
with a comment explaining exactly why: `get_context_block` is an MCP tool, so
`query` is composed by the MODEL even though the request carries the human's
authenticated identity. That part was right. The authority was then discarded one
layer down, in `db2_typed_fact_ingress()`:

    if (db2_fact_actor_from_request(0, &actor) != 0 &&
        db2_fact_actor_internal(
            authority == FACT_AUTHORITY_USER ? FACT_ACTOR_USER : FACT_ACTOR_MODEL, &actor) != 0)

`db2_fact_actor_from_request()` is tried FIRST, and it returns `FACT_ACTOR_USER`
for ANY authenticated principal. The declared `authority` was only a fallback for
when that failed — so whenever a request context existed, the deliberate MODEL was
overridden. And a request context always exists here: the call is an MCP tool
invocation inside an authenticated human's turn. The parameter was passed
correctly and honoured never.

### The fix

The declared authority now CAPS the actor rather than serving as a fallback for
it: under anything but `FACT_AUTHORITY_USER`, the actor is built internally at
`FACT_ACTOR_MODEL` and the request context is not consulted at all.

This is the same shape, and the same reasoning, as the already-fixed
`db2_fact_actor_capture_memory()` sitting a few lines away in `fact_mutation.c`:
"a note stored at MODEL authority is model-composed text, whoever was
authenticated when it was stored". That principle had been applied to the
memory-write path and not to the retraction path — the defect repeating at a
sibling call site, which is exactly where this class of bug hides.

Verified after the fix, same measurement:

    before:  872 A rank=30 life=persistent  inval=no
    after:   872 A rank=30 life=persistent  inval=no

### Scope check

Every other caller of `db2_fact_actor_from_request()` was examined. The
`ontology_evolution.c` (×3) and `kb_http_console.c` (×5) sites pass
`require_operator=1` and REFUSE when it fails — they demand an operator rather
than accepting a declared authority, a materially different contract.
`kb_service_memory.c:63` gates a read-only diagnostic trace and mutates nothing.
`db2_fact_ingest_text_with_evidence()` uses only the declared authority and never
consults the request context. One instance, fixed.

### The probe now asserts the outcome

`test-context-block.sh` asserts both directions, because either alone is
explainable by the path being dead:

    PASS: the user's Class-A fact SURVIVED the agent's query
    PASS: the model-authored Class-B fact was withdrawn, so the retraction
          path is live and the Class-A survival above means something

### A guard that was broken in the healthy case

While fixing it, the scan-fired guard turned out to be broken too:

    scans="$(grep -ac '...' /root/kb.log 2>/dev/null || echo 0)"

`grep -c` PRINTS 0 and EXITS 1 when there are no matches, so `|| echo 0` appended
a second line and this captured `"0\n0"`. The comparison below then died with
"integer expression expected" and the `if` fell through to PASS. The guard whose
job was to catch a false positive was itself broken in exactly the healthy case
where the log holds no such lines. Same bug I had already fixed in
`run-suite.sh`; `head -1` now applied here too.

### What this changes about the earlier conclusion

The earlier report that "both charter gaps are fixed" was correct for the paths
measured, but one path that this suite explicitly claimed to cover was not
actually being measured. Gap 1 in `get_context_block` was open until this run.
It is now fixed and verified with a positive control, and the probe can no longer
pass without checking the thing it exists to check.

## Validation on a genuinely fresh environment (CT 9080)

Both previous containers had accumulated state — 9078 across the whole session,
and 9079 which started clean but has since been through dozens of runs, manual
restarts and hand-applied fixes. Neither is a clean-install proof any more. So
CT 9080 was created from `pct create` and taken all the way up.

### What the fresh box proved that the others could not

**The real enrollment flow ran end to end for the first time.**

    enrolled by the real flow: tier=full bound to F050F945FF51D6D3A499FB12A4FC5D95

On 9078 and 9079 this step always answered `state=paired` — they had enrolled
long ago, so every later run took the already-enrolled shortcut and the full path
(`/v1/deploy/apply` → enrollment bearer → `/v1/cert/sign` → grant bound to the
certificate serial) had not actually been exercised since it was written. On a
container that had never enrolled, it ran, and it worked.

**Result:** `pass 17  fail 0  skip 1`, from `pct create` to a green suite, with
the gap-1 fix verified directly on it:

    before:  39 A rank=30 life=persistent  inval=no
    after:   39 A rank=30 life=persistent  inval=no

### One cosmetic gap the fresh box exposed

`make-mtls-certs.sh` printed a bare `ls: cannot access
'/root/tls/client-ca.crt'` in the middle of a successful setup. The client CA is
written by aimee's own PKI when it signs the first client certificate — which
happens in `enroll-first-user.sh`, a LATER step. On a box that had enrolled
before, the file was already there and this always printed a reassuring line.

The ordering cannot be reversed: enrollment itself requires
`config_server_api_mtls() > 0`, so mTLS must be configured — pointing at the
not-yet-existing path — before the enrollment that creates it. The server
tolerates the dangling path at startup, which is what makes the sequence work.
The script now says so instead of printing an error during a healthy run.

### An open finding: the sqlite shim and real Postgres disagree

`run-pg-tests.sh` exists to catch exactly this class of thing — "a guard that is
correct under the shim and wrong under libpq would look green all the way to
deployment" — and on the fresh box it caught something:

    unit-test-fact-lifecycle     PASS
    unit-test-fact-ingest        FAIL
      test_fact_ingest.c: Assertion `db2_fact_current_count("user") == 1' failed

Instrumented, the divergence is precise: a USER-authority retraction of `email`
reports success and changes nothing.

    DBG before user-retract: count=2
    DBG ingress rc=0        count=2
    DBG semantic rows=2

What is established about it:

- **Not caused by this branch.** The pre-change binary, built from `HEAD~1`,
  fails at the identical assertion under Postgres. (My first attempt at this
  control was wrong — `git checkout` reverts to HEAD, which already contained my
  committed change, so the "pristine" build was my own. Rebuilt from `HEAD~1`
  explicitly.)
- **Not a build artifact.** Reproduced with a clean `OBJDIR`, which rules out the
  mixed-backend trap the Makefile warns about (`db2_test_shim.o`'s flags depend
  on `AIMEE_TEST_PG` and a flag change is invisible to make).
- **Not what the live system does.** On this same container, against the same
  Postgres, retraction works in every measured direction: the model-authored
  Class-B row IS withdrawn, an account retracts a Class-A fact over TCP, and the
  loopback control in `test-retract-remote.sh` retracts. So the production path
  is not broken; something about the unit fixture's rows is.
- **CI does not surface it.** `unit-tests-pg` runs three shards against real
  Postgres and passes on this commit.

I could not isolate the root cause within this session, and I am not going to
assert one. What I will not do is quietly drop it: it is recorded here with the
evidence, because a shim/Postgres divergence in the typed-fact retraction path is
worth someone's attention even though the deployed behaviour is correct.

### What it did produce: a real diagnosability fix

Chasing it exposed a defect in the code this branch already touches.
`db2_typed_fact_ingress()` called

    (void)db2_fact_mutation_invalidate(&actor, "user", attr, NULL, NULL);

throwing away every outcome the function distinguishes — `-2` for an
episode/experience that may only be annotated, `-1` for a policy needing operator
authority or a failed write. The turn then completed normally with the fact still
standing, so "I forgot it" and "I refused to forget it" were indistinguishable
from outside and left nothing in the log. That is the same silent-failure family
as the rest of this branch, and it is precisely why the divergence above took so
long to characterise.

Refusals are legitimate outcomes here — an annotate-only target SHOULD survive —
so the fix logs rather than fails the turn. What it must not do is stay silent.

(Note for anyone re-running the instrumentation: `LOG_WARN` produces no output in
the unit-test binaries. Even the three deliberately-triggered "retraction scan
gave no answer" paths print nothing there, so the absence of a log line in that
context is not evidence of anything.)
