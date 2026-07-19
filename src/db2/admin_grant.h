/* db2/admin_grant.h: P1 tenancy — org-admin capability grants. Backed by
 * kb_admin_grant. source is 'oidc' | 'cert' | 'owner'. A grant is active while
 * revoked_at is empty. Tenant-scoped: every entry requires the RLS-enforcing
 * Postgres backend (db2_tenant_require_pg). Pure domain API. */
#ifndef DEC_DB2_ADMIN_GRANT_H
#define DEC_DB2_ADMIN_GRANT_H 1

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

   typedef struct
   {
      int64_t id;
      char identity_key[256];
      char source[16];
      char granted_at[32];
      char granted_by[128];
      char revoked_at[32];
   } db2_admin_grant_row_t;

   /* Grant org-admin to identity_key. Idempotent on identity_key: a re-grant
    * refreshes source/granted_by and clears revoked_at. Writes the row id to
    * *out_id (if non-NULL). Returns 0, a negative tenancy code, or -1. */
   int db2_admin_grant_add(const char *identity_key, const char *source, const char *granted_by,
                           int64_t *out_id);

   /* Revoke the active grant for identity_key (SET revoked_at=now WHERE not already
    * revoked). Idempotent. Returns 0, a negative tenancy code, or -1 on error. */
   int db2_admin_grant_revoke(const char *identity_key);

   /* 1 if an active (non-revoked) grant exists for identity_key, else 0. May return
    * a negative tenancy code when the backend is not Postgres. */
   int db2_admin_grant_is_active(const char *identity_key);

#ifdef __cplusplus
}
#endif

#endif /* DEC_DB2_ADMIN_GRANT_H */
