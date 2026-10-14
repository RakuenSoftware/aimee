# Audit storage outage and regression coverage

The detached Git runner could not start because its configured Aimee server was
unreachable. The server's SQLite audit admission check rejected its ledger on
every startup. Docker's `unless-stopped` policy retried more than 31,000 times.
This was not a missing GitHub login.

## Evidence and limits

- CT 100 on .253, endpoint `192.168.1.100:8743`, ran production revision
  `816740e90` with Debian SQLite `3.40.1-2+deb12u2`.
- The 15,713,677,312-byte ledger contained 73 consecutive zeroed allocated pages,
  3778179–3778251. The separate zeroed 1 GiB locking page is normal SQLite layout.
- First recorded startup rejection: 2026-10-03 16:31:14 UTC. Missing rows span
  12:33:46–12:39:25 UTC that day.
- Offline recovery retained 45,906,190 rows. Sequences 45210997–45211872 are
  missing (876 rows); the recovered chain is not continuous and cannot be
  admitted as a verified live ledger.
- The original SHA256 is
  `a0425f6dd505f70de80c4f307581ce75b519823b87e8aabee04ff7d5c75d2ac6`.
- Diagnostic capture files consumed 31,820,458,357 bytes. Retention bounded file
  count, not file size: several individual sessions grew beyond 7 GB. Archiving
  verified oversized captures reduced root filesystem use from 97% to 54%.

The initiating mechanism of the zeroed pages is **not proven**. No retained host
I/O error establishes a storage-device failure. SQLite's documented WAL-reset
race affects the deployed engine and can lose checkpointed pages under concurrent
connections, but the damaged file alone cannot prove that race caused this
incident. Disk pressure is likewise established, but is not proof that ENOSPC
caused corruption. Do not report either hypothesis as a demonstrated root cause.

Reference: <https://sqlite.org/wal.html#walreset>.

## Repairs

- Pin SHA256-checked SQLite 3.51.3 in the application image, preserving common
  distribution features including FTS5. Production admission rejects unsupported
  engines before opening the audit ledger.
- Check WAL/durability configuration errors; report SQLite's actual count error
  instead of only the generic startup message.
- Bound each diagnostic capture to 256 MiB (a smaller limit can be configured).
  On exhaustion, record a durable `size_limit` replay-gap marker and continue
  recording the ledger. The existing 16-file policy now bounds newly produced
  capture storage to 4 GiB. This does not delete or rotate the WORM ledger.
- Advertise readiness failure below 512 MiB free on the audit volume, using the
  background sampler. Capacity recovery restores readiness.
- Bound automatic crash retries to five. Integrity admission remains fail-closed.
- Explicit operator recovery uses a separate `AIMEE_AUDIT_RECOVERY_EPOCH` database;
  it does not overwrite or rehash the original. `/v1/audit/verify` keeps aggregate
  and historical verification **red** and reports live-chain verification
  separately. A new epoch starts with the loss manifest and a keyed checkpoint.

Preservation includes stopped snapshot `pre-audit-recovery-20261014`, an external
original database copy, a separate recovered copy, and SHA256-verified backups of
all ten diagnostic captures. Private evidence is under the operator's
`aimee-audit-recovery-20261014` directory, outside the source repository.

## Regression coverage

`Audit storage regressions` runs on pushes and pull requests into main/testing:

1. Zero a real table page, reproducing the production SQLite error, and require
   startup rejection without silently accepting or rewriting the ledger.
2. Fill a real 1 MiB tmpfs. Require append failure, verify every successful commit,
   release capacity, reopen/checkpoint the ledger, and append successfully again.
3. Two independent writers commit 512 rows while a third connection repeatedly
   checkpoints WAL. Require exact row count, gap-free hashes, and restart
   verification. This stress test does not claim deterministic reproduction of
   the upstream race; the version gate and pinned fix cover that dependency.
4. Cross the capture-size bound; require a durable replay-gap marker, a bounded
   file, and successful ledger writes after capture stops.
5. Reject an injected unsupported runtime; verify the pinned runtime and FTS5
   compatibility. Preserve the existing tamper, checkpoint-MAC and sealing tests.
6. A healthy module/database graph with insufficient audit capacity returns 503;
   restoring capacity returns 200.

Additional appliance checks must confirm health, audit history reporting,
restart persistence, the selected SQLite runtime, and actual detached Git/PR
operations. Automatic Codex reviews remain enabled; connector writes are not used.
