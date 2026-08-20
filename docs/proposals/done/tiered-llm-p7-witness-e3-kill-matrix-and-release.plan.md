# P7-witness-e3 full kill matrix and the release gate

> **Archived proposal.** This records the design as it was agreed, not the
> system as it behaves today; parts of it have since diverged. For current
> behaviour see `docs/`, or the code.

- **State:** implemented and validated on branch `rewrite/go-server-wfe` (through
  `ecbfbc35`); merged to `testing` via PR #1930 (2026-07-24). The kill matrix (daemon hard-kill,
  boot-tpm under swtpm, hardened boot over verify-full TLS both directions), the
  tamper scenarios (incl. comparison catching a coherent rewrite against a retained
  copy), and the raw-key canary scans are delivered. The release-gate flip is live:
  `kb_egress_release_allowed()` returns `witness_release_gate_open()` (five
  fail-closed terms). §4b/§4c document the runtime-role grant and hardened-bootstrap
  fixes found in self-review.
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

**Boot refusal under a real anchor — now PROVEN** (`run-p7-witness-boot-tpm.sh` +
`aimee-witness-boot-tpm-harness`, built `WITH_TPM2=1`, validated on CT260 swtpm +
PG17). `kb_witness_boot_check` returning −1 fires only when
`kb_vault_live_keys_allowed()` — i.e. under a real, unsealed custody anchor. The
harness provisions a TPM-sealed KEK, unseals it (so `live_keys_allowed()` is TRUE),
and drives the full composition against a real Postgres:

- while SEALED: no live keys, the boot check is a no-op (returns 0);
- unsealed + evidence signed by the current TPM-derived key: returns 0 (does not
  refuse spuriously);
- unsealed + a retained checkpoint signed by an underivable key: returns −1 with
  the operator-actionable message.

The two halves were already proven separately (`test_witness_tamper_pg` scenario 3
for foreign-key coverage detection; the swtpm seal barrier for live-keys-under-TPM);
this exercises the glue end to end under a real anchor. No validation-pending items
remain in the kill matrix.

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
core-dump scan of a real key-holding process remains a deployment-time operator
procedure, deferred beyond this umbrella.

## 4. The release gate

**Status: IMPLEMENTED and validated end-to-end under a real swtpm TPM2 anchor.**
§1–§3 all pass, so `kb_egress_release_allowed()` (`src/kb/kb_vault_policy.c`) now
returns the real conjunctive answer instead of an unconditional 0.

The real answer is conjunctive, and every term is required (each fail-closed — a
query that cannot run closes the gate):

- live keys are allowed under the selected custody anchor
  (`kb_vault_live_keys_allowed()`, which already excludes `file` and `mock`);
- witnessing is functional — the signing identity is derivable (on a key-holding kb
  witnessing is non-disableable, so this is the observable "active" signal);
- the anchor set covers every retained checkpoint's `signer_key_id`, none signed by
  a key this kb cannot derive (`db2_witness_checkpoint_anchor_coverage`);
- the latest signed checkpoint is not older than `KB_WITNESS_CHECKPOINT_MAX_AGE_S`
  (900s, well above the 60s cadence) — `db2_witness_checkpoint_freshness`;
- continuous verification's last result was clean
  (`kb_witness_verification_last_clean`; not-run and UNPROVEN both read as
  not-clean).

Any term failing means egress stays closed. The conspicuous
`AIMEE_P2B_INTEGRATION_TEST_OVERRIDE` build path is unchanged and remains the only
bypass.

**Validated** (`run-p7-witness-boot-tpm.sh` + `aimee-witness-boot-tpm-harness`,
`WITH_TPM2=1`, on CT260 swtpm + PG17): gate closed while sealed (term 1); closed
before the first verification (term 5 fail-closed); OPEN on a fully healthy live-key
kb; closed on a stale chain and re-opens when made fresh (term 4); closed, and the
boot check refuses, on an underivable-key checkpoint (term 3).

**Scope of the gate.** It is a health/liveness gate on top of the primary defenses
(evidence commits atomically before key use; tampering is caught by external
comparison). On a kb with zero checkpoints terms 3–5 hold vacuously, so a fresh kb
is governed only by term 1 — deliberate, so the first egress is not deadlocked
before any evidence exists. It does not attempt to catch a fully-compromised single
machine, which is the external comparison's job and outside the single-machine
threat model.

**Revocation** is subsumed by coverage for now: with a single KEK-derived signing
key and no rotation, "revoked" and "foreign" are the same condition (a
`signer_key_id` not equal to the current derivable one), which coverage already
catches. A real revocation list arrives with KEK rotation and widens term 3 then.

**What flipping this gate does and does not claim.** It claims evidence is
durably committed before key use, is continuously emitted, and that tampering is
detectable — locally when inconsistent, by comparison when coherent. It does not
claim any downstream service retained anything, that the witness export rebuilds
history, or that the residual is zero. Operator documentation ships with the
conditional-coverage statement the umbrella requires, including that with several
consumers the property rests on the intersection of what each retained.

## 4b. Hardened-tier runtime-role grants (found in self-review)

A serious bug found during self-review and fixed: the witness cadence, boot check,
and release gate run on the kb's RUNTIME connection (`aimee_kb_runtime` on the
hardened tier — `db2_init` asserts the connected role is non-owner, non-super,
no-CREATE, and does NOT own any tenant table), but that role had the witness tables
and the cadence functions REVOKE'd, and the C producer read `kb_vault_control` and
`kb_vault_witness_checkpoint` directly. So on a hardened production tier — the only
tier where the gate's live-keys term is even true — the cadence would have failed
with permission denied, the boot check could not run, and the gate would never open.

Root cause of the miss: **every witness integration test ran as the DB owner**, so
the runtime role's grants were never exercised. Reproduced as `aimee_kb_runtime`
(every read/function call denied), then fixed:

- runtime gets **read-only** SELECT on the evidence tables (non-secret — exported as
  logs) plus a permissive read RLS policy; no INSERT (WORM blocks UPDATE/DELETE), so
  forgery stays impossible;
- runtime gets EXECUTE on the cadence/emit functions;
- a new definer `org_vault_witness_control_fence()` exposes only the fence, so the
  producer no longer reads the owner-only control row directly;
- the internal/definer-nested helpers (`append`, `verify_shard`, digest/genesis)
  stay runtime-invisible. The evidence **write** path is unaffected — it is nested in
  the admission/rotation definers and runs as owner.

Regression: `scripts/p7_witness_runtime_role_pg_test.sql` runs the full
cadence/boot/gate surface AS `aimee_kb_runtime` (every op succeeds; every
forge/control/internal path denied) plus structural `has_*_privilege` catalog
assertions, wired into `run-p1-rls-gate.sh` (which provisions the three-role split).
Negative-controlled: revoking any needed grant fails the gate.

## 4c. Hardened-tier bootstrap (found in the same self-review, fixed)

Pushing on §4b surfaced a deeper, PRE-EXISTING gap that is not witness-specific but
gated the release gate's production path: a hardened kb connects as a non-owner
runtime role, yet `db2_init` unconditionally applied the full schema (owner-only
DDL), so a hardened kb could not start at all (`permission denied for schema
public`). No deployment currently sets `AIMEE_KB_HARDENED`, so it was dormant and
untested — but the release gate's live-keys term only holds on the hardened tier, so
without this the gate could never run in production.

The design was decided with a roundtable and matches the existing owner/migrate role
model (approach C: deploy-time migration + a hardened runtime presence-check):

- `db2_init`, on `db2_hardening_enabled()`, runs `db2_verify_pre_provisioned()` — a
  fully read-only check (embedding dim recorded + matching, `schema_version` recorded
  + ≥ `AIMEE_DB2_SCHEMA_VERSION`, representative objects present via
  `to_regclass`/`to_regprocedure`) INSTEAD of applying. It never issues DDL and fails
  closed on any absence/mismatch/error. The dev/owner path is unchanged (still
  auto-applies), gated strictly on the hardened flag.
- `schema.sql` records `schema_embedding_dim` (DO NOTHING, keeping the C
  record-or-check drift guard authoritative) and `schema_version` (DO UPDATE) at its
  end, so a plain `psql -f schema.sql` migrate is self-sufficient. `AIMEE_DB2_SCHEMA_VERSION`
  (`db2/db_schema.h`) is bumped in lockstep with the `schema.sql` literal on any
  change a runtime kb depends on; out-of-sync fails closed.

Validated end to end on real PG17 + TLS (`scripts/run-p7-hardened-boot-tls.sh`): a
real `aimee-kb` with `AIMEE_KB_HARDENED=1`, connecting as a non-owner runtime login
role over verify-full TLS against an owner-migrated schema, boots via the verify
(no DDL) and the witness cadence produces checkpoints as the runtime role; and,
fail-closed, a hardened kb against an UN-migrated schema refuses to start with an
operator-actionable message and never connects db2. With this, the hardened tier —
and thus the production release gate — is unblocked.

## 5. Deferred beyond this umbrella

- Automated cross-consumer comparison; it remains an operator procedure.
- Event-bus record/replay integration, which supplies reconstruction and is owned
  by its own proposal.
- Hardening the pre-existing `db2_kb_audit_append` read-then-insert sequence
  assignment, which this umbrella deliberately did not adopt and did not fix.
