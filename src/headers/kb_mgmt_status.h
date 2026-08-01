#ifndef DEC_KB_MGMT_STATUS_H
#define DEC_KB_MGMT_STATUS_H 1

#include <stddef.h>
#include <stdint.h>

#define KB_MGMT_STATUS_NONCE_LEN    32
#define KB_MGMT_STATUS_SIG_LEN      64
#define KB_MGMT_STATUS_KEY_LEN      32
#define KB_MGMT_STATUS_JSON_MAX     4096
#define KB_MGMT_CHECKPOINT_JSON_MAX 4096

typedef struct
{
   uint32_t version;
   char key_id[65];
   unsigned char nonce[KB_MGMT_STATUS_NONCE_LEN];
   char caller_issuer[601];
   char caller_serial_norm[129];
   char caller_fingerprint[65];
   char target_server_id[128];
   char target_mgmt_fingerprint[65];
   char purpose[32];
   uint64_t issued_at;
   uint64_t expires_at;
   uint64_t revocation_generation;
   unsigned char signature[KB_MGMT_STATUS_SIG_LEN];
} kb_mgmt_status_t;

typedef struct
{
   uint32_t version;
   char request_sha256[65];
   int revoked;
   uint64_t generation;
   uint64_t issued_at;
   uint64_t expires_at;
   char key_id[65];
   unsigned char signature[KB_MGMT_STATUS_SIG_LEN];
} kb_mgmt_checkpoint_t;

/* Build the domain-separated canonical transcript. */
int kb_mgmt_status_transcript(const kb_mgmt_status_t *, unsigned char *, size_t, size_t *);

/* Ed25519 raw-key signing and verification. Signing fills status->signature. */
int kb_mgmt_status_sign(kb_mgmt_status_t *, const unsigned char[KB_MGMT_STATUS_KEY_LEN]);
int kb_mgmt_status_verify_signature(const kb_mgmt_status_t *,
                                    const unsigned char[KB_MGMT_STATUS_KEY_LEN]);

/* Strict JSON wire codec. Numeric values are canonical unsigned decimal strings
 * so their complete uint64 range is not rounded through cJSON's double storage. */
int kb_mgmt_status_to_json(const kb_mgmt_status_t *, char *, size_t);
int kb_mgmt_status_from_json(const char *, kb_mgmt_status_t *);
/* Recover one canonical nonce from an otherwise-invalid object so the server
 * can consume an identifiable challenge on a failed verification attempt. */
int kb_mgmt_status_nonce_from_json(const char *, unsigned char[KB_MGMT_STATUS_NONCE_LEN]);

/* Validate fixed fields, time window, and generation anti-rollback. */
int kb_mgmt_status_validate(const kb_mgmt_status_t *, uint64_t now, uint64_t high_water);

/* Domain-separated, strict action-checkpoint response wire. */
int kb_mgmt_checkpoint_transcript(const kb_mgmt_checkpoint_t *, unsigned char *, size_t, size_t *);
int kb_mgmt_checkpoint_sign(kb_mgmt_checkpoint_t *, const unsigned char[KB_MGMT_STATUS_KEY_LEN]);
int kb_mgmt_checkpoint_verify_signature(const kb_mgmt_checkpoint_t *,
                                        const unsigned char[KB_MGMT_STATUS_KEY_LEN]);
int kb_mgmt_checkpoint_to_json(const kb_mgmt_checkpoint_t *, char *, size_t);
int kb_mgmt_checkpoint_from_json(const char *, kb_mgmt_checkpoint_t *);
int kb_mgmt_checkpoint_validate(const kb_mgmt_checkpoint_t *, uint64_t now, uint64_t high_water);

#endif
