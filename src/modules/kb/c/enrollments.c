/* kb_store/enrollments.c: redeemed-cert enrollment records — Postgres via libpq.
 * See enrollments.h. Mirrors the kb_store/decision_log.c access pattern. */

#include "enrollments.h"
#include "kb_store_internal.h"
#include "db_postgres.h"
#include "kb_identity.h" /* kb_cert_serial_normalize for the (issuer,serial) key */
#include "log.h"

#include <openssl/rand.h>

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static void row_from_stmt(aimee_pg_stmt_t *st, kb_store_enrollment_row_t *row)
{
   memset(row, 0, sizeof(*row));
   row->id = aimee_pg_column_int64(st, 0);
   const char *c;
   c = aimee_pg_column_text(st, 1);
   snprintf(row->scope, sizeof(row->scope), "%s", c ? c : "");
   c = aimee_pg_column_text(st, 2);
   snprintf(row->fingerprint, sizeof(row->fingerprint), "%s", c ? c : "");
   c = aimee_pg_column_text(st, 3);
   snprintf(row->serial, sizeof(row->serial), "%s", c ? c : "");
   c = aimee_pg_column_text(st, 4);
   snprintf(row->state, sizeof(row->state), "%s", c ? c : "");
   c = aimee_pg_column_text(st, 5);
   snprintf(row->issued_at, sizeof(row->issued_at), "%s", c ? c : "");
   c = aimee_pg_column_text(st, 6);
   snprintf(row->last_seen_at, sizeof(row->last_seen_at), "%s", c ? c : "");
   c = aimee_pg_column_text(st, 7);
   snprintf(row->expires_at, sizeof(row->expires_at), "%s", c ? c : "");
   c = aimee_pg_column_text(st, 8);
   snprintf(row->revoked_at, sizeof(row->revoked_at), "%s", c ? c : "");
   row->legacy = (int)aimee_pg_column_int64(st, 9);
   c = aimee_pg_column_text(st, 10);
   snprintf(row->authority_id, sizeof(row->authority_id), "%s", c ? c : "");
}

#define ENROLL_COLS                                                                                \
   "id, scope, fingerprint, serial, state, issued_at, last_seen_at, expires_at, revoked_at, "      \
   "legacy, authority_id"

static int authority_generate(char out[33])
{
   static const char hex[] = "0123456789abcdef";
   unsigned char raw[16];
   if (RAND_bytes(raw, sizeof(raw)) != 1)
      return -1;
   for (size_t i = 0; i < sizeof(raw); i++)
   {
      out[i * 2] = hex[raw[i] >> 4];
      out[i * 2 + 1] = hex[raw[i] & 0xf];
   }
   out[32] = '\0';
   return 0;
}

static void revcache_put(const char *fp, int revoked); /* defined below */

int kb_store_enrollment_insert(const char *scope, const char *fingerprint, const char *cert_issuer,
                               const char *cert_serial_norm, const char *expires_at, int legacy,
                               int64_t *out_id)
{
   if (!fingerprint || !fingerprint[0] || !cert_issuer || !cert_issuer[0] || !cert_serial_norm ||
       !cert_serial_norm[0])
      return -1;
   void *conn = kb_store_conn();
   if (!conn)
      return -1;
   char authority_id[33];
   if (authority_generate(authority_id) != 0)
      return -1;
   /* On a fingerprint conflict, refresh only metadata and ONLY for a non-revoked
    * row (the WHERE guard) — the redeem path must never resurrect a revoked cert.
    * Normal redeems present a fresh cert (new serial => new fingerprint), so the
    * conflict path is just idempotent-retry / re-registration. */
   const char *sql =
       "INSERT INTO kb_enrollments (scope, fingerprint, serial, expires_at, legacy, authority_id, "
       "cert_issuer, cert_serial_norm) VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8) "
       "ON CONFLICT (fingerprint) DO UPDATE SET scope=EXCLUDED.scope, serial=EXCLUDED.serial, "
       "expires_at=EXCLUDED.expires_at WHERE kb_enrollments.revoked_at='' AND "
       "kb_enrollments.cert_issuer=EXCLUDED.cert_issuer AND "
       "kb_enrollments.cert_serial_norm=EXCLUDED.cert_serial_norm RETURNING id";
   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", scope ? scope : "");
   aimee_pg_bind_text(st, "?2", fingerprint);
   aimee_pg_bind_text(st, "?3", cert_serial_norm);
   aimee_pg_bind_text(st, "?4", expires_at ? expires_at : "");
   aimee_pg_bind_int64(st, "?5", legacy ? 1 : 0);
   aimee_pg_bind_text(st, "?6", authority_id);
   aimee_pg_bind_text(st, "?7", cert_issuer);
   aimee_pg_bind_text(st, "?8", cert_serial_norm);
   aimee_pg_step_t step = aimee_pg_step(st, err, sizeof(err));
   int64_t id = (step == AIMEE_PG_ROW) ? aimee_pg_column_int64(st, 0) : 0;
   aimee_pg_finalize(st);
   if (step != AIMEE_PG_ROW)
      return -1; /* e.g. conflict on an already-revoked fp: left revoked, no id */
   if (out_id)
      *out_id = id;
   return 0;
}

int kb_store_enrollment_renew(const char *old_fingerprint, const char *old_issuer,
                              const char *old_serial_norm, const char *scope,
                              const char *new_fingerprint, const char *new_issuer,
                              const char *new_serial_norm, int64_t *out_id)
{
   if (!old_fingerprint || !old_fingerprint[0] || !old_issuer || !old_issuer[0] ||
       !old_serial_norm || !old_serial_norm[0] || !scope || !scope[0] || !new_fingerprint ||
       !new_fingerprint[0] || !new_issuer || !new_issuer[0] || !new_serial_norm ||
       !new_serial_norm[0])
      return -1;
   void *conn = kb_store_conn();
   if (!conn)
      return -1;
   char err[256] = "";
   aimee_pg_stmt_t *st =
       aimee_pg_prepare(conn, "SELECT kb_enrollment_renew(?1,?2,?3,?4,?5,?6,?7)", err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", old_fingerprint);
   aimee_pg_bind_text(st, "?2", old_issuer);
   aimee_pg_bind_text(st, "?3", old_serial_norm);
   aimee_pg_bind_text(st, "?4", scope);
   aimee_pg_bind_text(st, "?5", new_fingerprint);
   aimee_pg_bind_text(st, "?6", new_issuer);
   aimee_pg_bind_text(st, "?7", new_serial_norm);
   aimee_pg_step_t step = aimee_pg_step(st, err, sizeof(err));
   int64_t id = step == AIMEE_PG_ROW ? aimee_pg_column_int64(st, 0) : 0;
   aimee_pg_finalize(st);
   if (step != AIMEE_PG_ROW || id <= 0)
      return -1;
   if (out_id)
      *out_id = id;
   return 0;
}

int kb_store_enrollment_list(int limit, kb_store_enrollment_row_t *out, int max)
{
   if (!out || max <= 0)
      return -1;
   void *conn = kb_store_conn();
   if (!conn)
      return -1;
   if (limit <= 0 || limit > max)
      limit = max;
   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       conn, "SELECT " ENROLL_COLS " FROM kb_enrollments ORDER BY id DESC LIMIT ?1", err,
       sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_int64(st, "?1", limit);
   int n = 0;
   while (n < max && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
      row_from_stmt(st, &out[n++]);
   aimee_pg_finalize(st);
   return n;
}

int kb_store_enrollment_revoke(int64_t id, kb_store_enrollment_row_t *out)
{
   void *conn = kb_store_conn();
   if (!conn)
      return -1;
   /* Idempotent: revoking twice keeps the first revoked_at and still returns the
    * row, so a double-revoke is not an error. */
   const char *sql = "UPDATE kb_enrollments SET state='revoked', "
                     "revoked_at=COALESCE(NULLIF(revoked_at,''), pg_now_text()) "
                     "WHERE id=?1 RETURNING " ENROLL_COLS;
   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_int64(st, "?1", id);
   aimee_pg_step_t step = aimee_pg_step(st, err, sizeof(err));
   kb_store_enrollment_row_t row;
   int rc;
   if (step == AIMEE_PG_ROW)
   {
      row_from_stmt(st, &row);
      if (out)
         *out = row;
      rc = 0;
   }
   else
      rc = 1; /* no such id */
   aimee_pg_finalize(st);
   if (rc == 0)
      /* Prime the cache with the revoked verdict so it is honored immediately
       * AND continues to be honored if the DB later becomes unreachable — a
       * revoked cert stays denied through a subsequent outage. */
      revcache_put(row.fingerprint, 1);
   return rc;
}

/* --- is-revoked cache (short TTL over the DB source of truth) --- */

#define REVCACHE_MAX      256
#define REVCACHE_TTL_SECS 30
struct revcache_ent
{
   char fp[80];
   int revoked;
   time_t at;
};
static struct revcache_ent g_revcache[REVCACHE_MAX];
static pthread_mutex_t g_revcache_lock = PTHREAD_MUTEX_INITIALIZER;

static void revcache_put(const char *fp, int revoked)
{
   time_t now = time(NULL);
   pthread_mutex_lock(&g_revcache_lock);
   int slot = -1, oldest = 0;
   for (int i = 0; i < REVCACHE_MAX; i++)
   {
      if (g_revcache[i].fp[0] && strcmp(g_revcache[i].fp, fp) == 0)
      {
         slot = i; /* update in place — never a duplicate entry */
         break;
      }
      if (slot < 0 && !g_revcache[i].fp[0])
         slot = i;
      if (g_revcache[i].at < g_revcache[oldest].at)
         oldest = i;
   }
   if (slot < 0)
      slot = oldest; /* full and no match: evict oldest */
   snprintf(g_revcache[slot].fp, sizeof(g_revcache[slot].fp), "%s", fp);
   g_revcache[slot].revoked = revoked;
   g_revcache[slot].at = now;
   pthread_mutex_unlock(&g_revcache_lock);
}

int kb_store_enrollment_is_active_by_key(const char *cert_issuer, const char *cert_serial_norm)
{
   if (!cert_issuer || !cert_issuer[0] || !cert_serial_norm || !cert_serial_norm[0])
      return -1;
   void *conn = kb_store_conn();
   if (!conn)
      return -1;
   char err[256] = "";
   aimee_pg_stmt_t *st =
       aimee_pg_prepare(conn,
                        "SELECT state, revoked_at FROM kb_enrollments WHERE cert_issuer=?1 AND "
                        "cert_serial_norm=?2",
                        err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", cert_issuer);
   aimee_pg_bind_text(st, "?2", cert_serial_norm);
   aimee_pg_step_t step = aimee_pg_step(st, err, sizeof(err));
   int status = 0; /* an unknown identity is not active */
   if (step == AIMEE_PG_ROW)
   {
      const char *state = aimee_pg_column_text(st, 0);
      const char *revoked_at = aimee_pg_column_text(st, 1);
      if (state && strcmp(state, "active") == 0 && (!revoked_at || !revoked_at[0]))
         status = 1;
      else if (!state || (strcmp(state, "active") != 0 && strcmp(state, "revoked") != 0))
         status = -1;
   }
   else if (step != AIMEE_PG_DONE)
      status = -1;
   aimee_pg_finalize(st);
   return status;
}

int kb_store_enrollment_authority_resolve(const char *fingerprint, const char *cert_issuer,
                                          const char *cert_serial_norm, char out_authority[33])
{
   if (!fingerprint || !fingerprint[0] || !cert_issuer || !cert_issuer[0] || !cert_serial_norm ||
       !cert_serial_norm[0] || !out_authority)
      return -1;
   out_authority[0] = '\0';
   void *conn = kb_store_conn();
   if (!conn)
      return -1;
   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       conn,
       "SELECT authority_id FROM kb_enrollments WHERE fingerprint=?1 AND cert_issuer=?2 "
       "AND cert_serial_norm=?3 AND state='active' AND revoked_at=''",
       err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", fingerprint);
   aimee_pg_bind_text(st, "?2", cert_issuer);
   aimee_pg_bind_text(st, "?3", cert_serial_norm);
   aimee_pg_step_t step = aimee_pg_step(st, err, sizeof(err));
   int rc = 1;
   if (step == AIMEE_PG_ROW)
   {
      const char *authority = aimee_pg_column_text(st, 0);
      if (authority && strlen(authority) == 32)
      {
         memcpy(out_authority, authority, 33);
         rc = 0;
      }
      else
         rc = -1;
   }
   else if (step != AIMEE_PG_DONE)
      rc = -1;
   aimee_pg_finalize(st);
   return rc;
}

/* --- debounced last-seen --- */

#define SEEN_DEBOUNCE_MAX  256
#define SEEN_DEBOUNCE_SECS 300
struct seen_ent
{
   char fp[80];
   time_t at;
};
static struct seen_ent g_seen[SEEN_DEBOUNCE_MAX];
static pthread_mutex_t g_seen_lock = PTHREAD_MUTEX_INITIALIZER;

/* Returns 1 if we should write (debounce window elapsed), recording the write. */
static int seen_should_write(const char *fp)
{
   time_t now = time(NULL);
   pthread_mutex_lock(&g_seen_lock);
   int slot = -1, oldest = 0;
   for (int i = 0; i < SEEN_DEBOUNCE_MAX; i++)
   {
      if (g_seen[i].fp[0] && strcmp(g_seen[i].fp, fp) == 0)
      {
         if (now - g_seen[i].at < SEEN_DEBOUNCE_SECS)
         {
            pthread_mutex_unlock(&g_seen_lock);
            return 0;
         }
         g_seen[i].at = now;
         pthread_mutex_unlock(&g_seen_lock);
         return 1;
      }
      if (!g_seen[i].fp[0])
         slot = (slot < 0) ? i : slot;
      if (g_seen[i].at < g_seen[oldest].at)
         oldest = i;
   }
   if (slot < 0)
      slot = oldest;
   snprintf(g_seen[slot].fp, sizeof(g_seen[slot].fp), "%s", fp);
   g_seen[slot].at = now;
   pthread_mutex_unlock(&g_seen_lock);
   return 1;
}

void kb_store_enrollment_touch_last_seen(const char *fingerprint, const char *scope)
{
   if (!fingerprint || !fingerprint[0] || !seen_should_write(fingerprint))
      return;
   void *conn = kb_store_conn();
   if (!conn)
      return;
   char authority_id[33];
   if (authority_generate(authority_id) != 0)
      return;
   /* Upsert: backfill a legacy row for a pre-S2a cert on first use, else bump
    * last_seen. The conflict path touches ONLY last_seen_at — never state or
    * revoked_at — so a revoked cert is not resurrected by being used. */
   char err[256] = "";
   aimee_pg_stmt_t *st =
       aimee_pg_prepare(conn,
                        "INSERT INTO kb_enrollments "
                        "(scope, fingerprint, legacy, last_seen_at, authority_id) "
                        "VALUES (?1, ?2, 1, pg_now_text(), ?3) "
                        "ON CONFLICT (fingerprint) DO UPDATE SET last_seen_at=pg_now_text()",
                        err, sizeof(err));
   if (!st)
      return;
   aimee_pg_bind_text(st, "?1", scope ? scope : "");
   aimee_pg_bind_text(st, "?2", fingerprint);
   aimee_pg_bind_text(st, "?3", authority_id);
   aimee_pg_step(st, err, sizeof(err));
   aimee_pg_finalize(st);
}

/* --- console OIDC login config (single row id=1) --- */

int kb_store_console_oidc_get(kb_store_console_oidc_t *out)
{
   if (!out)
      return -1;
   memset(out, 0, sizeof(*out));
   void *conn = kb_store_conn();
   if (!conn)
      return -1;
   char err[256] = "";
   aimee_pg_stmt_t *st =
       aimee_pg_prepare(conn,
                        "SELECT issuer, audience, jwks_url, admin_claim, admin_values, updated_at"
                        " FROM kb_console_oidc WHERE id = 1",
                        err, sizeof(err));
   if (!st)
      return -1;
   int rc = 1;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      const char *c;
      c = aimee_pg_column_text(st, 0);
      snprintf(out->issuer, sizeof(out->issuer), "%s", c ? c : "");
      c = aimee_pg_column_text(st, 1);
      snprintf(out->audience, sizeof(out->audience), "%s", c ? c : "");
      c = aimee_pg_column_text(st, 2);
      snprintf(out->jwks_url, sizeof(out->jwks_url), "%s", c ? c : "");
      c = aimee_pg_column_text(st, 3);
      snprintf(out->admin_claim, sizeof(out->admin_claim), "%s", c ? c : "");
      c = aimee_pg_column_text(st, 4);
      snprintf(out->admin_values, sizeof(out->admin_values), "%s", c ? c : "");
      c = aimee_pg_column_text(st, 5);
      snprintf(out->updated_at, sizeof(out->updated_at), "%s", c ? c : "");
      rc = 0;
   }
   aimee_pg_finalize(st);
   return rc;
}

int kb_store_console_oidc_put(const kb_store_console_oidc_t *in)
{
   if (!in)
      return -1;
   void *conn = kb_store_conn();
   if (!conn)
      return -1;
   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       conn,
       "INSERT INTO kb_console_oidc (id, issuer, audience, jwks_url, admin_claim, admin_values,"
       " updated_at) VALUES (1, ?1, ?2, ?3, ?4, ?5, pg_now_text())"
       " ON CONFLICT (id) DO UPDATE SET issuer=EXCLUDED.issuer, audience=EXCLUDED.audience,"
       " jwks_url=EXCLUDED.jwks_url, admin_claim=EXCLUDED.admin_claim,"
       " admin_values=EXCLUDED.admin_values, updated_at=EXCLUDED.updated_at",
       err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", in->issuer);
   aimee_pg_bind_text(st, "?2", in->audience);
   aimee_pg_bind_text(st, "?3", in->jwks_url);
   aimee_pg_bind_text(st, "?4", in->admin_claim);
   aimee_pg_bind_text(st, "?5", in->admin_values);
   aimee_pg_step_t rc = aimee_pg_step(st, err, sizeof(err));
   aimee_pg_finalize(st);
   return (rc == AIMEE_PG_DONE || rc == AIMEE_PG_ROW) ? 0 : -1;
}
