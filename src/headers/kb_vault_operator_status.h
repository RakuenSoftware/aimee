#ifndef AIMEE_KB_VAULT_OPERATOR_STATUS_H
#define AIMEE_KB_VAULT_OPERATOR_STATUS_H

#include <stddef.h>
#include <stdint.h>

#define KB_VAULT_OPERATOR_SOCKET_PATH   "/run/aimee/vault-operator.sock"
#define KB_VAULT_OPERATOR_RUNTIME_DIR   "/run/aimee"
#define KB_VAULT_OPERATOR_LISTEN_FD     3
#define KB_VAULT_OPERATOR_HEADER_LEN    16u
#define KB_VAULT_OPERATOR_STATUS_LEN    80u
#define KB_VAULT_OPERATOR_IO_TIMEOUT_MS 5000u

typedef enum
{
   KB_VAULT_OPERATOR_OPCODE_STATUS = 1,
   KB_VAULT_OPERATOR_OPCODE_START = 2,  /* reserved for a later protocol */
   KB_VAULT_OPERATOR_OPCODE_RESUME = 3, /* reserved for a later protocol */
   KB_VAULT_OPERATOR_OPCODE_UNSEAL = 4, /* reserved for a later protocol */
} kb_vault_operator_opcode_t;

typedef enum
{
   KB_VAULT_OPERATOR_TRANSPORT_OK = 0,
   KB_VAULT_OPERATOR_TRANSPORT_BAD_FRAME = 1,
   KB_VAULT_OPERATOR_TRANSPORT_UNSUPPORTED_OPCODE = 2,
   KB_VAULT_OPERATOR_TRANSPORT_INTERNAL = 3,
} kb_vault_operator_transport_t;

typedef enum
{
   KB_VAULT_OPERATOR_STATE_UNSUPPORTED_RESERVED = 1,
   KB_VAULT_OPERATOR_STATE_DISABLED_RESERVED = 2,
   KB_VAULT_OPERATOR_STATE_SEALED_IDLE = 3,
   KB_VAULT_OPERATOR_STATE_OPERATIONAL = 4,
   KB_VAULT_OPERATOR_STATE_LOCAL_UNSEAL_REQUIRED = 5,
   KB_VAULT_OPERATOR_STATE_RESUME_REQUIRED = 6,
   KB_VAULT_OPERATOR_STATE_RECOVERY_REQUIRED = 7,
   KB_VAULT_OPERATOR_STATE_COMPLETED_SEALED = 8,
   KB_VAULT_OPERATOR_STATE_BACKEND_UNAVAILABLE = 9,
   KB_VAULT_OPERATOR_STATE_INTEGRITY_FAILURE = 10,
} kb_vault_operator_state_t;

typedef enum
{
   KB_VAULT_OPERATOR_OPERATION_NONE = 0,
   KB_VAULT_OPERATOR_OPERATION_PREPARING = 1,
   KB_VAULT_OPERATOR_OPERATION_CUSTODY_PREPARED = 2,
   KB_VAULT_OPERATOR_OPERATION_WRAPS_STAGED = 3,
   KB_VAULT_OPERATOR_OPERATION_RESEAL_COMMITTING = 4,
   KB_VAULT_OPERATOR_OPERATION_RESEALED = 5,
   KB_VAULT_OPERATOR_OPERATION_PROMOTED = 6,
   KB_VAULT_OPERATOR_OPERATION_COMPLETED = 7,
   KB_VAULT_OPERATOR_OPERATION_ABORTED = 8,
   KB_VAULT_OPERATOR_OPERATION_RECOVERY_REQUIRED = 9,
} kb_vault_operator_operation_state_t;

typedef enum
{
   KB_VAULT_OPERATOR_REMEDIATION_NONE = 0,
   KB_VAULT_OPERATOR_REMEDIATION_CONFIGURE = 1,
   KB_VAULT_OPERATOR_REMEDIATION_UNSEAL = 2,
   KB_VAULT_OPERATOR_REMEDIATION_RESUME = 3,
   KB_VAULT_OPERATOR_REMEDIATION_RECOVER = 4,
   KB_VAULT_OPERATOR_REMEDIATION_UPGRADE = 5,
   KB_VAULT_OPERATOR_REMEDIATION_BACKEND = 6,
   KB_VAULT_OPERATOR_REMEDIATION_INTEGRITY = 7,
   KB_VAULT_OPERATOR_REMEDIATION_FINALIZE = 8,
} kb_vault_operator_remediation_t;

typedef struct
{
   kb_vault_operator_state_t state;
   kb_vault_operator_operation_state_t operation_state;
   kb_vault_operator_remediation_t remediation;
   uint16_t flags;
   uint64_t seal_epoch;
   uint64_t control_fence;
   uint64_t old_generation;
   uint64_t new_generation;
   uint64_t last_opened_fence;
   unsigned char operation_id[16];
} kb_vault_operator_status_t;

typedef enum
{
   KB_VAULT_OPERATOR_CLIENT_OK = 0,
   KB_VAULT_OPERATOR_CLIENT_ACTION_REQUIRED = 2,
   KB_VAULT_OPERATOR_CLIENT_INTEGRITY_FAILURE = 3,
   KB_VAULT_OPERATOR_CLIENT_TRANSPORT_FAILURE = 4,
} kb_vault_operator_client_result_t;

/* The callback must fill one complete, secret-free projection. It returns zero
 * on success. The service validates the projection before putting it on wire. */
typedef int (*kb_vault_operator_status_fn)(kb_vault_operator_status_t *status, void *opaque);

typedef struct kb_vault_operator_service kb_vault_operator_service_t;

/* Pure, allocation-free codec seams. Decode functions reject noncanonical
 * fields and payload combinations. */
int kb_vault_operator_request_encode(kb_vault_operator_opcode_t opcode,
                                     unsigned char out[KB_VAULT_OPERATOR_HEADER_LEN]);
int kb_vault_operator_request_decode(const unsigned char input[KB_VAULT_OPERATOR_HEADER_LEN],
                                     uint16_t *opcode, uint32_t *payload_len);
int kb_vault_operator_response_encode(kb_vault_operator_transport_t result,
                                      const kb_vault_operator_status_t *status, unsigned char *out,
                                      size_t out_cap, size_t *out_len);
int kb_vault_operator_response_decode(const unsigned char *input, size_t input_len,
                                      kb_vault_operator_transport_t *result,
                                      kb_vault_operator_status_t *status);
int kb_vault_operator_status_validate(const kb_vault_operator_status_t *status);

/* Takes ownership of the fixed inherited listener fd, including on a failed
 * initialization attempt. The fd must be exactly KB_VAULT_OPERATOR_LISTEN_FD
 * and name the fixed root-owned socket. Stop closes admission, drains the
 * current callback, joins, and frees service. */
kb_vault_operator_service_t *
kb_vault_operator_service_start(int inherited_fd, kb_vault_operator_status_fn read_status,
                                void *opaque);
void kb_vault_operator_service_stop(kb_vault_operator_service_t *service);

/* Fixed-path client: no endpoint argument and no HTTP fallback. */
kb_vault_operator_client_result_t
kb_vault_operator_status_client(kb_vault_operator_status_t *status);
kb_vault_operator_client_result_t
kb_vault_operator_status_exit_mapping(const kb_vault_operator_status_t *status);
/* Startup serving policy: 0 permits general serving, 1 permits STATUS only,
 * and -1 is a fatal/ineligible status. */
int kb_vault_operator_startup_mode(kb_vault_operator_state_t state);
const char *kb_vault_operator_state_name(kb_vault_operator_state_t state);
const char *kb_vault_operator_operation_name(kb_vault_operator_operation_state_t state);
const char *kb_vault_operator_remediation_name(kb_vault_operator_remediation_t remediation);
int kb_vault_operator_status_format(const kb_vault_operator_status_t *status, int json, char *out,
                                    size_t out_cap);

/* Narrow single-connection seam for focused socketpair/fuzz tests. Production
 * callers use the joined service above. */
int kb_vault_operator_serve_connection(int fd, kb_vault_operator_status_fn read_status,
                                       void *opaque);

#endif
