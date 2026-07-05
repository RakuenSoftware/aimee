/* db1/harness_memory.c — see db1/harness_memory.h. SQL lives here; row mappers
 * use the db1_internal.h column helpers. */

#include "harness_memory.h"

#include "../harness_memory_common.h"
#include "aimee.h" /* now_utc */
#include "db1_internal.h"

#include <sqlite3.h>
#include <stdlib.h>
#include <string.h>

#define HMEM_COLS                                                                                  \
   "id, project, name, type, description, body, meta_json, content_hash, last_client, "            \
   "source_session, schema_version, deleted_at, created_at, updated_at"

int hmem_type_valid(const char *type)
{
   if (!type)
      return 0;
   return strcmp(type, "fact") == 0 || strcmp(type, "index") == 0 || strcmp(type, "note") == 0 ||
          strcmp(type, "scratch") == 0;
}

static void map_row(sqlite3_stmt *st, hmem_row_t *r)
{
   memset(r, 0, sizeof(*r));
   r->id = sqlite3_column_int64(st, 0);
   db1_copy_col_text(r->project, sizeof(r->project), st, 1);
   db1_copy_col_text(r->name, sizeof(r->name), st, 2);
   db1_copy_col_text(r->type, sizeof(r->type), st, 3);
   r->description = db1_dup_col_text(st, 4);
   r->body = db1_dup_col_text(st, 5);
   r->meta_json = db1_dup_col_text(st, 6);
   db1_copy_col_text(r->content_hash, sizeof(r->content_hash), st, 7);
   db1_copy_col_text(r->last_client, sizeof(r->last_client), st, 8);
   db1_copy_col_text(r->source_session, sizeof(r->source_session), st, 9);
   r->schema_version = sqlite3_column_int(st, 10);
   db1_copy_col_text(r->deleted_at, sizeof(r->deleted_at), st, 11);
   db1_copy_col_text(r->created_at, sizeof(r->created_at), st, 12);
   db1_copy_col_text(r->updated_at, sizeof(r->updated_at), st, 13);
}

void hmem_row_free_fields(hmem_row_t *row)
{
   if (!row)
      return;
   free(row->description);
   free(row->body);
   free(row->meta_json);
   row->description = row->body = row->meta_json = NULL;
}

void hmem_rows_free(hmem_row_t *rows, int n)
{
   if (!rows)
      return;
   for (int i = 0; i < n; i++)
      hmem_row_free_fields(&rows[i]);
   free(rows);
}

/* Does a live row already carry this hash? 1=yes, 0=no/absent. */
static int live_hash_matches(sqlite3 *db, const char *project, const char *name, const char *hash)
{
   sqlite3_stmt *st = NULL;
   if (sqlite3_prepare_v2(db,
                          "SELECT 1 FROM harness_memory WHERE project=? AND name=?"
                          " AND deleted_at IS NULL AND content_hash=?",
                          -1, &st, NULL) != SQLITE_OK)
      return 0;
   sqlite3_bind_text(st, 1, project, -1, SQLITE_STATIC);
   sqlite3_bind_text(st, 2, name, -1, SQLITE_STATIC);
   sqlite3_bind_text(st, 3, hash, -1, SQLITE_STATIC);
   int match = (sqlite3_step(st) == SQLITE_ROW);
   sqlite3_finalize(st);
   return match;
}

int hmem_upsert(const hmem_row_t *in, int64_t *out_id)
{
   if (!in || !in->project[0] || !in->name[0])
      return -1;
   const char *type = in->type[0] ? in->type : "fact";
   if (!hmem_type_valid(type))
      return -1;
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;

   const char *body = in->body ? in->body : "";
   const char *meta = (in->meta_json && in->meta_json[0]) ? in->meta_json : "{}";

   /* Always derive the hash from the values we are about to store — never trust
    * a caller-supplied in->content_hash, or a stale/wrong one could suppress a
    * real update via the no-op path below. */
   char hash[HMEM_HASH_LEN];
   if (hmem_content_hash(type, in->name, in->description, body, meta, hash) != 0)
      return -1;

   /* No-op when an existing live row already holds this content. */
   if (live_hash_matches(db, in->project, in->name, hash))
   {
      if (out_id)
      {
         sqlite3_stmt *q = NULL;
         if (sqlite3_prepare_v2(db, "SELECT id FROM harness_memory WHERE project=? AND name=?", -1,
                                &q, NULL) == SQLITE_OK)
         {
            sqlite3_bind_text(q, 1, in->project, -1, SQLITE_STATIC);
            sqlite3_bind_text(q, 2, in->name, -1, SQLITE_STATIC);
            if (sqlite3_step(q) == SQLITE_ROW)
               *out_id = sqlite3_column_int64(q, 0);
            sqlite3_finalize(q);
         }
      }
      return 0;
   }

   char ts[HMEM_TS_LEN];
   now_utc(ts, sizeof(ts));
   const char *client = in->last_client[0] ? in->last_client : "";
   const char *sess = in->source_session[0] ? in->source_session : "";
   int sv = in->schema_version > 0 ? in->schema_version : 1;

   static const char *sql =
       "INSERT INTO harness_memory"
       " (project,name,type,description,body,meta_json,content_hash,last_client,"
       "  source_session,schema_version,deleted_at,created_at,updated_at)"
       " VALUES (?,?,?,?,?,?,?,?,?,?,NULL,?,?)"
       " ON CONFLICT(project,name) DO UPDATE SET"
       "  type=excluded.type, description=excluded.description, body=excluded.body,"
       "  meta_json=excluded.meta_json, content_hash=excluded.content_hash,"
       "  last_client=excluded.last_client, source_session=excluded.source_session,"
       "  schema_version=excluded.schema_version, deleted_at=NULL,"
       "  updated_at=excluded.updated_at";
   sqlite3_stmt *st = NULL;
   if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_text(st, 1, in->project, -1, SQLITE_STATIC);
   sqlite3_bind_text(st, 2, in->name, -1, SQLITE_STATIC);
   sqlite3_bind_text(st, 3, type, -1, SQLITE_STATIC);
   /* description/body/meta_json may be caller-borrowed (e.g. cJSON-owned) and
    * only valid during this call — bind TRANSIENT so SQLite copies immediately,
    * decoupling row lifetime from the borrowed pointers. */
   if (in->description && in->description[0])
      sqlite3_bind_text(st, 4, in->description, -1, SQLITE_TRANSIENT);
   else
      sqlite3_bind_null(st, 4);
   sqlite3_bind_text(st, 5, body, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(st, 6, meta, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(st, 7, hash, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(st, 8, client, -1, SQLITE_STATIC);
   sqlite3_bind_text(st, 9, sess, -1, SQLITE_STATIC);
   sqlite3_bind_int(st, 10, sv);
   sqlite3_bind_text(st, 11, ts, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(st, 12, ts, -1, SQLITE_TRANSIENT);

   int rc = sqlite3_step(st);
   sqlite3_finalize(st);
   if (rc != SQLITE_DONE)
      return -1;

   if (out_id)
   {
      sqlite3_stmt *q = NULL;
      if (sqlite3_prepare_v2(db, "SELECT id FROM harness_memory WHERE project=? AND name=?", -1, &q,
                             NULL) == SQLITE_OK)
      {
         sqlite3_bind_text(q, 1, in->project, -1, SQLITE_STATIC);
         sqlite3_bind_text(q, 2, in->name, -1, SQLITE_STATIC);
         if (sqlite3_step(q) == SQLITE_ROW)
            *out_id = sqlite3_column_int64(q, 0);
         sqlite3_finalize(q);
      }
   }
   return 0;
}

int hmem_get(const char *project, const char *name, hmem_row_t *out)
{
   if (!project || !name || !out)
      return -1;
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;
   sqlite3_stmt *st = NULL;
   if (sqlite3_prepare_v2(db,
                          "SELECT " HMEM_COLS " FROM harness_memory"
                          " WHERE project=? AND name=? AND deleted_at IS NULL",
                          -1, &st, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_text(st, 1, project, -1, SQLITE_STATIC);
   sqlite3_bind_text(st, 2, name, -1, SQLITE_STATIC);
   int found = -1;
   if (sqlite3_step(st) == SQLITE_ROW)
   {
      map_row(st, out);
      found = 0;
   }
   sqlite3_finalize(st);
   return found;
}

static int collect(sqlite3_stmt *st, hmem_row_t **out, int *n)
{
   int cap = 0, count = 0;
   hmem_row_t *rows = NULL;
   while (sqlite3_step(st) == SQLITE_ROW)
   {
      if (count == cap)
      {
         int ncap = cap ? cap * 2 : 16;
         hmem_row_t *nr = realloc(rows, (size_t)ncap * sizeof(*nr));
         if (!nr)
         {
            hmem_rows_free(rows, count);
            return -1;
         }
         rows = nr;
         cap = ncap;
      }
      map_row(st, &rows[count]);
      count++;
   }
   *out = rows;
   *n = count;
   return 0;
}

int hmem_list(const char *project, hmem_row_t **out, int *n, int include_deleted)
{
   if (!project || !out || !n)
      return -1;
   *out = NULL;
   *n = 0;
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;
   const char *sql = include_deleted ? "SELECT " HMEM_COLS
                                       " FROM harness_memory WHERE project=? ORDER BY name"
                                     : "SELECT " HMEM_COLS " FROM harness_memory WHERE project=?"
                                       " AND deleted_at IS NULL ORDER BY name";
   sqlite3_stmt *st = NULL;
   if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_text(st, 1, project, -1, SQLITE_STATIC);
   int rc = collect(st, out, n);
   sqlite3_finalize(st);
   return rc;
}

int hmem_page_end(const hmem_row_t *rows, int n, int offset, size_t budget)
{
   if (!rows || n <= 0)
      return (offset < 0) ? 0 : (offset > n ? n : offset);
   if (offset < 0)
      offset = 0;
   size_t bytes = 0;
   int i = offset;
   for (; i < n; i++)
   {
      const hmem_row_t *r = &rows[i];
      /* Approximate the row's serialized JSON: the variable-length fields plus a
       * fixed slop for keys, the fixed-width columns, and punctuation. */
      size_t rowsz = (r->body ? strlen(r->body) : 0) +
                     (r->description ? strlen(r->description) : 0) +
                     (r->meta_json ? strlen(r->meta_json) : 0) + 512;
      if (i > offset && bytes + rowsz > budget)
         break; /* this row opens the next page */
      bytes += rowsz;
   }
   return i;
}

int hmem_search(const char *project, const char *query, hmem_row_t **out, int *n)
{
   if (!project || !query || !out || !n)
      return -1;
   *out = NULL;
   *n = 0;
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;
   sqlite3_stmt *st = NULL;
   if (sqlite3_prepare_v2(db,
                          "SELECT " HMEM_COLS " FROM harness_memory WHERE project=?"
                          " AND deleted_at IS NULL AND (name LIKE ? OR description LIKE ?"
                          " OR body LIKE ?) ORDER BY name",
                          -1, &st, NULL) != SQLITE_OK)
      return -1;
   char pat[512];
   snprintf(pat, sizeof(pat), "%%%s%%", query);
   sqlite3_bind_text(st, 1, project, -1, SQLITE_STATIC);
   sqlite3_bind_text(st, 2, pat, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(st, 3, pat, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(st, 4, pat, -1, SQLITE_TRANSIENT);
   int rc = collect(st, out, n);
   sqlite3_finalize(st);
   return rc;
}

int hmem_tombstone(const char *project, const char *name)
{
   if (!project || !name)
      return -1;
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;
   char ts[HMEM_TS_LEN];
   now_utc(ts, sizeof(ts));
   sqlite3_stmt *st = NULL;
   if (sqlite3_prepare_v2(db,
                          "UPDATE harness_memory SET deleted_at=? WHERE project=? AND name=?"
                          " AND deleted_at IS NULL",
                          -1, &st, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_text(st, 1, ts, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(st, 2, project, -1, SQLITE_STATIC);
   sqlite3_bind_text(st, 3, name, -1, SQLITE_STATIC);
   int rc = sqlite3_step(st);
   sqlite3_finalize(st);
   return (rc == SQLITE_DONE) ? 0 : -1;
}

int hmem_tombstone_prefix(const char *project, const char *dir)
{
   if (!project || !dir)
      return -1;
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;
   char ts[HMEM_TS_LEN];
   now_utc(ts, sizeof(ts));
   sqlite3_stmt *st = NULL;
   /* dir=="" → whole project; else exact dir or names under dir+slash */
   if (!dir[0])
   {
      if (sqlite3_prepare_v2(db,
                             "UPDATE harness_memory SET deleted_at=? WHERE project=?"
                             " AND deleted_at IS NULL",
                             -1, &st, NULL) != SQLITE_OK)
         return -1;
      sqlite3_bind_text(st, 1, ts, -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(st, 2, project, -1, SQLITE_STATIC);
   }
   else
   {
      /* Wildcard-free prefix match: substr() equality, NOT LIKE — a name char
       * like '_' is a LIKE metacharacter and would over-tombstone siblings. */
      if (sqlite3_prepare_v2(db,
                             "UPDATE harness_memory SET deleted_at=? WHERE project=?"
                             " AND deleted_at IS NULL AND (name=? OR substr(name,1,?)=?)",
                             -1, &st, NULL) != SQLITE_OK)
         return -1;
      char prefix[HMEM_NAME_LEN + 2];
      snprintf(prefix, sizeof(prefix), "%s/", dir);
      sqlite3_bind_text(st, 1, ts, -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(st, 2, project, -1, SQLITE_STATIC);
      sqlite3_bind_text(st, 3, dir, -1, SQLITE_STATIC);
      sqlite3_bind_int(st, 4, (int)strlen(prefix));
      sqlite3_bind_text(st, 5, prefix, -1, SQLITE_TRANSIENT);
   }
   int rc = sqlite3_step(st);
   int changes = sqlite3_changes(db);
   sqlite3_finalize(st);
   return (rc == SQLITE_DONE) ? changes : -1;
}
