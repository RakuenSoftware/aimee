/* kb_curator_serve.c: read-side serving for the deep-curator artifact graph.
 * See kb_curator_serve.h. No DB1 access. */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "kb_curator_serve.h"
#include "json_fluent.h"
#include "aimee.h"
#include "cJSON.h"
#include "db2/db2_internal.h"
#include "db2/db_postgres.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int kb_curator_implements_json(const char *topic, char *out, size_t out_cap)
{
   if (!out || out_cap < 64 || !topic || !topic[0])
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;

   /* code_unit --mentions--> entity, where the entity name is the topic OR the
    * entity is cited by a document whose path contains the topic. */
   static const char *sql =
       "SELECT DISTINCT cu.id, cu.payload, COALESCE(fc.source_id, '')"
       " FROM artifacts cu"
       " JOIN artifact_links l ON l.from_id = cu.id AND l.kind = 'mentions'"
       " JOIN artifacts e ON e.id = l.to_id AND e.kind = 'entity'"
       " LEFT JOIN artifact_citations fc"
       "   ON fc.artifact_id = cu.id AND fc.source_kind = 'kb_file'"
       " WHERE cu.kind = 'code_unit'"
       /* Match on the entity's name via the JSON operator, not a LIKE on the
        * raw payload: JSONB normalizes "name":"X" to "name": "X" (space after
        * the colon), so a no-space LIKE pattern never matches on Postgres. */
       "   AND (e.payload->>'name' = ?1"
       "        OR e.id IN (SELECT ec.artifact_id FROM artifact_citations ec"
       "                    JOIN kb_documents d ON CAST(d.id AS TEXT) = ec.source_id"
       "                    WHERE ec.source_kind = 'kb_document' AND d.file_path LIKE ?2))"
       " ORDER BY cu.id LIMIT 100";
   char docpat[300];
   snprintf(docpat, sizeof(docpat), "%%%s%%", topic);
   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", topic);
   aimee_pg_bind_text(st, "?2", docpat);

   int pos = 0;
   pos += snprintf(out + pos, out_cap - (size_t)pos, "{\"topic\":\"%s\",\"implements\":[", topic);
   int n = 0;
   while (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW && pos + 256 < (int)out_cap)
   {
      const char *cu_id = aimee_pg_column_text(st, 0);
      const char *pl = aimee_pg_column_text(st, 1);
      const char *file = aimee_pg_column_text(st, 2);
      if (!cu_id)
         continue;
      cJSON *pj = pl ? cJSON_Parse(pl) : NULL;
      const char *summary = pj ? jo_cstr(pj, "summary") : "";
      cJSON *o = cJSON_CreateObject();
      cJSON_AddStringToObject(o, "code_unit_id", cu_id);
      cJSON_AddStringToObject(o, "file_path", file ? file : "");
      cJSON_AddStringToObject(o, "summary", summary);
      cJSON_Delete(pj);
      char *os = cJSON_PrintUnformatted(o);
      cJSON_Delete(o);
      if (os)
      {
         pos += snprintf(out + pos, out_cap - (size_t)pos, "%s%s", n ? "," : "", os);
         free(os);
         n++;
      }
   }
   aimee_pg_finalize(st);
   snprintf(out + pos, out_cap - (size_t)pos, "]}");
   return n;
}

int kb_curator_synthesize_serve_json(const char *topic, char *out, size_t out_cap)
{
   if (!out || out_cap < 64 || !topic || !topic[0])
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;

   static const char *sql =
       "SELECT s.id, s.payload FROM artifacts s"
       " JOIN artifact_links l ON l.from_id = s.id AND l.kind = 'about'"
       " JOIN artifacts e ON e.id = l.to_id AND e.kind = 'entity'"
       " WHERE s.kind = 'synthesis' AND s.state IN ('proposed', 'committed')"
       /* Match the topic against the entity name via the JSON operator: JSONB
        * stores "name": "X" (space after the colon), so a no-space LIKE pattern
        * on the raw payload never matches on Postgres. */
       "   AND e.payload->>'name' = ?1 ORDER BY s.created_at DESC, s.id DESC LIMIT 1";
   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", topic);

   char id[64] = "";
   cJSON *pj = NULL;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      const char *c_id = aimee_pg_column_text(st, 0);
      const char *c_pl = aimee_pg_column_text(st, 1);
      snprintf(id, sizeof(id), "%s", c_id ? c_id : "");
      pj = c_pl ? cJSON_Parse(c_pl) : NULL;
   }
   aimee_pg_finalize(st);
   if (!id[0])
   {
      cJSON_Delete(pj);
      snprintf(out, out_cap,
               "{\"topic\":\"%s\",\"synthesis_id\":\"\",\"text\":\"\",\"sources\":[]}", topic);
      return -1;
   }

   cJSON *o = cJSON_CreateObject();
   cJSON_AddStringToObject(o, "topic", topic);
   cJSON_AddStringToObject(o, "synthesis_id", id);
   cJSON_AddStringToObject(o, "text", pj ? jo_cstr(pj, "text") : "");
   /* citations: prefer the payload array, else the artifact_citations rows. */
   int nsrc = 0;
   cJSON *sources = cJSON_AddArrayToObject(o, "sources");
   const cJSON *cites = pj ? cJSON_GetObjectItemCaseSensitive(pj, "citations") : NULL;
   if (cJSON_IsArray(cites))
   {
      const cJSON *c = NULL;
      cJSON_ArrayForEach(c, cites)
      {
         if (cJSON_IsString(c) && c->valuestring[0])
         {
            cJSON_AddItemToArray(sources, cJSON_CreateString(c->valuestring));
            nsrc++;
         }
      }
   }
   cJSON_Delete(pj);
   char *os = cJSON_PrintUnformatted(o);
   cJSON_Delete(o);
   if (os)
   {
      snprintf(out, out_cap, "%s", os);
      free(os);
   }
   return nsrc;
}

int kb_curator_contradictions_json(int limit, char *out, size_t out_cap)
{
   if (!out || out_cap < 64)
      return -1;
   if (limit < 1)
      limit = 20;
   if (limit > 100)
      limit = 100;
   void *conn = db2_conn();
   if (!conn)
      return -1;

   static const char *sql =
       "SELECT l.from_id, l.to_id, ca.payload, cb.payload FROM artifact_links l"
       " JOIN artifacts ca ON ca.id = l.from_id"
       " JOIN artifacts cb ON cb.id = l.to_id"
       " WHERE l.kind = 'contradicts' ORDER BY l.from_id, l.to_id LIMIT ?1";
   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_int(st, "?1", limit);

   int pos = 0;
   pos += snprintf(out + pos, out_cap - (size_t)pos, "{\"contradictions\":[");
   int n = 0;
   while (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW && pos + 512 < (int)out_cap)
   {
      const char *a = aimee_pg_column_text(st, 0);
      const char *b = aimee_pg_column_text(st, 1);
      const char *pa = aimee_pg_column_text(st, 2);
      const char *pb = aimee_pg_column_text(st, 3);
      if (!a || !b)
         continue;
      cJSON *ja = pa ? cJSON_Parse(pa) : NULL;
      cJSON *jb = pb ? cJSON_Parse(pb) : NULL;
      cJSON *o = cJSON_CreateObject();
      cJSON_AddStringToObject(o, "a", a);
      cJSON_AddStringToObject(o, "b", b);
      cJSON_AddStringToObject(o, "a_text", ja ? jo_cstr(ja, "text") : "");
      cJSON_AddStringToObject(o, "b_text", jb ? jo_cstr(jb, "text") : "");
      cJSON_Delete(ja);
      cJSON_Delete(jb);
      char *os = cJSON_PrintUnformatted(o);
      cJSON_Delete(o);
      if (os)
      {
         pos += snprintf(out + pos, out_cap - (size_t)pos, "%s%s", n ? "," : "", os);
         free(os);
         n++;
      }
   }
   aimee_pg_finalize(st);
   snprintf(out + pos, out_cap - (size_t)pos, "]}");
   return n;
}
