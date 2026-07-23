# P7-witness-e2 atomic append, emission, and continuous verification

- **State:** planned; depends on E1 merging first.
- **Depends on:** E1 (record, checkpoint, export form, evidence log), P7-reseal
  D3b, the kb audit chain, and the P9a telemetry seam.
- **Enables:** E3's kill matrix and, only after it, the release-gate flip.
- **Umbrella:** `tiered-llm-p7-external-worm-witness.plan.md`.

## What E2 is

E1 built the evidence machinery with no caller. E2 makes it real: it wires the
witness append into all three source ledgers, produces checkpoints on a cadence,
emits everything outward, verifies the chain continuously, and ships the offline
verifier an operator actually uses during an incident.

**E2 does not touch `kb_egress_release_allowed()`.** That function keeps
returning 0. The gate belongs to E3, after the kill matrix proves the crash and
restart behavior of everything E2 introduces. A test matrix that runs after
release is not a release gate.

## 1. Atomic witness append at all three call sites

E1 provides one SECURITY DEFINER function that appends the evidence row and
advances the shard head in the caller's transaction. E2 calls it from exactly
three places, and nowhere else.

**Reseal — `kb_vault_rewrap_worm`.** Add the call inside the existing definer
functions in `src/db2/schema.sql` that already `PERFORM
org_vault_rewrap_worm_append`: the `intent`, `resealed`, `completed`, `abort`,
and `recovery_required` transitions. Each already performs its control-row
update, operation-row write, and WORM append in one plpgsql body, so the witness
append joins that transaction with no restructuring.

**D3b open — `kb_vault_open_event`.** Same shape: the completed-open and
idle-open functions already update `kb_vault_control`, insert the open event, and
`PERFORM` an audit append together.

**Admission — `kb_audit_event`.** This one requires modifying C, not SQL.
`db2_kb_audit_append` (`src/db2/kb_audit_worm.c`) opens its own `BEGIN`, reads
the current head, computes the row hash, inserts, and commits. The witness insert
and shard advance go **inside that function, before its existing commit**.
Wrapping a new transaction around the call would leave exactly the crash window
the atomicity invariant exists to close.

**The witness call must not inherit the audit chain's read-then-insert pattern.**
That function establishes its sequence and predecessor with `SELECT … ORDER BY
seq DESC LIMIT 1` followed by `INSERT`, which is not safe against concurrent
appenders. The witness shard advance uses E1's atomic `UPDATE … SET seq = seq + 1
… RETURNING` instead. Whether the pre-existing audit-chain pattern needs
hardening is a real question but a separate one; this slice must not silently
adopt it.

On `witness_concurrency_exhausted` from E1's bounded retry, the caller aborts the
source event. Evidence that cannot be appended means the source event does not
commit — that is the invariant working, not an error to swallow.

**Reachability is enforced by tooling, not review.** E1's gate asserted no
production symbol reaches the witness code. E2 asserts the inverse by the same
link-level or symbol-level check: the witness append symbol is reachable from
exactly these three call sites and nowhere else. A softer assertion here would
discard the precedent E1's gate exists to set.

## 2. Checkpoint cadence

A background producer signs a checkpoint every **N appended records or T
seconds, whichever comes first**, using E1's construction: scan the shard-head
table, build the depth-64 sparse Merkle tree, cross-check every head against the
head recomputed from the evidence log, and sign only if they agree.

The producer honors E1's rules exactly: fenced signer (take and revalidate the
`kb_vault_control` fence in the signing transaction, loser aborts before
signing), persist before emit, typed `head_log_mismatch` /
`checkpoint_shard_ceiling_exceeded` / `checkpoint_deadline_exceeded` failures
each raising an integrity alert.

A stalled checkpoint producer is a degradation, not a safe idle state: appends
and emission continue while new signed roots stop, so the unanchored window
grows. E2 surfaces the age of the latest signed checkpoint as a first-class
health signal for exactly this reason.

## 3. Emission

**Log/OTLP path — all evidence bytes.** Witness records, signed checkpoints,
their leaf snapshots, and inclusion proofs. Emission reads committed state only.

**Metrics path — numbers only.** Latest checkpoint sequence, latest checkpoint
age, evidence count, emission backlog depth, failed-send count, and
verification-failure counters, on the existing P9a surface
(`src/kb/http/kb_http_telemetry.c`). No bytes, no roots, no signatures.

Alerts fire on locally measurable state — backlog depth, failed sends, checkpoint
age — and never on inferred downstream health. With no consumer registry, aimee
cannot distinguish "collector is down" from "no collector configured," and must
not pretend to.

Emission never blocks admission. A full or failing emission path raises an alert
and drops nothing from the durable log, which is the system of record.

## 4. Continuous verification

A background verifier walks the evidence log and checks: witness predecessor
linkage per shard, the genesis sentinel on first-in-shard records only, source
predecessor linkage where the source ledger is a chain, epoch/fence against the
control row's history (typed `stale_fence`), and every retained checkpoint's
signature against the anchor set including its revocation list.

Failures raise typed integrity alerts. Verification runs on a cadence and at
boot.

**Boot fails closed on a key-holding kb** when verification is disabled, when the
trust anchor set is missing, empty, or malformed, or when a retained checkpoint
names a `signer_key_id` absent from the anchor set. This matches the existing
non-disableable-WORM posture: a key-holding kb never starts unable to verify its
own evidence.

## 5. The offline verifier tool

A standalone tool that takes emitted bytes plus an out-of-band trust anchor and
verifies them with **no access to aimee's database** — because during an incident
the database is the thing under suspicion.

It verifies a checkpoint's signature and root, an inclusion proof against a
signed root (honoring the key-hash bit ordering E1 pins), and a record range's
linkage between two checkpoints. It reports `continuity_unproven` versus
`continuity_broken` distinctly, and for every gap it surfaces the leaf sets
immediately before and after so the operator can compare heads across it.

`continuity_unproven` is rendered as a work item, never as a pass. The tool's
output must not let an operator mistake a routine emission gap for a clean
result — that is precisely the shape an attacker would hide a fork behind.

## 6. What E2 does not do

- No change to `kb_egress_release_allowed()`; production egress stays closed.
- No consumer registry, delivery receipt, watermark, or budget.
- No automated cross-consumer comparison; the tool is operator-driven.
- No claim that emitted evidence was retained by anyone.

## 7. Validation gates

### Unit / default build

- Cadence fires on both N-records and T-seconds, whichever first.
- Emission renders committed state only; an uncommitted checkpoint is never
  emitted.
- Verifier detects broken witness linkage, a wrong genesis sentinel, a stale
  fence, a revoked signer key, and a bad signature, each typed distinctly.
- The offline tool verifies from bytes alone with no database handle available.

### ASAN / UBSAN

- Emission and verification paths under injected allocation and I/O failure; no
  leak, no use-after-free, no callback after a fail-closed transition.

### Real PG17 on CT103

- Atomicity proven **per ledger** under injected failure at each statement:
  admission, reseal, and D3b open each either commit source-plus-witness together
  or neither.
- `witness_concurrency_exhausted` aborts the source event.
- Two instances racing to sign the same checkpoint sequence: the loser aborts
  before signing; no emitted checkpoint is ever uncommitted.
- Checkpoint cross-check catches injected shard-head tampering.
- Boot fails closed on missing, empty, malformed, and revoked-key anchor sets.

### CT260 real daemon

- Real aimee-kb, real PG17, swtpm, and a log/OTLP consumer **configured to
  durably retain the stream** — records as well as checkpoints.
- A locally rewritten chain is caught by comparison against retained copies,
  using the offline tool and nothing else.
- A fork hidden behind a suppressed checkpoint is caught by cross-gap leaf
  comparison; a fork between two emitted checkpoints is caught by the retained
  record stream. Both are exercised, because they are different attacks.

### Canary scan

- Database, files, logs, crash artifacts, and the bytes actually emitted, with
  the `provider:cred` field asserted to be a stable identifier and never a
  credential handle or wrapped-key reference.

## 8. Deferred

- The full restart and signal-level kill matrix, and only then the release-gate
  flip (E3).
- Automated cross-consumer comparison; it stays an operator procedure.
