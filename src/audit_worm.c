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
#include "audit_worm.h"
#include "cJSON.h"
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

/* HMAC-SHA256 over wfe_sha256_raw (standard construction; the chain key is 32
 * bytes, below the 64-byte block size, so no key pre-hash is needed). */
static void worm_hmac_sha256(const unsigned char *key, size_t keylen, const unsigned char *msg,
                             size_t mlen, unsigned char mac[32])
{
   unsigned char k[64];
   memset(k, 0, sizeof k);
   if (keylen > 64)
   {
      unsigned char kh[32];
      wfe_sha256_raw(key, keylen, kh);
      memcpy(k, kh, 32);
   }
   else
      memcpy(k, key, keylen);
   unsigned char ipad[64], opad[64];
   for (int i = 0; i < 64; i++)
   {
      ipad[i] = k[i] ^ 0x36;
      opad[i] = k[i] ^ 0x5c;
   }
   unsigned char *ib = malloc(64 + mlen);
   unsigned char inner[32];
   if (!ib)
   {
      memset(mac, 0, 32);
      return;
   }
   memcpy(ib, ipad, 64);
   memcpy(ib + 64, msg, mlen);
   wfe_sha256_raw(ib, 64 + mlen, inner);
   free(ib);
   unsigned char ob[96];
   memcpy(ob, opad, 64);
   memcpy(ob + 64, inner, 32);
   wfe_sha256_raw(ob, 96, mac);
}

/* Load (creating from CSPRNG on first use) the dedicated chain/checkpoint key —
 * distinct from the args-hash .audit-key (R1-3). key_id = first 16 hex of
 * SHA256(key), so a rotated key surfaces a new id. Returns 0 on success. */
static int worm_chain_key_load(unsigned char key[32], char key_id[17])
{
   char path[1024];
   snprintf(path, sizeof path, "%s/.audit-chain-key", aimee_home());
   int fd = open(path, O_RDONLY);
   if (fd >= 0)
   {
      ssize_t n = read(fd, key, 32);
      close(fd);
      if (n != 32)
         return -1;
   }
   else
   {
      int rf = open("/dev/urandom", O_RDONLY);
      if (rf < 0)
         return -1;
      ssize_t n = read(rf, key, 32);
      close(rf);
      if (n != 32)
         return -1;
      int wf = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
      if (wf < 0)
      {
         /* Lost a create race: another writer made it — read theirs. */
         int r2 = open(path, O_RDONLY);
         if (r2 < 0)
            return -1;
         ssize_t m = read(r2, key, 32);
         close(r2);
         if (m != 32)
            return -1;
      }
      else
      {
         if (write(wf, key, 32) != 32)
         {
            close(wf);
            return -1;
         }
         close(wf);
      }
   }
   unsigned char kh[32];
   wfe_sha256_raw(key, 32, kh);
   static const char *h = "0123456789abcdef";
   for (int i = 0; i < 8; i++)
   {
      key_id[2 * i] = h[kh[i] >> 4];
      key_id[2 * i + 1] = h[kh[i] & 0x0f];
   }
   key_id[16] = '\0';
   return 0;
}

/* MAC a checkpoint commits over: the head (hash+seq) it attests, under key_id.
 * Length-prefixed like the row hash so fields are unambiguous. */
static void worm_ckpt_mac_hex(const unsigned char key[32], const char *head_hash,
                              long long head_seq, const char *key_id, char out_hex[65])
{
   char sb[32];
   snprintf(sb, sizeof sb, "%lld", head_seq);
   dstr_t m;
   dstr_init(&m);
   dstr_append_str(&m, AUDIT_WORM_DOMAIN "|ckpt|");
   const char *f[] = {head_hash, sb, key_id};
   for (int i = 0; i < 3; i++)
   {
      dstr_appendf(&m, "%zu:", strlen(f[i]));
      dstr_append_str(&m, f[i]);
   }
   unsigned char mac[32];
   worm_hmac_sha256(key, 32, (const unsigned char *)m.data, dstr_len(&m), mac);
   dstr_free(&m);
   hex32(mac, out_hex);
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

int audit_worm_checkpoint(void)
{
   unsigned char key[32];
   char key_id[17];
   if (worm_chain_key_load(key, key_id) != 0)
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
   worm_ckpt_mac_hex(key, prev, head_seq, key_id, mac_hex);
   char detail[256];
   snprintf(detail, sizeof detail, "{\"key_id\":\"%s\",\"mac\":\"%s\"}", key_id, mac_hex);

   char ts[32];
   time_t now = time(NULL);
   struct tm tmv;
   gmtime_r(&now, &tmv);
   strftime(ts, sizeof ts, "%Y-%m-%dT%H:%M:%SZ", &tmv);

   char row_hash[65];
   compute_row_hash(seq, "system", "", "chain.checkpoint", "", "ok", key_id, detail, prev,
                    row_hash);

   sqlite3_stmt *ins = NULL;
   if (sqlite3_prepare_v2(
           db,
           "INSERT INTO audit_event(seq, ts, actor_role, actor_principal, action, subject,"
           " verdict, detail, key_id, prev_hash, row_hash)"
           " VALUES(?,?,'system','','chain.checkpoint','','ok',?,?,?,?)",
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
   if (sqlite3_prepare_v2(db,
                          "SELECT seq, actor_role, actor_principal, action, subject, verdict,"
                          " key_id, detail, prev_hash, row_hash FROM audit_event ORDER BY seq ASC",
                          -1, &q, NULL) != SQLITE_OK)
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
      /* Checkpoint rows additionally carry a MAC over the head they attest; verify
       * it under the chain key so a forged/altered checkpoint is caught even if its
       * row_hash was recomputed. `prev` is this checkpoint's head_hash, seq-1 its
       * head_seq. */
      if (action && strcmp(action, "chain.checkpoint") == 0)
      {
         if (!ckey_loaded)
         {
            if (worm_chain_key_load(ckey, ckey_id) != 0)
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
         worm_ckpt_mac_hex(ckey, prev, seq - 1, ckey_id, got);
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
      aimee_log(LOG_ERROR, "audit_worm", "seal VACUUM INTO failed: %s", emsg ? emsg : "(nil)");
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
      aimee_log(LOG_WARN, "audit_worm", "sealed %s but could not open to set immutable flag", path);
   else if (imm == 1)
      aimee_log(LOG_WARN, "audit_worm",
                "sealed %s crypto-only (no CAP_LINUX_IMMUTABLE / unsupported FS)", path);
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
   }
   sqlite3_finalize(q);
   pthread_mutex_unlock(&g_worm_mu);
   if (head_seq)
      *head_seq = head;
   if (last_ckpt_seq)
      *last_ckpt_seq = ckpt;
   /* A checkpoint at seq C attests the head at seq C-1, so rows after C-1 are
    * unattested. Green only when a checkpoint covers the current head. */
   return (ckpt >= head) ? AUDIT_WORM_VERIFY_GREEN : AUDIT_WORM_VERIFY_AMBER;
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
   if (sqlite3_prepare_v2(g_worm_db,
                          "SELECT seq, ts, actor_role, actor_principal, action, subject, verdict,"
                          " detail, key_id FROM audit_event ORDER BY seq DESC LIMIT ? OFFSET ?",
                          -1, &q, NULL) == SQLITE_OK)
   {
      sqlite3_bind_int64(q, 1, limit);
      sqlite3_bind_int64(q, 2, offset);
      while (sqlite3_step(q) == SQLITE_ROW)
      {
         cJSON *o = cJSON_CreateObject();
         cJSON_AddNumberToObject(o, "seq", (double)sqlite3_column_int64(q, 0));
         const char *cols[] = {"ts",      "actor_role", "actor_principal", "action",
                               "subject", "verdict",    "detail",          "key_id"};
         for (int i = 0; i < 8; i++)
         {
            const unsigned char *v = sqlite3_column_text(q, i + 1);
            cJSON_AddStringToObject(o, cols[i], v ? (const char *)v : "");
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
