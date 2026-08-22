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
