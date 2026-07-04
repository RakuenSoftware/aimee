/* db2/enrollments.c: redeemed-cert enrollment records — Postgres via libpq.
 * See enrollments.h. Mirrors the db2/decision_log.c access pattern. */

#include "enrollments.h"
#include "db2_internal.h"
#include "db_postgres.h"
#include "log.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static void row_from_stmt(aimee_pg_stmt_t *st, db2_enrollment_row_t *row)
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
}

#define ENROLL_COLS                                                                                \
   "id, scope, fingerprint, serial, state, issued_at, last_seen_at, expires_at, revoked_at, "      \
   "legacy"

static void revcache_put(const char *fp, int revoked); /* defined below */

int db2_enrollment_insert(const char *scope, const char *fingerprint, const char *serial,
                          const char *expires_at, int legacy, int64_t *out_id)
{
   if (!fingerprint || !fingerprint[0])
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;
   /* On a fingerprint conflict, refresh only metadata and ONLY for a non-revoked
    * row (the WHERE guard) — the redeem path must never resurrect a revoked cert.
    * Normal redeems present a fresh cert (new serial => new fingerprint), so the
    * conflict path is just idempotent-retry / re-registration. */
   const char *sql =
       "INSERT INTO kb_enrollments (scope, fingerprint, serial, expires_at, legacy) "
       "VALUES (?1, ?2, ?3, ?4, ?5) "
       "ON CONFLICT (fingerprint) DO UPDATE SET scope=EXCLUDED.scope, serial=EXCLUDED.serial, "
       "expires_at=EXCLUDED.expires_at WHERE kb_enrollments.revoked_at='' RETURNING id";
   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", scope ? scope : "");
   aimee_pg_bind_text(st, "?2", fingerprint);
   aimee_pg_bind_text(st, "?3", serial ? serial : "");
   aimee_pg_bind_text(st, "?4", expires_at ? expires_at : "");
   aimee_pg_bind_int64(st, "?5", legacy ? 1 : 0);
   aimee_pg_step_t step = aimee_pg_step(st, err, sizeof(err));
   int64_t id = (step == AIMEE_PG_ROW) ? aimee_pg_column_int64(st, 0) : 0;
   aimee_pg_finalize(st);
   if (step != AIMEE_PG_ROW)
      return -1; /* e.g. conflict on an already-revoked fp: left revoked, no id */
   if (out_id)
      *out_id = id;
   return 0;
}

int db2_enrollment_list(int limit, db2_enrollment_row_t *out, int max)
{
   if (!out || max <= 0)
      return -1;
   void *conn = db2_conn();
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

int db2_enrollment_revoke(int64_t id, db2_enrollment_row_t *out)
{
   void *conn = db2_conn();
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
   db2_enrollment_row_t row;
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

void db2_enrollment_cache_flush(void)
{
   pthread_mutex_lock(&g_revcache_lock);
   memset(g_revcache, 0, sizeof(g_revcache));
   pthread_mutex_unlock(&g_revcache_lock);
}

static int revcache_get(const char *fp, int *revoked)
{
   time_t now = time(NULL);
   pthread_mutex_lock(&g_revcache_lock);
   for (int i = 0; i < REVCACHE_MAX; i++)
   {
      if (g_revcache[i].fp[0] && strcmp(g_revcache[i].fp, fp) == 0)
      {
         if (now - g_revcache[i].at <= REVCACHE_TTL_SECS)
         {
            *revoked = g_revcache[i].revoked;
            pthread_mutex_unlock(&g_revcache_lock);
            return 1;
         }
         g_revcache[i].fp[0] = '\0'; /* expired */
         break;
      }
   }
   pthread_mutex_unlock(&g_revcache_lock);
   return 0;
}

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

/* Throttled warning when the revocation check fails open (at most once/60s), so
 * an outage-induced revocation gap is visible without flooding the log. */
static void revcache_warn_failopen(const char *fingerprint, const char *why)
{
   static time_t last = 0;
   time_t now = time(NULL);
   if (now - last < 60)
      return;
   last = now;
   aimee_log(LOG_WARN, "kb_enroll",
             "revocation check FAILED OPEN (%s) for fingerprint %.12s — a revoked cert may be "
             "accepted until the DB recovers",
             why ? why : "?", fingerprint ? fingerprint : "");
}

int db2_enrollment_is_revoked(const char *fingerprint)
{
   if (!fingerprint || !fingerprint[0])
      return 0;
   int cached = 0;
   if (revcache_get(fingerprint, &cached))
      return cached;

   /* Fail-open decision (documented in docs/KB_CONSOLE.md): on a DB outage an
    * UNKNOWN cert is treated as not-revoked, trading a bounded revocation gap for
    * availability — fail-closed would let a DB blip lock out every mTLS client of
    * the shared kb. KNOWN revocations still hold through an outage because revoke
    * primes the cache with revoked=1 (checked above, before this DB path). The
    * fail-open is logged (throttled) so an operator can see it. */
   void *conn = db2_conn();
   if (!conn)
   {
      revcache_warn_failopen(fingerprint, "db unavailable");
      return 0;
   }
   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       conn, "SELECT state FROM kb_enrollments WHERE fingerprint=?1", err, sizeof(err));
   if (!st)
   {
      revcache_warn_failopen(fingerprint, err);
      return 0;
   }
   aimee_pg_bind_text(st, "?1", fingerprint);
   int revoked = 0;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      const char *state = aimee_pg_column_text(st, 0);
      revoked = (state && strcmp(state, "revoked") == 0) ? 1 : 0;
   }
   aimee_pg_finalize(st);
   revcache_put(fingerprint, revoked);
   return revoked;
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

void db2_enrollment_touch_last_seen(const char *fingerprint, const char *scope)
{
   if (!fingerprint || !fingerprint[0] || !seen_should_write(fingerprint))
      return;
   void *conn = db2_conn();
   if (!conn)
      return;
   /* Upsert: backfill a legacy row for a pre-S2a cert on first use, else bump
    * last_seen. The conflict path touches ONLY last_seen_at — never state or
    * revoked_at — so a revoked cert is not resurrected by being used. */
   char err[256] = "";
   aimee_pg_stmt_t *st =
       aimee_pg_prepare(conn,
                        "INSERT INTO kb_enrollments (scope, fingerprint, legacy, last_seen_at) "
                        "VALUES (?1, ?2, 1, pg_now_text()) "
                        "ON CONFLICT (fingerprint) DO UPDATE SET last_seen_at=pg_now_text()",
                        err, sizeof(err));
   if (!st)
      return;
   aimee_pg_bind_text(st, "?1", scope ? scope : "");
   aimee_pg_bind_text(st, "?2", fingerprint);
   aimee_pg_step(st, err, sizeof(err));
   aimee_pg_finalize(st);
}
