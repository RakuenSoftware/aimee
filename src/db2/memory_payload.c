/* memory_payload.c: DB2 payload builders for vector memory points,
 * plus a few small helpers (key/content lookup, key existence,
 * memory count, embedding upsert). Postgres via libpq. */

#include "memory_payload.h"

#include "db_postgres.h"
#include "cJSON.h"
#include "db2_internal.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MP_ERRBUF 256

static void memory_payload_add_entities(void *conn, int64_t memory_id, cJSON *payload)
{
   static const char *sql = "SELECT entity FROM memory_entities WHERE memory_id = ?1"
                            " ORDER BY weight DESC LIMIT 5";
   char err[MP_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return;
   aimee_pg_bind_int64(st, "?1", memory_id);

   cJSON *arr = cJSON_CreateArray();
   while (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      const char *ent = aimee_pg_column_text(st, 0);
      if (ent && ent[0])
         cJSON_AddItemToArray(arr, cJSON_CreateString(ent));
   }
   aimee_pg_finalize(st);

   if (cJSON_GetArraySize(arr) > 0)
      cJSON_AddItemToObject(payload, "entities", arr);
   else
      cJSON_Delete(arr);
}

char *db2_memory_build_memory_payload(int64_t memory_id)
{
   if (memory_id <= 0)
      return NULL;
   void *conn = db2_conn();
   if (!conn)
      return NULL;

   static const char *sql =
       "SELECT m.tier, m.kind, m.source_session, m.valid_from, m.valid_until,"
       "  m.created_at, m.updated_at,"
       "  (SELECT scope_value FROM memory_scopes"
       "   WHERE memory_id = m.id AND scope_type = 'project' LIMIT 1) AS project,"
       "  (SELECT scope_value FROM memory_scopes"
       "   WHERE memory_id = m.id AND scope_type = 'workspace' LIMIT 1) AS workspace"
       " FROM memories m WHERE m.id = ?1";
   char err[MP_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return NULL;
   aimee_pg_bind_int64(st, "?1", memory_id);
   if (aimee_pg_step(st, err, sizeof(err)) != AIMEE_PG_ROW)
   {
      aimee_pg_finalize(st);
      return NULL;
   }
   /* Buffer all row values before issuing the follow-up SELECT in
    * memory_payload_add_entities; libpq exposes one active result per
    * connection, so the cursor must be released first. */
   char tier[64], kind[64], source_session[128];
   char valid_from[64], valid_until[64];
   char created_at[64], updated_at[64];
   char project[256], workspace[256];
   int has_project, has_workspace;

   {
      const char *t = aimee_pg_column_text(st, 0);
      const char *k = aimee_pg_column_text(st, 1);
      const char *ss = aimee_pg_column_text(st, 2);
      const char *vf = aimee_pg_column_text(st, 3);
      const char *vu = aimee_pg_column_text(st, 4);
      const char *ca = aimee_pg_column_text(st, 5);
      const char *ua = aimee_pg_column_text(st, 6);
      const char *pj = aimee_pg_column_text(st, 7);
      const char *ws = aimee_pg_column_text(st, 8);
      snprintf(tier, sizeof(tier), "%s", t ? t : "");
      snprintf(kind, sizeof(kind), "%s", k ? k : "");
      snprintf(source_session, sizeof(source_session), "%s", ss ? ss : "");
      snprintf(valid_from, sizeof(valid_from), "%s", vf ? vf : "");
      snprintf(valid_until, sizeof(valid_until), "%s", vu ? vu : "");
      snprintf(created_at, sizeof(created_at), "%s", ca ? ca : "");
      snprintf(updated_at, sizeof(updated_at), "%s", ua ? ua : "");
      has_project = (pj && pj[0]) ? 1 : 0;
      has_workspace = (ws && ws[0]) ? 1 : 0;
      snprintf(project, sizeof(project), "%s", pj ? pj : "");
      snprintf(workspace, sizeof(workspace), "%s", ws ? ws : "");
   }
   aimee_pg_finalize(st);

   const char *primary_scope = has_project ? "project" : (has_workspace ? "workspace" : "global");

   cJSON *payload = cJSON_CreateObject();
   cJSON_AddStringToObject(payload, "record_type", "memory");
   cJSON_AddNumberToObject(payload, "memory_id", (double)memory_id);
   cJSON_AddStringToObject(payload, "tier", tier);
   cJSON_AddStringToObject(payload, "kind", kind);
   cJSON_AddStringToObject(payload, "primary_scope", primary_scope);
   if (has_project)
      cJSON_AddStringToObject(payload, "project", project);
   if (has_workspace)
      cJSON_AddStringToObject(payload, "workspace", workspace);
   if (source_session[0])
      cJSON_AddStringToObject(payload, "source_session", source_session);
   if (valid_from[0])
      cJSON_AddStringToObject(payload, "valid_from", valid_from);
   if (valid_until[0])
      cJSON_AddStringToObject(payload, "valid_until", valid_until);
   if (created_at[0])
      cJSON_AddStringToObject(payload, "created_at", created_at);
   if (updated_at[0])
      cJSON_AddStringToObject(payload, "updated_at", updated_at);

   memory_payload_add_entities(conn, memory_id, payload);

   char *payload_json = cJSON_PrintUnformatted(payload);
   cJSON_Delete(payload);
   return payload_json;
}

int db2_memory_provenance_by_id(int64_t memory_id, char *kind_out, int kind_len, char *source_out,
                                int source_len, char *version_out, int version_len)
{
   if (kind_out && kind_len > 0)
      kind_out[0] = '\0';
   if (source_out && source_len > 0)
      source_out[0] = '\0';
   if (version_out && version_len > 0)
      version_out[0] = '\0';
   if (memory_id <= 0)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;
   static const char *sql = "SELECT kind, source_session, updated_at FROM memories WHERE id = ?1";
   char err[MP_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_int64(st, "?1", memory_id);
   int step = aimee_pg_step(st, err, sizeof(err));
   if (step != AIMEE_PG_ROW)
   {
      aimee_pg_finalize(st);
      return step == AIMEE_PG_DONE ? 0 : -1; /* 0 = no such row (deleted/superseded) */
   }
   const char *k = aimee_pg_column_text(st, 0);
   const char *s = aimee_pg_column_text(st, 1);
   const char *v = aimee_pg_column_text(st, 2);
   if (kind_out && kind_len > 0)
      snprintf(kind_out, (size_t)kind_len, "%s", k ? k : "");
   if (source_out && source_len > 0)
      snprintf(source_out, (size_t)source_len, "%s", s ? s : "");
   if (version_out && version_len > 0)
      snprintf(version_out, (size_t)version_len, "%s", v ? v : "");
   aimee_pg_finalize(st);
   return 1;
}

char *db2_memory_build_unit_payload(int64_t unit_id, int64_t *memory_id_out)
{
   if (unit_id <= 0)
      return NULL;
   void *conn = db2_conn();
   if (!conn)
      return NULL;

   static const char *sql =
       "SELECT u.memory_id, u.unit_type, u.memory_kind, m.tier, m.kind,"
       "  m.source_session, m.valid_from, m.valid_until,"
       "  u.created_at AS unit_created_at, m.updated_at AS memory_updated_at,"
       "  (SELECT scope_value FROM memory_scopes"
       "   WHERE memory_id = m.id AND scope_type = 'project' LIMIT 1) AS project,"
       "  (SELECT scope_value FROM memory_scopes"
       "   WHERE memory_id = m.id AND scope_type = 'workspace' LIMIT 1) AS workspace"
       " FROM memory_units u JOIN memories m ON m.id = u.memory_id WHERE u.id = ?1";
   char err[MP_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return NULL;
   aimee_pg_bind_int64(st, "?1", unit_id);
   if (aimee_pg_step(st, err, sizeof(err)) != AIMEE_PG_ROW)
   {
      aimee_pg_finalize(st);
      return NULL;
   }
   /* Buffer everything before releasing the cursor (see memory payload). */
   int64_t memory_id = aimee_pg_column_int64(st, 0);
   char unit_type[64], memory_kind[64];
   char tier[64], kind[64], source_session[128];
   char valid_from[64], valid_until[64];
   char created_at[64], updated_at[64];
   char project[256], workspace[256];
   int has_project, has_workspace;

   {
      const char *ut = aimee_pg_column_text(st, 1);
      const char *mk = aimee_pg_column_text(st, 2);
      const char *t = aimee_pg_column_text(st, 3);
      const char *k = aimee_pg_column_text(st, 4);
      const char *ss = aimee_pg_column_text(st, 5);
      const char *vf = aimee_pg_column_text(st, 6);
      const char *vu = aimee_pg_column_text(st, 7);
      const char *ca = aimee_pg_column_text(st, 8);
      const char *ua = aimee_pg_column_text(st, 9);
      const char *pj = aimee_pg_column_text(st, 10);
      const char *ws = aimee_pg_column_text(st, 11);
      snprintf(unit_type, sizeof(unit_type), "%s", ut ? ut : "");
      snprintf(memory_kind, sizeof(memory_kind), "%s", mk ? mk : "");
      snprintf(tier, sizeof(tier), "%s", t ? t : "");
      snprintf(kind, sizeof(kind), "%s", k ? k : "");
      snprintf(source_session, sizeof(source_session), "%s", ss ? ss : "");
      snprintf(valid_from, sizeof(valid_from), "%s", vf ? vf : "");
      snprintf(valid_until, sizeof(valid_until), "%s", vu ? vu : "");
      snprintf(created_at, sizeof(created_at), "%s", ca ? ca : "");
      snprintf(updated_at, sizeof(updated_at), "%s", ua ? ua : "");
      has_project = (pj && pj[0]) ? 1 : 0;
      has_workspace = (ws && ws[0]) ? 1 : 0;
      snprintf(project, sizeof(project), "%s", pj ? pj : "");
      snprintf(workspace, sizeof(workspace), "%s", ws ? ws : "");
   }
   aimee_pg_finalize(st);

   const char *primary_scope = has_project ? "project" : (has_workspace ? "workspace" : "global");

   cJSON *payload = cJSON_CreateObject();
   cJSON_AddStringToObject(payload, "record_type", "unit");
   cJSON_AddNumberToObject(payload, "unit_id", (double)unit_id);
   cJSON_AddNumberToObject(payload, "memory_id", (double)memory_id);
   cJSON_AddStringToObject(payload, "unit_type", unit_type);
   cJSON_AddStringToObject(payload, "memory_kind", memory_kind);
   cJSON_AddStringToObject(payload, "tier", tier);
   cJSON_AddStringToObject(payload, "kind", kind);
   cJSON_AddStringToObject(payload, "primary_scope", primary_scope);
   if (has_project)
      cJSON_AddStringToObject(payload, "project", project);
   if (has_workspace)
      cJSON_AddStringToObject(payload, "workspace", workspace);
   if (source_session[0])
      cJSON_AddStringToObject(payload, "source_session", source_session);
   if (valid_from[0])
      cJSON_AddStringToObject(payload, "valid_from", valid_from);
   if (valid_until[0])
      cJSON_AddStringToObject(payload, "valid_until", valid_until);
   if (created_at[0])
      cJSON_AddStringToObject(payload, "created_at", created_at);
   if (updated_at[0])
      cJSON_AddStringToObject(payload, "updated_at", updated_at);

   memory_payload_add_entities(conn, memory_id, payload);

   char *payload_json = cJSON_PrintUnformatted(payload);
   cJSON_Delete(payload);
   if (memory_id_out)
      *memory_id_out = memory_id;
   return payload_json;
}

int db2_memory_get_key_content(int64_t memory_id, char *key_out, int key_len, char *content_out,
                               int content_len)
{
   if (memory_id <= 0 || !key_out || key_len <= 0 || !content_out || content_len <= 0)
      return -1;
   key_out[0] = '\0';
   content_out[0] = '\0';
   void *conn = db2_conn();
   if (!conn)
      return -1;

   char err[MP_ERRBUF] = "";
   aimee_pg_stmt_t *st =
       aimee_pg_prepare(conn, "SELECT key, content FROM memories WHERE id = ?1", err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_int64(st, "?1", memory_id);
   int rc = -1;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      const char *key = aimee_pg_column_text(st, 0);
      const char *content = aimee_pg_column_text(st, 1);
      snprintf(key_out, key_len, "%s", key ? key : "");
      snprintf(content_out, content_len, "%s", content ? content : "");
      rc = 0;
   }
   aimee_pg_finalize(st);
   return rc;
}

int db2_memory_key_exists(const char *key)
{
   if (!key || !key[0])
      return 0;
   void *conn = db2_conn();
   if (!conn)
      return -1;

   char err[MP_ERRBUF] = "";
   aimee_pg_stmt_t *st =
       aimee_pg_prepare(conn, "SELECT 1 FROM memories WHERE key = ?1 LIMIT 1", err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", key);
   int found = (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW) ? 1 : 0;
   aimee_pg_finalize(st);
   return found;
}

int64_t db2_memory_count(void)
{
   void *conn = db2_conn();
   if (!conn)
      return 0;
   char err[MP_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, "SELECT COUNT(*) FROM memories", err, sizeof(err));
   if (!st)
      return 0;
   int64_t n = 0;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
      n = aimee_pg_column_int64(st, 0);
   aimee_pg_finalize(st);
   return n;
}
