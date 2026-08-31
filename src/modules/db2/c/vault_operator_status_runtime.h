#ifndef AIMEE_DB2_VAULT_OPERATOR_STATUS_RUNTIME_H
#define AIMEE_DB2_VAULT_OPERATOR_STATUS_RUNTIME_H

#include <stddef.h>
#include <stdint.h>

#define DB2_VAULT_ORCHESTRATOR_LOGIN_ROLE  "aimee_kb_vault_orchestrator_login"
#define DB2_VAULT_ORCHESTRATOR_ACTIVE_ROLE "aimee_kb_vault_orchestrator"

enum
{
   DB2_VAULT_OPERATOR_OK = 0,
   DB2_VAULT_OPERATOR_UNAVAILABLE = -1,
   DB2_VAULT_OPERATOR_INTEGRITY = -2,
};

typedef enum
{
   DB2_VAULT_PROVIDER_AVAILABLE_SEALED = 1,
   DB2_VAULT_PROVIDER_AVAILABLE_UNSEALED = 2,
   DB2_VAULT_PROVIDER_UNAVAILABLE = 3,
   DB2_VAULT_PROVIDER_MALFORMED = 4,
} db2_vault_provider_status_t;

typedef enum
{
   DB2_VAULT_STATE_SEALED_IDLE = 3,
   DB2_VAULT_STATE_OPERATIONAL = 4,
   DB2_VAULT_STATE_LOCAL_UNSEAL_REQUIRED = 5,
   DB2_VAULT_STATE_RESUME_REQUIRED = 6,
   DB2_VAULT_STATE_RECOVERY_REQUIRED = 7,
   DB2_VAULT_STATE_COMPLETED_SEALED = 8,
   DB2_VAULT_STATE_BACKEND_UNAVAILABLE = 9,
   DB2_VAULT_STATE_INTEGRITY_FAILURE = 10,
} db2_vault_operator_state_t;

typedef enum
{
   DB2_VAULT_OPERATION_NONE = 0,
   DB2_VAULT_OPERATION_PREPARING = 1,
   DB2_VAULT_OPERATION_CUSTODY_PREPARED = 2,
   DB2_VAULT_OPERATION_WRAPS_STAGED = 3,
   DB2_VAULT_OPERATION_RESEAL_COMMITTING = 4,
   DB2_VAULT_OPERATION_RESEALED = 5,
   DB2_VAULT_OPERATION_PROMOTED = 6,
   DB2_VAULT_OPERATION_COMPLETED = 7,
   DB2_VAULT_OPERATION_ABORTED = 8,
   DB2_VAULT_OPERATION_RECOVERY_REQUIRED = 9,
} db2_vault_operation_state_t;

typedef enum
{
   DB2_VAULT_REMEDIATION_NONE = 0,
   DB2_VAULT_REMEDIATION_UNSEAL = 2,
   DB2_VAULT_REMEDIATION_RESUME = 3,
   DB2_VAULT_REMEDIATION_RECOVER = 4,
   DB2_VAULT_REMEDIATION_BACKEND = 6,
   DB2_VAULT_REMEDIATION_INTEGRITY = 7,
   DB2_VAULT_REMEDIATION_FINALIZE = 8,
} db2_vault_remediation_t;

typedef struct
{
   int64_t seal_epoch;
   int64_t control_fence;
   int64_t last_opened_fence;
   int sealed;
   int operation_present;
   db2_vault_operation_state_t operation_state;
   int64_t operation_seal_epoch;
   int64_t operation_fence;
   int64_t old_generation;
   int64_t new_generation;
   unsigned char operation_id[16];
   char failure_class[65];
} db2_vault_operator_snapshot_t;

typedef struct
{
   unsigned rows;
   unsigned columns;
   unsigned char is_null[2][11];
   char value[2][11][129];
} db2_vault_operator_db_result_t;

typedef struct
{
   void *(*open)(void *context, const char *conninfo, int64_t deadline_ms, char *errbuf,
                 size_t errlen);
   void (*close)(void *context, void *connection);
   /* Returns OK, UNAVAILABLE, or INTEGRITY.  Production maps only the fixed
    * operator facade's SQLSTATE 55000 to typed integrity. */
   int (*query)(void *context, void *connection, const char *sql, int64_t deadline_ms,
                db2_vault_operator_db_result_t *result, char *errbuf, size_t errlen);
   int (*transaction_idle)(void *context, void *connection);
} db2_vault_operator_db_vtable_t;

typedef struct
{
   void *connection;
   const db2_vault_operator_db_vtable_t *database;
   void *database_context;
   int transaction_active;
   int mutex_initialized;
   void *mutex_storage[8];
} db2_vault_operator_runtime_t;

typedef int (*db2_vault_provider_status_fn)(void *context, db2_vault_provider_status_t *out);

typedef struct
{
   db2_vault_operator_state_t state;
   db2_vault_remediation_t remediation;
   db2_vault_provider_status_t provider;
   db2_vault_operator_snapshot_t snapshot;
} db2_vault_operator_status_t;

int db2_vault_operator_runtime_open(db2_vault_operator_runtime_t *, const char *conninfo,
                                    char *errbuf, size_t errlen);
int db2_vault_operator_runtime_open_with_vtable(db2_vault_operator_runtime_t *,
                                                const char *conninfo,
                                                const db2_vault_operator_db_vtable_t *,
                                                void *database_context, char *errbuf,
                                                size_t errlen);
void db2_vault_operator_runtime_close(db2_vault_operator_runtime_t *);

/* One complete transaction-owned database snapshot. */
int db2_vault_operator_runtime_snapshot(db2_vault_operator_runtime_t *,
                                        db2_vault_operator_snapshot_t *);

/* Database snapshot, provider-local read, database snapshot.  Motion retries
 * at most three complete attempts and is reported as integrity failure. */
int db2_vault_operator_runtime_status(db2_vault_operator_runtime_t *, db2_vault_provider_status_fn,
                                      void *provider_context, db2_vault_operator_status_t *);

int db2_vault_operator_snapshot_equal(const db2_vault_operator_snapshot_t *,
                                      const db2_vault_operator_snapshot_t *);

/* Pure connection-policy seam for production rejection tests. */
int db2_vault_operator_conninfo_allowed_for_test(const char *conninfo, int *tcp_out);

#endif
