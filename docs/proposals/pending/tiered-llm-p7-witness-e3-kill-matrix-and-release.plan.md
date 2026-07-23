# P7-witness-e3 full kill matrix and the release gate

- **State:** planned; depends on E1 and E2 merging first.
- **Depends on:** E1, E2, and every merged P7 reseal slice through D3b.
- **Enables:** the P2b production release gate — the last open P7 line item.
- **Umbrella:** `tiered-llm-p7-external-worm-witness.plan.md`.

## What E3 is, and why the gate lives here

E3 runs the exhaustive restart and signal-level kill matrix across every P7
durable boundary — the reseal boundaries D3b enumerated plus the evidence-append,
shard-advance, checkpoint, and emission boundaries E2 introduced — and **only
after that matrix passes** makes `kb_egress_release_allowed()` return a real
answer.

The gate is here rather than at the end of E2 for one reason: a test matrix that
runs after release is not a release gate. E2 introduces the durable boundaries;
proving their crash behavior is what earns production egress, so the code change
that opens production sits in the slice that does the proving.

**The gate is not a boolean over "the matrix passed."** It is a per-boundary pass
list with typed evidence for each entry. "The kill matrix passed" as a single
assertion is exactly the kind of claim that survives a partially-skipped run.

## 1. Kill matrix

**Status: NOT STARTED — this is what blocks the release gate.** It needs a real
aimee-kb process on a host with swtpm and a retaining consumer (the plan's CT260
shape), driven through its normal interfaces and killed at each boundary. Nothing
in E2 or §2 substitutes for it: they prove the logic is right, not that a kill
between two durable writes leaves a recoverable state.

Build exact head with PostgreSQL 17, libtss2, swtpm, and a log/OTLP consumer
configured to durably retain both records and checkpoints. Drive a real aimee-kb
process through its normal interfaces, with more than 257 retained DEKs and
multiple shards spanning several tenants and providers.

Kill the daemon after each externally observable or durable boundary:

**Witness boundaries (new in this umbrella):**

1. witness record encode, before the append transaction opens;
2. shard-head `UPDATE … RETURNING`, before the evidence insert;
3. evidence insert, before the source event's commit;
4. the combined source-plus-witness COMMIT;
5. `witness_concurrency_exhausted` after bounded retry exhaustion;
6. checkpoint shard-head scan, mid-scan;
7. checkpoint log cross-check, immediately before a detected mismatch;
8. checkpoint signature generation, after signing and before persistence;
9. checkpoint COMMIT, before emission;
10. leaf snapshot and inclusion proof emission;
11. record emission, mid-batch;
12. continuous verification, mid-walk.

**Reseal and operator boundaries (D3b's matrix, re-run with witnessing active):**
every boundary D3b enumerated, from frame parse and typed authorization through
NV increment, active-blob rename, promotion, terminal cleanup, open COMMIT, epoch
synchronization, and mutation response — now with a witness append inside each
transaction that has one.

After each kill, restart using only PostgreSQL, swtpm state, and the artifact
directory. Required outcomes:

- The source event and its witness row are both present or both absent. Never one
  without the other, for any of the three ledgers.
- No shard head advances without its evidence row, and no shard sequence has a
  gap.
- No checkpoint is emitted that is not durably committed.
- Replaying the same request never creates a second operation, NV increment,
  evidence row, shard advance, checkpoint, or open edge.
- Status resolves to exactly one safe state per boundary, and restart never
  auto-resumes.

## 2. Tamper detection, proven end to end

**Status: BUILT and validated on real PG17.** All four scenarios are implemented
and passing:

| scenario | where | result |
|---|---|---|
| locally inconsistent tampering | `src/tests/test_witness_tamper_pg.c` | caught locally: `verify_shard` raises `P7W01` and the producer refuses to sign (`HEAD_MISMATCH`) |
| coherent local rewrite | `src/tests/test_witness_tamper_pg.c` | local verification passes **and** the post-tamper stream alone is conflict-free (both asserted); comparison against the retained copy exposes it — 3 duplicates at untouched positions, 2 conflicts at rewritten ones |
| fork behind a suppressed checkpoint | `src/tests/test_witness_tamper_scenarios.c` | `continuity_unproven`, `any_tamper == 0` — a work item, neither a pass nor a tamper claim |
| fork between two emitted checkpoints | `src/tests/test_witness_tamper_scenarios.c` | continuity `OK` and every signature valid — detection comes **only** from the retained record stream |

Run via `scripts/run-p7-witness-integration.sh` (live-store half, isolated
databases) and the `unit-test-witness-tamper-scenarios` unit target (offline half).

Two properties of the suite are deliberate and load-bearing:

- **The negative half is asserted, not just the positive half.** The coherent
  rewrite test requires that local verification *passes* and that the post-tamper
  stream alone shows *no* conflict. Without those assertions the scenario could
  silently degrade into scenario 1 — reproducing easy detection while appearing to
  prove the hard case.
- **Every detection assertion was negative-controlled.** Removing the fork drops
  `records_conflict` to 0; removing the suppression returns continuity to `OK`. A
  detector that always fires detects nothing, so the suite is checked to fail when
  the tampering is absent.

Four scenarios, each starting from a healthy witnessed deployment with a
retaining consumer:

- **Locally inconsistent tampering** — break a witness predecessor link, regress
  a shard sequence, corrupt a checkpoint signature. Continuous verification
  detects each locally, with no external round trip, and types the reason.
- **Coherent local rewrite** — rewrite evidence rows and shard heads together so
  every local artifact agrees. Local verification passes; the checkpoint
  cross-check catches the head divergence via `head_log_mismatch`, and where the
  attacker also suppressed checkpoints, comparison against retained copies
  catches it.
- **Fork behind a suppressed checkpoint** — emit A, B, and E; suppress C and D
  while forking between them. The offline verifier reports
  `continuity_unproven`, surfaces the cross-gap leaf sets, and the operator's
  comparison exposes the fork.
- **Fork between two emitted checkpoints** — no gap at all; A and B are both
  emitted, signed, and mutually consistent with a rewritten interval. The gap
  affordance does *not* fire. Detection comes only from the retained record
  stream. This scenario is mandatory precisely because it is the one the
  checkpoint stream alone cannot catch.

Each runs through the offline tool with no database access, because that is the
posture during a real incident.

### What §2 does and does not discharge

It proves the *detection logic* is correct against a real store and real emitted
bytes. It does **not** discharge §1: these tests drive the library and SQL surface
directly, not a real aimee-kb process being killed at a durable boundary. Crash
atomicity remains unproven until §1 runs. The release gate in §4 stays closed.

## 3. Canary scan

Database, files, logs, crash artifacts, and the bytes actually emitted to the
consumer. No raw KEK or DEK material anywhere. `provider:cred` asserted to be a
stable identifier, never a credential handle or wrapped-key reference.

## 4. The release gate

Only after §1–§3 pass does `kb_egress_release_allowed()`
(`src/kb/kb_vault_policy.c`) return a real answer instead of an unconditional 0.

The real answer is conjunctive, and every term is required:

- live keys are allowed under the selected custody anchor
  (`kb_vault_live_keys_allowed()`, which already excludes `file` and `mock`);
- witnessing is active and chain verification is on — non-disableable on a
  key-holding kb, per the umbrella;
- the trust anchor set is present, well-formed, and covers every retained
  checkpoint's `signer_key_id`, with no revoked key accepted;
- the latest signed checkpoint is not older than its configured bound;
- continuous verification's last result was clean.

Any term failing means egress stays closed. The conspicuous
`AIMEE_P2B_INTEGRATION_TEST_OVERRIDE` build path remains the only bypass, and
remains conspicuous.

**What flipping this gate does and does not claim.** It claims evidence is
durably committed before key use, is continuously emitted, and that tampering is
detectable — locally when inconsistent, by comparison when coherent. It does not
claim any downstream service retained anything, that the witness export rebuilds
history, or that the residual is zero. Operator documentation ships with the
conditional-coverage statement the umbrella requires, including that with several
consumers the property rests on the intersection of what each retained.

## 5. Deferred beyond this umbrella

- Automated cross-consumer comparison; it remains an operator procedure.
- Event-bus record/replay integration, which supplies reconstruction and is owned
  by its own proposal.
- Hardening the pre-existing `db2_kb_audit_append` read-then-insert sequence
  assignment, which this umbrella deliberately did not adopt and did not fix.
