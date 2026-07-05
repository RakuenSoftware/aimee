/* audit_worm.c: per-service WORM audit store (SQLite) — S0. See audit_worm.h. */
#include <pthread.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "aimee_home.h"
#include "audit_worm.h"
#include "dstr.h"
#include "log.h"
#include "wfe_def.h" /* wfe_sha256_raw */

/* Single-writer serialization: every append allocates seq + chains + commits
 * under this mutex, so the chain is total-ordered and seq is gap-free. The cached
 * handle is opened with FULLMUTEX as a backstop, but writes never overlap. */
static pthread_mutex_t g_worm_mu = PTHREAD_MUTEX_INITIALIZER;
static sqlite3 *g_worm_db = NULL;

/* Tables + append-only WORM triggers. DB-level triggers stop bugs/casual edits;
 * they are NOT the adversarial guarantee (a process with file write access can
 * drop them) — that is the hash-chain (+ MAC checkpoints and OS-sealed segments
 * in later slices). PRAGMAs are applied separately (they return rows). */
static const char *WORM_SCHEMA_SQL =
    "CREATE TABLE IF NOT EXISTS audit_event ("
    "  seq INTEGER PRIMARY KEY,"
    "  ts TEXT NOT NULL,"
    "  actor_role TEXT NOT NULL,"
    "  actor_principal TEXT NOT NULL,"
    "  action TEXT NOT NULL,"
    "  subject TEXT NOT NULL,"
    "  verdict TEXT NOT NULL,"
    "  detail TEXT NOT NULL,"
    "  key_id TEXT NOT NULL DEFAULT '',"
    "  prev_hash TEXT NOT NULL,"
    "  row_hash TEXT NOT NULL);"
    "CREATE TRIGGER IF NOT EXISTS audit_event_no_update BEFORE UPDATE ON audit_event"
    "  BEGIN SELECT RAISE(ABORT, 'WORM: audit_event is append-only'); END;"
    "CREATE TRIGGER IF NOT EXISTS audit_event_no_delete BEFORE DELETE ON audit_event"
    "  BEGIN SELECT RAISE(ABORT, 'WORM: audit_event is append-only'); END;";

static void hex32(const unsigned char in[32], char out[65])
{
   static const char *h = "0123456789abcdef";
   for (int i = 0; i < 32; i++)
   {
      out[2 * i] = h[in[i] >> 4];
      out[2 * i + 1] = h[in[i] & 0x0f];
   }
   out[64] = '\0';
}

/* row_hash = SHA256( DOMAIN "\n" prev_hash "\n" <length-prefixed fixed fields> ).
 * ts is deliberately NOT hashed (advisory; seq is the sole ordering authority).
 * Length-prefixing (`<byte-len>:<bytes>` per field, fixed order) is injective, so
 * no field value can be confused with a delimiter. */
static void compute_row_hash(long long seq, const char *actor_role, const char *actor_principal,
                             const char *action, const char *subject, const char *verdict,
                             const char *key_id, const char *detail, const char *prev_hash,
                             char out_hex[65])
{
   char seqbuf[32];
   snprintf(seqbuf, sizeof seqbuf, "%lld", seq);
   const char *fields[] = {seqbuf,  actor_role, actor_principal, action,
                           subject, verdict,    key_id,          detail};
   dstr_t m;
   dstr_init(&m);
   dstr_append_str(&m, AUDIT_WORM_DOMAIN);
   dstr_append_char(&m, '\n');
   dstr_append_str(&m, prev_hash);
   dstr_append_char(&m, '\n');
   for (int i = 0; i < 8; i++)
   {
      const char *v = fields[i] ? fields[i] : "";
      dstr_appendf(&m, "%zu:", strlen(v));
      dstr_append_str(&m, v);
   }
   unsigned char dig[32];
   wfe_sha256_raw(m.data, dstr_len(&m), dig);
   dstr_free(&m);
   hex32(dig, out_hex);
}

/* Create every path component up to (not including) the final '/'. */
static void ensure_parent_dir(const char *path)
{
   char buf[1024];
   snprintf(buf, sizeof buf, "%s", path);
   char *slash = strrchr(buf, '/');
   if (!slash || slash == buf)
      return;
   *slash = '\0';
   for (char *p = buf + 1; *p; p++)
   {
      if (*p == '/')
      {
         *p = '\0';
         mkdir(buf, 0700);
         *p = '/';
      }
   }
   mkdir(buf, 0700);
}

/* Caller must hold g_worm_mu. Opens + schematizes the store if not already open. */
static int worm_open_locked(const char *db_path)
{
   if (g_worm_db)
      return 0;
   ensure_parent_dir(db_path);
   sqlite3 *db = NULL;
   int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX;
   if (sqlite3_open_v2(db_path, &db, flags, NULL) != SQLITE_OK)
   {
      aimee_log(LOG_ERROR, "audit_worm", "open failed: %s", db ? sqlite3_errmsg(db) : "(nil)");
      if (db)
         sqlite3_close(db);
      return -1;
   }
   sqlite3_busy_timeout(db, 15000);
   sqlite3_exec(db, "PRAGMA journal_mode=WAL", NULL, NULL, NULL);
   sqlite3_exec(db, "PRAGMA synchronous=FULL", NULL, NULL, NULL); /* fsync-durable commits */
   char *emsg = NULL;
   if (sqlite3_exec(db, WORM_SCHEMA_SQL, NULL, NULL, &emsg) != SQLITE_OK)
   {
      aimee_log(LOG_ERROR, "audit_worm", "schema apply failed: %s", emsg ? emsg : "(nil)");
      sqlite3_free(emsg);
      sqlite3_close(db);
      return -1;
   }
   g_worm_db = db;
   return 0;
}

/* Caller must hold g_worm_mu. Lazy-open at the default $AIMEE_HOME path. */
static int worm_open_locked_default(void)
{
   char path[1024];
   snprintf(path, sizeof path, "%s/audit/worm-live.db", aimee_home());
   return worm_open_locked(path);
}

int audit_worm_init(void)
{
   pthread_mutex_lock(&g_worm_mu);
   int rc = worm_open_locked_default();
   pthread_mutex_unlock(&g_worm_mu);
   return rc;
}

int audit_worm_init_at(const char *db_path)
{
   if (!db_path || !db_path[0])
      return -1;
   pthread_mutex_lock(&g_worm_mu);
   int rc = worm_open_locked(db_path);
   pthread_mutex_unlock(&g_worm_mu);
   return rc;
}

int audit_worm_append(const char *actor_role, const char *actor_principal, const char *action,
                      const char *subject, const char *verdict, const char *detail)
{
   if (!action || !subject || !verdict)
      return -1;
   actor_role = actor_role ? actor_role : "";
   actor_principal = actor_principal ? actor_principal : "";
   detail = detail ? detail : "";

   pthread_mutex_lock(&g_worm_mu);
   int rc = -1;
   if (!g_worm_db && worm_open_locked_default() != 0)
      goto done;
   sqlite3 *db = g_worm_db;

   if (sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK)
      goto done;

   /* Gap-free seq + chain head, read inside the write txn (single writer, so no
    * other appender can interleave). Genesis prev is 32 zero bytes (hex). */
   long long seq = 1;
   char prev[65];
   snprintf(prev, sizeof prev, "%s", AUDIT_WORM_GENESIS_PREV);
   sqlite3_stmt *q = NULL;
   if (sqlite3_prepare_v2(db, "SELECT seq, row_hash FROM audit_event ORDER BY seq DESC LIMIT 1", -1,
                          &q, NULL) == SQLITE_OK)
   {
      if (sqlite3_step(q) == SQLITE_ROW)
      {
         seq = sqlite3_column_int64(q, 0) + 1;
         const unsigned char *ph = sqlite3_column_text(q, 1);
         if (ph)
            snprintf(prev, sizeof prev, "%s", (const char *)ph);
      }
   }
   sqlite3_finalize(q);

   char ts[32];
   time_t now = time(NULL);
   struct tm tmv;
   gmtime_r(&now, &tmv);
   strftime(ts, sizeof ts, "%Y-%m-%dT%H:%M:%SZ", &tmv);

   char row_hash[65];
   compute_row_hash(seq, actor_role, actor_principal, action, subject, verdict, "", detail, prev,
                    row_hash);

   sqlite3_stmt *ins = NULL;
   if (sqlite3_prepare_v2(db,
                          "INSERT INTO audit_event(seq, ts, actor_role, actor_principal, action,"
                          " subject, verdict, detail, key_id, prev_hash, row_hash)"
                          " VALUES(?,?,?,?,?,?,?,?,'',?,?)",
                          -1, &ins, NULL) != SQLITE_OK)
   {
      sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
      goto done;
   }
   sqlite3_bind_int64(ins, 1, seq);
   sqlite3_bind_text(ins, 2, ts, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(ins, 3, actor_role, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(ins, 4, actor_principal, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(ins, 5, action, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(ins, 6, subject, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(ins, 7, verdict, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(ins, 8, detail, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(ins, 9, prev, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(ins, 10, row_hash, -1, SQLITE_TRANSIENT);
   int step = sqlite3_step(ins);
   sqlite3_finalize(ins);
   if (step != SQLITE_DONE)
   {
      sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
      goto done;
   }
   if (sqlite3_exec(db, "COMMIT", NULL, NULL, NULL) != SQLITE_OK)
   {
      sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
      goto done;
   }
   rc = 0;
done:
   pthread_mutex_unlock(&g_worm_mu);
   return rc;
}

int audit_worm_verify_chain(char *err, size_t errlen)
{
   if (err && errlen)
      err[0] = '\0';
   pthread_mutex_lock(&g_worm_mu);
   int rc = 0;
   if (!g_worm_db && worm_open_locked_default() != 0)
   {
      if (err)
         snprintf(err, errlen, "store not open");
      pthread_mutex_unlock(&g_worm_mu);
      return -1;
   }
   sqlite3_stmt *q = NULL;
   if (sqlite3_prepare_v2(g_worm_db,
                          "SELECT seq, actor_role, actor_principal, action, subject, verdict,"
                          " key_id, detail, prev_hash, row_hash FROM audit_event ORDER BY seq ASC",
                          -1, &q, NULL) != SQLITE_OK)
   {
      if (err)
         snprintf(err, errlen, "query prepare failed");
      pthread_mutex_unlock(&g_worm_mu);
      return -1;
   }
   char prev[65];
   snprintf(prev, sizeof prev, "%s", AUDIT_WORM_GENESIS_PREV);
   long long expect = 1;
   while (sqlite3_step(q) == SQLITE_ROW)
   {
      long long seq = sqlite3_column_int64(q, 0);
      const char *role = (const char *)sqlite3_column_text(q, 1);
      const char *principal = (const char *)sqlite3_column_text(q, 2);
      const char *action = (const char *)sqlite3_column_text(q, 3);
      const char *subject = (const char *)sqlite3_column_text(q, 4);
      const char *verdict = (const char *)sqlite3_column_text(q, 5);
      const char *key_id = (const char *)sqlite3_column_text(q, 6);
      const char *detail = (const char *)sqlite3_column_text(q, 7);
      const char *stored_prev = (const char *)sqlite3_column_text(q, 8);
      const char *stored_row = (const char *)sqlite3_column_text(q, 9);
      if (seq != expect)
      {
         if (err)
            snprintf(err, errlen, "seq gap: expected %lld, got %lld", expect, seq);
         rc = -1;
         break;
      }
      if (!stored_prev || strcmp(stored_prev, prev) != 0)
      {
         if (err)
            snprintf(err, errlen, "prev_hash break at seq %lld", seq);
         rc = -1;
         break;
      }
      char rh[65];
      compute_row_hash(seq, role ? role : "", principal ? principal : "", action ? action : "",
                       subject ? subject : "", verdict ? verdict : "", key_id ? key_id : "",
                       detail ? detail : "", prev, rh);
      if (!stored_row || strcmp(rh, stored_row) != 0)
      {
         if (err)
            snprintf(err, errlen, "row_hash mismatch at seq %lld (tampered)", seq);
         rc = -1;
         break;
      }
      snprintf(prev, sizeof prev, "%s", stored_row);
      expect = seq + 1;
   }
   sqlite3_finalize(q);
   pthread_mutex_unlock(&g_worm_mu);
   return rc;
}

long audit_worm_count(void)
{
   pthread_mutex_lock(&g_worm_mu);
   if (!g_worm_db && worm_open_locked_default() != 0)
   {
      pthread_mutex_unlock(&g_worm_mu);
      return -1;
   }
   long n = -1;
   sqlite3_stmt *q = NULL;
   if (sqlite3_prepare_v2(g_worm_db, "SELECT COUNT(*) FROM audit_event", -1, &q, NULL) == SQLITE_OK)
   {
      if (sqlite3_step(q) == SQLITE_ROW)
         n = (long)sqlite3_column_int64(q, 0);
   }
   sqlite3_finalize(q);
   pthread_mutex_unlock(&g_worm_mu);
   return n;
}

void audit_worm_close(void)
{
   pthread_mutex_lock(&g_worm_mu);
   if (g_worm_db)
   {
      sqlite3_close(g_worm_db);
      g_worm_db = NULL;
   }
   pthread_mutex_unlock(&g_worm_mu);
}
