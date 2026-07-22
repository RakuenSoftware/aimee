/* db2/membership.h: P1 tenancy — team membership. Backed by kb_team_membership.
 *
 * identity_key is the CANONICAL immutable identity ('oidc:<iss>:<sub>' /
 * 'cert:<issuer>:<serial>'), never a bare sub/email. At most one default team per
 * identity_key. Tenant-scoped: every entry requires the RLS-enforcing Postgres
 * backend (db2_tenant_require_pg). Pure domain API. */
#ifndef DEC_DB2_MEMBERSHIP_H
#define DEC_DB2_MEMBERSHIP_H 1

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

   typedef struct
   {
      int64_t id;
      char identity_key[256];
      int64_t team;
      int is_default;
      char created_at[32];
   } db2_membership_row_t;

   /* Add a membership (identity_key -> team). Idempotent on (identity_key, team):
    * a conflict leaves the existing row and returns 0 with *out_id unchanged.
    * On insert writes the new id to *out_id (if non-NULL). Returns 0, a negative
    * tenancy code, or -1. */
   int db2_membership_add(const char *identity_key, int64_t team, int is_default, int64_t *out_id);

   /* Remove a membership. Returns 0 on success (idempotent), a negative tenancy
    * code, or -1 on error. */
   int db2_membership_remove(const char *identity_key, int64_t team);

   /* List memberships for an identity (ORDER BY id), up to `max`. Returns the count
    * written, a negative tenancy code, or -1. */
   int db2_membership_list_for_identity(const char *identity_key, db2_membership_row_t *out,
                                        int max);

   /* Fill up to `max` team ids for an identity. Returns the count, a negative
    * tenancy code, or -1. */
   int db2_membership_teams(const char *identity_key, int64_t *out_teams, int max);

   /* Resolve the identity's default team into *out_team. Returns 0 if a default
    * exists, -1 if none, or a negative tenancy code. */
   int db2_membership_default_team(const char *identity_key, int64_t *out_team);

#ifdef __cplusplus
}
#endif

#endif /* DEC_DB2_MEMBERSHIP_H */
