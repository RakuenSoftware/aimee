/* db2/oidc_jwks.h: P1 tenancy — fleet-wide trusted OIDC JWKS (I10). Backed by
 * kb_oidc_jwks. Org-global (NOT team-scoped) so every stateless kb instance
 * agrees on trusted keys and IdP rotation converges within TTL. A key is active
 * while retired_at is empty. Requires the RLS-enforcing Postgres backend
 * (db2_tenant_require_pg). Pure domain API. */
#ifndef DEC_DB2_OIDC_JWKS_H
#define DEC_DB2_OIDC_JWKS_H 1

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

   typedef struct
   {
      int64_t id;
      char issuer[256];
      char kid[128];
      char jwk_json[4096];
      char added_at[32];
      char retired_at[32];
   } db2_jwks_row_t;

   /* Add (or refresh) a trusted JWK. Idempotent on (issuer, kid): a conflict
    * updates jwk_json and clears retired_at (un-retires). Writes the row id to
    * *out_id (if non-NULL). Returns 0, a negative tenancy code, or -1. */
   int db2_jwks_add(const char *issuer, const char *kid, const char *jwk_json, int64_t *out_id);

   /* Retire a key (SET retired_at=now). Returns 0, a negative tenancy code, or -1
    * on error. */
   int db2_jwks_retire(const char *issuer, const char *kid);

   /* List active (non-retired) keys for `issuer` (ORDER BY id), up to `max`.
    * Returns the count written, a negative tenancy code, or -1. */
   int db2_jwks_list_active(const char *issuer, db2_jwks_row_t *out, int max);

#ifdef __cplusplus
}
#endif

#endif /* DEC_DB2_OIDC_JWKS_H */
