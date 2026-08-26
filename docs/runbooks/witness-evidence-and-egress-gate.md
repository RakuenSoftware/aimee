# Runbook: the witness evidence chain, the offline verifier, and the P2b egress gate

The tamper-evident witness chain (P7). aimee-kb holds a hash-chained, WORM evidence
store and **is the system of record**; it exports every record, signed checkpoint,
and leaf snapshot outward as log/OTLP frames (metrics carry numbers only). This
runbook is for the operator who configures the retaining consumer, holds the trust
anchor, responds to an integrity alert, and understands exactly what the release
gate does and does not promise.

## 0. Know what the witness chain claims

**Claims.** Every witnessed event (a `vault.key_use` audit append, a reseal event,
a D3b open event) commits its evidence row **atomically, in the same transaction as
the source event**, so a source event can never commit without its evidence, and a
failed witness append aborts the source event. Signed checkpoints periodically bind
the shard heads under an Ed25519 root. Tampering is **detectable**:

- **Locally-inconsistent** tampering (an edited row whose stored hash no longer
  matches, a regressed shard sequence, a corrupt checkpoint signature) is caught
  **unconditionally** by the local cross-check and continuous verification.
- **Coherent rewrite or rollback:** when the attacker rewrites the rows and shard heads, then signs
  them consistently, detection requires comparison against a copy retained off-host (this runbook's
  offline verifier), because by construction the
  local store is then self-consistent.

**Does NOT claim.** It does not claim any downstream consumer retained anything, it
does not reconstruct history (that is the event-bus record/replay work), and it does
not defend a **fully-compromised single machine** that can rewrite all three of a
user's PC, aimee-server, and aimee-kb consistently. The defense against that is the
breadth of external copies, not the local chain.

> **Conditional-coverage statement (ship this to operators verbatim).** Detection of
> a coherent rewrite rests entirely on retained off-host copies. With a single
> consumer, coverage is exactly what that consumer retained. With several consumers,
> coverage is the **intersection** of what each retained over the incident window;
> a gap no consumer covered is a gap in detection. Retention is the operator's
> responsibility; aimee-kb cannot tell "no consumer configured" from "consumer down"
> and does not pretend to.

## 1. Configure a retaining consumer

Evidence rides the ordinary log path as base64 of the exact export frame:

```
<ts> INFO  kb.witness.evidence: kind=record     b64=<BASE64>
<ts> INFO  kb.witness.evidence: kind=checkpoint b64=<BASE64>
<ts> INFO  kb.witness.evidence: kind=snapshot   b64=<BASE64>
```

Point a log/OTLP collector at aimee-kb's stderr and **retain these lines durably**,
on a host the attacker would have to compromise separately. Retain **records as well
as checkpoints**. The record stream is what catches a fork between two emitted
checkpoints, which the checkpoint stream alone cannot. The `kind=` tag is a grep
convenience; the verifier reads the frame header, not the tag.

Numbers-only health metrics arrive on the P9a surface as
`aimee_org_witness_*` gauges: evidence count, shard count, latest checkpoint
sequence and age, and `aimee_org_witness_emit_backlog_records` /
`_backlog_checkpoints`. A backlog that only grows means retained copies are falling
behind. Alert on it.

## 2. The trust anchor

Each checkpoint is signed by a vault-held Ed25519 key derived from the server KEK
(`HKDF`), so it inherits the KEK's custody sealing and needs no separate storage. Its
**public** key is the out-of-band anchor a consumer pins. One anchor line per key:

```
<32-hex key_id>:<64-hex ed25519 pubkey>[:revoked]
```

The pubkey is KEK-derived and does **not** appear in the emitted frames (checkpoints
carry only the 16-byte `key_id`, a hash of the pubkey), so capture it from the kb
itself. A **key-holding kb logs the anchor once at boot**, in the exact anchor-file
form:

```
<ts> INFO  kb.witness: trust anchor (pin this with retained copies): <32-hex key_id>:<64-hex pubkey>
```

Grep that line from the boot log of a trusted instance and store it with the
retained stream, out of band. Because nothing rotates the server KEK yet, the anchor
is stable for the life of the KEK; a checkpoint naming any other `key_id` is a
foreign/restored database or tampering (see §4).

## 3. Verify a retained copy with `aimee-witness-verify`

Run it **entirely from captured bytes**, with no `aimee-kb`, database, or network, from a
host the attacker did not control. This is the detection-by-comparison tool.

```
aimee-witness-verify <stream-file> <anchor-file>
```

- **`<stream-file>`:** the concatenated emitted frames. Decode the `b64=` fields from
  the retained log lines and concatenate them in log order:
  `grep -oE 'b64=[A-Za-z0-9+/=]+' kb.log | sed 's/^b64=//' | while read b; do printf '%s' "$b" | base64 -d; done > stream.bin`
- **`<anchor-file>`:** the anchor lines from §2 (blank lines and `#` comments ignored).

Exit codes: **0** verified (continuity may be `unproven`, which is a reported work item, not
a failure); **1** tampering detected (broken chain, bad/unknown/revoked signature,
bad proof, a fork, a malformed frame); **2** usage/IO error.

The report lines break down record chains, checkpoint signatures, leaf-snapshot
root-rebuilds, and continuity. A `continuity: UNPROVEN` result means a checkpoint's
predecessor does not link. From bytes alone, this is indistinguishable from a suppressed
intermediate checkpoint. **Do not conclude "clean" on UNPROVEN**: compare the
cross-gap leaf sets against another retained copy to decide fork vs. gap.

To compare two copies (the coherent-rewrite case), concatenate both retained streams
into one `stream.bin` and verify: a record position that carries two **different**
records is a fork (`seq-conflicts` in the report); byte-identical repeats are benign
re-emission and are collapsed.

## 4. Responding to integrity alerts

All are logged under `kb.witness`. None crashes the kb; appends and (subject to the
gate) egress continue, but the alert must be actioned.

| log line | meaning | action |
|---|---|---|
| `INTEGRITY: checkpoint refused ... head_log_mismatch` | a shard head diverged from its evidence log; the producer refused to sign over it | a local inconsistency; investigate the shard immediately because the latest signed root is stale until resolved |
| `INTEGRITY: retained checkpoints failed verification (..; bad_signature / unknown_key / continuity_broken ...)` | continuous verification found a bad signature, a foreign key, or an impossible chain | treat as tampering or a restored foreign DB; verify against retained copies (§3) |
| `checkpoint continuity UNPROVEN over the retained window` | a predecessor does not link (gap or fork) | operator work item; compare cross-gap leaves against a retained copy |
| `INTEGRITY: witness record digest parity failed; emission halted` | a stored row and its canonical encoding disagree; emission stopped at that record | the store is corrupt or the encoder drifted; do not trust emitted bytes past this point, and investigate before resuming |
| `evidence emission sink rejected a frame; backlog will retry` | the log/OTLP path is full or failing | fix the collector; the durable store is unaffected, the backlog gauge shows the lag |

A **key-holding kb refuses to start** (`kb_witness_boot_check`) when it holds a
checkpoint signed by a key it cannot derive, or when it cannot confirm it can verify
its own evidence. That is intended: a kb that cannot check its own evidence must not
serve. The error names the offending `signer_key_id`.

## 5. The P2b egress release gate

On a **key-holding KB** with a real, unsealed TPM2, PKCS#11, or KMS custody anchor (never
`file`/`mock`), production egress is allowed only when **every** term holds, each
fail-closed:

1. live keys are allowed under the selected custody anchor;
2. witnessing is functional (the signing identity is derivable);
3. every retained checkpoint is signed by a key this kb can derive (no foreign key);
4. the latest signed checkpoint is not older than `KB_WITNESS_CHECKPOINT_MAX_AGE_S`
   (900 s, well above the 60 s checkpoint cadence);
5. continuous verification's last result was clean.

If any term fails, egress returns `503 egress unavailable`. A **dev / file-custody /
mock** KB has no live keys, so the gate is always closed there. This is expected,
not a fault. A kb whose checkpoint chain has **stalled** (term 4) or whose last
verification was dirty or unproven (term 5) closes egress until healthy; check the
`kb.witness` log and the `aimee_org_witness_checkpoint_age_seconds` gauge.

This gate is a **health/liveness** layer on top of the primary defenses (atomic
append; external comparison). It is not itself the anti-tamper mechanism against a
fully-compromised machine. The only bypass is a conspicuous
`AIMEE_P2B_INTEGRATION_TEST_OVERRIDE` build, which is never a production artifact.

## 6. Validation status

Validated on real PostgreSQL 17 (`.253`) and a software TPM (`swtpm`, `.253`):
source-plus-witness atomicity across all three ledgers, restart/hard-kill recovery
(a real process `kill -9`'d mid-cadence recovers with no gap and the emitted bytes
verify offline), all four tamper-detection scenarios, a canary scan proving no key
material in evidence, and the full release-gate conjunction under a real TPM2 anchor
(closed while sealed, open when healthy, closed on a stale chain or a foreign key).
The gate is fail-closed by construction: a dev kb is unaffected.
