# Data governance and retained history

PII belongs in Vault. Memory retains an opaque reference to the protected value, with the
original provenance and applicable access policy. A reference identifies a value; it does not
grant permission to resolve it. Raw PII must not be copied into ordinary memory, derived facts,
embedding inputs, model requests, ordinary logs or content backups.

A refused write is not a successful protected write. A correct write first establishes encrypted,
principal-bound Vault custody, then publishes the reference. Failure or an unknown result must
not fall back to plaintext, another principal or an older credential. Retries must reconcile the
same operation rather than creating unrelated custody entries. The current candidate's ordinary
memory path does not yet satisfy this contract; its bounded screening and optional protected
intake must not be represented as automatic PII protection.

## Retention and availability

Domain records and their history are retained. Retirement, invalidation, revocation and custody
unavailability determine whether a record can be used. They do not authorize physical removal of
payloads or history. There is no subject-erasure workflow to enable. Existing retained records
must not become available again through old creation retries or backup restoration.

Keep access-policy changes, protected-reference generations, provenance and unavailable outcomes
in the relevant history. WORM evidence remains append-only and must not copy protected values.
Operational cleanup of a disposable test environment is separate from application record lifecycle.

This is the required data contract, not a claim that every existing credential API already follows
it. The Vault's legacy mutable delete/overwrite paths require separate review against retained
history; the immutable custody contracts alone do not establish that every Vault entry is retained.

## Storage and backups

Vault encryption protects credential values; it does not encrypt Native memory journals, SQL,
embeddings, canonical transcripts or ordinary log files. PostgreSQL's optional LUKS storage covers
its own filesystem. Protect the other stores independently and do not infer encryption from the
presence of a Vault.

The default software-custody Vault root key is stored beside its ciphertext. A matched backup of
that directory includes both. Such a backup supports recovery but does not protect itself against
someone who can read it. Protect matched backups with independent encryption and custody of the
backup recovery key. Do not describe an unencrypted complete backup as theft-protected merely
because its credential values are ciphertext.

Restore matched application home, Vault, Native state and PostgreSQL snapshots into an isolated
instance with the original identity and keys. Verify reference ownership, current availability,
record versions, integrity and failure behavior before using the restored copy. Do not regenerate
missing keys as a recovery shortcut or reactivate retired data.
