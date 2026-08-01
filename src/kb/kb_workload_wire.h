/* kb_workload_wire.h: strict binary wire contract for the workload helper. */
#ifndef DEC_KB_WORKLOAD_WIRE_H
#define DEC_KB_WORKLOAD_WIRE_H 1

#include "kb_workload_provider.h"

#include <stddef.h>
#include <stdint.h>

#define KB_WORKLOAD_WIRE_HEADER_LEN  16U
#define KB_WORKLOAD_WIRE_FRAME_MAX   65536U
#define KB_WORKLOAD_WIRE_PAYLOAD_MAX (KB_WORKLOAD_WIRE_FRAME_MAX - KB_WORKLOAD_WIRE_HEADER_LEN)
#define KB_WORKLOAD_WIRE_TOKEN_MAX   16384U
#define KB_WORKLOAD_WIRE_PLAIN_MAX   KB_WORKLOAD_UNWRAP_CAP
#define KB_WORKLOAD_WIRE_CIPHER_MAX  KB_WORKLOAD_WRAP_CAP
#define KB_WORKLOAD_WIRE_PROOF_MIN   8U
#define KB_WORKLOAD_WIRE_PROOF_MAX   80U

typedef enum
{
   KB_WORKLOAD_OP_ATTEST = 1,
   KB_WORKLOAD_OP_WRAP = 2,
   KB_WORKLOAD_OP_UNWRAP = 3
} kb_workload_operation_t;

typedef struct
{
   const unsigned char *ptr;
   size_t len;
} kb_workload_wire_view_t;

typedef struct
{
   kb_workload_operation_t operation;
   kb_workload_result_t status;
   kb_workload_wire_view_t token;
   kb_workload_wire_view_t proof_anchor_id;
   kb_workload_wire_view_t custody_anchor_id;
   kb_workload_wire_view_t proof;
   kb_workload_wire_view_t data;
} kb_workload_wire_response_t;

/* Build one complete request frame. ATTEST requires data_len==0; WRAP and
 * UNWRAP enforce their plaintext/ciphertext bounds. */
int kb_workload_wire_build_request(kb_workload_operation_t operation,
                                   const unsigned char challenge[KB_WORKLOAD_CHALLENGE_LEN],
                                   const unsigned char binding[KB_WORKLOAD_BINDING_LEN],
                                   const void *data, size_t data_len, unsigned char *out,
                                   size_t cap, size_t *out_len);

/* Parse exactly one response frame. Views borrow `frame` and remain valid only
 * while it remains unchanged. A well-formed non-OK response returns 0 with its
 * status and empty views; malformed input returns -1 and zeroes `out`. */
int kb_workload_wire_parse_response(const unsigned char *frame, size_t frame_len,
                                    kb_workload_operation_t expected_operation,
                                    kb_workload_wire_response_t *out);

#endif /* DEC_KB_WORKLOAD_WIRE_H */
