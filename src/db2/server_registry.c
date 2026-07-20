#include "server_registry.h"
#include "db2_internal.h"
#include "db2_tenant.h"
#include "db_postgres.h"
#include <stdio.h>
#include <string.h>
static void cp(char *d, size_t n, const char *s)
{
   snprintf(d, n, "%s", s ? s : "");
}
int db2_server_registry_list(int64_t team, db2_server_row_t *out, int max)
{
   if (db2_tenant_require_pg() != 0 || !out || max <= 0)
      return -1;
   void *c = db2_conn();
   if (!c)
      return -1;
   char e[256];
   aimee_pg_stmt_t *s = aimee_pg_prepare(
       c,
       "SELECT server_id,cert_cn,mgmt_cert_cn,endpoint,status,health,version,team_id FROM "
       "kb_server_registry WHERE team_id=?1 ORDER BY server_id",
       e, sizeof(e));
   if (!s)
      return -1;
   aimee_pg_bind_int64(s, "?1", team);
   int n = 0;
   while (n < max && aimee_pg_step(s, e, sizeof(e)) == AIMEE_PG_ROW)
   {
      db2_server_row_t *r = &out[n++];
      memset(r, 0, sizeof(*r));
      cp(r->server_id, sizeof(r->server_id), aimee_pg_column_text(s, 0));
      cp(r->cert_cn, sizeof(r->cert_cn), aimee_pg_column_text(s, 1));
      cp(r->mgmt_cert_cn, sizeof(r->mgmt_cert_cn), aimee_pg_column_text(s, 2));
      cp(r->endpoint, sizeof(r->endpoint), aimee_pg_column_text(s, 3));
      cp(r->status, sizeof(r->status), aimee_pg_column_text(s, 4));
      cp(r->health, sizeof(r->health), aimee_pg_column_text(s, 5));
      cp(r->version, sizeof(r->version), aimee_pg_column_text(s, 6));
      r->team_id = aimee_pg_column_int64(s, 7);
   }
   aimee_pg_finalize(s);
   return n;
}
int db2_server_registry_heartbeat(const char *id, const char *cn, const char *health,
                                  const char *version)
{
   if (db2_tenant_require_pg() != 0 || !id || !cn)
      return -1;
   void *c = db2_conn();
   if (!c)
      return -1;
   char e[256];
   aimee_pg_stmt_t *s = aimee_pg_prepare(
       c,
       "UPDATE kb_server_registry SET health=?3,version=?4,last_seen=now(),updated_at=now() WHERE "
       "server_id=?1 AND cert_cn=?2 AND status='active'",
       e, sizeof(e));
   if (!s)
      return -1;
   aimee_pg_bind_text(s, "?1", id);
   aimee_pg_bind_text(s, "?2", cn);
   aimee_pg_bind_text(s, "?3", health ? health : "");
   aimee_pg_bind_text(s, "?4", version ? version : "");
   int rc = aimee_pg_step(s, e, sizeof(e)) == AIMEE_PG_DONE ? 0 : -1;
   aimee_pg_finalize(s);
   return rc;
}

int db2_server_registry_get(int64_t team, const char *id, db2_server_row_t *r)
{
   if (db2_tenant_require_pg() != 0 || !id || !r)
      return -1;
   void *c = db2_conn();
   if (!c)
      return -1;
   char e[256];
   aimee_pg_stmt_t *s = aimee_pg_prepare(
       c,
       "SELECT server_id,cert_cn,mgmt_cert_cn,endpoint,status,health,version,team_id FROM "
       "kb_server_registry WHERE team_id=?1 AND server_id=?2",
       e, sizeof(e));
   if (!s)
      return -1;
   aimee_pg_bind_int64(s, "?1", team);
   aimee_pg_bind_text(s, "?2", id);
   int rc = -1;
   if (aimee_pg_step(s, e, sizeof(e)) == AIMEE_PG_ROW)
   {
      memset(r, 0, sizeof(*r));
      cp(r->server_id, sizeof(r->server_id), aimee_pg_column_text(s, 0));
      cp(r->cert_cn, sizeof(r->cert_cn), aimee_pg_column_text(s, 1));
      cp(r->mgmt_cert_cn, sizeof(r->mgmt_cert_cn), aimee_pg_column_text(s, 2));
      cp(r->endpoint, sizeof(r->endpoint), aimee_pg_column_text(s, 3));
      cp(r->status, sizeof(r->status), aimee_pg_column_text(s, 4));
      cp(r->health, sizeof(r->health), aimee_pg_column_text(s, 5));
      cp(r->version, sizeof(r->version), aimee_pg_column_text(s, 6));
      r->team_id = aimee_pg_column_int64(s, 7);
      rc = 0;
   }
   aimee_pg_finalize(s);
   return rc;
}
