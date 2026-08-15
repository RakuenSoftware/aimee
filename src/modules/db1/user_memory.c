/* user_memory.c: per-user structured memory store (db1). See user_memory.h. */
#include "user_memory.h"

#include "db1_internal.h"
#include "cJSON.h"
#include <sqlite3.h>
#include <string.h>

/* Selector SQL mirrors db2_memory_list_recall_section (db2/memory_score_fields.c):
 * identity  = tier IN (L2..L5) AND kind='fact' AND key LIKE identity/name/role/user/self;
 * preferences = tier IN (L2..L5) AND kind='preference'.
 * Same tier band + key conventions so db1 (user) and db2 (org) rows are
 * selected consistently and merge cleanly in memory_recall. */
int db1_user_memory_list_recall(db1_user_recall_section_t section, db1_user_memory_row_t *rows,
                                int cap)
{
   if (!rows || cap <= 0)
      return 0;
   sqlite3 *db = db1_conn();
   if (!db)
      return 0;

   const char *sql = NULL;
   if (section == DB1_USER_RECALL_IDENTITY)
      sql = "SELECT id, tier, kind, key, content FROM user_memories"
            " WHERE tier IN ('L2','L3','L4','L5') AND kind='fact'"
            " AND lifecycle_state='active'"
            " AND (key LIKE 'identity:%' OR key LIKE 'name:%' OR key LIKE 'role:%'"
            "      OR key LIKE 'user:%' OR key LIKE 'self:%')"
            " ORDER BY confidence DESC, id DESC LIMIT ?1";
   else if (section == DB1_USER_RECALL_PREFERENCES)
      sql = "SELECT id, tier, kind, key, content FROM user_memories"
            " WHERE tier IN ('L2','L3','L4','L5') AND kind='preference'"
            " AND lifecycle_state='active'"
            " ORDER BY confidence DESC, id DESC LIMIT ?1";
   else
      return 0;

   sqlite3_stmt *st = NULL;
   if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
      return 0;
   sqlite3_bind_int(st, 1, cap);

   int n = 0;
   while (n < cap && sqlite3_step(st) == SQLITE_ROW)
   {
      rows[n].id = sqlite3_column_int64(st, 0);
      db1_copy_col_text(rows[n].tier, sizeof(rows[n].tier), st, 1);
      db1_copy_col_text(rows[n].kind, sizeof(rows[n].kind), st, 2);
      db1_copy_col_text(rows[n].key, sizeof(rows[n].key), st, 3);
      db1_copy_col_text(rows[n].content, sizeof(rows[n].content), st, 4);
      n++;
   }
   sqlite3_finalize(st);
   return n;
}

void db1_user_memory_merge_into_array(cJSON *arr, db1_user_recall_section_t section,
                                      const char *why)
{
   if (!cJSON_IsArray(arr))
      return;
   db1_user_memory_row_t rows[32];
   int n = db1_user_memory_list_recall(section, rows, 32);
   /* Insert in reverse so the highest-ranked db1 row ends up first. */
   for (int i = n - 1; i >= 0; i--)
   {
      /* db1 wins: drop any org row with the same key. Index is collected inside
       * the loop and the delete runs AFTER it exits — never during iteration. */
      int idx = 0, rm = -1;
      cJSON *it = NULL;
      cJSON_ArrayForEach(it, arr)
      {
         const char *k = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(it, "key"));
         if (k && strcmp(k, rows[i].key) == 0)
         {
            rm = idx;
            break;
         }
         idx++;
      }
      if (rm >= 0)
         cJSON_DeleteItemFromArray(arr, rm);

      cJSON *row = cJSON_CreateObject();
      if (!row)
         break;
      cJSON_AddNumberToObject(row, "memory_id", (double)rows[i].id);
      cJSON_AddStringToObject(row, "tier", rows[i].tier);
      cJSON_AddStringToObject(row, "kind", rows[i].kind);
      cJSON_AddStringToObject(row, "key", rows[i].key);
      cJSON_AddStringToObject(row, "text", rows[i].content);
      cJSON_AddStringToObject(row, "why", why ? why : "");
      cJSON_AddStringToObject(row, "scope", "user");
      cJSON_InsertItemInArray(arr, 0, row);
   }
}

int db1_user_memory_any(void)
{
   sqlite3 *db = db1_conn();
   if (!db)
      return 0;
   sqlite3_stmt *st = NULL;
   if (sqlite3_prepare_v2(db, "SELECT 1 FROM user_memories LIMIT 1", -1, &st, NULL) != SQLITE_OK)
      return 0;
   int any = (sqlite3_step(st) == SQLITE_ROW);
   sqlite3_finalize(st);
   return any;
}

int db1_user_memory_upsert(const char *kind, const char *tier, const char *key, const char *content,
                           double confidence, const char *source_session)
{
   if (!kind || !kind[0] || !key || !key[0])
      return -1;
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;

   /* Insert-or-update by the UNIQUE(kind,key) constraint. */
   const char *sql =
       "INSERT INTO user_memories (kind, tier, key, content, confidence, source_session, "
       "updated_at)"
       " VALUES (?1, ?2, ?3, ?4, ?5, ?6, datetime('now'))"
       " ON CONFLICT(kind, key) DO UPDATE SET content=excluded.content, tier=excluded.tier,"
       " confidence=excluded.confidence, source_session=excluded.source_session,"
       " lifecycle_state='active', updated_at=datetime('now')";
   sqlite3_stmt *st = NULL;
   if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_text(st, 1, kind, -1, SQLITE_STATIC);
   sqlite3_bind_text(st, 2, (tier && tier[0]) ? tier : "L2", -1, SQLITE_STATIC);
   sqlite3_bind_text(st, 3, key, -1, SQLITE_STATIC);
   sqlite3_bind_text(st, 4, content ? content : "", -1, SQLITE_STATIC);
   sqlite3_bind_double(st, 5, confidence);
   sqlite3_bind_text(st, 6, source_session ? source_session : "", -1, SQLITE_STATIC);
   int rc = sqlite3_step(st);
   sqlite3_finalize(st);
   return (rc == SQLITE_DONE) ? 0 : -1;
}
