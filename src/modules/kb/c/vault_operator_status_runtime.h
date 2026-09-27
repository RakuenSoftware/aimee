#ifndef AIMEE_KB_STORE_VAULT_OPERATOR_STATUS_RUNTIME_H
#define AIMEE_KB_STORE_VAULT_OPERATOR_STATUS_RUNTIME_H

#include <stddef.h>
#include <stdint.h>

#define KB_STORE_VAULT_ORCHESTRATOR_LOGIN_ROLE  "aimee_kb_vault_orchestrator_login"
#define KB_STORE_VAULT_ORCHESTRATOR_ACTIVE_ROLE "aimee_kb_vault_orchestrator"

enum
{
   KB_STORE_VAULT_OPERATOR_OK = 0,
   KB_STORE_VAULT_OPERATOR_UNAVAILABLE = -1,
   KB_STORE_VAULT_OPERATOR_INTEGRITY = -2,
};

typedef enum
{
   KB_STORE_VAULT_PROVIDER_AVAILABLE_SEALED = 1,
   KB_STORE_VAULT_PROVIDER_AVAILABLE_UNSEALED = 2,
   KB_STORE_VAULT_PROVIDER_UNAVAILABLE = 3,
   KB_STORE_VAULT_PROVIDER_MALFORMED = 4,
} kb_store_vault_provider_status_t;

typedef enum
{
   KB_STORE_VAULT_STATE_SEALED_IDLE = 3,
   KB_STORE_VAULT_STATE_OPERATIONAL = 4,
   KB_STORE_VAULT_STATE_LOCAL_UNSEAL_REQUIRED = 5,
   KB_STORE_VAULT_STATE_RESUME_REQUIRED = 6,
   KB_STORE_VAULT_STATE_RECOVERY_REQUIRED = 7,
   KB_STORE_VAULT_STATE_COMPLETED_SEALED = 8,
   KB_STORE_VAULT_STATE_BACKEND_UNAVAILABLE = 9,
   KB_STORE_VAULT_STATE_INTEGRITY_FAILURE = 10,
} kb_store_vault_operator_state_t;

typedef enum
{
   KB_STORE_VAULT_OPERATION_NONE = 0,
   KB_STORE_VAULT_OPERATION_PREPARING = 1,
   KB_STORE_VAULT_OPERATION_CUSTODY_PREPARED = 2,
   KB_STORE_VAULT_OPERATION_WRAPS_STAGED = 3,
   KB_STORE_VAULT_OPERATION_RESEAL_COMMITTING = 4,
   KB_STORE_VAULT_OPERATION_RESEALED = 5,
   KB_STORE_VAULT_OPERATION_PROMOTED = 6,
   KB_STORE_VAULT_OPERATION_COMPLETED = 7,
   KB_STORE_VAULT_OPERATION_ABORTED = 8,
   KB_STORE_VAULT_OPERATION_RECOVERY_REQUIRED = 9,
} kb_store_vault_operation_state_t;

typedef enum
{
   KB_STORE_VAULT_REMEDIATION_NONE = 0,
   KB_STORE_VAULT_REMEDIATION_UNSEAL = 2,
   KB_STORE_VAULT_REMEDIATION_RESUME = 3,
   KB_STORE_VAULT_REMEDIATION_RECOVER = 4,
   KB_STORE_VAULT_REMEDIATION_BACKEND = 6,
   KB_STORE_VAULT_REMEDIATION_INTEGRITY = 7,
   KB_STORE_VAULT_REMEDIATION_FINALIZE = 8,
} kb_store_vault_remediation_t;

typedef struct
{
   int64_t seal_epoch;
   int64_t control_fence;
   int64_t last_opened_fence;
   int sealed;
   int operation_present;
   kb_store_vault_operation_state_t operation_state;
   int64_t operation_seal_epoch;
   int64_t operation_fence;
   int64_t old_generation;
   int64_t new_generation;
   unsigned char operation_id[16];
   char failure_class[65];
} kb_store_vault_operator_snapshot_t;

typedef struct
{
   unsigned rows;
   unsigned columns;
   unsigned char is_null[2][11];
   char value[2][11][129];
} kb_store_vault_operator_db_result_t;

typedef struct
{
   void *(*open)(void *context, const char *conninfo, int64_t deadline_ms, char *errbuf,
                 size_t errlen);
   void (*close)(void *context, void *connection);
   /* Returns OK, UNAVAILABLE, or INTEGRITY.  Production maps only the fixed
    * operator facade's SQLSTATE 55000 to typed integrity. */
   int (*query)(void *context, void *connection, const char *sql, int64_t deadline_ms,
                kb_store_vault_operator_db_result_t *result, char *errbuf, size_t errlen);
   int (*transaction_idle)(void *context, void *connection);
} kb_store_vault_operator_db_vtable_t;

typedef struct
{
   void *connection;
   const kb_store_vault_operator_db_vtable_t *database;
   void *database_context;
   int transaction_active;
   int mutex_initialized;
   void *mutex_storage[8];
} kb_store_vault_operator_runtime_t;

typedef int (*kb_store_vault_provider_status_fn)(void *context,
                                                 kb_store_vault_provider_status_t *out);

typedef struct
{
   kb_store_vault_operator_state_t state;
   kb_store_vault_remediation_t remediation;
   kb_store_vault_provider_status_t provider;
   kb_store_vault_operator_snapshot_t snapshot;
} kb_store_vault_operator_status_t;

int kb_store_vault_operator_runtime_open(kb_store_vault_operator_runtime_t *, const char *conninfo,
                                         char *errbuf, size_t errlen);
int kb_store_vault_operator_runtime_open_with_vtable(kb_store_vault_operator_runtime_t *,
                                                     const char *conninfo,
                                                     const kb_store_vault_operator_db_vtable_t *,
                                                     void *database_context, char *errbuf,
                                                     size_t errlen);
void kb_store_vault_operator_runtime_close(kb_store_vault_operator_runtime_t *);

/* One complete transaction-owned database snapshot. */
int kb_store_vault_operator_runtime_snapshot(kb_store_vault_operator_runtime_t *,
                                             kb_store_vault_operator_snapshot_t *);

/* Database snapshot, provider-local read, database snapshot.  Motion retries
 * at most three complete attempts and is reported as integrity failure. */
int kb_store_vault_operator_runtime_status(kb_store_vault_operator_runtime_t *,
                                           kb_store_vault_provider_status_fn,
                                           void *provider_context,
                                           kb_store_vault_operator_status_t *);

int kb_store_vault_operator_snapshot_equal(const kb_store_vault_operator_snapshot_t *,
                                           const kb_store_vault_operator_snapshot_t *);

/* Pure connection-policy seam for production rejection tests. */

#endif
