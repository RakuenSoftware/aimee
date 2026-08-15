/* db1/fsnap.c: file-snapshot checkpoints for session rewind — SQLite impl.
 *
 * Schema:
 *   file_snapshots(id, session_id, turn, label, created_at)
 *   file_snapshot_entries(snapshot_id, path, existed, content BLOB)
 *     existed=0 marks a file that was absent at snapshot time; restore
 *     will delete it.
 *
 * Disk I/O is hosted here alongside the SQL; read_file_bytes and
 * ensure_parent_dir are private to this TU. */

#include "fsnap.h"
#include "db1_internal.h"
#include "log.h"
#include "platform_path.h"

#include <sqlite3.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Read full contents of `path` into a malloc'd buffer. *out_len receives
 * the byte count. Returns 0 on success, 1 if the file does not exist,
 * -1 on other errors. On success the caller must free *out. */
static int read_file_bytes(const char *path, unsigned char **out, size_t *out_len)
{
   *out = NULL;
   *out_len = 0;
   struct stat st;
   if (stat(path, &st) != 0)
   {
      if (errno == ENOENT)
         return 1;
      return -1;
   }
   if (!S_ISREG(st.st_mode))
      return -1;
   FILE *f = fopen(path, "rb");
   if (!f)
      return -1;
   size_t cap = (size_t)st.st_size;
   unsigned char *buf = (cap > 0) ? (unsigned char *)malloc(cap) : NULL;
   if (cap > 0 && !buf)
   {
      fclose(f);
      return -1;
   }
   size_t got = 0;
   while (got < cap)
   {
      size_t n = fread(buf + got, 1, cap - got, f);
      if (n == 0)
         break;
      got += n;
   }
   fclose(f);
   *out = buf;
   *out_len = got;
   return 0;
}

static int ensure_parent_dir(const char *path)
{
   char tmp[1024];
   snprintf(tmp, sizeof(tmp), "%s", path);
   char *slash = strrchr(tmp, '/');
   if (!slash || slash == tmp)
      return 0;
   *slash = '\0';
   return platform_mkdir_p(tmp, 0755);
}

int64_t db1_fsnap_create(const char *session_id, int turn, const char *label)
{
   if (!session_id)
      return -1;
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;

   sqlite3_stmt *stmt = NULL;
   if (sqlite3_prepare_v2(db,
                          "INSERT INTO file_snapshots(session_id, turn, label, created_at) "
                          "VALUES(?, ?, ?, datetime('now'))",
                          -1, &stmt, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_text(stmt, 1, session_id, -1, SQLITE_STATIC);
   sqlite3_bind_int(stmt, 2, turn);
   sqlite3_bind_text(stmt, 3, label ? label : "", -1, SQLITE_STATIC);
   int rc = sqlite3_step(stmt);
   sqlite3_finalize(stmt);
   if (rc != SQLITE_DONE)
      return -1;
   return (int64_t)sqlite3_last_insert_rowid(db);
}

int db1_fsnap_record_file(int64_t snap_id, const char *path)
{
   if (snap_id <= 0 || !path || !path[0])
      return -1;
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;

   unsigned char *content = NULL;
   size_t clen = 0;
   int rrc = read_file_bytes(path, &content, &clen);
   if (rrc < 0)
      return -1;
   int existed = (rrc == 0) ? 1 : 0;

   sqlite3_stmt *del = NULL;
   if (sqlite3_prepare_v2(db,
                          "DELETE FROM file_snapshot_entries WHERE snapshot_id = ? AND path = ?",
                          -1, &del, NULL) == SQLITE_OK)
   {
      sqlite3_bind_int64(del, 1, snap_id);
      sqlite3_bind_text(del, 2, path, -1, SQLITE_STATIC);
      sqlite3_step(del);
      sqlite3_finalize(del);
   }

   sqlite3_stmt *ins = NULL;
   if (sqlite3_prepare_v2(db,
                          "INSERT INTO file_snapshot_entries(snapshot_id, path, existed, content) "
                          "VALUES(?, ?, ?, ?)",
                          -1, &ins, NULL) != SQLITE_OK)
   {
      free(content);
      return -1;
   }
   sqlite3_bind_int64(ins, 1, snap_id);
   sqlite3_bind_text(ins, 2, path, -1, SQLITE_STATIC);
   sqlite3_bind_int(ins, 3, existed);
   if (existed)
      sqlite3_bind_blob(ins, 4, content, (int)clen, SQLITE_STATIC);
   else
      sqlite3_bind_null(ins, 4);
   int rc = sqlite3_step(ins);
   sqlite3_finalize(ins);
   free(content);
   return (rc == SQLITE_DONE) ? 0 : -1;
}

int64_t db1_fsnap_get_or_create(const char *session_id, int turn, const char *label)
{
   if (!session_id)
      return -1;
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;

   sqlite3_stmt *sel = NULL;
   if (sqlite3_prepare_v2(db,
                          "SELECT id FROM file_snapshots WHERE session_id = ? AND turn = ? "
                          "AND label = ? ORDER BY id DESC LIMIT 1",
                          -1, &sel, NULL) != SQLITE_OK)
      return db1_fsnap_create(session_id, turn, label);

   sqlite3_bind_text(sel, 1, session_id, -1, SQLITE_STATIC);
   sqlite3_bind_int(sel, 2, turn);
   sqlite3_bind_text(sel, 3, label ? label : "", -1, SQLITE_STATIC);
   int64_t existing = -1;
   if (sqlite3_step(sel) == SQLITE_ROW)
      existing = sqlite3_column_int64(sel, 0);
   sqlite3_finalize(sel);
   if (existing > 0)
      return existing;
   return db1_fsnap_create(session_id, turn, label);
}

int db1_fsnap_prune(const char *session_id, int keep)
{
   if (!session_id || keep < 0)
      return -1;
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;

   sqlite3_stmt *cnt = NULL;
   if (sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM file_snapshots WHERE session_id = ?", -1, &cnt,
                          NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_text(cnt, 1, session_id, -1, SQLITE_STATIC);
   int total = 0;
   if (sqlite3_step(cnt) == SQLITE_ROW)
      total = sqlite3_column_int(cnt, 0);
   sqlite3_finalize(cnt);
   if (total <= keep)
      return 0;
   int to_prune = total - keep;

   sqlite3_stmt *del = NULL;
   if (sqlite3_prepare_v2(db,
                          "DELETE FROM file_snapshots WHERE id IN ("
                          "  SELECT id FROM file_snapshots WHERE session_id = ? "
                          "  ORDER BY id ASC LIMIT ?)",
                          -1, &del, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_text(del, 1, session_id, -1, SQLITE_STATIC);
   sqlite3_bind_int(del, 2, to_prune);
   int rc = sqlite3_step(del);
   sqlite3_finalize(del);
   return (rc == SQLITE_DONE) ? to_prune : -1;
}

int db1_fsnap_list(const char *session_id, fsnap_info_t *out, int max)
{
   if (!session_id || !out || max <= 0)
      return -1;
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;

   sqlite3_stmt *stmt = NULL;
   if (sqlite3_prepare_v2(
           db,
           "SELECT s.id, s.turn, s.session_id, s.created_at, s.label, "
           "       (SELECT COUNT(*) FROM file_snapshot_entries e WHERE e.snapshot_id = s.id) "
           "FROM file_snapshots s WHERE s.session_id = ? "
           "ORDER BY s.id DESC LIMIT ?",
           -1, &stmt, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_text(stmt, 1, session_id, -1, SQLITE_STATIC);
   sqlite3_bind_int(stmt, 2, max);
   int n = 0;
   while (n < max && sqlite3_step(stmt) == SQLITE_ROW)
   {
      fsnap_info_t *r = &out[n];
      memset(r, 0, sizeof(*r));
      r->id = sqlite3_column_int64(stmt, 0);
      r->turn = sqlite3_column_int(stmt, 1);
      const unsigned char *sid = sqlite3_column_text(stmt, 2);
      const unsigned char *ts = sqlite3_column_text(stmt, 3);
      const unsigned char *lb = sqlite3_column_text(stmt, 4);
      snprintf(r->session_id, sizeof(r->session_id), "%s", sid ? (const char *)sid : "");
      snprintf(r->created_at, sizeof(r->created_at), "%s", ts ? (const char *)ts : "");
      snprintf(r->label, sizeof(r->label), "%s", lb ? (const char *)lb : "");
      r->file_count = sqlite3_column_int(stmt, 5);
      n++;
   }
   sqlite3_finalize(stmt);
   return n;
}

int db1_fsnap_get(int64_t snap_id, fsnap_info_t *out)
{
   if (snap_id <= 0 || !out)
      return -1;
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;

   sqlite3_stmt *stmt = NULL;
   if (sqlite3_prepare_v2(
           db,
           "SELECT s.id, s.turn, s.session_id, s.created_at, s.label, "
           "       (SELECT COUNT(*) FROM file_snapshot_entries e WHERE e.snapshot_id = s.id) "
           "FROM file_snapshots s WHERE s.id = ?",
           -1, &stmt, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_int64(stmt, 1, snap_id);
   int rc = sqlite3_step(stmt);
   int found = 0;
   if (rc == SQLITE_ROW)
   {
      memset(out, 0, sizeof(*out));
      out->id = sqlite3_column_int64(stmt, 0);
      out->turn = sqlite3_column_int(stmt, 1);
      const unsigned char *sid = sqlite3_column_text(stmt, 2);
      const unsigned char *ts = sqlite3_column_text(stmt, 3);
      const unsigned char *lb = sqlite3_column_text(stmt, 4);
      snprintf(out->session_id, sizeof(out->session_id), "%s", sid ? (const char *)sid : "");
      snprintf(out->created_at, sizeof(out->created_at), "%s", ts ? (const char *)ts : "");
      snprintf(out->label, sizeof(out->label), "%s", lb ? (const char *)lb : "");
      out->file_count = sqlite3_column_int(stmt, 5);
      found = 1;
   }
   sqlite3_finalize(stmt);
   return found ? 0 : -1;
}

int db1_fsnap_restore(int64_t snap_id, int *files_restored, int *files_deleted)
{
   if (snap_id <= 0)
      return -1;
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;

   sqlite3_stmt *stmt = NULL;
   if (sqlite3_prepare_v2(
           db, "SELECT path, existed, content FROM file_snapshot_entries WHERE snapshot_id = ?", -1,
           &stmt, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_int64(stmt, 1, snap_id);

   int restored = 0;
   int deleted = 0;
   int failed = 0;
   while (sqlite3_step(stmt) == SQLITE_ROW)
   {
      const unsigned char *p = sqlite3_column_text(stmt, 0);
      int existed = sqlite3_column_int(stmt, 1);
      const void *blob = sqlite3_column_blob(stmt, 2);
      int blob_len = sqlite3_column_bytes(stmt, 2);
      if (!p)
         continue;
      const char *path = (const char *)p;

      if (!existed)
      {
         if (unlink(path) == 0 || errno == ENOENT)
            deleted++;
         else
         {
            LOG_WARN("fsnap", "restore: unlink '%s' failed: %s", path, strerror(errno));
            failed++;
         }
         continue;
      }

      if (ensure_parent_dir(path) != 0)
      {
         LOG_WARN("fsnap", "restore: mkdir parent of '%s' failed", path);
         failed++;
         continue;
      }
      FILE *f = fopen(path, "wb");
      if (!f)
      {
         LOG_WARN("fsnap", "restore: open '%s' failed: %s", path, strerror(errno));
         failed++;
         continue;
      }
      size_t n = (blob && blob_len > 0) ? fwrite(blob, 1, (size_t)blob_len, f) : 0;
      int ok = (blob_len <= 0) || (n == (size_t)blob_len);
      fclose(f);
      if (ok)
         restored++;
      else
      {
         LOG_WARN("fsnap", "restore: short write to '%s'", path);
         failed++;
      }
   }
   sqlite3_finalize(stmt);

   if (files_restored)
      *files_restored = restored;
   if (files_deleted)
      *files_deleted = deleted;
   return failed == 0 ? 0 : -1;
}
