#ifndef AIMEE_VAULT_RESEAL_ORCHESTRATOR_H
#define AIMEE_VAULT_RESEAL_ORCHESTRATOR_H

#include "org_vault_rewrap.h"
#include "vault_custody_tpm2.h"
#include "vault_mutation_budget.h"
#include "vault_server_key.h"

#include <stddef.h>
#include <stdint.h>

#define VAULT_RESEAL_ORCHESTRATOR_SECRET_MAX 4096
#define VAULT_RESEAL_ORCHESTRATOR_EDGE_MAX   16

typedef enum
{
   VAULT_RESEAL_ORCHESTRATOR_START = 1,
   VAULT_RESEAL_ORCHESTRATOR_RESUME
} vault_reseal_orchestrator_mode_t;

typedef enum
{
   VAULT_RESEAL_ORCHESTRATOR_COMPLETED = 0,
   VAULT_RESEAL_ORCHESTRATOR_SAFE_RETRY,
   VAULT_RESEAL_ORCHESTRATOR_BUSY,
   VAULT_RESEAL_ORCHESTRATOR_ABORTED,
   VAULT_RESEAL_ORCHESTRATOR_RECOVERY_REQUIRED,
   VAULT_RESEAL_ORCHESTRATOR_INTEGRITY,
   VAULT_RESEAL_ORCHESTRATOR_INVALID,
   VAULT_RESEAL_ORCHESTRATOR_UNSUPPORTED,
   VAULT_RESEAL_ORCHESTRATOR_ERROR
} vault_reseal_orchestrator_result_t;

typedef struct
{
   vault_reseal_orchestrator_mode_t mode;
   uint8_t operation_id[VAULT_RESEAL_OPERATION_ID_LEN];
   const char *actor;
   const char *request_id;
   const uint8_t *provider_secret;
   size_t provider_secret_len;
   vault_mutation_budget_t *budget;
} vault_reseal_orchestrator_request_t;

typedef struct
{
   int has_state;
   db2_vault_rewrap_state_t state;
   int64_t seal_epoch, fencing_token, old_generation, new_generation;
   char failure_class[65];
} vault_reseal_orchestrator_output_t;

typedef struct
{
   int (*supported)(void);
   int (*nv_generation)(const char *, uint64_t *);
   int (*prepare)(const uint8_t[16], uint64_t, const uint8_t[VAULT_KEK_LEN], const char *,
                  vault_tpm2_reseal_receipt_t *);
   int (*discover)(const uint8_t[16], uint64_t, const char *, vault_tpm2_reseal_receipt_t *,
                   vault_tpm2_reseal_status_t *);
   int (*recover_kek)(const vault_tpm2_reseal_receipt_t *, const char *, uint8_t[VAULT_KEK_LEN]);
   int (*status)(const vault_tpm2_reseal_receipt_t *, const char *, vault_tpm2_reseal_status_t *);
   int (*commit)(const vault_tpm2_reseal_receipt_t *, const char *, vault_tpm2_reseal_status_t *);
   int (*abort)(const vault_tpm2_reseal_receipt_t *, const char *);
   int (*guard_begin)(void **);
   int (*guard_sync_epoch)(void *, uint64_t);
   int (*guard_unseal)(void *, const void *, size_t);
   int (*guard_seal)(void *);
   int (*guard_with_active_kek)(void *, vault_maintenance_kek_fn, void *);
   int (*guard_end)(void **);
   int (*random)(uint8_t *, size_t);
   int (*dek_wrap)(const uint8_t[VAULT_KEK_LEN], const uint8_t[VAULT_DEK_LEN],
                   uint8_t[VAULT_WRAPPED_DEK_LEN]);
   int (*dek_unwrap)(const uint8_t[VAULT_KEK_LEN], const uint8_t[VAULT_WRAPPED_DEK_LEN],
                     uint8_t[VAULT_DEK_LEN]);
   int (*check_wrap)(const uint8_t[VAULT_KEK_LEN], uint8_t[VAULT_WRAPPED_DEK_LEN]);
   int (*check_verify)(const uint8_t[VAULT_KEK_LEN], const uint8_t[VAULT_WRAPPED_DEK_LEN]);
} vault_reseal_custody_ops_t;

typedef struct
{
   const db2_vault_rewrap_ops_t *db;
   const vault_reseal_custody_ops_t *custody;
} vault_reseal_orchestrator_deps_t;

extern const vault_reseal_custody_ops_t vault_reseal_custody_default_ops;
extern const vault_reseal_orchestrator_deps_t vault_reseal_orchestrator_default_deps;

vault_reseal_orchestrator_result_t
vault_reseal_orchestrator_run(const vault_reseal_orchestrator_request_t *request,
                              const vault_reseal_orchestrator_deps_t *deps,
                              vault_reseal_orchestrator_output_t *output);

#endif
