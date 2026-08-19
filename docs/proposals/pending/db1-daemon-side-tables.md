# Proposal: the DB1 tables the daemon still opens directly

- **State:** OPEN. Every DECLARED family is served and `catalog_complete` is
  true, which is a weaker claim than it sounds: it means every family in the
  catalog is active, not that every DB1 table has a family. These do not.

Three files under `src/server/` still call `db1_conn()` and run SQL, and no
family claims them:

| file | `db1_conn()` | tables |
| --- | --- | --- |
| `pki.c` | 11 | `pki_certs`, `pki_mtls_ramp` |
| `server_mgmt_status.c` | 5 | `server_mgmt_nonce`, `server_mgmt_status_hwm` |
| `server_mgmt_jwks_cache.c` | 3 | `server_management_jwks_cache` |

This is the same gap `db1-transactions-across-the-boundary.md` noted in passing
("some of those tables have no family yet, so they are not blocked today -- but
they are the same shape"). They are now the only thing between this migration
and the doctrine it exists to serve, which is that the daemon holds no state.

## What is NOT the problem

Transactions. Every `BEGIN IMMEDIATE` in all three files opens and commits
inside a single function -- `persist_cert_row`, `pki_revoke`,
`pki_mtls_ramp_init`, `pki_mtls_note_presentation`, `pki_mtls_ramp_ready`,
`pki_mtls_ramp_advance`, `server_mgmt_nonce_issue_purpose`,
`server_mgmt_nonce_issue`, `server_mgmt_jwks_cache_install`. That is the case
the boundary already serves, and it is what made guardrail_state and lifecycle
migratable once the claim was checked rather than assumed.

Nor is it size. Most of `server_mgmt_jwks_cache.c` is bundle loading and
signature checking -- 780 lines of which three functions touch the database.
The storage seam is small, as it was for ensemble.

## What IS the problem: binary does not cross this wire

`server_management_jwks_cache` stores raw bytes:

    envelope_sha256   BLOB, exactly 32 bytes
    manifest_sha256   BLOB, exactly 32 bytes
    trust_bundle_sha256 BLOB, exactly 32 bytes
    jwks_bytes        BLOB
    envelope_bytes    BLOB

`db1-fields-v2` carries length-prefixed fields, so bytes CAN travel, but every
payload kind the generator has is NUL-terminated text -- a digest containing a
zero byte would arrive truncated, and `CRYPTO_memcmp` against a truncated digest
compares 32 bytes of which some are whatever was in the buffer. That is a
silent-comparison failure in the management-token trust path, which is the worst
possible place for one.

`pki_certs` will have the same shape (certificates and keys are bytes).

## The decision that is owed

A `blob` payload kind, almost certainly hex on the wire: a fixed doubling, no
escaping rules to get wrong, and a digest that arrives short fails to parse
rather than comparing as something else. The alternatives are base64 (denser,
more decoder to get wrong) or raw bytes with an explicit length (no encoding
cost, but every text-shaped emitter in the generator would need to stop assuming
NUL termination).

It is a small addition and I did not make it here, because the first thing it
touches is the cache that decides whether a management token is trusted, and
"the digests compare equal" is exactly the property a hasty encoding breaks
without failing loudly. It wants its own change, with a round-trip test that
puts a digest containing an embedded zero byte through the wire and back.

## What that unblocks

All three files, and with them the last direct `db1_conn()` calls in the daemon.
After that the only DB1 sources it links are `db1_init`, `db1_write`,
`diagnostics` and `maintenance` -- infrastructure rather than storage -- and
`secrets.c`, which never touched DB1 at all.
