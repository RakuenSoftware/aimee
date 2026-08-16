/* db2/kb_service_backend_export.c: filtered memory export/import for kb.export / kb.import RPCs. */

#include "kb_service_backend_export.h"
#include "../headers/aimee.h" /* memory_t for memory_query.h */
#include "kb_service_backend.h"
#include "db2_internal.h"
#include "db_postgres.h"
#include "memory_query.h"
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

cJSON *db2_kb_service_memory_export_filtered_json(const char *workspace, const char *kind,
                                                  const char *since_iso, int include_archived)
{
   char sql[2048];
   int param_idx = 0;
   const char *params[4];
   int n_params = 0;
   int filter_kind = kind && kind[0] && strcmp(kind, "all") != 0;

   /* Build SELECT with optional filters. Keep workspace lookup as scalar subqueries
    * to avoid duplicate rows when a memory carries multiple scope tags. */
   int offset =
       snprintf(sql, sizeof(sql),
                "SELECT m.id, m.tier, m.kind, m.key, m.content, m.confidence, "
                "m.use_count, m.lifecycle_state, m.created_at, m.updated_at, m.source_session, "
                "COALESCE((SELECT ms.scope_value FROM memory_scopes ms "
                "          WHERE ms.memory_id = m.id AND ms.scope_type = 'workspace' "
                "          ORDER BY ms.scope_value LIMIT 1), "
                "         (SELECT mw.workspace FROM memory_workspaces mw "
                "          WHERE mw.memory_id = m.id ORDER BY mw.workspace LIMIT 1), '') "
                "AS workspace "
                "FROM memories m");

   offset += snprintf(sql + offset, sizeof(sql) - (size_t)offset, " WHERE 1=1");

   if (workspace)
   {
      param_idx++;
      params[n_params++] = workspace;
      offset += snprintf(sql + offset, sizeof(sql) - (size_t)offset,
                         " AND (EXISTS (SELECT 1 FROM memory_scopes msf "
                         "              WHERE msf.memory_id = m.id "
                         "                AND msf.scope_type = 'workspace' "
                         "                AND msf.scope_value = $%d) "
                         "      OR EXISTS (SELECT 1 FROM memory_workspaces mwf "
                         "                 WHERE mwf.memory_id = m.id "
                         "                   AND mwf.workspace = $%d))",
                         param_idx, param_idx);
   }
   if (filter_kind)
   {
      param_idx++;
      params[n_params++] = kind;
      offset +=
          snprintf(sql + offset, sizeof(sql) - (size_t)offset, " AND m.kind = $%d", param_idx);
   }
   if (since_iso)
   {
      param_idx++;
      params[n_params++] = since_iso;
      offset += snprintf(sql + offset, sizeof(sql) - (size_t)offset, " AND m.created_at >= $%d",
                         param_idx);
   }
   if (!include_archived)
   {
      offset +=
          snprintf(sql + offset, sizeof(sql) - (size_t)offset, " AND m.lifecycle_state = 'active'");
   }

   offset += snprintf(sql + offset, sizeof(sql) - (size_t)offset, " ORDER BY m.id");

   void *conn = db2_conn();
   if (!conn)
      return NULL;

   char errbuf[256];
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, errbuf, sizeof(errbuf));
   if (!st)
      return NULL;

   /* Bind parameters in order */
   for (int i = 0; i < n_params; i++)
   {
      char pname[16];
      snprintf(pname, sizeof(pname), "$%d", i + 1);
      aimee_pg_bind_text(st, pname, params[i]);
   }

   cJSON *memories_arr = cJSON_CreateArray();
   int count = 0;

   aimee_pg_step_t r;
   while ((r = aimee_pg_step(st, errbuf, sizeof(errbuf))) == AIMEE_PG_ROW)
   {
      cJSON *obj = cJSON_CreateObject();
      if (!obj)
      {
         cJSON_Delete(memories_arr);
         aimee_pg_finalize(st);
         return NULL;
      }

      cJSON_AddNumberToObject(obj, "id", (double)aimee_pg_column_int64(st, 0));
      cJSON_AddStringToObject(obj, "tier", aimee_pg_column_text(st, 1));
      cJSON_AddStringToObject(obj, "kind", aimee_pg_column_text(st, 2));
      cJSON_AddStringToObject(obj, "key", aimee_pg_column_text(st, 3));
      cJSON_AddStringToObject(obj, "content", aimee_pg_column_text(st, 4));
      cJSON_AddNumberToObject(obj, "confidence", aimee_pg_column_double(st, 5));
      cJSON_AddNumberToObject(obj, "use_count", (double)aimee_pg_column_int64(st, 6));
      cJSON_AddStringToObject(obj, "lifecycle_state", aimee_pg_column_text(st, 7));
      cJSON_AddStringToObject(obj, "created_at", aimee_pg_column_text(st, 8));
      cJSON_AddStringToObject(obj, "updated_at", aimee_pg_column_text(st, 9));
      cJSON_AddStringToObject(obj, "source_session", aimee_pg_column_text(st, 10));
      cJSON_AddStringToObject(obj, "workspace", aimee_pg_column_text(st, 11));

      cJSON_AddItemToArray(memories_arr, obj);
      count++;
   }

   if (r == AIMEE_PG_ERR)
   {
      cJSON_Delete(memories_arr);
      aimee_pg_finalize(st);
      return NULL;
   }

   aimee_pg_finalize(st);

   /* Query entity_profiles */
   st = aimee_pg_prepare(conn,
                         "SELECT entity_id, canonical_name, observation_count, card_json "
                         "FROM entity_profiles ORDER BY entity_id",
                         errbuf, sizeof(errbuf));
   cJSON *entities_arr = cJSON_CreateArray();

   if (st)
   {
      while ((r = aimee_pg_step(st, errbuf, sizeof(errbuf))) == AIMEE_PG_ROW)
      {
         cJSON *obj = cJSON_CreateObject();
         if (!obj)
         {
            cJSON_Delete(entities_arr);
            aimee_pg_finalize(st);
            cJSON_Delete(memories_arr);
            return NULL;
         }

         cJSON_AddStringToObject(obj, "entity_id", aimee_pg_column_text(st, 0));
         cJSON_AddStringToObject(obj, "canonical_name", aimee_pg_column_text(st, 1));
         cJSON_AddNumberToObject(obj, "observation_count", (double)aimee_pg_column_int64(st, 2));
         cJSON_AddStringToObject(obj, "card_json", aimee_pg_column_text(st, 3));

         cJSON_AddItemToArray(entities_arr, obj);
      }
      aimee_pg_finalize(st);
   }

   /* Build exported_at timestamp */
   char exported_at_buf[64];
   time_t now = time(NULL);
   struct tm *tm = gmtime(&now);
   strftime(exported_at_buf, sizeof(exported_at_buf), "%Y-%m-%dT%H:%M:%SZ", tm);

   /* Build response envelope */
   cJSON *resp = cJSON_CreateObject();
   if (!resp)
   {
      cJSON_Delete(memories_arr);
      cJSON_Delete(entities_arr);
      return NULL;
   }

   cJSON_AddStringToObject(resp, "status", "ok");
   cJSON_AddStringToObject(resp, "schema_version", "1");
   cJSON_AddStringToObject(resp, "exported_at", exported_at_buf);
   if (workspace && workspace[0])
      cJSON_AddStringToObject(resp, "workspace", workspace);
   if (kind && kind[0])
      cJSON_AddStringToObject(resp, "kind", kind);
   if (since_iso && since_iso[0])
      cJSON_AddStringToObject(resp, "since", since_iso);
   cJSON_AddItemToObject(resp, "memories", memories_arr);
   cJSON_AddItemToObject(resp, "entity_profiles", entities_arr);
   cJSON_AddNumberToObject(resp, "count", (double)count);

   return resp;
}

int db2_kb_service_memory_import_json(cJSON *memories_arr, const char *workspace_override,
                                      int dry_run, int *imported_out)
{
   if (imported_out)
      *imported_out = 0;
   if (!cJSON_IsArray(memories_arr))
      return -1;

   int count = 0;
   cJSON *item;
   cJSON_ArrayForEach(item, memories_arr)
   {
      cJSON *tier_j = cJSON_GetObjectItemCaseSensitive(item, "tier");
      cJSON *kind_j = cJSON_GetObjectItemCaseSensitive(item, "kind");
      cJSON *key_j = cJSON_GetObjectItemCaseSensitive(item, "key");
      cJSON *content_j = cJSON_GetObjectItemCaseSensitive(item, "content");
      cJSON *conf_j = cJSON_GetObjectItemCaseSensitive(item, "confidence");
      cJSON *sess_j = cJSON_GetObjectItemCaseSensitive(item, "source_session");

      if (!cJSON_IsString(kind_j) || !cJSON_IsString(key_j) || !cJSON_IsString(content_j))
         continue; /* skip malformed */

      const char *tier = cJSON_IsString(tier_j) ? tier_j->valuestring : "L0";
      double confidence = cJSON_IsNumber(conf_j) ? conf_j->valuedouble : 1.0;
      const char *session_id = cJSON_IsString(sess_j) ? sess_j->valuestring : "";

      if (!dry_run)
      {
         cJSON *r =
             db2_kb_service_memory_insert_json(tier, kind_j->valuestring, key_j->valuestring,
                                               content_j->valuestring, confidence, session_id);
         cJSON *id_j = cJSON_GetObjectItemCaseSensitive(r, "id");
         if (workspace_override && workspace_override[0] && cJSON_IsNumber(id_j))
         {
            int64_t id = (int64_t)id_j->valuedouble;
            db2_memory_scope_tag_insert(id, "workspace", workspace_override);
            db2_memory_workspace_tag_insert(id, workspace_override);
         }
         cJSON_Delete(r);
      }
      count++;
   }

   if (imported_out)
      *imported_out = count;
   return 0;
}
