#ifndef AIMEE_KB_VAULT_OPERATOR_MUTATION_H
#define AIMEE_KB_VAULT_OPERATOR_MUTATION_H

#include "kb_vault_operator_status.h"

#include <stddef.h>
#include <stdint.h>

typedef enum
{
   KB_VAULT_MUTATION_DB_OK = 0,
   KB_VAULT_MUTATION_DB_NOT_FOUND,
   KB_VAULT_MUTATION_DB_BUSY,
   KB_VAULT_MUTATION_DB_TRANSIENT,
   KB_VAULT_MUTATION_DB_INTEGRITY,
   KB_VAULT_MUTATION_DB_INVALID,
   KB_VAULT_MUTATION_DB_UNSUPPORTED,
} kb_vault_mutation_db_result_t;

typedef enum
{
   KB_VAULT_MUTATION_BINDING_ACTIVE = 1,
   KB_VAULT_MUTATION_BINDING_COMPLETED,
   KB_VAULT_MUTATION_BINDING_OPENED,
} kb_vault_mutation_binding_state_t;

typedef struct
{
   kb_vault_mutation_binding_state_t state;
   uint8_t request_id[16];
   uint8_t operation_id[16];
   uint64_t old_generation;
   uint64_t new_generation;
   uint64_t seal_epoch;
   uint64_t fence;
} kb_vault_mutation_binding_t;

typedef enum
{
   KB_VAULT_MUTATION_AUTHORIZED = 0,
   KB_VAULT_MUTATION_AUTH_WRONG_SECRET,
   KB_VAULT_MUTATION_AUTH_BACKEND_UNAVAILABLE,
   KB_VAULT_MUTATION_AUTH_INTEGRITY,
   KB_VAULT_MUTATION_AUTH_UNSUPPORTED,
} kb_vault_mutation_auth_result_t;

/* A reseal step reports COMPLETED when D2 reached its durable completed state.
 * It does not report OPERATIONAL: only the D3 finalizer may do that. */
typedef enum
{
   KB_VAULT_MUTATION_STEP_COMPLETED = 0,
   KB_VAULT_MUTATION_STEP_SAFE_RETRY,
   KB_VAULT_MUTATION_STEP_BUSY,
   KB_VAULT_MUTATION_STEP_ABORTED,
   KB_VAULT_MUTATION_STEP_RECOVERY_REQUIRED,
   KB_VAULT_MUTATION_STEP_INTEGRITY,
   KB_VAULT_MUTATION_STEP_INVALID,
   KB_VAULT_MUTATION_STEP_UNSUPPORTED,
   KB_VAULT_MUTATION_STEP_BACKEND_UNAVAILABLE,
} kb_vault_mutation_step_result_t;

typedef enum
{
   KB_VAULT_MUTATION_RESEAL_START = 1,
   KB_VAULT_MUTATION_RESEAL_RESUME,
} kb_vault_mutation_reseal_mode_t;

typedef struct
{
   int (*read_status)(kb_vault_operator_status_t *status, void *opaque);
   int (*singleton_revalidate)(void *opaque);
   kb_vault_mutation_auth_result_t (*authorization_preflight)(
       kb_vault_operator_opcode_t opcode, const kb_vault_operator_status_t *status,
       const uint8_t *secret, size_t secret_len, uint64_t *authorized_generation, void *opaque);

   kb_vault_mutation_db_result_t (*start_lookup)(const uint8_t request_id[16], int locked,
                                                 kb_vault_mutation_binding_t *binding,
                                                 void *opaque);
   kb_vault_mutation_db_result_t (*start_reserve)(const uint8_t request_id[16],
                                                  const uint8_t candidate_operation_id[16],
                                                  uint64_t old_generation, uint64_t new_generation,
                                                  kb_vault_mutation_binding_t *binding,
                                                  int *created, void *opaque);
   kb_vault_mutation_db_result_t (*discover_active)(kb_vault_mutation_binding_t *binding,
                                                    void *opaque);
   kb_vault_mutation_db_result_t (*lookup_completed)(kb_vault_mutation_binding_t *binding,
                                                     void *opaque);
   int (*random_operation_id)(uint8_t operation_id[16], void *opaque);

   kb_vault_mutation_step_result_t (*run_reseal)(kb_vault_mutation_reseal_mode_t mode,
                                                 const uint8_t request_id[16],
                                                 const kb_vault_mutation_binding_t *binding,
                                                 const uint8_t *secret, size_t secret_len,
                                                 void *opaque);
   kb_vault_operator_result_t (*finalize_completed)(const kb_vault_mutation_binding_t *binding,
                                                    const kb_vault_operator_status_t *status,
                                                    const uint8_t *secret, size_t secret_len,
                                                    void *opaque);
   kb_vault_operator_result_t (*unseal_idle)(const kb_vault_operator_status_t *status,
                                             const uint8_t *secret, size_t secret_len,
                                             void *opaque);
   kb_vault_operator_result_t (*unseal_local)(const kb_vault_operator_status_t *status,
                                              const uint8_t *secret, size_t secret_len,
                                              void *opaque);

   /* Called only by kb_vault_operator_mutation_after_secret_wipe, never while
    * the protected ingress arena exists. */
   int (*publish_activation)(const kb_vault_operator_status_t *status, void *opaque);
   void (*fail_closed_seal)(void *opaque);
} kb_vault_operator_mutation_deps_t;

typedef struct
{
   kb_vault_operator_mutation_deps_t deps;
   void *opaque;
   kb_vault_operator_status_t pending_activation;
   int pending_activation_valid;
} kb_vault_operator_mutation_t;

int kb_vault_operator_mutation_init(kb_vault_operator_mutation_t *mutation,
                                    const kb_vault_operator_mutation_deps_t *deps, void *opaque);
void kb_vault_operator_mutation_destroy(kb_vault_operator_mutation_t *mutation);

/* Drop-in implementation of kb_vault_operator_mutation_fn. The surrounding UDS
 * service already serializes calls. This function never retains secret bytes. */
int kb_vault_operator_mutation_execute(kb_vault_operator_opcode_t opcode,
                                       const uint8_t request_id[16], const uint8_t *secret,
                                       size_t secret_len, kb_vault_operator_result_t *result,
                                       void *opaque);

/* The transport must call this after closing/cleansing the protected secret and
 * before reading the mutation response status. It performs the final fresh
 * status/singleton proof and publishes activation. A non-operational mutation is
 * a successful no-op. */
int kb_vault_operator_mutation_after_secret_wipe(kb_vault_operator_mutation_t *mutation);

#endif
