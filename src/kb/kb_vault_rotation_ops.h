#ifndef AIMEE_KB_VAULT_ROTATION_OPS_H
#define AIMEE_KB_VAULT_ROTATION_OPS_H 1

#include "kb_identity.h"
#include "org_vault_rotation.h"

#include <stddef.h>
#include <stdint.h>

enum
{
   KB_VAULT_OP_OK = 0,
   KB_VAULT_OP_DEFINITE_FAILURE = -1,
   KB_VAULT_OP_UNCERTAIN = 1,
   KB_VAULT_OP_RETRY = 2,
   KB_VAULT_OP_COMPLETE = 3,
   KB_VAULT_OP_REMEDIATION_REQUIRED = 4
};

typedef struct
{
   /* Long callbacks call heartbeat at least every ttl_seconds/3. The lease and
    * heartbeat context are valid only for the duration of that callback. */
   int ttl_seconds;
   int (*heartbeat)(void *ctx);
   void *heartbeat_ctx;
} kb_vault_rotation_lease_t;

typedef struct
{
   int (*resolve_current)(void *ctx, const char *operation_key, const db2_vault_rotation_row_t *row,
                          const kb_vault_rotation_lease_t *lease, char *vendor_ref, size_t ref_cap);
   /* Every successful provision must first reconcile operation_key. Set
    * prior_orphan_reconciled even when reconciliation proves no orphan exists. */
   int (*provision)(void *ctx, const char *operation_key, const db2_vault_rotation_row_t *row,
                    const kb_vault_rotation_lease_t *lease, unsigned char *secret,
                    size_t secret_cap, size_t *secret_len, char *vendor_ref, size_t ref_cap,
                    int *prior_orphan_reconciled);
   int (*probe)(void *ctx, const char *operation_key, const db2_vault_rotation_row_t *row,
                const kb_vault_rotation_lease_t *lease, const unsigned char *secret,
                size_t secret_len);
   int (*revoke)(void *ctx, const char *operation_key, const db2_vault_rotation_row_t *row,
                 const kb_vault_rotation_lease_t *lease, const char *vendor_ref, char *receipt,
                 size_t receipt_cap);
   int (*reconcile)(void *ctx, const char *operation_key, const db2_vault_rotation_row_t *row,
                    const kb_vault_rotation_lease_t *lease, char *vendor_ref, size_t ref_cap,
                    int *exists, char *evidence, size_t evidence_cap);
} kb_vault_rotation_provider_t;

/* Startup-only: the provider registry is immutable after the first operation;
 * provider_ctx must remain valid for the process lifetime and be thread-safe. */
int kb_vault_rotation_ops_register(const kb_vault_rotation_provider_t *provider,
                                   void *provider_ctx);
int kb_vault_rotation_ops_step(const kb_principal_t *caller, int64_t team_id, int64_t rotation_id,
                               const char *owner, int ttl_seconds);
int kb_vault_rotation_ops_heartbeat(const kb_principal_t *caller, int64_t team_id,
                                    int64_t rotation_id, const char *owner, int64_t token,
                                    int ttl_seconds);
int kb_vault_rotation_ops_remediate(const kb_principal_t *caller, int64_t team_id,
                                    int64_t rotation_id, const char *owner, int ttl_seconds);

#endif
