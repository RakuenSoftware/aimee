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
int db2_server_registry_pending(const db2_server_pending_t *p, char *status, size_t status_cap)
{
   if (db2_tenant_require_pg() != 0 || !p || !p->operation || !p->server_id || !p->endpoint ||
       !p->client_cn || !p->management_cn || !p->client_csr_digest || !p->management_csr_digest ||
       !status || !status_cap)
      return -1;
   void *c = db2_conn();
   if (!c)
      return -1;
   char e[256];
   aimee_pg_stmt_t *s = aimee_pg_prepare(
       c, "SELECT status FROM kb_server_registry_pending(?1,?2,?3,?4,?5,?6,?7,?8,?9)", e,
       sizeof(e));
   if (!s)
      return -1;
   aimee_pg_bind_text(s, "?1", p->operation);
   aimee_pg_bind_text(s, "?2", p->server_id);
   aimee_pg_bind_int64(s, "?3", p->team_id);
   aimee_pg_bind_text(s, "?4", p->endpoint);
   aimee_pg_bind_text(s, "?5", p->client_cn);
   aimee_pg_bind_text(s, "?6", p->management_cn);
   aimee_pg_bind_text(s, "?7", p->client_csr_digest);
   aimee_pg_bind_text(s, "?8", p->management_csr_digest);
   aimee_pg_bind_int64(s, "?9", p->ttl_seconds);
   int rc = -1;
   if (aimee_pg_step(s, e, sizeof(e)) == AIMEE_PG_ROW)
   {
      cp(status, status_cap, aimee_pg_column_text(s, 0));
      rc = 0;
   }
   aimee_pg_finalize(s);
   return rc;
}

int db2_server_registry_finalize(const char *operation, const char *client_csr_digest,
                                 const char *management_csr_digest,
                                 const db2_server_cert_identity_t *client,
                                 const db2_server_cert_identity_t *management, char *status,
                                 size_t status_cap)
{
   if (db2_tenant_require_pg() != 0 || !operation || !client_csr_digest || !management_csr_digest ||
       !client || !client->issuer || !client->serial_norm || !client->fingerprint || !management ||
       !management->issuer || !management->serial_norm || !management->fingerprint || !status ||
       !status_cap)
      return -1;
   void *c = db2_conn();
   if (!c)
      return -1;
   char e[256];
   aimee_pg_stmt_t *s = aimee_pg_prepare(
       c, "SELECT status FROM kb_server_registry_finalize(?1,?2,?3,?4,?5,?6,?7,?8,?9)", e,
       sizeof(e));
   if (!s)
      return -1;
   aimee_pg_bind_text(s, "?1", operation);
   aimee_pg_bind_text(s, "?2", client_csr_digest);
   aimee_pg_bind_text(s, "?3", management_csr_digest);
   aimee_pg_bind_text(s, "?4", client->issuer);
   aimee_pg_bind_text(s, "?5", client->serial_norm);
   aimee_pg_bind_text(s, "?6", client->fingerprint);
   aimee_pg_bind_text(s, "?7", management->issuer);
   aimee_pg_bind_text(s, "?8", management->serial_norm);
   aimee_pg_bind_text(s, "?9", management->fingerprint);
   int rc = -1;
   if (aimee_pg_step(s, e, sizeof(e)) == AIMEE_PG_ROW)
   {
      cp(status, status_cap, aimee_pg_column_text(s, 0));
      rc = 0;
   }
   aimee_pg_finalize(s);
   return rc;
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
       "kb_server_registry_list(?1)",
       e, sizeof(e));
   if (!s)
      return -1;
   aimee_pg_bind_int64(s, "?1", team);
   int n = 0;
   aimee_pg_step_t step = AIMEE_PG_DONE;
   while (n < max && (step = aimee_pg_step(s, e, sizeof(e))) == AIMEE_PG_ROW)
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
   if (step == AIMEE_PG_ERR)
      return -1;
   return n;
}
int db2_server_registry_heartbeat(const char *id, const char *issuer, const char *serial,
                                  const char *fingerprint, const char *health, const char *version)
{
   if (db2_tenant_require_pg() != 0 || !id || !issuer || !serial || !fingerprint)
      return -1;
   void *c = db2_conn();
   if (!c)
      return -1;
   char e[256];
   aimee_pg_stmt_t *s =
       aimee_pg_prepare(c, "SELECT kb_server_registry_heartbeat(?1,?2,?3,?4,?5,?6)", e, sizeof(e));
   if (!s)
      return -1;
   aimee_pg_bind_text(s, "?1", id);
   aimee_pg_bind_text(s, "?2", issuer);
   aimee_pg_bind_text(s, "?3", serial);
   aimee_pg_bind_text(s, "?4", fingerprint);
   aimee_pg_bind_text(s, "?5", health ? health : "");
   aimee_pg_bind_text(s, "?6", version ? version : "");
   int rc = -1;
   if (aimee_pg_step(s, e, sizeof(e)) == AIMEE_PG_ROW)
   {
      const char *allowed = aimee_pg_column_text(s, 0);
      rc = allowed && (allowed[0] == 't' || allowed[0] == '1') ? 0 : -1;
   }
   aimee_pg_finalize(s);
   return rc;
}

int db2_server_registry_client_match(const char *id, int64_t team, const char *issuer,
                                     const char *serial, const char *fingerprint)
{
   if (db2_tenant_require_pg() != 0 || !id || team <= 0 || !issuer || !serial || !fingerprint)
      return -1;
   void *c = db2_conn();
   if (!c)
      return -1;
   char e[256];
   aimee_pg_stmt_t *s =
       aimee_pg_prepare(c, "SELECT kb_server_registry_client_match(?1,?2,?3,?4,?5)", e, sizeof(e));
   if (!s)
      return -1;
   aimee_pg_bind_text(s, "?1", id);
   aimee_pg_bind_int64(s, "?2", team);
   aimee_pg_bind_text(s, "?3", issuer);
   aimee_pg_bind_text(s, "?4", serial);
   aimee_pg_bind_text(s, "?5", fingerprint);
   int rc = -1;
   if (aimee_pg_step(s, e, sizeof(e)) == AIMEE_PG_ROW)
   {
      const char *allowed = aimee_pg_column_text(s, 0);
      rc = allowed && (allowed[0] == 't' || allowed[0] == '1') ? 1 : 0;
   }
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
       "kb_server_registry_list(?1) WHERE server_id=?2",
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

int db2_server_registry_snapshot(int64_t team, const char *id, db2_server_snapshot_t *r)
{
   if (db2_tenant_require_pg() != 0 || !id || !r)
      return -1;
   void *c = db2_conn();
   if (!c)
      return -1;
   char e[256];
   aimee_pg_stmt_t *s = aimee_pg_prepare(
       c,
       "SELECT server_id,endpoint,status,mgmt_issuer,mgmt_serial_norm,mgmt_fingerprint,"
       "enrollment_state,revoked_at,revocation_generation FROM "
       "kb_server_registry_snapshot(?1,?2)",
       e, sizeof(e));
   if (!s)
      return -1;
   aimee_pg_bind_int64(s, "?1", team);
   aimee_pg_bind_text(s, "?2", id);
   int rc = -1;
   aimee_pg_step_t step = aimee_pg_step(s, e, sizeof(e));
   if (step == AIMEE_PG_ROW)
   {
      memset(r, 0, sizeof(*r));
      cp(r->server_id, sizeof(r->server_id), aimee_pg_column_text(s, 0));
      cp(r->endpoint, sizeof(r->endpoint), aimee_pg_column_text(s, 1));
      cp(r->status, sizeof(r->status), aimee_pg_column_text(s, 2));
      cp(r->management_issuer, sizeof(r->management_issuer), aimee_pg_column_text(s, 3));
      cp(r->management_serial_norm, sizeof(r->management_serial_norm), aimee_pg_column_text(s, 4));
      cp(r->management_fingerprint, sizeof(r->management_fingerprint), aimee_pg_column_text(s, 5));
      cp(r->enrollment_state, sizeof(r->enrollment_state), aimee_pg_column_text(s, 6));
      cp(r->revoked_at, sizeof(r->revoked_at), aimee_pg_column_text(s, 7));
      r->revocation_generation = aimee_pg_column_int64(s, 8);
      rc = 0;
   }
   else if (step == AIMEE_PG_DONE)
      rc = 1;
   aimee_pg_finalize(s);
   return rc;
}

int db2_management_status_lookup(const char *issuer, const char *serial, const char *fingerprint,
                                 const char *target, const char *purpose, int64_t *generation,
                                 char *target_fingerprint, size_t target_fingerprint_len)
{
   if (db2_tenant_require_pg() != 0 || !issuer || !serial || !fingerprint || !target || !purpose ||
       !generation || !target_fingerprint || target_fingerprint_len < 65)
      return -1;
   void *c = db2_conn();
   if (!c)
      return -1;
   char e[256];
   aimee_pg_stmt_t *s =
       aimee_pg_prepare(c,
                        "SELECT revocation_generation,target_mgmt_fingerprint FROM "
                        "kb_management_status_lookup(?1,?2,?3,?4,?5)",
                        e, sizeof(e));
   if (!s)
      return -1;
   aimee_pg_bind_text(s, "?1", issuer);
   aimee_pg_bind_text(s, "?2", serial);
   aimee_pg_bind_text(s, "?3", fingerprint);
   aimee_pg_bind_text(s, "?4", target);
   aimee_pg_bind_text(s, "?5", purpose);
   int rc = -1;
   if (aimee_pg_step(s, e, sizeof(e)) == AIMEE_PG_ROW)
   {
      int64_t g = aimee_pg_column_int64(s, 0);
      const char *fp = aimee_pg_column_text(s, 1);
      if (g >= 1 && fp && strlen(fp) == 64)
      {
         *generation = g;
         cp(target_fingerprint, target_fingerprint_len, fp);
         rc = 0;
      }
   }
   aimee_pg_finalize(s);
   return rc;
}
