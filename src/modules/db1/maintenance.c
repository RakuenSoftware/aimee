/* db1/maintenance.c: path-based DB1 file maintenance.
 *
 * Implements db1_default_path, db1_backup, db1_check, db1_recover, and
 * the integrity-check helper used by db1/diagnostics.c.  The
 * prepared-statement-cache helpers stay in db.c — they're a runtime
 * concern that operates on a live handle, not a path. */

#include "maintenance.h"

#include "aimee.h"
#include "aimee_home.h"
#include "platform_path.h"

#include <sqlite3.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

const char *db1_default_path(void)
{
   /* Thread-local: returned-pointer scratch. This is the db1_default_path hook
    * config_default_db1_path() delegates to, reached from config_load() on
    * several concurrent threads — a process-global static buffer is a data race
    * (see the matching fix in config.c / aimee_home.c). */
   static __thread char path[MAX_PATH_LEN];
   static __thread char cached_base[MAX_PATH_LEN];
   const char *base = aimee_home();
   if (!base)
      base = "/tmp/.config/aimee";

   if (path[0] && strcmp(cached_base, base) == 0)
      return path;

   snprintf(path, sizeof(path), "%s/aimee.db", base);
   snprintf(cached_base, sizeof(cached_base), "%s", base);
   return path;
}

int db1_backup(const char *db_path, const char *out_path)
{
   if (!db_path)
      db_path = db1_default_path();

   sqlite3 *src = NULL;
   if (sqlite3_open_v2(db_path, &src, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK)
   {
      if (src)
         sqlite3_close(src);
      return -1;
   }

   /* Generate output path if not provided */
   char auto_path[MAX_PATH_LEN];
   if (!out_path)
   {
      char ts[32];
      now_utc(ts, sizeof(ts));
      /* Replace colons and spaces for filesystem safety */
      for (char *p = ts; *p; p++)
         if (*p == ':' || *p == ' ')
            *p = '-';
      snprintf(auto_path, sizeof(auto_path), "%s.manual.%s", db_path, ts);
      out_path = auto_path;
   }

   sqlite3 *dst = NULL;
   if (sqlite3_open(out_path, &dst) != SQLITE_OK)
   {
      sqlite3_close(src);
      if (dst)
         sqlite3_close(dst);
      return -1;
   }

   sqlite3_backup *b = sqlite3_backup_init(dst, "main", src, "main");
   if (!b)
   {
      sqlite3_close(src);
      sqlite3_close(dst);
      return -1;
   }

   sqlite3_backup_step(b, -1);
   int rc = sqlite3_backup_finish(b);
   sqlite3_close(src);
   sqlite3_close(dst);

   if (rc != SQLITE_OK)
   {
      unlink(out_path);
      return -1;
   }

   chmod(out_path, 0600);
   fprintf(stderr, "backup saved to %s\n", out_path);
   return 0;
}

int db1_check(const char *db_path, int full)
{
   if (!db_path)
      db_path = db1_default_path();

   sqlite3 *db = NULL;
   if (sqlite3_open_v2(db_path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK)
   {
      fprintf(stderr, "db check: cannot open %s\n", db_path);
      if (db)
         sqlite3_close(db);
      return -1;
   }

   const char *pragma = full ? "PRAGMA integrity_check" : "PRAGMA quick_check";
   sqlite3_stmt *stmt = NULL;
   int ok = 1;

   if (sqlite3_prepare_v2(db, pragma, -1, &stmt, NULL) == SQLITE_OK)
   {
      while (sqlite3_step(stmt) == SQLITE_ROW)
      {
         const char *result = (const char *)sqlite3_column_text(stmt, 0);
         if (result && strcmp(result, "ok") != 0)
         {
            fprintf(stderr, "%s\n", result);
            ok = 0;
         }
      }
      sqlite3_finalize(stmt);
   }
   else
   {
      fprintf(stderr, "db check: failed to run %s\n", pragma);
      ok = 0;
   }

   sqlite3_close(db);

   if (ok)
      fprintf(stderr, "ok\n");

   return ok ? 0 : -1;
}

int db1_quick_check_sqlite(sqlite3 *db)
{
   sqlite3_stmt *stmt = NULL;
   int ok = 1;

   if (sqlite3_prepare_v2(db, "PRAGMA quick_check", -1, &stmt, NULL) == SQLITE_OK)
   {
      while (sqlite3_step(stmt) == SQLITE_ROW)
      {
         const char *result = (const char *)sqlite3_column_text(stmt, 0);
         if (result && strcmp(result, "ok") != 0)
         {
            ok = 0;
            break;
         }
      }
      sqlite3_finalize(stmt);
   }
   else
      ok = 0;

   return ok ? 0 : -1;
}

int db1_recover(const char *db_path, int force)
{
   if (!db_path)
      db_path = db1_default_path();

   /* Search for backup files: .bak.N where N is highest first */
   char bak[MAX_PATH_LEN];
   int found_version = -1;

   for (int v = 999; v >= 0; v--)
   {
      snprintf(bak, sizeof(bak), "%s.bak.%d", db_path, v);
      struct stat st;
      if (stat(bak, &st) == 0)
      {
         /* Validate the backup with quick_check */
         sqlite3 *bdb = NULL;
         if (sqlite3_open_v2(bak, &bdb, SQLITE_OPEN_READONLY, NULL) == SQLITE_OK)
         {
            if (db1_quick_check_sqlite(bdb) == 0)
            {
               found_version = v;
               sqlite3_close(bdb);
               break;
            }
            sqlite3_close(bdb);
         }
      }
   }

   if (found_version < 0)
   {
      if (!force)
      {
         fprintf(stderr, "aimee: database corrupted and no valid backup found.\n"
                         "To start fresh (all memories, rules, and tasks will be lost):\n"
                         "  aimee db recover --force\n");
         return -1;
      }

      /* Force: remove the corrupted database so db1_init creates a fresh one */
      unlink(db_path);
      char wal[MAX_PATH_LEN], shm[MAX_PATH_LEN];
      snprintf(wal, sizeof(wal), "%s-wal", db_path);
      snprintf(shm, sizeof(shm), "%s-shm", db_path);
      unlink(wal);
      unlink(shm);
      fprintf(stderr, "aimee: created fresh database (previous data lost)\n");
      return 0;
   }

   /* Restore from backup using the backup API for atomicity */
   snprintf(bak, sizeof(bak), "%s.bak.%d", db_path, found_version);

   sqlite3 *src = NULL;
   if (sqlite3_open_v2(bak, &src, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK)
   {
      if (src)
         sqlite3_close(src);
      return -1;
   }

   /* Remove existing corrupted db first */
   unlink(db_path);
   char wal[MAX_PATH_LEN], shm[MAX_PATH_LEN];
   snprintf(wal, sizeof(wal), "%s-wal", db_path);
   snprintf(shm, sizeof(shm), "%s-shm", db_path);
   unlink(wal);
   unlink(shm);

   sqlite3 *dst = NULL;
   if (sqlite3_open(db_path, &dst) != SQLITE_OK)
   {
      sqlite3_close(src);
      if (dst)
         sqlite3_close(dst);
      return -1;
   }

   sqlite3_backup *b = sqlite3_backup_init(dst, "main", src, "main");
   if (!b)
   {
      sqlite3_close(src);
      sqlite3_close(dst);
      return -1;
   }

   sqlite3_backup_step(b, -1);
   int rc = sqlite3_backup_finish(b);
   sqlite3_close(src);
   sqlite3_close(dst);

   if (rc != SQLITE_OK)
      return -1;

   chmod(db_path, 0600);
   fprintf(stderr, "aimee: recovered from backup version %d\n", found_version);
   return 0;
}
