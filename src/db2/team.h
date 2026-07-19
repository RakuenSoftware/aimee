/* db2/team.h: P1 tenancy — teams (billing/ownership root). Backed by kb_team.
 *
 * Tenant-scoped: every entry point requires the RLS-enforcing Postgres backend
 * (db2_tenant_require_pg) and reads/writes under the caller's tenant context.
 * Pure domain API — no backend types in any signature. */
#ifndef DEC_DB2_TEAM_H
#define DEC_DB2_TEAM_H 1

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

   typedef struct
   {
      int64_t id;
      char name[128];
      char created_at[32];
      char operator_id[128];
   } db2_team_row_t;

   /* Create a team. On success writes the new id to *out_id (if non-NULL).
    * Returns 0, a negative tenancy code, or -1 on error. */
   int db2_team_create(const char *name, const char *operator_id, int64_t *out_id);

   /* List visible teams (ORDER BY id), up to `max`. Returns the count written, a
    * negative tenancy code, or -1. */
   int db2_team_list(db2_team_row_t *out, int max);

   /* Load a team by id. Returns 0 on success, a negative tenancy code, or -1 if
    * absent/error. */
   int db2_team_get(int64_t id, db2_team_row_t *out);

   /* Load a team by unique name. Returns 0 on success, a negative tenancy code, or
    * -1 if absent/error. */
   int db2_team_get_by_name(const char *name, db2_team_row_t *out);

#ifdef __cplusplus
}
#endif

#endif /* DEC_DB2_TEAM_H */
