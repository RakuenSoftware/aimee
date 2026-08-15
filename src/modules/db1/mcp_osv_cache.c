/* mcp_osv_cache.c: DB1 storage for MCP OSV package verdicts. */
#include "db1_internal.h"
#include "interaction_events.h"
#include "mcp_osv_cache.h"

#include "cJSON.h"

#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *safe_text(const char *s)
{
   return s ? s : "";
}

static void row_from_stmt(sqlite3_stmt *st, db1_mcp_osv_cache_row_t *row)
{
   memset(row, 0, sizeof(*row));
   snprintf(row->ecosystem, sizeof(row->ecosystem), "%s",
            safe_text((const char *)sqlite3_column_text(st, 0)));
   snprintf(row->name, sizeof(row->name), "%s",
            safe_text((const char *)sqlite3_column_text(st, 1)));
   snprintf(row->version, sizeof(row->version), "%s",
            safe_text((const char *)sqlite3_column_text(st, 2)));
   snprintf(row->verdict, sizeof(row->verdict), "%s",
            safe_text((const char *)sqlite3_column_text(st, 3)));
   snprintf(row->advisory_ids, sizeof(row->advisory_ids), "%s",
            safe_text((const char *)sqlite3_column_text(st, 4)));
   row->checked_at = sqlite3_column_int64(st, 5);
   snprintf(row->checked_at_text, sizeof(row->checked_at_text), "%s",
            safe_text((const char *)sqlite3_column_text(st, 6)));
}

int db1_mcp_osv_cache_get(const char *ecosystem, const char *name, const char *version,
                          int ttl_hours, db1_mcp_osv_cache_row_t *out)
{
   sqlite3 *db = db1_conn();
   if (!db || !ecosystem || !ecosystem[0] || !name || !name[0] || !out)
      return -1;

   const char *sql = "SELECT ecosystem, name, version, verdict, advisory_ids, checked_at,"
                     " strftime('%Y-%m-%dT%H:%M:%SZ', checked_at, 'unixepoch')"
                     " FROM mcp_osv_cache"
                     " WHERE ecosystem = ?1 AND name = ?2 AND version = ?3"
                     " AND (?4 <= 0 OR checked_at >= strftime('%s','now') - (?4 * 3600))";
   sqlite3_stmt *st = NULL;
   if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_text(st, 1, ecosystem, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(st, 2, name, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(st, 3, version ? version : "", -1, SQLITE_TRANSIENT);
   sqlite3_bind_int(st, 4, ttl_hours);

   int rc = sqlite3_step(st);
   if (rc == SQLITE_ROW)
   {
      row_from_stmt(st, out);
      sqlite3_finalize(st);
      return 1;
   }
   sqlite3_finalize(st);
   return rc == SQLITE_DONE ? 0 : -1;
}

int db1_mcp_osv_cache_upsert(const char *ecosystem, const char *name, const char *version,
                             const char *verdict, const char *advisory_ids)
{
   sqlite3 *db = db1_conn();
   if (!db || !ecosystem || !ecosystem[0] || !name || !name[0] || !verdict || !verdict[0])
      return -1;

   const char *sql =
       "INSERT INTO mcp_osv_cache(ecosystem, name, version, verdict, advisory_ids, checked_at)"
       " VALUES(?1, ?2, ?3, ?4, ?5, strftime('%s','now'))"
       " ON CONFLICT(ecosystem, name, version) DO UPDATE SET"
       " verdict = excluded.verdict,"
       " advisory_ids = excluded.advisory_ids,"
       " checked_at = excluded.checked_at";
   sqlite3_stmt *st = NULL;
   if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_text(st, 1, ecosystem, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(st, 2, name, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(st, 3, version ? version : "", -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(st, 4, verdict, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(st, 5, advisory_ids ? advisory_ids : "", -1, SQLITE_TRANSIENT);
   int rc = sqlite3_step(st);
   sqlite3_finalize(st);
   return rc == SQLITE_DONE ? 0 : -1;
}

int db1_mcp_osv_cache_list(db1_mcp_osv_cache_row_t *out, int max)
{
   sqlite3 *db = db1_conn();
   if (!db || !out || max <= 0)
      return -1;

   const char *sql =
       "SELECT ecosystem, name, version, verdict, advisory_ids, checked_at,"
       " strftime('%Y-%m-%dT%H:%M:%SZ', checked_at, 'unixepoch')"
       " FROM mcp_osv_cache ORDER BY checked_at DESC, ecosystem, name, version LIMIT ?1";
   sqlite3_stmt *st = NULL;
   if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_int(st, 1, max);
   int n = 0;
   int rc;
   while ((rc = sqlite3_step(st)) == SQLITE_ROW && n < max)
      row_from_stmt(st, &out[n++]);
   sqlite3_finalize(st);
   return rc == SQLITE_DONE ? n : -1;
}

int db1_mcp_osv_audit(const char *client_name, const char *ecosystem, const char *name,
                      const char *version, const char *verdict, const char *action,
                      const char *advisory_ids)
{
   cJSON *payload = cJSON_CreateObject();
   if (!payload)
      return -1;
   cJSON_AddStringToObject(payload, "client", safe_text(client_name));
   cJSON_AddStringToObject(payload, "ecosystem", safe_text(ecosystem));
   cJSON_AddStringToObject(payload, "name", safe_text(name));
   cJSON_AddStringToObject(payload, "version", safe_text(version));
   cJSON_AddStringToObject(payload, "verdict", safe_text(verdict));
   cJSON_AddStringToObject(payload, "action", safe_text(action));
   cJSON_AddStringToObject(payload, "advisory_ids", safe_text(advisory_ids));
   char *json = cJSON_PrintUnformatted(payload);
   cJSON_Delete(payload);
   if (!json)
      return -1;
   int rc = ie_record("", IE_MCP_PACKAGE_CHECK, "system", json,
                      (action && strcmp(action, "block") == 0) ? "blocked" : "ok");
   free(json);
   return rc;
}
