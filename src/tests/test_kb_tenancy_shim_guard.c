/* test_kb_tenancy_shim_guard.c (B1/N1): every tenant-scoped db2 entrypoint must
 * hard-fail with DB2_ERR_TENANT_REQUIRES_PG on the SQLite shim — RLS cannot be
 * enforced there, so a tenant op must never silently run unprotected. The pg
 * accessors are stubbed: db2_tenant_require_pg() returns early on the shim, so no
 * accessor is ever reached; the stubs exist only to satisfy the linker. */

#include "db2_tenant.h"
#include "team.h"
#include "project.h"
#include "membership.h"
#include "admin_grant.h"
#include "write_tier_grant.h"
#include "oidc_jwks.h"
#include "db_postgres.h"

#include <stdint.h>
#include <stdio.h>

/* --- stubs: force the shim backend; accessors are unreachable on this path --- */
int aimee_pg_is_shim(void)
{
   return 1;
}
void *(db2_conn)(void)
{
   return NULL;
}

/* Real code reaches the pool through the db2_conn() macro, which expands to
 * db2_conn_at(site) so a lazy acquire can be attributed. Route the stub. */
void *db2_conn_at(const char *site)
{
   (void)site;
   return (db2_conn)();
}
/* The real symbol is db2_lease_begin_at; db2_lease_begin is a macro in db2.h
 * that records the caller's file:line for stuck-lease attribution. */
void db2_lease_begin_at(const char *site)
{
}
void db2_lease_end(void)
{
}
aimee_pg_stmt_t *aimee_pg_prepare(void *c, const char *s, char *e, size_t n)
{
   (void)c;
   (void)s;
   (void)e;
   (void)n;
   return NULL;
}
void aimee_pg_finalize(aimee_pg_stmt_t *s)
{
   (void)s;
}
aimee_pg_step_t aimee_pg_step(aimee_pg_stmt_t *s, char *e, size_t n)
{
   (void)s;
   (void)e;
   (void)n;
   return AIMEE_PG_ERR;
}
int aimee_pg_bind_text(aimee_pg_stmt_t *s, const char *k, const char *v)
{
   (void)s;
   (void)k;
   (void)v;
   return 0;
}
int aimee_pg_bind_int64(aimee_pg_stmt_t *s, const char *k, int64_t v)
{
   (void)s;
   (void)k;
   (void)v;
   return 0;
}
int aimee_pg_exec(void *c, const char *s, char *e, size_t n)
{
   (void)c;
   (void)s;
   (void)e;
   (void)n;
   return 0;
}
int64_t aimee_pg_column_int64(aimee_pg_stmt_t *s, int c)
{
   (void)s;
   (void)c;
   return 0;
}
int aimee_pg_column_int(aimee_pg_stmt_t *s, int c)
{
   (void)s;
   (void)c;
   return 0;
}
const char *aimee_pg_column_text(aimee_pg_stmt_t *s, int c)
{
   (void)s;
   (void)c;
   return "";
}

static int fails = 0;
#define REQUIRES_PG(call, name)                                                                    \
   do                                                                                              \
   {                                                                                               \
      int rc = (call);                                                                             \
      if (rc != DB2_ERR_TENANT_REQUIRES_PG)                                                        \
      {                                                                                            \
         printf("FAIL: %s did not hard-fail on shim (rc=%d)\n", name, rc);                         \
         fails++;                                                                                  \
      }                                                                                            \
   } while (0)

int main(void)
{
   int64_t id = 0;
   int64_t teams[4];

   REQUIRES_PG(db2_tenant_require_pg(), "db2_tenant_require_pg");
   REQUIRES_PG(db2_maintenance_scope_begin(DB2_MAINTENANCE_INGEST, "p"),
               "db2_maintenance_scope_begin");

   /* Worker wiring is deliberately inert on the SQLite backend: there is no
    * content RLS to satisfy, so the thread-local job survives only long enough
    * to tell begin_current that no transaction is needed. */
   if (db2_maintenance_job_enter(DB2_MAINTENANCE_INGEST, "p") != 0 ||
       !db2_maintenance_job_active() || db2_maintenance_scope_begin_current() != 0 ||
       db2_maintenance_context_apply_current() != 0 ||
       db2_maintenance_job_enter(DB2_MAINTENANCE_CURATOR, "p") != DB2_ERR_MAINTENANCE_INVALID)
   {
      printf("FAIL: maintenance job context is not inert and non-nestable on shim\n");
      fails++;
   }
   db2_maintenance_job_leave();
   if (db2_maintenance_job_active())
   {
      printf("FAIL: maintenance job context survived leave\n");
      fails++;
   }

   /* Every tenant-scoped entry across all five modules must hard-fail. */
   REQUIRES_PG(db2_team_create("t", "op", &id), "db2_team_create");
   REQUIRES_PG(db2_team_list(NULL, 0), "db2_team_list");
   REQUIRES_PG(db2_team_get(1, NULL), "db2_team_get");
   REQUIRES_PG(db2_team_get_by_name("t", NULL), "db2_team_get_by_name");

   REQUIRES_PG(db2_project_create(1, "p", "team-open", "op", &id), "db2_project_create");
   REQUIRES_PG(db2_project_list(1, NULL, 0), "db2_project_list");
   REQUIRES_PG(db2_project_get(1, NULL), "db2_project_get");

   REQUIRES_PG(db2_membership_add("k", 1, 0, &id), "db2_membership_add");
   REQUIRES_PG(db2_membership_remove("k", 1), "db2_membership_remove");
   REQUIRES_PG(db2_membership_list_for_identity("k", NULL, 0), "db2_membership_list_for_identity");
   REQUIRES_PG(db2_membership_teams("k", teams, 4), "db2_membership_teams");
   REQUIRES_PG(db2_membership_default_team("k", teams), "db2_membership_default_team");

   REQUIRES_PG(db2_admin_grant_add("k", "oidc", "by", &id), "db2_admin_grant_add");
   REQUIRES_PG(db2_admin_grant_revoke("k"), "db2_admin_grant_revoke");
   REQUIRES_PG(db2_admin_grant_is_active("k"), "db2_admin_grant_is_active");

   {
      kb_identity_tier_t tier;
      db2_write_tier_grant_row_t rows[2];
      size_t count = 0;
      REQUIRES_PG(db2_write_tier_grant_lookup("srv", 1, "owner", &tier),
                  "db2_write_tier_grant_lookup");
      REQUIRES_PG(db2_write_tier_grant_set("srv", 1, "owner", KB_IDENTITY_TIER_DATA, "by"),
                  "db2_write_tier_grant_set");
      REQUIRES_PG(db2_write_tier_grant_revoke("srv", 1, "owner"), "db2_write_tier_grant_revoke");
      REQUIRES_PG(db2_write_tier_grant_list("srv", 1, rows, 2, &count),
                  "db2_write_tier_grant_list");
   }

   REQUIRES_PG(db2_jwks_add("iss", "kid", "{}", &id), "db2_jwks_add");
   REQUIRES_PG(db2_jwks_retire("iss", "kid"), "db2_jwks_retire");
   REQUIRES_PG(db2_jwks_list_active("iss", NULL, 0), "db2_jwks_list_active");

   if (fails == 0)
      printf("test_kb_tenancy_shim_guard: all passed\n");
   return fails ? 1 : 0;
}
