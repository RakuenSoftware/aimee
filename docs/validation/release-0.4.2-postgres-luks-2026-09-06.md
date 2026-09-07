# PostgreSQL LUKS validation, 2026-09-06

Status: the encrypted storage foundation passed its initial real-container test.
The full optional-KB, immutable-role and unified-image release scope is still in
progress. This report does not declare 0.4.2 ready.

## Implemented foundation

The PostgreSQL Go module obtains one volume-bound unlock credential through core's
restricted Vault resource. Vault is its sole persistent store. The PostgreSQL
container receives it through a local socket into locked, non-dumpable memory,
then sends it to cryptsetup on stdin. There is no key environment variable,
command-line key, plaintext key file, TPM enrollment or alternate credential store.

The storage supervisor initializes LUKS2 and ext4 before launching the existing
secure PostgreSQL entrypoint. TLS private material and reconciliation logs stay
inside the encrypted mount; only the public trust certificate is exported. The
supervisor stops PostgreSQL before unmounting and closing the device. It rejects
unknown existing data, mismatched volume identities and duplicate storage owners.

Vault mutations now serialize across core resource processes. Corrupt Vault files
are errors rather than absent credentials, and concurrent master-key creation
cannot replace the winner. Explicit master-key rotation retains its existing
replacement behavior and preserves the LUKS credential.

## Evidence

A disposable VM, `9432` (`aimee-luks-042-20260906`), was created on `.253` for this
validation. The previous unprivileged container test environment cannot exercise
real device-mapper mounts. Production guests were not changed.

The real container harness passed **11/11** checks, recorded in
[postgres-luks.json](release-0.4.2-postgres-luks-2026-09-06/postgres-luks.json):

- No database files before Vault unlock; successful first boot with real PostgreSQL.
- One LUKS2 keyslot, with no external enrollment tokens.
- A database canary reads back correctly and is absent from raw ciphertext.
- Application restart, clean database restart and concurrent-owner rejection.
- Missing/corrupt Vault records leave ciphertext unchanged and prevent initdb.
- Restoring Vault and recovering after `SIGKILL` retain the database record.

Native Vault tests passed with 128 concurrent credential writes, 16 competing
master-key creators, corrupt-store rejection, volume binding, and master-key
rotation. Go storage and module-runtime tests passed with the race detector.

The real run exposed and resolved container-specific issues: the core host has
large sparse static arenas, so the protected helper uses on-fault memory locking
with an explicit memory-lock limit; device-mapper's block major must be discovered
from the host; and loop devices require explicit atomic allocation because the
container does not run udev.

The offline PostgreSQL 18 upgrade passed **13/13** checks in
[postgres-luks-upgrade.json](release-0.4.2-postgres-luks-2026-09-06/postgres-luks-upgrade.json),
including complete Unicode record preservation, source-tree verification and
restarts of the migrated encrypted cluster. The original plaintext volume is
preserved read-only for rollback; its eventual removal is an operator action.

## Follow-up validation

This report records the storage foundation at the time it was tested. The later
[unified deployment report](release-0.4.2-unified-2026-09-07.md) covers standard
vector extensions, both immutable roles, local models, Compose and browser setup.
Publishing and release qualification are tracked there separately.
