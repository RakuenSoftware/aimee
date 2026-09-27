# Evidence-event isolation during indexed deletion

PR #2990 CI at `91a1f02969` intermittently returned `memory module unavailable`
for keyed shared destruction. Indexing the deletion fixture before mutation
reproduced a PostgreSQL `23514` violation of `memory_evidence_events_check`.
Bare parents could delete successfully, making the failure depend on background
index timing. The separately observed episode miss was a fixture-key collision;
its repair is recorded with [parent-version validation](memory-parent-version-lookups-2026-09-21.md).

The evidence trigger created one event per changed object, then rewrote every
event in the shared changeset with the latest object's references. A cascaded
purge followed by a derived-object update therefore gave the purge nonempty
references, violating its content-free audit contract. In non-purge batches the
same broad update could silently replace another object's references.

Schema 29 captures the exact inserted change ID and updates only its emitted
event. Purge events retain empty references; each other event keeps its own
object identity, transport proof and correlation. The existing purge constraint,
atomic receipt, canonical mutation, invalidation and audit checks remain enforced.
The narrower update also avoids rewriting the accumulated events on every item.
Existing audit history is preserved rather than reconstructed from current rows.

Validation:

- The runtime-role deletion regression first builds episodes, units and summaries,
  then exercises model retirement and user destruction, injected receipt failure,
  rollback, replay and exact-version admission. It checks each event's object
  reference and verifies multiple cascaded purges. The previous schema fails;
  the migrated schema passes this regression.
- [Schema 28 → 29 → 29](memory-evidence-event-isolation-2026-09-21/schema-upgrade.json)
  preserves exact earlier receipts, canonical records and evidence history. A
  mixed purge/edit changeset succeeds with isolated references; verifier ACLs
  remain restricted.
- The fresh E2E gate now waits for the fixture's actual indexing job and confirms
  its summaries and units before observing the deletion version. It never retries
  a failed mutation to obtain a passing result.
- Encrypted T2 at `a49f6c0101` exposed another fixture collision: the profile
  check expected a manually seeded generated mention to survive indexing. It now
  checks the persistent authored relation, retaining the same parent visibility
  assertion without relying on a temporary generated row.

The full Go/PostgreSQL memory suite, cross-object evidence-lifecycle SQL suite,
standalone export, all 77 lint gates, and documentation/link checks pass. Targeted
runtime-role race tests, native build/routing and all 17 S1 checks pass. Fresh application and harness `800f4ef2f4756bd3a083bd714c4d658f8cb34936`
passed **1,561/1,561 checks** on owned `.253` deployments: [969 T2 checks](memory-evidence-event-isolation-2026-09-21/fresh-t2-800f4ef2f4.json)
and [592 T3 checks](memory-evidence-event-isolation-2026-09-21/fresh-t3-800f4ef2f4.json).
This includes indexing before the destructive mutation, all seven typed source
checks, and all 37 asynchronous native checks per placement. Actual provider
request bytes remained within the configured 32 KiB ceilings. All nine
[image identities and three actual caps](memory-evidence-event-isolation-2026-09-21/image-identities-800f4ef2f4.json)
were verified; all nine owned containers and nine empty networks were removed,
preserving images, volumes and raw receipts. Latest-head GitHub CI was still
running when this evidence was collected. This repair
does not complete MR-02, MR-04 or MR-06, nor certify old event references as correct.
