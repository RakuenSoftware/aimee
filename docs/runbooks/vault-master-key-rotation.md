# Rotate the vault master key

This is offline, operator-gated maintenance. It rewraps credential data keys; it does not change the
provider secrets themselves.

## Before

- stop `aimee-server` and prevent another instance from starting;
- verify `AIMEE_HOME` and the vault are owned only by the runtime user;
- take an encrypted, access-controlled backup of the full vault;
- record the backup checksum and restore command;
- confirm you can reissue provider credentials if restore fails.

The server caches its key. Never rotate against a live process.

## Rotate

Run as the server runtime user with the real `AIMEE_HOME`:

```bash
aimee-server --rotate-master-key
```

The command creates its own private pre-rotation backup, rewraps every server-decryptable data key,
and writes the new master key last with atomic replace and sync. A present but unreadable or short
key is an error; it is never overwritten.

## Verify

1. Check exit status and the reported rewrap count.
2. Start the server.
3. Probe one credential from each provider/agent class.
4. Run `aimee audit verify`.
5. Check the new key owner, mode `0600`, and modification time.
6. Restart once more and repeat a probe to prove the key was persisted, not only cached.

Keep the pre-rotation backup until all checks pass. Then remove it through the deployment's approved
secure-retention process. It contains old-key ciphertext and remains sensitive.

## Failure

If the command fails, leave the server stopped. Restore the complete vault and key from one backup as
a unit, verify permissions, then retry. Never combine a new key with old wraps or the reverse.

A leaked provider credential still needs rotation at the provider followed by a vault update. Master
key rotation alone does not revoke it.
