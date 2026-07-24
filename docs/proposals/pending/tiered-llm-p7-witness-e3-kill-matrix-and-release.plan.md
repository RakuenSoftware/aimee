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

**Status: substantially covered on PG17; one custody-gated wrapper remains
validation-pending.** The boundaries are covered by three complementary gates,
each matched to what the boundary actually is, rather than by one fragile
SIGKILL-timing harness:

| boundary class | how it is proven | where |
|---|---|---|
| inside the source transaction (1–4) | a kill and an abort are the same event to Postgres, so aborting at each point proves it deterministically | `p7_witness_atomicity_pg_test.sql` |
| checkpoint scan / cross-check inside the REPEATABLE READ txn (6–7) | same — transaction-atomic; the producer refuses on `head_log_mismatch` rather than signing a divergent shard | `test_witness_tamper_pg` scenario 1 |
| signature-gen-before-persist (8) | transaction never commits ⇒ no checkpoint; the signed bytes in memory are simply lost and re-signed next tick | producer flow; covered by 4 |
| checkpoint committed, process dies before emission (9) | simulated restart (`db2_shutdown`+`db2_init`, a fresh pool re-reading only durable state) emits the committed checkpoint — never lost | `test_witness_recovery_pg` |
| snapshot/record emission dies mid-batch (10–11) | simulated restart resumes with no record skipped, no gap; re-emitted frames are benign duplicates, not forks | `test_witness_recovery_pg` |
| continuous verification mid-walk (12) | read-only, no durable state; a kill just re-runs next tick | trivially safe; exercised live by the daemon gate |
| **real process, real wall-clock timer, real `kill -9`** | the actual production cadence produces + emits on the live log path, is hard-killed (Postgres aborts the in-flight txn), restarts clean, and the emitted bytes verify offline; DB is gap-free | `run-p7-witness-daemon-kill.sh` |

The `witness_concurrency_exhausted` boundary (5) is the bounded-SERIALIZABLE-retry
wrapper carried forward from the E1 review (see the E2 plan §8); the two-producer
race is proven in the E2 plan's validation section.

**Remaining, validation-pending:** the boot **refusal** path
(`kb_witness_boot_check` returning −1) fires only when `kb_vault_live_keys_allowed()`
— i.e. under a real custody anchor (TPM/HSM/KMS). Its *logic* is proven
deterministically on real PG (`test_witness_tamper_pg` scenario 3: the coverage
check detects a foreign `signer_key_id`), and the daemon gate proves
`kb_witness_boot_check` runs at startup and returns 0 under file custody. What is
not yet exercised is the ~6-line wrapper actually refusing startup under a live
anchor. This needs a KMS/TPM custody harness and is the one item still owed before
the release-gate flip.

The reference kill points enumerated below are retained as the specification the
gates above discharge:

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

**Status: BUILT and validated on real PG17** (`src/tests/test_witness_canary_pg.c`).
Proves empirically what the format guarantees by construction:

- extracts the two real secrets a leak would expose — the server KEK, and the
  HKDF-derived witness signing seed (re-derived with the exact production params so
  it is scanned INDEPENDENTLY of the KEK) — and requires that neither appears in the
  emitted stream OR in a raw dump of every bytea/text witness column (raw and hex);
- requires the `provider_cred` sentinel to survive by **exact SQL equality** across
  all seeded rows (not substring, so a wrapped or digested value is caught), in both
  emission and storage.

Guarded against a vacuous pass: the KEK is asserted non-trivial, the scanner is
positive-controlled against a buffer that contains the KEK, and the table dump is
asserted non-truncated so a secret cannot hide past a cutoff.

Not separately exercised here: files and crash artifacts. The evidence never
touches the filesystem on the kb (the durable store is Postgres; emission is the
log path), and no witness code writes a temp file or is expected to core-dump with
evidence resident, so the DB + emitted-bytes scan covers the reachable surface. A
core-dump scan would require a real anchor build and is folded into the same
validation-pending item as the boot-refusal path.

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
