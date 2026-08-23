/* db1/approach_failures.c: approach-level negative knowledge, per machine. */

#include "approach_failures.h"
#include "db1_internal.h"

#include <sqlite3.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#define AF_COLUMNS                                                                                 \
   "id, goal_signature, goal_text, goal_tokens, approach_signature, approach_text,"                \
   " failure_mode, source, source_ref, occurrences, created_at, updated_at"

static void af_copy(char *dst, size_t dstsz, const unsigned char *src)
{
   snprintf(dst, dstsz, "%s", src ? (const char *)src : "");
}

static void af_load_row(sqlite3_stmt *stmt, db1_approach_failure_t *out)
{
   memset(out, 0, sizeof(*out));
   out->id = sqlite3_column_int64(stmt, 0);
   af_copy(out->goal_signature, sizeof(out->goal_signature), sqlite3_column_text(stmt, 1));
   af_copy(out->goal_text, sizeof(out->goal_text), sqlite3_column_text(stmt, 2));
   af_copy(out->goal_tokens, sizeof(out->goal_tokens), sqlite3_column_text(stmt, 3));
   af_copy(out->approach_signature, sizeof(out->approach_signature), sqlite3_column_text(stmt, 4));
   af_copy(out->approach_text, sizeof(out->approach_text), sqlite3_column_text(stmt, 5));
   af_copy(out->failure_mode, sizeof(out->failure_mode), sqlite3_column_text(stmt, 6));
   af_copy(out->source, sizeof(out->source), sqlite3_column_text(stmt, 7));
   af_copy(out->source_ref, sizeof(out->source_ref), sqlite3_column_text(stmt, 8));
   out->occurrences = sqlite3_column_int(stmt, 9);
   af_copy(out->created_at, sizeof(out->created_at), sqlite3_column_text(stmt, 10));
   af_copy(out->updated_at, sizeof(out->updated_at), sqlite3_column_text(stmt, 11));
}

int db1_approach_failure_record(const char *goal_signature, const char *goal_text,
                                const char *goal_tokens, const char *approach_signature,
                                const char *approach_text, const char *failure_mode,
                                const char *source, const char *source_ref)
{
   if (!goal_signature || !goal_signature[0] || !approach_signature || !approach_signature[0])
      return -1;
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;

   /* The (goal, approach) pair is the identity: meeting the same dead end
    * again is more evidence for one row, not a second row. */
   sqlite3_stmt *stmt = NULL;
   static const char *sql =
       "INSERT INTO approach_failures (goal_signature, goal_text, goal_tokens,"
       " approach_signature, approach_text, failure_mode, source, source_ref, occurrences)"
       " VALUES (?, ?, ?, ?, ?, ?, ?, ?, 1)"
       " ON CONFLICT (goal_signature, approach_signature) DO UPDATE SET"
       " occurrences = occurrences + 1, failure_mode = excluded.failure_mode,"
       " updated_at = datetime('now')";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_text(stmt, 1, goal_signature, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 2, goal_text ? goal_text : "", -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 3, goal_tokens ? goal_tokens : "", -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 4, approach_signature, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 5, approach_text ? approach_text : "", -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 6, failure_mode ? failure_mode : "", -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 7, source ? source : "", -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 8, source_ref ? source_ref : "", -1, SQLITE_TRANSIENT);
   int rc = sqlite3_step(stmt);
   sqlite3_finalize(stmt);
   return rc == SQLITE_DONE ? 0 : -1;
}

int db1_approach_failure_candidates(const char *must_contain, db1_approach_failure_t *out, int max)
{
   if (!out || max <= 0)
      return -1;
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;

   const char *needle = (must_contain && must_contain[0]) ? must_contain : "";
   char like[DB1_APPROACH_TOKENS_LEN + 8];
   snprintf(like, sizeof(like), "%%%s%%", needle);

   sqlite3_stmt *stmt = NULL;
   static const char *sql = "SELECT " AF_COLUMNS " FROM approach_failures"
                            " WHERE (? = '' OR goal_tokens LIKE ?)"
                            " ORDER BY occurrences DESC, updated_at DESC, id DESC LIMIT ?";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_text(stmt, 1, needle, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 2, like, -1, SQLITE_TRANSIENT);
   sqlite3_bind_int(stmt, 3, max);

   int n = 0;
   while (n < max && sqlite3_step(stmt) == SQLITE_ROW)
      af_load_row(stmt, &out[n++]);
   sqlite3_finalize(stmt);
   return n;
}
