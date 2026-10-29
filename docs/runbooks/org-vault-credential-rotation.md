# Rotate an org-vault egress credential

This rotates the provider credential behind one team's egress binding for one model: today the
IAM access key of a Bedrock slot. It is not the vault master key; for that see
[Rotate the vault master key](vault-master-key-rotation.md).

You file a request. The running `aimee-kb` daemon does the work, because only it holds the vault
custody and reads the slot's signed high-water mark. The request never carries key material, and
neither does any answer, log line or audit record: vendor references are key ids.

## Before

- The vault must use KMS custody. File, mock and TPM2 custody keep no signed high-water mark per
  slot, and the daemon rejects the request with that reason.
- The IAM user in the slot must be allowed to manage its own keys
  (`iam:ListAccessKeys`, `iam:CreateAccessKey`, `iam:DeleteAccessKey` on
  `arn:aws:iam::<account>:user/${aws:username}`), and must hold exactly one access key. A second key
  means somebody else owns one, so the rotation refuses rather than touch it.
- You need the KB database credential and admin authority (the owner principal).

## Rotate

```bash
aimee-kb vault rotate <team_id> <model_id> --wait
```

The daemon takes the request within about fifteen seconds and drives the fenced workflow:

1. **provision**: reads the current key through the vault (admitted and written to the WORM chain),
   deletes any key a lost earlier attempt created, then creates the new key;
2. **probe**: STS `GetCallerIdentity` with the new key must name the same user. A key IAM has not
   propagated yet is *uncertain* and is probed again on later ticks;
3. **activate**: the new version becomes current, and the signed high-water mark moves to it;
4. **revoke**: deletes the old key and confirms IAM no longer lists it;
5. **retire**.

`--wait` returns 0 when the rotation retires, 1 when it was rejected or failed, and 2 when it is
still running at the fifteen-minute limit. The daemon keeps driving it either way; a restart
resumes it from its last durable step.

Without `--wait` the command prints the request id and returns at once.

## Watch

```bash
aimee-kb vault rotations <team_id>
```

lists the team's requests, newest first, with the request state (`pending`, `accepted`,
`rejected` and why), the rotation it started, the rotation's state, and on failure the phase and
the vendor's error. The daemon also logs every rotation that retires or stops under
`kb.vault.rotation`.

## When a rotation fails

A failed rotation stays failed: the daemon does not retry a definite vendor refusal on its own,
and the slot cannot rotate again until it is resolved. Fix the cause (usually the IAM policy),
then:

```bash
aimee-kb vault remediate <team_id> <rotation_id> --wait
```

Remediation revokes the key the rotation created and never activated, confirms that with the
vendor, checks the high-water mark still names the old version, and retires the rotation. The old
key was never touched, so egress keeps working throughout. File a new `rotate` afterwards.

A rotation never fails *after* activation: once the new version is current, egress already uses
it, and the old key is only revoked. A revoke that IAM refuses or leaves unconfirmed is retried on
every tick until IAM confirms it; the rotation stays `activated` meanwhile, and after five minutes
without progress the daemon logs a `kb.vault.rotation` warning, then repeats it hourly. Fix the
policy and it completes by itself.
