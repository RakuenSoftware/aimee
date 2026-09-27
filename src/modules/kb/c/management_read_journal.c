#include "management_read_journal.h"
#include "db2_internal.h"
#include "db2_tenant.h"
#include "db_postgres.h"
#include "platform_random.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

static db2_management_read_result_t classify(const char *s)
{
   if (!s)
      return DB2_MANAGEMENT_READ_UNAVAILABLE;
   if (!strcmp(s, "22023"))
      return DB2_MANAGEMENT_READ_INVALID;
   if (!strcmp(s, "28000") || !strcmp(s, "42501"))
      return DB2_MANAGEMENT_READ_DENIED;
   if (!strcmp(s, "23505"))
      return DB2_MANAGEMENT_READ_CONFLICT;
   if (!strcmp(s, "40001") || !strcmp(s, "55000") || !strcmp(s, "P0002"))
      return DB2_MANAGEMENT_READ_INTEGRITY;
   return DB2_MANAGEMENT_READ_UNAVAILABLE;
}

static int text(aimee_pg_stmt_t *st, int col, char *out, size_t cap)
{
   const char *s = aimee_pg_column_is_null(st, col) ? NULL : aimee_pg_column_text(st, col);
   size_t n = s ? strnlen(s, cap) : cap;
   if (!s || !n || n == cap)
      return -1;
   memset(out, 0, cap);
   memcpy(out, s, n);
   return 0;
}

static int i64(aimee_pg_stmt_t *st, int col, int64_t *out)
{
   const char *s = aimee_pg_column_is_null(st, col) ? NULL : aimee_pg_column_text(st, col);
   char *end = NULL;
   errno = 0;
   long long v = s ? strtoll(s, &end, 10) : 0;
   if (!s || !*s || errno || !end || *end)
      return -1;
   *out = (int64_t)v;
   return 0;
}

static int decode(aimee_pg_stmt_t *st, db2_management_read_intent_t *o)
{
   return aimee_pg_column_count(st) != 18 || text(st, 0, o->correlation_id, 65) ||
          text(st, 1, o->jti, 65) || i64(st, 2, &o->team_id) ||
          text(st, 3, o->actor_identity, sizeof(o->actor_identity)) ||
          text(st, 4, o->target_server_id, sizeof(o->target_server_id)) ||
          text(st, 5, o->request_sha256, 65) || text(st, 6, o->kid, 65) ||
          i64(st, 7, &o->issued_at) || i64(st, 8, &o->expires_at) ||
          i64(st, 9, &o->issuance_deadline_epoch) ||
          text(st, 10, o->local_cert_issuer, sizeof(o->local_cert_issuer)) ||
          text(st, 11, o->local_cert_serial_norm, sizeof(o->local_cert_serial_norm)) ||
          text(st, 12, o->local_cert_fingerprint, 65) ||
          text(st, 13, o->target_mgmt_issuer, sizeof(o->target_mgmt_issuer)) ||
          text(st, 14, o->target_mgmt_serial_norm, sizeof(o->target_mgmt_serial_norm)) ||
          text(st, 15, o->target_mgmt_fingerprint, 65) || i64(st, 16, &o->revocation_generation) ||
          i64(st, 17, &o->publication_generation);
}

db2_management_read_result_t db2_management_read_publication_generation(int64_t *out)
{
   if (!out)
      return DB2_MANAGEMENT_READ_INVALID;
   *out = 0;
   char error[256] = "";
   aimee_pg_stmt_t *st =
       aimee_pg_prepare(db2_conn(), "SELECT public.kb_management_read_publication_generation()",
                        error, sizeof(error));
   aimee_pg_step_t step = st ? aimee_pg_step(st, error, sizeof(error)) : AIMEE_PG_ERR;
   db2_management_read_result_t rc = DB2_MANAGEMENT_READ_UNAVAILABLE;
   int64_t generation = 0;
   if (st && step == AIMEE_PG_ROW && aimee_pg_column_count(st) == 1 && !i64(st, 0, &generation) &&
       generation > 0 && generation <= INT16_MAX &&
       aimee_pg_step(st, error, sizeof(error)) == AIMEE_PG_DONE)
      rc = DB2_MANAGEMENT_READ_OK;
   else if (st && step == AIMEE_PG_ERR)
      rc = classify(aimee_pg_sqlstate(st));
   else if (st)
      rc = DB2_MANAGEMENT_READ_INTEGRITY;
   if (st)
      aimee_pg_finalize(st);
   if (rc == DB2_MANAGEMENT_READ_OK)
      *out = generation;
   return rc;
}

db2_management_read_result_t
db2_management_read_intent_start(const kb_principal_t *principal, int64_t team, const char *server,
                                 server_mgmt_read_selector_t selector, const char *external_path,
                                 const uint8_t nonce[32], const char *digest, const char *issuer,
                                 const char *installation, int ttl,
                                 db2_management_read_intent_t *out)
{
   if (out)
      memset(out, 0, sizeof(*out));
   const char *selector_name = server_mgmt_read_selector_name(selector);
   if (!principal || !out || team < 1 || !server || !selector_name || !external_path || !nonce ||
       !digest || !issuer || !installation || ttl < 1 || ttl > 90)
      return DB2_MANAGEMENT_READ_INVALID;
   char corr[65], jti[65];
   if (platform_random_hex(corr, 64) || platform_random_hex(jti, 64))
      return DB2_MANAGEMENT_READ_UNAVAILABLE;
   int tx = db2_tenant_scope_begin(principal, team);
   if (tx)
      return tx == DB2_ERR_TENANT_DENIED || tx == DB2_ERR_TENANT_UNAUTHENTICATED
                 ? DB2_MANAGEMENT_READ_DENIED
                 : DB2_MANAGEMENT_READ_UNAVAILABLE;
   char error[256] = "";
   aimee_pg_stmt_t *st =
       aimee_pg_prepare(db2_conn(),
                        "SELECT * FROM public.kb_management_read_intent_start(?1,?2,?3,?4,"
                        "?5,'GET',?6,?7,?8,?9,?10,?11)",
                        error, sizeof(error));
   int bound = !st || aimee_pg_bind_text(st, "?1", corr) || aimee_pg_bind_text(st, "?2", jti) ||
               aimee_pg_bind_int64(st, "?3", team) || aimee_pg_bind_text(st, "?4", server) ||
               aimee_pg_bind_text(st, "?5", selector_name) ||
               aimee_pg_bind_text(st, "?6", external_path) ||
               aimee_pg_bind_blob(st, "?7", nonce, 32) || aimee_pg_bind_text(st, "?8", digest) ||
               aimee_pg_bind_text(st, "?9", issuer) || aimee_pg_bind_int(st, "?10", ttl) ||
               aimee_pg_bind_text(st, "?11", installation);
   aimee_pg_step_t step = bound ? AIMEE_PG_ERR : aimee_pg_step(st, error, sizeof(error));
   db2_management_read_result_t rc = DB2_MANAGEMENT_READ_UNAVAILABLE;
   db2_management_read_intent_t candidate = {0};
   if (!bound && step == AIMEE_PG_ROW && !decode(st, &candidate) &&
       aimee_pg_step(st, error, sizeof(error)) == AIMEE_PG_DONE)
      rc = DB2_MANAGEMENT_READ_OK;
   else if (!bound && step == AIMEE_PG_ERR)
      rc = classify(aimee_pg_sqlstate(st));
   else if (!bound)
      rc = DB2_MANAGEMENT_READ_INTEGRITY;
   if (st)
      aimee_pg_finalize(st);
   if (rc != DB2_MANAGEMENT_READ_OK)
   {
      db2_tenant_scope_rollback();
      return rc;
   }
   if (strcmp(candidate.correlation_id, corr) || strcmp(candidate.jti, jti) ||
       candidate.team_id != team || strcmp(candidate.target_server_id, server) ||
       strcmp(candidate.request_sha256, digest))
   {
      db2_tenant_scope_rollback();
      return DB2_MANAGEMENT_READ_INTEGRITY;
   }
   if (db2_tenant_scope_commit())
      return DB2_MANAGEMENT_READ_COMMIT_AMBIGUOUS;
   *out = candidate;
   return DB2_MANAGEMENT_READ_OK;
}
