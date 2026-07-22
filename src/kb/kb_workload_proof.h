/* kb_workload_proof.h: canonical P-256 helper proof verification. */
#ifndef DEC_KB_WORKLOAD_PROOF_H
#define DEC_KB_WORKLOAD_PROOF_H 1

#include "kb_workload_wire.h"

#include <stddef.h>

#define KB_WORKLOAD_PROOF_SPKI_MAX       256U
#define KB_WORKLOAD_PROOF_TRANSCRIPT_LEN 287U

typedef struct kb_workload_proof_key kb_workload_proof_key_t;

/* Load a canonical DER SubjectPublicKeyInfo containing exactly a valid P-256
 * public key. The input is fully consumed and must equal its DER re-encoding. */
int kb_workload_proof_key_load_der(const unsigned char *der, size_t der_len,
                                   kb_workload_proof_key_t **out);
void kb_workload_proof_key_close(kb_workload_proof_key_t *);
int kb_workload_proof_anchor_id(const kb_workload_proof_key_t *,
                                unsigned char out[KB_WORKLOAD_ANCHOR_LEN]);

/* Build the fixed 287-byte domain-separated transcript. Data bounds and the
 * empty ATTEST request/response hashes are enforced from `operation`. */
int kb_workload_proof_transcript(kb_workload_operation_t operation,
                                 const unsigned char challenge[KB_WORKLOAD_CHALLENGE_LEN],
                                 const unsigned char binding[KB_WORKLOAD_BINDING_LEN],
                                 const unsigned char *token, size_t token_len,
                                 const unsigned char proof_anchor_id[KB_WORKLOAD_ANCHOR_LEN],
                                 const unsigned char custody_anchor_id[KB_WORKLOAD_ANCHOR_LEN],
                                 const void *request_data, size_t request_data_len,
                                 const void *response_data, size_t response_data_len,
                                 unsigned char out[KB_WORKLOAD_PROOF_TRANSCRIPT_LEN]);

/* Verify a minimally encoded, low-S DER ECDSA-P256/SHA-256 signature. */
int kb_workload_proof_verify(const kb_workload_proof_key_t *, kb_workload_operation_t operation,
                             const unsigned char challenge[KB_WORKLOAD_CHALLENGE_LEN],
                             const unsigned char binding[KB_WORKLOAD_BINDING_LEN],
                             const unsigned char *token, size_t token_len,
                             const unsigned char proof_anchor_id[KB_WORKLOAD_ANCHOR_LEN],
                             const unsigned char custody_anchor_id[KB_WORKLOAD_ANCHOR_LEN],
                             const void *request_data, size_t request_data_len,
                             const void *response_data, size_t response_data_len,
                             const unsigned char *signature_der, size_t signature_len);

#endif /* DEC_KB_WORKLOAD_PROOF_H */
