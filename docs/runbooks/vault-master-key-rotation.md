# Runbook: rotate the vault `.server-master.key`

Rotate the server-sealed master key that protects the credential vault (D13 of the
[cred-vault-consolidation](../proposals/done/cred-vault-consolidation.md) proposal).
**Operator-gated, offline maintenance op** — never automatic.

## 0. What this does (and does not)

The 32-byte `.server-master.key` lives `0600` beside the vault at
`<AIMEE_HOME>/.vault/.server-master.key`. The **server KEK** is HKDF-derived from it and
wraps every server-decryptable credential's data-encryption key (DEK):

- the **server principal** (`server`) holds the server KEK as its **primary** wrap
  (`wrapped_dek`) — these are the shared delegate keys (minimax/mistral/glm/codex…);
- every **user principal** that has a dual-access entry holds it in `wrapped_dek_server`.

Rotation mints a fresh master key and **re-wraps** those DEKs from the old server KEK to the
new one. It is a **re-wrap, not a re-encrypt**: no credential plaintext, nonce, ciphertext or
tag is touched, and the per-user zero-knowledge `wrapped_dek` (under a user/login KEK) is
left untouched. Rotation defeats a leaked/suspected-compromised *master key file*; it is
**not** key-per-credential rotation and does not change the provider secrets themselves
(rotate those at the provider, then `aimee agent key import`).

Trust boundary is unchanged: the new master key is still a `0600` file on the server volume
(no HSM/KMS), defeating backup/disk theft, not host root.

## 1. Why it is offline

`aimee-server` caches one process-wide server KEK. A *live* rotation would leave autonomous
decrypts failing for the window between re-wrapping a credential and swapping the key — every
delegate would 401 mid-rotation. So rotation runs with the **server stopped**, against the
on-disk vault, and exits.

## 2. Procedure

```sh
# 1. Stop the server (so nothing reads/writes the vault during rotation).
systemctl stop aimee-server          # or: docker compose stop aimee-server

# 2. Rotate. Runs as the server's runtime user, with the same AIMEE_HOME.
#    Backs up the whole .vault/ directory FIRST, then re-wraps, then swaps the key.
sudo -u aimee AIMEE_HOME=/var/lib/aimee aimee-server --rotate-master-key

#    Example output:
#    aimee-server: master key rotated — re-wrapped 5 credential(s) across 1 principal(s).
#      Pre-rotation backup: /var/lib/aimee/.vault.rotate-bak.12345
#      Verify delegates authenticate, then remove the backup.

# 3. Restart and verify a delegate authenticates from the vault.
systemctl start aimee-server
aimee delegate minimax "ping" --persona engineer    # or any vaulted agent

# 4. Once verified, remove the pre-rotation backup (it holds the OLD-key ciphertext).
shred -u /var/lib/aimee/.vault.rotate-bak.12345/* 2>/dev/null || true
rm -rf /var/lib/aimee/.vault.rotate-bak.12345
```

## 3. Safety / failure handling

- **Atomic + reversible.** The entire `.vault/` is copied to `.vault.rotate-bak.<pid>` (0700)
  **before** any mutation. If any principal's re-wrap fails (wrong key / tamper) **or** the
  new master key cannot be persisted, the vault is **restored from that backup** and the
  command aborts non-zero with the vault unchanged — never a new-wrapped vault under an old
  master, nor the reverse.
- **Crash mid-run.** The new master key is written **last** (atomic tmp+rename+fsync), only
  after every re-wrap has succeeded. A crash before that leaves the old master + old wraps
  intact; a crash after is a fully-rotated, consistent vault. If in doubt, restore the
  `.vault.rotate-bak.<pid>` directory over `.vault/` and retry.
- **Fail-closed key reads.** A present-but-unreadable/short master key is **never**
  overwritten (it aborts), so a transient IO error cannot orphan the vault.
- **No master key yet?** If the vault has never had a server-principal write,
  `--rotate-master-key` reports "nothing to rotate" and exits 0 (the key is minted lazily on
  the first write).

## 4. Verification checklist

- [ ] command exited 0 and reported a non-zero re-wrap count (matching your agent count);
- [ ] `aimee delegate <agent> …` authenticates from the vault after restart;
- [ ] `<AIMEE_HOME>/.vault/.server-master.key` mtime is fresh and the file is `0600`;
- [ ] pre-rotation backup removed once delegates are verified.
