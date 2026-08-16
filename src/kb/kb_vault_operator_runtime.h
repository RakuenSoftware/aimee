#ifndef AIMEE_KB_VAULT_OPERATOR_RUNTIME_H
#define AIMEE_KB_VAULT_OPERATOR_RUNTIME_H

#include "modules/db2/c/vault_operator_rewrap_runtime.h"
#include "kb_vault_activation_latch.h"
#include "kb_vault_operator_mutation.h"
#include "kb_vault_tpm_runtime_lock.h"
#include "modules/vault/vault_reseal_orchestrator.h"

#include <pthread.h>

typedef struct kb_vault_operator_runtime_platform
{
   int (*read_status)(db2_vault_operator_runtime_t *, kb_vault_operator_status_t *);
   int (*singleton_revalidate)(kb_vault_tpm_runtime_lock_t *);
   int (*random)(uint8_t *, size_t);
   vault_custody_auth_result_t (*authorization_preflight)(const void *, size_t, uint64_t);
   vault_custody_auth_result_t (*authorization_preflight_current)(const void *, size_t, uint64_t *);

   int (*dispatch)(const uint8_t[16], db2_vault_operator_rewrap_binding_t *, int *);
   int (*reserve)(const uint8_t[16], const uint8_t[16], int64_t, int64_t,
                  db2_vault_operator_rewrap_binding_t *, int *);
   int (*active)(db2_vault_operator_rewrap_binding_t *, int *);
   int (*completed)(const uint8_t[16], const uint8_t[16], db2_vault_operator_completed_t *);
   int (*completed_active)(const uint8_t[16], db2_vault_operator_completed_t *);
   int (*current_check_page)(const db2_vault_rewrap_cursor_t *, int, db2_vault_rewrap_check_t *,
                             size_t, size_t *, db2_vault_rewrap_cursor_t *, int64_t *);
   int (*open_completed)(const db2_vault_operator_completed_t *,
                         db2_vault_operator_open_result_t *);
   int (*open_idle)(const uint8_t[16], int64_t, int64_t, int64_t,
                    db2_vault_operator_open_result_t *);
   int (*open_event)(const uint8_t[32], db2_vault_operator_open_event_t *);
   int (*recover_uncertain)(void);

   vault_reseal_orchestrator_result_t (*orchestrator_run)(
       const vault_reseal_orchestrator_request_t *, const vault_reseal_orchestrator_deps_t *,
       vault_reseal_orchestrator_output_t *);
   int (*receipt_decode)(const uint8_t *, size_t, vault_tpm2_reseal_receipt_t *);
   int (*receipt_status)(const vault_tpm2_reseal_receipt_t *, const char *,
                         vault_tpm2_reseal_status_t *);
   int (*receipt_cleanup)(const vault_tpm2_reseal_receipt_t *, const char *,
                          vault_tpm2_cleanup_authorization_t);
   int (*guard_begin)(vault_maintenance_guard_t **);
   int (*guard_sync)(vault_maintenance_guard_t *, uint64_t);
   vault_custody_auth_result_t (*guard_unseal)(vault_maintenance_guard_t *, const void *, size_t);
   int (*guard_with_kek)(vault_maintenance_guard_t *, vault_maintenance_kek_fn, void *);
   int (*guard_end)(vault_maintenance_guard_t **);
   int (*guard_end_operational)(vault_maintenance_guard_t **, uint64_t);
   vault_custody_local_status_t (*local_status)(void);
   int (*kek_check_verify)(const uint8_t[VAULT_KEK_LEN], const uint8_t[VAULT_WRAPPED_DEK_LEN]);
   int (*seal)(void);
   int (*publish)(kb_vault_activation_latch_t *, const kb_vault_operator_status_t *);
} kb_vault_operator_runtime_platform_t;

typedef struct
{
   db2_vault_operator_runtime_t *database;
   kb_vault_tpm_runtime_lock_t *singleton;
   kb_vault_activation_latch_t *activation;
   const kb_vault_operator_runtime_platform_t *platform;
   vault_reseal_orchestrator_deps_t orchestrator_deps;
   pthread_mutex_t mutex;
   db2_vault_operator_open_result_t activation_open;
   db2_vault_operator_open_event_t activation_event;
   kb_vault_operator_status_t activation_status;
   int activation_proof_valid;
   int activation_has_event;
   int general_serving;
   int database_bound;
   int initialized;
} kb_vault_operator_runtime_t;

/* Production binding. The database runtime and TPM singleton remain owned by
 * kb_main and must outlive this object. The activation latch is likewise
 * caller-owned. */
int kb_vault_operator_runtime_init(kb_vault_operator_runtime_t *, db2_vault_operator_runtime_t *,
                                   kb_vault_tpm_runtime_lock_t *, kb_vault_activation_latch_t *);

/* Injected seam for the focused choreography suite. Passing NULL deps selects
 * the dedicated production DB adapter and the ordinary TPM2 D2 custody seam. */
int kb_vault_operator_runtime_init_with_platform(kb_vault_operator_runtime_t *,
                                                 db2_vault_operator_runtime_t *,
                                                 kb_vault_tpm_runtime_lock_t *,
                                                 kb_vault_activation_latch_t *,
                                                 const kb_vault_operator_runtime_platform_t *,
                                                 const vault_reseal_orchestrator_deps_t *);

void kb_vault_operator_runtime_destroy(kb_vault_operator_runtime_t *);
void kb_vault_operator_runtime_fill_deps(kb_vault_operator_runtime_t *,
                                         kb_vault_operator_mutation_deps_t *);

/* Transport-compatible status projection over this composite opaque. */
int kb_vault_operator_runtime_read_status(kb_vault_operator_status_t *, void *);

/* Main-thread acquire validation after the latch wakes. Re-reads the exact
 * opened event where one was committed by this process, then requires a fresh
 * byte-identical operational status and live singleton/provider state. */
int kb_vault_operator_runtime_activation_validate(kb_vault_operator_runtime_t *,
                                                  const kb_vault_operator_status_t *);

/* Called exactly once after general serving has been activated. Later
 * successful rotations still perform the full open proof but do not attempt to
 * republish the one-way startup latch. */
int kb_vault_operator_runtime_mark_general_serving(kb_vault_operator_runtime_t *);

#endif
