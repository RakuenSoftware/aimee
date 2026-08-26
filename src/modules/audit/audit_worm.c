/* audit_worm.c: per-service WORM audit store (SQLite). S0 = store + hash-chain +
 * triggers; S1 = dedicated chain key + MAC checkpoints + verify. See audit_worm.h. */
#include <fcntl.h>
#include <linux/fs.h> /* FS_IOC_GETFLAGS/SETFLAGS, FS_IMMUTABLE_FL */
#include <pthread.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "aimee_home.h"
#include <aimee/audit/audit_worm.h>
#include <aimee/audit/audit_worm_chain.h>
#include "cJSON.h"
#include "dstr.h"
#include "headers/aimee_sha256.h" /* aimee_sha256_raw */

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
    "  hash_version TEXT NOT NULL DEFAULT 'v1-partial',"
    "  actor_role TEXT NOT NULL,"
    "  actor_principal TEXT NOT NULL,"
    "  actor_issuer TEXT NOT NULL DEFAULT '',"
    "  actor_subject TEXT NOT NULL DEFAULT '',"
    "  transport_cn TEXT NOT NULL DEFAULT '',"
    "  team_id INTEGER NOT NULL DEFAULT 0,"
    "  selected_default_from TEXT NOT NULL DEFAULT '',"
    "  action TEXT NOT NULL,"
    "  subject TEXT NOT NULL,"
    "  verdict TEXT NOT NULL,"
    "  detail TEXT NOT NULL,"
    "  key_id TEXT NOT NULL DEFAULT '',"
    "  event_id TEXT NOT NULL DEFAULT '',"
    "  prev_hash TEXT NOT NULL,"
    "  row_hash TEXT NOT NULL);"
    "CREATE TRIGGER IF NOT EXISTS audit_event_no_update BEFORE UPDATE ON audit_event"
    "  BEGIN SELECT RAISE(ABORT, 'WORM: audit_event is append-only'); END;"
    "CREATE TRIGGER IF NOT EXISTS audit_event_no_delete BEFORE DELETE ON audit_event"
    "  BEGIN SELECT RAISE(ABORT, 'WORM: audit_event is append-only'); END;";

static int worm_has_column(sqlite3 *db, const char *table, const char *column)
{
   char sql[128];
   snprintf(sql, sizeof sql, "PRAGMA table_info(%s)", table);
   sqlite3_stmt *q = NULL;
   if (sqlite3_prepare_v2(db, sql, -1, &q, NULL) != SQLITE_OK)
      return 0;
   int found = 0;
   while (sqlite3_step(q) == SQLITE_ROW)
   {
      const char *name = (const char *)sqlite3_column_text(q, 1);
      if (name && strcmp(name, column) == 0)
      {
         found = 1;
         break;
      }
   }
   sqlite3_finalize(q);
   return found;
}

/* New rows use the v2 canonical record, which binds time and attribution.
 * Legacy v1 rows remain explicitly labelled v1-partial and use the old verifier.
 * The separately stored producer event ID occupies the canonical key-id binding
 * slot for idempotent outbox rows so rewriting it is also detectable. */

/* HMAC-SHA256 over wfe_sha256_raw (standard construction; the chain key is 32
 * bytes, below the 64-byte block size, so no key pre-hash is needed). */

/* Load (creating from CSPRNG on first use) the dedicated chain/checkpoint key —
 * distinct from the args-hash .audit-key (R1-3). key_id = first 16 hex of
 * SHA256(key), so a rotated key surfaces a new id. Returns 0 on success. */

/* MAC a checkpoint commits over: the head (hash+seq) it attests, under key_id.
 * Length-prefixed like the row hash so fields are unambiguous. */

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
      fprintf(stderr, "audit_worm: open failed: %s\n", db ? sqlite3_errmsg(db) : "(nil)");
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
      fprintf(stderr, "audit_worm: schema apply failed: %s\n", emsg ? emsg : "(nil)");
      sqlite3_free(emsg);
      sqlite3_close(db);
      return -1;
   }
   /* event_id is delivery metadata for strong retry idempotency. It is empty
    * for ordinary in-process appends, so existing stores migrate additively and
    * retain their byte-identical evidence-chain hashes. */
   if (!worm_has_column(db, "audit_event", "event_id") &&
       sqlite3_exec(db, "ALTER TABLE audit_event ADD COLUMN event_id TEXT NOT NULL DEFAULT ''",
                    NULL, NULL, &emsg) != SQLITE_OK)
   {
      fprintf(stderr, "audit_worm: event_id migration failed: %s\n", emsg ? emsg : "(nil)");
      sqlite3_free(emsg);
      sqlite3_close(db);
      return -1;
   }
   if (sqlite3_exec(db,
                    "CREATE UNIQUE INDEX IF NOT EXISTS audit_event_event_id"
                    " ON audit_event(event_id) WHERE event_id <> ''",
                    NULL, NULL, &emsg) != SQLITE_OK)
   {
      fprintf(stderr, "audit_worm: event_id index failed: %s\n", emsg ? emsg : "(nil)");
      sqlite3_free(emsg);
      sqlite3_close(db);
      return -1;
   }
   /* Online migration for pre-v2 stores. Duplicate-column errors are expected
    * on subsequent opens; no existing row is silently upgraded to full-field
    * evidence. */
   static const char *const v2_columns[] = {
       "ALTER TABLE audit_event ADD COLUMN hash_version TEXT NOT NULL DEFAULT 'v1-partial'",
       "ALTER TABLE audit_event ADD COLUMN actor_issuer TEXT NOT NULL DEFAULT ''",
       "ALTER TABLE audit_event ADD COLUMN actor_subject TEXT NOT NULL DEFAULT ''",
       "ALTER TABLE audit_event ADD COLUMN transport_cn TEXT NOT NULL DEFAULT ''",
       "ALTER TABLE audit_event ADD COLUMN team_id INTEGER NOT NULL DEFAULT 0",
       "ALTER TABLE audit_event ADD COLUMN selected_default_from TEXT NOT NULL DEFAULT ''"};
   for (size_t i = 0; i < sizeof(v2_columns) / sizeof(v2_columns[0]); i++)
      (void)sqlite3_exec(db, v2_columns[i], NULL, NULL, NULL);
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

int audit_worm_init_at(const char *db_path)
{
   if (!db_path || !db_path[0])
      return -1;
   pthread_mutex_lock(&g_worm_mu);
   int rc = worm_open_locked(db_path);
   pthread_mutex_unlock(&g_worm_mu);
   return rc;
}

static int worm_text_equal(sqlite3_stmt *q, int column, const char *want)
{
   const char *have = (const char *)sqlite3_column_text(q, column);
   return strcmp(have ? have : "", want ? want : "") == 0;
}

static int worm_append(const char *event_id, const char *event_ts, const char *actor_role,
                       const char *actor_principal, const char *action, const char *subject,
                       const char *verdict, const char *detail, long long *seq_out)
{
   if (!action || !subject || !verdict)
      return -1;
   actor_role = actor_role ? actor_role : "";
   actor_principal = actor_principal ? actor_principal : "";
   event_id = event_id ? event_id : "";
   detail = detail ? detail : "";
   if (seq_out)
      *seq_out = 0;

   /* Bound detail (R2-8): the store is immutable forever, so a runaway payload
    * would be permanent bloat. Cap at AUDIT_WORM_DETAIL_MAX with a visible marker
    * folded INTO the hashed value, so the truncation is itself tamper-evident.
    * Per-action allowlisted schemas + secret scanning are a follow-up. */
   char detail_capped[AUDIT_WORM_DETAIL_MAX + 96];
   size_t dlen = strlen(detail);
   if (dlen > AUDIT_WORM_DETAIL_MAX)
   {
      snprintf(detail_capped, sizeof detail_capped, "%.*s\"[worm-truncated %zu bytes]\"",
               AUDIT_WORM_DETAIL_MAX, detail, dlen - (size_t)AUDIT_WORM_DETAIL_MAX);
      detail = detail_capped;
   }

   pthread_mutex_lock(&g_worm_mu);
   int rc = -1;
   if (!g_worm_db && worm_open_locked_default() != 0)
      goto done;
   sqlite3 *db = g_worm_db;

   if (sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK)
      goto done;

   if (event_id[0])
   {
      sqlite3_stmt *existing = NULL;
      if (sqlite3_prepare_v2(
              db,
              "SELECT seq,ts,actor_role,actor_principal,action,subject,verdict,detail"
              " FROM audit_event WHERE event_id=?",
              -1, &existing, NULL) != SQLITE_OK)
      {
         sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
         goto done;
      }
      sqlite3_bind_text(existing, 1, event_id, -1, SQLITE_TRANSIENT);
      int found = sqlite3_step(existing);
      if (found == SQLITE_ROW)
      {
         long long prior_seq = sqlite3_column_int64(existing, 0);
         int same = worm_text_equal(existing, 1, event_ts) &&
                    worm_text_equal(existing, 2, actor_role) &&
                    worm_text_equal(existing, 3, actor_principal) &&
                    worm_text_equal(existing, 4, action) && worm_text_equal(existing, 5, subject) &&
                    worm_text_equal(existing, 6, verdict) && worm_text_equal(existing, 7, detail);
         sqlite3_finalize(existing);
         sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
         if (!same)
            goto done;
         if (seq_out)
            *seq_out = prior_seq;
         rc = 0;
         goto done;
      }
      sqlite3_finalize(existing);
      if (found != SQLITE_DONE)
      {
         sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
         goto done;
      }
   }

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

   char generated_ts[32];
   const char *ts = event_ts;
   if (!ts || !ts[0])
   {
      time_t now = time(NULL);
      struct tm tmv;
      gmtime_r(&now, &tmv);
      strftime(generated_ts, sizeof generated_ts, "%Y-%m-%dT%H:%M:%SZ", &tmv);
      ts = generated_ts;
   }

   char row_hash[65];
   audit_worm_row_hash_v2(seq, ts, actor_role, actor_principal, "", "", "", 0, "", action, subject,
                          verdict, event_id, detail, prev, row_hash);

   sqlite3_stmt *ins = NULL;
   if (sqlite3_prepare_v2(
           db,
           "INSERT INTO audit_event(seq, ts, hash_version, actor_role, actor_principal, action,"
           " subject, verdict, detail, key_id, event_id, prev_hash, row_hash)"
           " VALUES(?,?,'v2-full',?,?,?,?,?,?,'',?,?,?)",
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
   sqlite3_bind_text(ins, 9, event_id, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(ins, 10, prev, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(ins, 11, row_hash, -1, SQLITE_TRANSIENT);
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
   if (seq_out)
      *seq_out = seq;
   rc = 0;
done:
   pthread_mutex_unlock(&g_worm_mu);
   return rc;
}

int audit_worm_append(const char *actor_role, const char *actor_principal, const char *action,
                      const char *subject, const char *verdict, const char *detail)
{
   return worm_append("", NULL, actor_role, actor_principal, action, subject, verdict, detail,
                      NULL);
}

int audit_worm_append_idempotent(const char *event_id, const char *ts, const char *actor_role,
                                 const char *actor_principal, const char *action,
                                 const char *subject, const char *verdict, const char *detail,
                                 long long *seq_out)
{
   if (!event_id || !event_id[0] || !ts || !ts[0])
      return -1;
   return worm_append(event_id, ts, actor_role, actor_principal, action, subject, verdict, detail,
                      seq_out);
}

int audit_worm_checkpoint(void)
{
   unsigned char key[32];
   char key_id[17];
   if (audit_worm_chain_key_load(key, key_id) != 0)
      return -1;

   pthread_mutex_lock(&g_worm_mu);
   int rc = -1;
   if (!g_worm_db && worm_open_locked_default() != 0)
      goto done;
   sqlite3 *db = g_worm_db;

   if (sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK)
      goto done;

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

   /* The checkpoint (row `seq`) attests the current head = the last row before it
    * (`seq-1`, whose row_hash is `prev`). A genesis checkpoint (seq=1) commits the
    * empty head at seq 0. */
   long long head_seq = seq - 1;
   char mac_hex[65];
   audit_worm_ckpt_mac(key, prev, head_seq, key_id, mac_hex);
   char detail[256];
   snprintf(detail, sizeof detail, "{\"key_id\":\"%s\",\"mac\":\"%s\"}", key_id, mac_hex);

   char ts[32];
   time_t now = time(NULL);
   struct tm tmv;
   gmtime_r(&now, &tmv);
   strftime(ts, sizeof ts, "%Y-%m-%dT%H:%M:%SZ", &tmv);

   char row_hash[65];
   audit_worm_row_hash_v2(seq, ts, "system", "", "", "", "", 0, "", "chain.checkpoint", "", "ok",
                          key_id, detail, prev, row_hash);

   sqlite3_stmt *ins = NULL;
   if (sqlite3_prepare_v2(db,
                          "INSERT INTO audit_event(seq, ts, hash_version, actor_role, "
                          "actor_principal, action, subject,"
                          " verdict, detail, key_id, prev_hash, row_hash)"
                          " VALUES(?,?,'v2-full','system','','chain.checkpoint','','ok',?,?,?,?)",
                          -1, &ins, NULL) != SQLITE_OK)
   {
      sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
      goto done;
   }
   sqlite3_bind_int64(ins, 1, seq);
   sqlite3_bind_text(ins, 2, ts, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(ins, 3, detail, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(ins, 4, key_id, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(ins, 5, prev, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(ins, 6, row_hash, -1, SQLITE_TRANSIENT);
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

/* Extract the "mac" hex string from a checkpoint row's detail JSON into out
 * (>=65 bytes). Returns 0 on success. */
static int worm_ckpt_extract_mac(const char *detail, const char *want_key_id, char out[65])
{
   cJSON *root = detail ? cJSON_Parse(detail) : NULL;
   if (!root)
      return -1;
   int rc = -1;
   cJSON *mac = cJSON_GetObjectItemCaseSensitive(root, "mac");
   cJSON *kid = cJSON_GetObjectItemCaseSensitive(root, "key_id");
   if (cJSON_IsString(mac) && mac->valuestring && cJSON_IsString(kid) && kid->valuestring &&
       want_key_id && strcmp(kid->valuestring, want_key_id) == 0)
   {
      snprintf(out, 65, "%s", mac->valuestring);
      rc = 0;
   }
   cJSON_Delete(root);
   return rc;
}

/* Verify a WORM db handle — the live store OR a sealed snapshot — recomputing the
 * hash-chain and every checkpoint MAC. 0 if intact, -1 on the first break (reason
 * in err). No locking: the caller owns the handle. */
static int worm_verify_db(sqlite3 *db, char *err, size_t errlen)
{
   if (err && errlen)
      err[0] = '\0';
   sqlite3_stmt *q = NULL;
   const char *event_column = worm_has_column(db, "audit_event", "event_id") ? "event_id" : "''";
   char verify_sql[512];
   snprintf(verify_sql, sizeof(verify_sql),
            "SELECT seq,ts,hash_version,actor_role,actor_principal,actor_issuer,"
            " actor_subject,transport_cn,team_id,selected_default_from,action,subject,"
            " verdict,key_id,%s,detail,prev_hash,row_hash FROM audit_event ORDER BY seq ASC",
            event_column);
   if (sqlite3_prepare_v2(db, verify_sql, -1, &q, NULL) != SQLITE_OK)
   {
      if (err)
         snprintf(err, errlen, "query prepare failed");
      return -1;
   }
   int rc = 0;
   char prev[65];
   snprintf(prev, sizeof prev, "%s", AUDIT_WORM_GENESIS_PREV);
   long long expect = 1;
   unsigned char ckey[32];
   char ckey_id[17];
   int ckey_loaded = 0;
   int step_rc = SQLITE_OK;
   while ((step_rc = sqlite3_step(q)) == SQLITE_ROW)
   {
      long long seq = sqlite3_column_int64(q, 0);
      const char *ts = (const char *)sqlite3_column_text(q, 1);
      const char *version = (const char *)sqlite3_column_text(q, 2);
      const char *role = (const char *)sqlite3_column_text(q, 3);
      const char *principal = (const char *)sqlite3_column_text(q, 4);
      const char *issuer = (const char *)sqlite3_column_text(q, 5);
      const char *actor_subject = (const char *)sqlite3_column_text(q, 6);
      const char *transport = (const char *)sqlite3_column_text(q, 7);
      long long team_id = sqlite3_column_int64(q, 8);
      const char *selected_default = (const char *)sqlite3_column_text(q, 9);
      const char *action = (const char *)sqlite3_column_text(q, 10);
      const char *subject = (const char *)sqlite3_column_text(q, 11);
      const char *verdict = (const char *)sqlite3_column_text(q, 12);
      const char *key_id = (const char *)sqlite3_column_text(q, 13);
      const char *event_id = (const char *)sqlite3_column_text(q, 14);
      const char *detail = (const char *)sqlite3_column_text(q, 15);
      const char *stored_prev = (const char *)sqlite3_column_text(q, 16);
      const char *stored_row = (const char *)sqlite3_column_text(q, 17);
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
      const char *binding = event_id && event_id[0] ? event_id : (key_id ? key_id : "");
      if (version && strcmp(version, "v2-full") == 0)
         audit_worm_row_hash_v2(
             seq, ts ? ts : "", role ? role : "", principal ? principal : "", issuer ? issuer : "",
             actor_subject ? actor_subject : "", transport ? transport : "", team_id,
             selected_default ? selected_default : "", action ? action : "", subject ? subject : "",
             verdict ? verdict : "", binding, detail ? detail : "", prev, rh);
      else
         audit_worm_row_hash(seq, role ? role : "", principal ? principal : "",
                             action ? action : "", subject ? subject : "", verdict ? verdict : "",
                             binding, detail ? detail : "", prev, rh);
      if (!stored_row || strcmp(rh, stored_row) != 0)
      {
         if (err)
            snprintf(err, errlen, "row_hash mismatch at seq %lld (tampered)", seq);
         rc = -1;
         break;
      }
      /* Checkpoint rows additionally carry a MAC over the head they attest; verify
       * it under the chain key so a forged/altered checkpoint is caught even if its
       * row_hash was recomputed. `prev` is this checkpoint's head_hash, seq-1 its
       * head_seq. */
      if (action && strcmp(action, "chain.checkpoint") == 0)
      {
         if (!ckey_loaded)
         {
            if (audit_worm_chain_key_load(ckey, ckey_id) != 0)
            {
               if (err)
                  snprintf(err, errlen, "cannot load chain key to verify checkpoint at seq %lld",
                           seq);
               rc = -1;
               break;
            }
            ckey_loaded = 1;
         }
         char want[65];
         if (!key_id || strcmp(key_id, ckey_id) != 0 ||
             worm_ckpt_extract_mac(detail ? detail : "", ckey_id, want) != 0)
         {
            if (err)
               snprintf(err, errlen, "checkpoint at seq %lld has unknown/absent key or MAC", seq);
            rc = -1;
            break;
         }
         char got[65];
         audit_worm_ckpt_mac(ckey, prev, seq - 1, ckey_id, got);
         if (strcmp(got, want) != 0)
         {
            if (err)
               snprintf(err, errlen, "checkpoint MAC mismatch at seq %lld (forged)", seq);
            rc = -1;
            break;
         }
      }
      snprintf(prev, sizeof prev, "%s", stored_row);
      expect = seq + 1;
   }
   if (rc == 0 && step_rc != SQLITE_DONE)
   {
      if (err)
         snprintf(err, errlen, "query failed while verifying chain");
      rc = -1;
   }
   sqlite3_finalize(q);
   return rc;
}

int audit_worm_verify_chain(char *err, size_t errlen)
{
   if (err && errlen)
      err[0] = '\0';
   pthread_mutex_lock(&g_worm_mu);
   if (!g_worm_db && worm_open_locked_default() != 0)
   {
      if (err)
         snprintf(err, errlen, "store not open");
      pthread_mutex_unlock(&g_worm_mu);
      return -1;
   }
   int rc = worm_verify_db(g_worm_db, err, errlen);
   pthread_mutex_unlock(&g_worm_mu);
   return rc;
}

/* Verify a sealed snapshot file (read-only) with the same chain + MAC checks. */
int audit_worm_verify_file(const char *db_path, char *err, size_t errlen)
{
   if (err && errlen)
      err[0] = '\0';
   sqlite3 *db = NULL;
   if (sqlite3_open_v2(db_path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK)
   {
      if (err)
         snprintf(err, errlen, "cannot open sealed segment %s", db_path);
      if (db)
         sqlite3_close(db);
      return -1;
   }
   int rc = worm_verify_db(db, err, errlen);
   sqlite3_close(db);
   return rc;
}

/* Best-effort OS immutability for a sealed segment via the FS immutable flag.
 * Returns 0 if the file is now immutable, 1 if the flag could not be set
 * (unprivileged / unsupported FS) — the crypto chain + MAC remain the guarantee
 * (R2-7 degrade-to-crypto-only), -1 if the path can't be opened. */
static int worm_make_immutable(const char *path)
{
   int fd = open(path, O_RDONLY);
   if (fd < 0)
      return -1;
   int flags = 0;
   if (ioctl(fd, FS_IOC_GETFLAGS, &flags) < 0)
   {
      close(fd);
      return 1; /* FS doesn't support the flag */
   }
   flags |= FS_IMMUTABLE_FL;
   int rc = ioctl(fd, FS_IOC_SETFLAGS, &flags);
   close(fd);
   return rc < 0 ? 1 : 0; /* EPERM (no CAP_LINUX_IMMUTABLE) → degraded */
}

/* Seal an immutable point-in-time snapshot of the store: force a checkpoint (so
 * the snapshot's head is attested), VACUUM INTO a sealed file
 * audit/audit-sealed-<hi_seq>.db, then make it OS-immutable (best-effort). Fills
 * out_path (the sealed file) and *out_immutable (1 if kernel-immutable, 0 if
 * crypto-only). Returns 0 on a sealed+verifiable snapshot, -1 on failure. The
 * live store keeps accumulating; automatic rotation/pruning is a follow-up. */
int audit_worm_seal(char *out_path, size_t out_cap, int *out_immutable)
{
   audit_worm_checkpoint(); /* best-effort: attest the head being sealed */
   pthread_mutex_lock(&g_worm_mu);
   int rc = -1;
   char path[1024] = "";
   if (!g_worm_db && worm_open_locked_default() != 0)
      goto done;
   long long hi = 0;
   sqlite3_stmt *q = NULL;
   if (sqlite3_prepare_v2(g_worm_db, "SELECT COALESCE(MAX(seq),0) FROM audit_event", -1, &q,
                          NULL) == SQLITE_OK &&
       sqlite3_step(q) == SQLITE_ROW)
      hi = sqlite3_column_int64(q, 0);
   sqlite3_finalize(q);

   snprintf(path, sizeof path, "%s/audit/audit-sealed-%lld.db", aimee_home(), hi);
   ensure_parent_dir(path);
   unlink(
       path); /* replace a prior (mutable) seal at this head; a +i one blocks — seal then fails */
   char vsql[1200];
   snprintf(vsql, sizeof vsql, "VACUUM INTO '%s'", path);
   char *emsg = NULL;
   if (sqlite3_exec(g_worm_db, vsql, NULL, NULL, &emsg) != SQLITE_OK)
   {
      fprintf(stderr, "audit_worm: seal VACUUM INTO failed: %s\n", emsg ? emsg : "(nil)");
      sqlite3_free(emsg);
      goto done;
   }
   rc = 0;
done:
   pthread_mutex_unlock(&g_worm_mu);
   if (rc != 0)
      return -1;
   int imm = worm_make_immutable(path);
   if (imm < 0)
      fprintf(stderr, "audit_worm: sealed %s but could not set immutable flag\n", path);
   else if (imm == 1)
      fprintf(stderr,
              "audit_worm: sealed %s crypto-only (no CAP_LINUX_IMMUTABLE / unsupported FS)\n",
              path);
   if (out_path)
      snprintf(out_path, out_cap, "%s", path);
   if (out_immutable)
      *out_immutable = (imm == 0);
   return 0;
}

/* Full status verify: chain + checkpoint MACs, plus the amber "uncheckpointed
 * tail" signal. Returns AUDIT_WORM_VERIFY_GREEN (fully attested), _AMBER (chain
 * intact but head is beyond the newest checkpoint), or _RED (a break). head_seq /
 * last_ckpt_seq are filled when non-NULL. */
int audit_worm_verify(char *err, size_t errlen, long *head_seq, long *last_ckpt_seq)
{
   if (audit_worm_verify_chain(err, errlen) != 0)
      return AUDIT_WORM_VERIFY_RED;
   long head = 0, ckpt = 0;
   int query_ok = 0;
   pthread_mutex_lock(&g_worm_mu);
   sqlite3_stmt *q = NULL;
   if (sqlite3_prepare_v2(g_worm_db,
                          "SELECT COALESCE(MAX(seq),0),"
                          " COALESCE(MAX(CASE WHEN action='chain.checkpoint' THEN seq END),0)"
                          " FROM audit_event",
                          -1, &q, NULL) == SQLITE_OK &&
       sqlite3_step(q) == SQLITE_ROW)
   {
      head = (long)sqlite3_column_int64(q, 0);
      ckpt = (long)sqlite3_column_int64(q, 1);
      query_ok = 1;
   }
   sqlite3_finalize(q);
   pthread_mutex_unlock(&g_worm_mu);
   if (!query_ok)
   {
      if (err && errlen)
         snprintf(err, errlen, "cannot read WORM verification head");
      return AUDIT_WORM_VERIFY_RED;
   }
   if (head_seq)
      *head_seq = head;
   if (last_ckpt_seq)
      *last_ckpt_seq = ckpt;
   if (head == 0)
   {
      if (err && errlen)
         snprintf(err, errlen, "chain empty — nothing verified");
      return AUDIT_WORM_VERIFY_RED;
   }
   /* A checkpoint at seq C attests the head at seq C-1, so rows after C-1 are
    * unattested. Green only when a checkpoint covers the current head. */
   return (ckpt >= head) ? AUDIT_WORM_VERIFY_GREEN : AUDIT_WORM_VERIFY_AMBER;
}

int audit_worm_startup_verify(char *err, size_t errlen, long *head_seq, long *last_ckpt_seq)
{
   long head = 0, ckpt = 0;
   int status = audit_worm_verify(err, errlen, &head, &ckpt);
   if (status == AUDIT_WORM_VERIFY_RED)
      return -1;

   /* Record the exact non-empty state admitted at every process start. This also
    * repairs a legitimate unattested tail left by a crash after a durable append. */
   if (head > 0 && audit_worm_checkpoint() != 0)
   {
      if (err && errlen)
         snprintf(err, errlen, "startup checkpoint failed");
      return -1;
   }

   status = audit_worm_verify(err, errlen, &head, &ckpt);
   if (head_seq)
      *head_seq = head;
   if (last_ckpt_seq)
      *last_ckpt_seq = ckpt;
   if (status != AUDIT_WORM_VERIFY_GREEN)
   {
      if (err && errlen && !err[0])
         snprintf(err, errlen, "WORM store is not fully checkpoint-attested");
      return -1;
   }
   return 0;
}

cJSON *audit_worm_read_page(long offset, long limit, long *total)
{
   if (total)
      *total = 0;
   if (limit <= 0)
      limit = 500;
   if (offset < 0)
      offset = 0;
   cJSON *arr = cJSON_CreateArray();
   pthread_mutex_lock(&g_worm_mu);
   if (!g_worm_db && worm_open_locked_default() != 0)
   {
      pthread_mutex_unlock(&g_worm_mu);
      return arr;
   }
   if (total)
   {
      sqlite3_stmt *c = NULL;
      if (sqlite3_prepare_v2(g_worm_db, "SELECT COUNT(*) FROM audit_event", -1, &c, NULL) ==
              SQLITE_OK &&
          sqlite3_step(c) == SQLITE_ROW)
         *total = (long)sqlite3_column_int64(c, 0);
      sqlite3_finalize(c);
   }
   sqlite3_stmt *q = NULL;
   if (sqlite3_prepare_v2(
           g_worm_db,
           "SELECT seq,ts,hash_version,actor_role,actor_principal,actor_issuer,"
           " actor_subject,transport_cn,team_id,selected_default_from,action,subject,"
           " verdict,detail,key_id FROM audit_event ORDER BY seq DESC LIMIT ? OFFSET ?",
           -1, &q, NULL) == SQLITE_OK)
   {
      sqlite3_bind_int64(q, 1, limit);
      sqlite3_bind_int64(q, 2, offset);
      while (sqlite3_step(q) == SQLITE_ROW)
      {
         cJSON *o = cJSON_CreateObject();
         cJSON_AddNumberToObject(o, "seq", (double)sqlite3_column_int64(q, 0));
         const char *cols[] = {"ts",           "hash_version",  "actor_role",  "actor_principal",
                               "actor_issuer", "actor_subject", "transport_cn"};
         for (int i = 0; i < 7; i++)
         {
            const unsigned char *v = sqlite3_column_text(q, i + 1);
            cJSON_AddStringToObject(o, cols[i], v ? (const char *)v : "");
         }
         cJSON_AddNumberToObject(o, "team_id", (double)sqlite3_column_int64(q, 8));
         const char *tail_cols[] = {
             "selected_default_from", "action", "subject", "verdict", "detail", "key_id"};
         for (int i = 0; i < 6; i++)
         {
            const unsigned char *v = sqlite3_column_text(q, i + 9);
            cJSON_AddStringToObject(o, tail_cols[i], v ? (const char *)v : "");
         }
         cJSON_AddItemToArray(arr, o);
      }
   }
   sqlite3_finalize(q);
   pthread_mutex_unlock(&g_worm_mu);
   return arr;
}

int audit_worm_metric_snapshot(void)
{
   /* Compute a verdict-mix + total summary over the store, then append it as a
    * metric.snapshot row so the metrics history is itself hash-chained + verifiable
    * (a tamper-evident metrics-over-time record). Counts are read outside the append
    * txn, so a concurrent append may make them off-by-one — acceptable for a
    * periodic snapshot. */
   long total = 0, allow = 0, block = 0, rewrite = 0, approval = 0;
   pthread_mutex_lock(&g_worm_mu);
   if (!g_worm_db && worm_open_locked_default() != 0)
   {
      pthread_mutex_unlock(&g_worm_mu);
      return -1;
   }
   sqlite3_stmt *q = NULL;
   if (sqlite3_prepare_v2(g_worm_db, "SELECT verdict, COUNT(*) FROM audit_event GROUP BY verdict",
                          -1, &q, NULL) == SQLITE_OK)
   {
      while (sqlite3_step(q) == SQLITE_ROW)
      {
         const char *v = (const char *)sqlite3_column_text(q, 0);
         long n = (long)sqlite3_column_int64(q, 1);
         total += n;
         if (v && strcmp(v, "allow") == 0)
            allow = n;
         else if (v && strcmp(v, "block") == 0)
            block = n;
         else if (v && strcmp(v, "rewrite") == 0)
            rewrite = n;
         else if (v && strcmp(v, "approval_required") == 0)
            approval = n;
      }
   }
   sqlite3_finalize(q);
   pthread_mutex_unlock(&g_worm_mu);

   char detail[320];
   snprintf(detail, sizeof detail,
            "{\"total\":%ld,\"allow\":%ld,\"block\":%ld,\"rewrite\":%ld,\"approval_required\":%ld}",
            total, allow, block, rewrite, approval);
   return audit_worm_append("system", "", "metric.snapshot", "", "ok", detail);
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
