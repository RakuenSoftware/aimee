/* db2/project.h: P1 tenancy — projects (child of a team). Backed by kb_project.
 *
 * Tenant-scoped: every entry point requires the RLS-enforcing Postgres backend
 * (db2_tenant_require_pg) and reads/writes under the caller's tenant context.
 * access_mode is 'team-open' | 'restricted'. Pure domain API. */
#ifndef DEC_DB2_PROJECT_H
#define DEC_DB2_PROJECT_H 1

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

   typedef struct
   {
      int64_t id;
      int64_t parent;
      char name[128];
      char access_mode[16];
      char created_at[32];
      char operator_id[128];
   } db2_project_row_t;

   /* Create a project under team `parent`. On success writes the new id to
    * *out_id (if non-NULL). Returns 0, a negative tenancy code, or -1. */
   int db2_project_create(int64_t parent, const char *name, const char *access_mode,
                          const char *operator_id, int64_t *out_id);

   /* List projects (ORDER BY id), up to `max`. parent<=0 lists all visible
    * projects; parent>0 restricts to that team. Returns the count written, a
    * negative tenancy code, or -1. */
   int db2_project_list(int64_t parent, db2_project_row_t *out, int max);

   /* Load a project by id. Returns 0 on success, a negative tenancy code, or -1 if
    * absent/error. */
   int db2_project_get(int64_t id, db2_project_row_t *out);

#ifdef __cplusplus
}
#endif

#endif /* DEC_DB2_PROJECT_H */
