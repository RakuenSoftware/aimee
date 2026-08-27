#ifndef AIMEE_VAULT_WITNESS_RECORD_H
#define AIMEE_VAULT_WITNESS_RECORD_H

#include <stddef.h>
#include <stdint.h>

/* P7-witness-e1: canonical witness-record wire format and digest.
 *
 * A witness record is the *evidence* of one source-ledger event (an audit-chain
 * append, a reseal rewrap event, or a D3b open event) — never the audited
 * payload, never any key material. Its digest binds the complete logical event
 * so a copy retained off-host can be compared byte-for-byte against the local
 * store. A bug in encoding or digesting produces evidence that looks well-formed
 * locally yet is worthless for comparison, so the format is fixed here, matched
 * by the SQL side (`src/modules/db2/c/schema.sql`), and pinned by stored test vectors.
 *
 * This module is pure and production-uninvoked in E1: it has no database, no I/O,
 * and is not reachable from admission, the reseal orchestrator, or any route.
 */

#ifdef __cplusplus
extern "C"
{
#endif

/* Which local ledger the source entry came from. The three have different
 * identity shapes and different chain properties, so the witness must be able to
 * tell them apart; an unknown value is rejected, never ignored. */
typedef enum
{
   VAULT_WITNESS_SRC_AUDIT = 0, /* legacy/external audit source with predecessor */
   VAULT_WITNESS_SRC_REWRAP = 1, /* kb_vault_rewrap_worm: no source predecessor link */
   VAULT_WITNESS_SRC_OPEN = 2    /* kb_vault_open_event: row hash, no predecessor link */
} vault_witness_source_t;

/* Field caps. Tenant/provider are bounded shard-key components; actor matches the
 * legacy audit actor bound (575). These are validated by the decoder. */
#define VAULT_WITNESS_TENANT_MAX 128
#define VAULT_WITNESS_PROVIDER_MAX 128
#define VAULT_WITNESS_SOURCE_ID_MAX 256
#define VAULT_WITNESS_REQUEST_ID_MAX 200
#define VAULT_WITNESS_PRINCIPAL_MAX 575
#define VAULT_WITNESS_PROVIDER_CRED_MAX 256
#define VAULT_WITNESS_GROUP_ID_MAX 256
#define VAULT_WITNESS_TIMESTAMP_MAX 40

/* Largest a single encoded record can be, for buffer sizing by callers. */
#define VAULT_WITNESS_RECORD_MAX 2048

/* Domain-separation labels. Any change to a label is a breaking format change. */
#define VAULT_WITNESS_DIGEST_LABEL "aimee-vault-witness-v1"
#define VAULT_WITNESS_GENESIS_LABEL "aimee-vault-witness-genesis-v1"

/* One witness record. String fields are NUL-terminated C strings within their
 * caps. Hash fields are exactly 32 bytes. `has_source_pred` is 1 only for the
 * audit ledger (the one with a real predecessor); for rewrap/open it is 0 and
 * `source_pred_hash` must be all-zero. `is_first_in_shard` is 1 only for
 * shard_seq==1, where `witness_pred_hash` must equal the genesis sentinel. */
typedef struct vault_witness_record
{
   vault_witness_source_t source;
   uint64_t seal_epoch;
   uint64_t fencing_token;
   uint64_t shard_seq;
   int has_source_pred;
   int is_first_in_shard;
   uint8_t source_hash[32];
   uint8_t source_pred_hash[32];  /* all-zero when has_source_pred == 0 */
   uint8_t witness_pred_hash[32]; /* genesis sentinel when is_first_in_shard */
   char source_id[VAULT_WITNESS_SOURCE_ID_MAX + 1];
   char tenant[VAULT_WITNESS_TENANT_MAX + 1];
   char provider[VAULT_WITNESS_PROVIDER_MAX + 1];
   char request_id[VAULT_WITNESS_REQUEST_ID_MAX + 1];
   char principal[VAULT_WITNESS_PRINCIPAL_MAX + 1];
   char provider_cred[VAULT_WITNESS_PROVIDER_CRED_MAX + 1];
   char group_id[VAULT_WITNESS_GROUP_ID_MAX + 1];
   char timestamp[VAULT_WITNESS_TIMESTAMP_MAX + 1];
} vault_witness_record_t;

/* Compute the genesis sentinel for a shard key:
 *   SHA-256( VAULT_WITNESS_GENESIS_LABEL || pack_text(tenant) || pack_text(provider) )
 * where pack_text is a 4-byte big-endian length prefix (int4send) followed by the
 * UTF-8 bytes, matching `org_vault_rewrap_pack_bytes` in schema.sql. An all-zero
 * placeholder would be forgeable as "first record"; a shard-bound sentinel is not.
 * Returns 0 on success, -1 on invalid input (NULL, over-long components). */
int vault_witness_genesis_sentinel(const char *tenant, const char *provider,
                                   uint8_t out[32]);

/* Compute the shard-key hash used as the sparse-Merkle key: the leading 8 bytes
 * of SHA-256( pack_text(tenant) || pack_text(provider) ), returned big-endian.
 * Returns 0 on success, -1 on invalid input. */
int vault_witness_shard_key_hash(const char *tenant, const char *provider,
                                 uint8_t out[8]);

/* Canonical digest of the complete logical event: SHA-256 over the length-prefixed
 * preimage (see the .c for the exact field order). The digest — not the wire
 * layout — is the comparison anchor, and it is what the SQL side reproduces. A
 * naive concatenation without length prefixes is forgeable by shifting a field
 * boundary, so every variable-length field is length-prefixed. Returns 0 on
 * success, -1 on invalid record. */
int vault_witness_record_digest(const vault_witness_record_t *r, uint8_t digest[32]);

/* Encode a validated record to its self-describing wire form. On success writes
 * the byte length to *out_len. Returns 0 on success, -1 on invalid record or
 * insufficient buffer (`cap` too small). The output is cleansed on failure. */
int vault_witness_record_encode(const vault_witness_record_t *r, uint8_t *out, size_t cap,
                                size_t *out_len);

/* Decode and fully validate a wire record. Rejects: bad magic/version, a declared
 * length that does not equal `wire_len`, an unknown discriminator, an empty or
 * over-long shard key, any field over its cap, a non-32-byte hash region, a
 * set source-predecessor on a ledger that has none, a non-zero source predecessor
 * when the flag is clear, the genesis sentinel on a non-first record, or any
 * other value on a first record. There is no "unknown field, ignore" path.
 * Returns 0 on success, -1 on any rejection; `r` is cleansed on failure. */
int vault_witness_record_decode(const uint8_t *wire, size_t wire_len, vault_witness_record_t *r);

/* Constant-time equality by canonical encoding. Returns 1 if equal, 0 otherwise. */
int vault_witness_record_equal(const vault_witness_record_t *a, const vault_witness_record_t *b);

#ifdef __cplusplus
}
#endif

#endif /* AIMEE_VAULT_WITNESS_RECORD_H */
