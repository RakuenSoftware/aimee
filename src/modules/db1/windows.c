/* db1/windows.c: per-session conversation windows and local indexes. */

#include "db1_windows.h"
#include "db1_internal.h"

#include <sqlite3.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

static int build_placeholders(char *buf, size_t buf_sz, int count)
{
   int pos = 0;

   if (!buf || buf_sz == 0 || count <= 0)
      return -1;

   buf[0] = '\0';
   for (int i = 0; i < count; i++)
   {
      int wrote = snprintf(buf + pos, buf_sz - (size_t)pos, "%s?", i > 0 ? "," : "");
      if (wrote <= 0 || (size_t)wrote >= buf_sz - (size_t)pos)
         return -1;
      pos += wrote;
   }

   return 0;
}

int db1_windows_session_scan_state(const char *session_id, int *count_out, int *max_seq_out)
{
   if (!session_id || !count_out || !max_seq_out)
      return -1;
   *count_out = 0;
   *max_seq_out = 0;

   sqlite3 *db = db1_conn();
   if (!db)
      return -1;

   sqlite3_stmt *stmt = NULL;
   static const char *sql = "SELECT COUNT(*), COALESCE(MAX(seq), 0)"
                            " FROM windows WHERE session_id = ?";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_text(stmt, 1, session_id, -1, SQLITE_TRANSIENT);
   int rc = -1;
   if (sqlite3_step(stmt) == SQLITE_ROW)
   {
      *count_out = sqlite3_column_int(stmt, 0);
      *max_seq_out = sqlite3_column_int(stmt, 1);
      rc = 0;
   }
   sqlite3_finalize(stmt);
   return rc;
}

int db1_window_session_id(int64_t window_id, char *out, size_t out_sz)
{
   sqlite3 *db = NULL;
   sqlite3_stmt *stmt = NULL;
   int rc = 0;

   if (window_id <= 0 || !out || out_sz == 0)
      return -1;
   out[0] = '\0';

   db = db1_conn();
   if (!db)
      return -1;

   static const char *sql = "SELECT session_id FROM windows WHERE id = ?";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_int64(stmt, 1, window_id);

   if (sqlite3_step(stmt) == SQLITE_ROW)
   {
      const char *session_id = (const char *)sqlite3_column_text(stmt, 0);
      snprintf(out, out_sz, "%s", session_id ? session_id : "");
      rc = 1;
   }

   sqlite3_finalize(stmt);
   return rc;
}

int64_t db1_window_create_raw(const char *session_id, int seq, const char *summary,
                              const char *created_at)
{
   if (!session_id || !created_at)
      return -1;

   sqlite3 *db = db1_conn();
   if (!db)
      return -1;

   sqlite3_stmt *stmt = NULL;
   static const char *sql = "INSERT INTO windows (session_id, seq, summary, created_at, tier)"
                            " VALUES (?, ?, ?, ?, 'raw')";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_text(stmt, 1, session_id, -1, SQLITE_TRANSIENT);
   sqlite3_bind_int(stmt, 2, seq);
   sqlite3_bind_text(stmt, 3, summary ? summary : "", -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 4, created_at, -1, SQLITE_TRANSIENT);
   int rc = sqlite3_step(stmt);
   sqlite3_finalize(stmt);
   if (rc != SQLITE_DONE)
      return -1;
   return sqlite3_last_insert_rowid(db);
}

static int insert_child_row(const char *sql, int64_t window_id, const char *value)
{
   if (window_id <= 0 || !value)
      return -1;

   sqlite3 *db = db1_conn();
   if (!db)
      return -1;

   sqlite3_stmt *stmt = NULL;
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_int64(stmt, 1, window_id);
   sqlite3_bind_text(stmt, 2, value, -1, SQLITE_TRANSIENT);
   int rc = sqlite3_step(stmt);
   sqlite3_finalize(stmt);
   return (rc == SQLITE_DONE) ? 0 : -1;
}

int db1_window_add_term(int64_t window_id, const char *term)
{
   return insert_child_row("INSERT INTO window_terms (window_id, term) VALUES (?, ?)", window_id,
                           term);
}

int db1_window_add_file(int64_t window_id, const char *file_path)
{
   return insert_child_row("INSERT INTO window_files (window_id, file_path) VALUES (?, ?)",
                           window_id, file_path);
}

int db1_windows_list_ids_by_tier_before_days(const char *tier, int older_than_days,
                                             int64_t *out_ids, int max)
{
   if (!tier || !out_ids || max <= 0)
      return 0;

   sqlite3 *db = db1_conn();
   if (!db)
      return -1;

   sqlite3_stmt *stmt = NULL;
   static const char *sql = "SELECT id FROM windows"
                            " WHERE tier = ?"
                            " AND created_at < datetime('now', '-' || ? || ' days')"
                            " ORDER BY id LIMIT ?";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_text(stmt, 1, tier, -1, SQLITE_TRANSIENT);
   sqlite3_bind_int(stmt, 2, older_than_days);
   sqlite3_bind_int(stmt, 3, max);
   int n = 0;
   while (n < max && sqlite3_step(stmt) == SQLITE_ROW)
      out_ids[n++] = sqlite3_column_int64(stmt, 0);
   sqlite3_finalize(stmt);
   return n;
}

int db1_windows_find_candidates_by_terms(const char *const *terms, int term_count,
                                         db1_window_search_candidate_t *out, int max)
{
   sqlite3 *db = NULL;
   sqlite3_stmt *stmt = NULL;
   char placeholders[MAX_QUERY_LEN];
   char sql[MAX_QUERY_LEN];
   int count = 0;

   if (!terms || term_count <= 0 || !out || max <= 0)
      return 0;

   if (build_placeholders(placeholders, sizeof(placeholders), term_count) != 0)
      return -1;

   int wrote = snprintf(sql, sizeof(sql),
                        "SELECT w.id, w.session_id, w.seq, w.summary, w.created_at,"
                        " COUNT(DISTINCT wt.term) AS match_count"
                        " FROM windows w"
                        " JOIN window_terms wt ON wt.window_id = w.id"
                        " WHERE LOWER(wt.term) IN (%s)"
                        " GROUP BY w.id"
                        " ORDER BY match_count DESC, w.id"
                        " LIMIT ?",
                        placeholders);
   if (wrote <= 0 || (size_t)wrote >= sizeof(sql))
      return -1;

   db = db1_conn();
   if (!db)
      return -1;
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;

   for (int i = 0; i < term_count; i++)
      sqlite3_bind_text(stmt, i + 1, terms[i], -1, SQLITE_TRANSIENT);
   sqlite3_bind_int(stmt, term_count + 1, max);

   memset(out, 0, (size_t)max * sizeof(out[0]));
   while (count < max && sqlite3_step(stmt) == SQLITE_ROW)
   {
      db1_window_search_candidate_t *row = &out[count++];
      const char *session_id = (const char *)sqlite3_column_text(stmt, 1);
      const char *summary = (const char *)sqlite3_column_text(stmt, 3);
      const char *created_at = (const char *)sqlite3_column_text(stmt, 4);

      row->window_id = sqlite3_column_int64(stmt, 0);
      row->seq = sqlite3_column_int(stmt, 2);
      row->match_count = sqlite3_column_int(stmt, 5);
      snprintf(row->session_id, sizeof(row->session_id), "%s", session_id ? session_id : "");
      snprintf(row->summary, sizeof(row->summary), "%s", summary ? summary : "");
      snprintf(row->created_at, sizeof(row->created_at), "%s", created_at ? created_at : "");
   }

   sqlite3_finalize(stmt);
   return count;
}

int db1_window_list_files(int64_t window_id, char out[][MAX_PATH_LEN], int max)
{
   sqlite3 *db = NULL;
   sqlite3_stmt *stmt = NULL;
   int count = 0;

   if (window_id <= 0 || !out || max <= 0)
      return 0;

   db = db1_conn();
   if (!db)
      return -1;

   static const char *sql = "SELECT file_path FROM window_files"
                            " WHERE window_id = ?"
                            " ORDER BY rowid";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_int64(stmt, 1, window_id);

   while (count < max && sqlite3_step(stmt) == SQLITE_ROW)
   {
      const char *file_path = (const char *)sqlite3_column_text(stmt, 0);
      snprintf(out[count], MAX_PATH_LEN, "%s", file_path ? file_path : "");
      count++;
   }

   sqlite3_finalize(stmt);
   return count;
}

int db1_window_index_summary(int64_t window_id, const char *summary)
{
   sqlite3 *db = NULL;
   sqlite3_stmt *stmt = NULL;

   if (window_id <= 0 || !summary)
      return -1;

   db = db1_conn();
   if (!db)
      return -1;

   static const char *sql = "INSERT INTO window_fts (rowid, summary) VALUES (?, ?)";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_int64(stmt, 1, window_id);
   sqlite3_bind_text(stmt, 2, summary, -1, SQLITE_TRANSIENT);

   int rc = sqlite3_step(stmt);
   sqlite3_finalize(stmt);
   return (rc == SQLITE_DONE) ? 0 : -1;
}

static int db1_window_fts_available(void)
{
   sqlite3 *db = db1_conn();
   if (!db)
      return 0;
   sqlite3_stmt *stmt = NULL;
   static const char *sql =
       "SELECT 1 FROM sqlite_master WHERE type IN ('table','view') AND name='window_fts' LIMIT 1";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return 0;
   int hit = (sqlite3_step(stmt) == SQLITE_ROW) ? 1 : 0;
   sqlite3_finalize(stmt);
   return hit;
}

static int db1_window_fts_search_query(const char *query, db1_window_lexical_hit_t *out, int max)
{
   sqlite3 *db = NULL;
   sqlite3_stmt *stmt = NULL;
   int count = 0;

   if (!query || !query[0] || !out || max <= 0)
      return 0;

   db = db1_conn();
   if (!db)
      return -1;

   static const char *sql = "SELECT rowid, rank FROM window_fts WHERE window_fts MATCH ?";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_text(stmt, 1, query, -1, SQLITE_TRANSIENT);

   memset(out, 0, (size_t)max * sizeof(out[0]));
   while (count < max && sqlite3_step(stmt) == SQLITE_ROW)
   {
      out[count].window_id = sqlite3_column_int64(stmt, 0);
      out[count].rank = sqlite3_column_double(stmt, 1);
      count++;
   }

   sqlite3_finalize(stmt);
   return count;
}

static int db1_window_build_lexical_query(const char *const *terms, int term_count, char *out,
                                          size_t out_len)
{
   if (!terms || term_count <= 0 || !out || out_len == 0)
      return 0;

   out[0] = '\0';
   size_t pos = 0;
   for (int i = 0; i < term_count; i++)
   {
      if (!terms[i] || !terms[i][0])
         continue;
      if (pos >= out_len - 1)
         break;
      if (pos > 0)
      {
         int n = snprintf(out + pos, out_len - pos, " OR ");
         if (n <= 0 || pos + (size_t)n >= out_len)
            break;
         pos += (size_t)n;
      }
      int n = snprintf(out + pos, out_len - pos, "\"%s\"", terms[i]);
      if (n <= 0 || pos + (size_t)n >= out_len)
         break;
      pos += (size_t)n;
   }

   return out[0] ? 1 : 0;
}

int db1_windows_find_lexical_hits(const char *const *terms, int term_count,
                                  db1_window_lexical_hit_t *out, int max)
{
   if (!terms || term_count <= 0 || !out || max <= 0)
      return 0;
   if (!db1_window_fts_available())
      return 0;

   char query[MAX_QUERY_LEN];
   if (!db1_window_build_lexical_query(terms, term_count, query, sizeof(query)))
      return 0;

   return db1_window_fts_search_query(query, out, max);
}

int db1_window_set_tier(int64_t window_id, const char *tier)
{
   if (window_id <= 0 || !tier)
      return -1;

   sqlite3 *db = db1_conn();
   if (!db)
      return -1;

   sqlite3_stmt *stmt = NULL;
   static const char *sql = "UPDATE windows SET tier = ? WHERE id = ?";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_text(stmt, 1, tier, -1, SQLITE_TRANSIENT);
   sqlite3_bind_int64(stmt, 2, window_id);
   int rc = sqlite3_step(stmt);
   sqlite3_finalize(stmt);
   return (rc == SQLITE_DONE) ? 0 : -1;
}

int db1_window_prune_terms_keep_top(int64_t window_id, int keep)
{
   if (window_id <= 0 || keep <= 0)
      return -1;

   sqlite3 *db = db1_conn();
   if (!db)
      return -1;

   sqlite3_stmt *stmt = NULL;
   static const char *sql = "DELETE FROM window_terms WHERE window_id = ?"
                            " AND rowid NOT IN"
                            " (SELECT rowid FROM window_terms"
                            "  WHERE window_id = ?"
                            "  ORDER BY LENGTH(term) DESC, term LIMIT ?)";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_int64(stmt, 1, window_id);
   sqlite3_bind_int64(stmt, 2, window_id);
   sqlite3_bind_int(stmt, 3, keep);
   int rc = sqlite3_step(stmt);
   sqlite3_finalize(stmt);
   return (rc == SQLITE_DONE) ? 0 : -1;
}

int db1_window_delete_all_files(int64_t window_id)
{
   if (window_id <= 0)
      return -1;

   sqlite3 *db = db1_conn();
   if (!db)
      return -1;

   sqlite3_stmt *stmt = NULL;
   static const char *sql = "DELETE FROM window_files WHERE window_id = ?";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_int64(stmt, 1, window_id);
   int rc = sqlite3_step(stmt);
   sqlite3_finalize(stmt);
   return (rc == SQLITE_DONE) ? 0 : -1;
}

int db1_window_prune_files_keep_top(int64_t window_id, int keep)
{
   if (window_id <= 0 || keep <= 0)
      return -1;

   sqlite3 *db = db1_conn();
   if (!db)
      return -1;

   sqlite3_stmt *stmt = NULL;
   static const char *sql = "DELETE FROM window_files WHERE window_id = ?"
                            " AND rowid NOT IN"
                            " (SELECT rowid FROM window_files WHERE window_id = ?"
                            "  ORDER BY rowid LIMIT ?)";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_int64(stmt, 1, window_id);
   sqlite3_bind_int64(stmt, 2, window_id);
   sqlite3_bind_int(stmt, 3, keep);
   int rc = sqlite3_step(stmt);
   sqlite3_finalize(stmt);
   return (rc == SQLITE_DONE) ? 0 : -1;
}

int db1_windows_delete_after_turn(const char *session_id, int turn)
{
   if (!session_id)
      return -1;

   sqlite3 *db = db1_conn();
   if (!db)
      return -1;

   sqlite3_stmt *stmt = NULL;
   static const char *sql = "DELETE FROM windows WHERE session_id = ? AND seq > ?";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_text(stmt, 1, session_id, -1, SQLITE_TRANSIENT);
   sqlite3_bind_int(stmt, 2, turn);
   int rc = sqlite3_step(stmt);
   int changed = (rc == SQLITE_DONE) ? sqlite3_changes(db) : -1;
   sqlite3_finalize(stmt);
   return changed;
}
