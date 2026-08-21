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

Seeded `user/email` as user-stated Class A, then had the action run with the agent's own
retraction-cue query:

    user-stated (Class A) before: A current
    after the agent's query:      A current

and the fact is still recalled into the block (`- email: theo@example.com`).

**This one is not yet proven, and the doc should say so.** The Class-B control *also*
survived, because the retraction scan is served by a memory module over the module bus
(`AIMEE_MEMORY_EVENT_EXTRACT_INDEX`) and no such module binary ships today — the kb logs
`retraction scan gave no answer; not retracting this turn` and skips the retraction
entirely. So the Class-A result here is consistent with the fix but does not demonstrate
it: nothing was retracted either way. The unit test `test_fact_ingest.c` covers this
path directly with a stub scanner, at both authorities, and passes on real Postgres.

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

## What this did not cover

- **The mTLS/distributed topology.** The asserted-caller path
  (`X-Aimee-Caller-Subject` → `KB_PRIN_HOST` actor) is wired and unit-tested, but this
  run used the plain-loopback deployment, so the host-actor branch of
  `kb_memory_request_authority()` was not exercised against a live mTLS listener.
- **The drain end to end.** Driving a stored note through to a committed typed fact
  needs the curator LLM lane, which had no synthesis endpoint configured here. The
  provenance→authority mapping is unit-tested (`fact_authority_from_provenance`, ten
  cases including NULL/empty/unknown) and the stamping is verified above, but the two
  halves were not observed joined up in one live run.
- **The memory module, and this is the largest gap.** The retraction scan AND the
  pattern extractor are both served by a memory module over the module bus
  (`AIMEE_MEMORY_EVENT_EXTRACT_INDEX`); only the client side and a header constant
  exist in the tree, and `kb_module_stage_adapters_configure()` registers the
  module-backed providers unconditionally with no in-process fallback. So in any
  current deployment the kb logs `retraction scan gave no answer` and `pattern
  extraction gave no answer`, and the whole typed-fact ingest path is inert.

  This is pre-existing and belongs to the module-migration work, not here. It is
  fail-closed in both directions — a missing scan retracts nothing, a missing
  extractor commits nothing — which is the correct way for it to be absent, and it
  is why the `get_context_block` scenario above is inconclusive. But it means the
  provenance→authority wiring, though verified at the stamping end, cannot be
  observed end to end until that module ships. Worth its own issue.
