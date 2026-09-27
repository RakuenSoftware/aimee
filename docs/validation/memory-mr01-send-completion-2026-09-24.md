# MR-01 send completion and mandatory rule observations — 2026-09-24

This change supersedes the bounded lease in
[memory-mr01-storage-guard-2026-09-24.md](memory-mr01-storage-guard-2026-09-24.md).
A timeout cannot establish that a suspended sender will never resume. Committed
send guards therefore remain active until explicit completion, including across
owner connection loss and after their original five-second deadline. The deadline
still bounds normal HTTP acquisition/write scheduling and the temporal admission
window; it no longer grants permission to mutate protected source records.

Both owners acknowledge guard protocol version 2. The host refuses older
expiring-lease acknowledgements. Completion needs only the opaque check token,
not the retained source payload. Each unacknowledged owner is retried three times synchronously;
a generic successful response is insufficient without the release acknowledgement.
The first native outage run exposed a persistent guard after the owner recovered. The host now retains only the opaque completion requests and verified caller frame, then retries in a bounded background worker (60 rounds, at most 128 pending workers). Explicit owner acknowledgements are still required. Exhaustion or process loss leaves protection active for operator recovery. Private
migration 35 appends the change without changing migration 34's checksum.

Hard-rule retrieval observes both individual rule revisions and the collection
revision in one SQL statement. The collection is observed even when empty, so
adding a mandatory rule invalidates an earlier selection. Edits, delete/restore,
expiry and conversion to a soft rule invalidate retained observations. The native
projection carries these references to the same release owner; semantic rule
writes participate in the storage barrier. Normal acknowledgement of a retained
one-shot reminder remains possible while the guard is active, without allowing
an action edit or rearm.

## Interrupted-send recovery

There is no timeout-based or automatic background orphan cleanup. This favors
source consistency over write availability during an unresolved send. Operators
must use this sequence for each affected shared or private storage owner:

1. Stop and verify termination of the sending host and its delegated senders.
   A paused process, expired deadline or unreachable host is not termination.
   If sender identity is absent or shared by several instances, stop or fence
   every possible holder before proceeding.
2. As the storage owner, inspect `memory_send_leases` for `token`,
   `sender_identity` and `expires_at`. These fields contain no retained memory
   text. Runtime clients cannot list tokens or modify the guard tables.
3. For each token whose sender is now unable to resume, execute
   `SELECT memory_send_guard_end('<verified-token>')` in that owner's schema.
   This completion is idempotent. Never clear another live sender's guard.
4. Restart the host and retrieve fresh context before a new attempt. Preserve
   prepared/admitted/observed receipt history. Clearing a guard does not prove
   whether a provider received or processed the interrupted request.

## Validation

The initial strict-completion [full race and export run](memory-mr01-send-completion-2026-09-24/initial-race-export.txt)
passed in 222.815 and 5.183 seconds. The native
[completion acknowledgement tests](memory-mr01-send-completion-2026-09-24/ingress-completion.txt)
pass transient retries and unresolved completion. The
[rule/source targeted suite](memory-mr01-send-completion-2026-09-24/rule-targeted-race.txt)
passes in 14.380 seconds, including restricted runtime roles, empty collection
invalidation, rule edit/restore, deletion and protected mutation refusal.
The combined [full race suite and export build](memory-mr01-send-completion-2026-09-24/combined-race-export.txt) passed in 352.067 and 5.890 seconds. Native context refusal also passed. The first combined run exposed an outdated synthetic rule schema and a tiny protected allocation that no longer fits the mandatory revision metadata; its [failure log](memory-mr01-send-completion-2026-09-24/first-combined-failure.txt) is retained. The fixture now supplies the current schema and verifies explicit overflow at 128 tokens; no source contract is silently dropped. The [183-check HTTP run](memory-mr01-send-completion-2026-09-24/http/checks.json) passed on `c5212162e`, including owner termination, database restart, verified stopped-owner recovery, and both audiences for identity, preference and pending-commitment recall. The later native outage run failed on a mutation blocked by an unresolved completion; [failure evidence](memory-mr01-send-completion-2026-09-24/native-before/native-async.json) is retained. The background completion repair passes its native acknowledgement/recovery test; its new-image replay remains pending.

These results alone do not certify MR-01. Remaining endpoint/temporal contracts
and final Server/KB parity are tracked in the
[closeout checklist](../proposals/pending/memory-reliability-01-closeout.md).
Draft schema and application validation runs only on CT109. Released CT100 0.4.5
is not migrated to this candidate.
