/* kb_reasoning.c: Datalog-based graph reasoning over DB2 artifacts.
 * See
 * docs/proposals/accepted/graph-reasoning-case-based-recall-and-contradiction-logic.md
 */

#include "kb_reasoning.h"
#include "modules/db2/c/artifacts.h"
#include "modules/db2/c/db2_internal.h"
#include "modules/db2/c/db_postgres.h"
#include "aimee.h"
#include "log.h"
#include "headers/platform_process.h"

#include <cJSON.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REASONING_DEFAULT_ROW_BUDGET    10000
#define REASONING_DEFAULT_TIME_LIMIT_MS 5000

/* ---- fact snapshot builder ---- */

static cJSON *build_fact_array(const char *scope_kind, const char *scope_id)
{
   cJSON *facts = cJSON_CreateArray();
   if (!facts)
      return NULL;

   void *conn = db2_conn();
   if (!conn)
      return facts;

   char err[256] = "";

   /* artifact(id, kind, state, scope_kind, scope_id, confidence, committed_at,
    *           valid_from, valid_until) — 9 fields after the pred tag */
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       conn,
       "SELECT id, kind, state, scope_kind, scope_id, confidence::text, committed_at,"
       "       coalesce(payload->>'valid_from',''), coalesce(payload->>'valid_until','')"
       "  FROM artifacts"
       " WHERE (?1 = '' OR scope_kind = ?1)"
       "   AND (?2 = '' OR scope_id   = ?2)"
       " LIMIT 2000",
       err, sizeof(err));
   if (st)
   {
      aimee_pg_bind_text(st, "?1", scope_kind ? scope_kind : "");
      aimee_pg_bind_text(st, "?2", scope_id ? scope_id : "");
      while (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
      {
         cJSON *row = cJSON_CreateArray();
         if (!row)
            break;
         cJSON_AddItemToArray(row, cJSON_CreateString("artifact"));
         for (int c = 0; c < 9; c++)
         {
            const char *v = aimee_pg_column_text(st, c);
            cJSON_AddItemToArray(row, cJSON_CreateString(v ? v : ""));
         }
         cJSON_AddItemToArray(facts, row);
      }
      aimee_pg_finalize(st);
   }

   st = aimee_pg_prepare(conn,
                         "UPDATE artifacts"
                         "   SET last_accessed_at = CURRENT_TIMESTAMP"
                         " WHERE id IN ("
                         "       SELECT id FROM artifacts"
                         "        WHERE (?1 = '' OR scope_kind = ?1)"
                         "          AND (?2 = '' OR scope_id   = ?2)"
                         "        LIMIT 2000"
                         "       )",
                         err, sizeof(err));
   if (st)
   {
      aimee_pg_bind_text(st, "?1", scope_kind ? scope_kind : "");
      aimee_pg_bind_text(st, "?2", scope_id ? scope_id : "");
      aimee_pg_step(st, err, sizeof(err));
      aimee_pg_finalize(st);
   }

   /* artifact_link(from_id, to_id, kind) */
   st = aimee_pg_prepare(conn,
                         "SELECT al.from_id, al.to_id, al.kind"
                         "  FROM artifact_links al"
                         "  JOIN artifacts a ON a.id = al.from_id"
                         " WHERE (?1 = '' OR a.scope_kind = ?1)"
                         "   AND (?2 = '' OR a.scope_id   = ?2)"
                         " LIMIT 2000",
                         err, sizeof(err));
   if (st)
   {
      aimee_pg_bind_text(st, "?1", scope_kind ? scope_kind : "");
      aimee_pg_bind_text(st, "?2", scope_id ? scope_id : "");
      while (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
      {
         cJSON *row = cJSON_CreateArray();
         if (!row)
            break;
         cJSON_AddItemToArray(row, cJSON_CreateString("artifact_link"));
         for (int c = 0; c < 3; c++)
         {
            const char *v = aimee_pg_column_text(st, c);
            cJSON_AddItemToArray(row, cJSON_CreateString(v ? v : ""));
         }
         cJSON_AddItemToArray(facts, row);
      }
      aimee_pg_finalize(st);
   }

   /* entity_edge(source, relation, target, weight, valid_from, valid_until) */
   st = aimee_pg_prepare(conn,
                         "SELECT source, relation, target, weight::text, valid_from, valid_until"
                         "  FROM entity_edges"
                         " WHERE COALESCE(edge_origin,'') <> 'code_projection' OR EXISTS ("
                         " SELECT 1 FROM code_projection_generations g"
                         " JOIN projects p ON p.name=g.project"
                         " WHERE g.id=entity_edges.projection_generation_id"
                         " AND g.state='visible' AND p.lifecycle_state='current')"
                         " LIMIT 4000",
                         err, sizeof(err));
   if (st)
   {
      while (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
      {
         cJSON *row = cJSON_CreateArray();
         if (!row)
            break;
         cJSON_AddItemToArray(row, cJSON_CreateString("entity_edge"));
         for (int c = 0; c < 6; c++)
         {
            const char *v = aimee_pg_column_text(st, c);
            cJSON_AddItemToArray(row, cJSON_CreateString(v ? v : ""));
         }
         cJSON_AddItemToArray(facts, row);
      }
      aimee_pg_finalize(st);
   }

   return facts;
}

/* ---- kb_reasoning_query ---- */

int kb_reasoning_query(const char *query, const char *bindings_json, const char *scope_kind,
                       const char *scope_id, kb_reasoning_result_t *result_out)
{
   if (!query || !result_out)
      return -1;
   /* Copied out: the command is passed to platform_exec_pipe further down, past
    * other accessor calls that would reclaim the thread-local buffer. */
   char datalog_command[CONFIG_COPY_MAX];
   config_reasoning_datalog_command_copy(datalog_command, sizeof(datalog_command));
   if (!datalog_command[0])
      return -1;

   memset(result_out, 0, sizeof(*result_out));

   int row_budget = config_reasoning_row_budget() > 0 ? config_reasoning_row_budget()
                                                      : REASONING_DEFAULT_ROW_BUDGET;
   int time_limit_ms = config_reasoning_time_limit_ms() > 0 ? config_reasoning_time_limit_ms()
                                                            : REASONING_DEFAULT_TIME_LIMIT_MS;

   cJSON *facts = build_fact_array(scope_kind, scope_id);
   if (!facts)
      return -1;

   cJSON *req = cJSON_CreateObject();
   if (!req)
   {
      cJSON_Delete(facts);
      return -1;
   }
   cJSON_AddNumberToObject(req, "version", 1);
   cJSON_AddStringToObject(req, "role", "reason");
   cJSON_AddStringToObject(req, "model_version", "datalog-semi-naive-v1");
   cJSON_AddStringToObject(req, "prompt_version", "ruleset-v1");

   cJSON *scope_obj = cJSON_CreateObject();
   cJSON_AddStringToObject(scope_obj, "kind", scope_kind ? scope_kind : "");
   cJSON_AddStringToObject(scope_obj, "id", scope_id ? scope_id : "");
   cJSON_AddItemToObject(req, "scope", scope_obj);

   cJSON *inputs = cJSON_CreateObject();
   cJSON_AddStringToObject(inputs, "query", query);

   if (bindings_json && bindings_json[0])
   {
      cJSON *b = cJSON_Parse(bindings_json);
      cJSON_AddItemToObject(inputs, "bindings", b ? b : cJSON_CreateObject());
   }
   else
   {
      cJSON_AddItemToObject(inputs, "bindings", cJSON_CreateObject());
   }

   cJSON_AddItemToObject(inputs, "facts", facts);
   cJSON_AddStringToObject(inputs, "ruleset", "v1");
   cJSON_AddNumberToObject(inputs, "limit", KB_REASONING_MAX_BINDINGS);
   cJSON_AddNumberToObject(inputs, "row_budget", row_budget);
   cJSON_AddNumberToObject(inputs, "time_limit_ms", time_limit_ms);
   cJSON_AddItemToObject(req, "inputs", inputs);

   char *req_str = cJSON_PrintUnformatted(req);
   cJSON_Delete(req);
   if (!req_str)
      return -1;

   char *out = NULL;
   size_t out_len = 0;
   int rc = platform_exec_pipe(datalog_command, req_str, strlen(req_str), &out, &out_len);
   free(req_str);

   if (rc != 0 || !out)
   {
      aimee_log(LOG_DEBUG, "reasoning", "datalog sidecar exit=%d query=%s", rc, query);
      free(out);
      return -1;
   }

   cJSON *resp = cJSON_ParseWithLength(out, out_len);
   free(out);
   if (!resp)
   {
      aimee_log(LOG_DEBUG, "reasoning", "datalog sidecar invalid JSON");
      return -1;
   }

   cJSON *status = cJSON_GetObjectItemCaseSensitive(resp, "status");
   if (!cJSON_IsString(status) || strcmp(status->valuestring, "ok") != 0)
   {
      cJSON *err_j = cJSON_GetObjectItemCaseSensitive(resp, "error");
      aimee_log(LOG_DEBUG, "reasoning", "datalog sidecar error: %s",
                cJSON_IsString(err_j) ? err_j->valuestring : "unknown");
      cJSON_Delete(resp);
      return -1;
   }

   cJSON *stats = cJSON_GetObjectItemCaseSensitive(resp, "stats");
   if (cJSON_IsObject(stats))
   {
      cJSON *fe = cJSON_GetObjectItemCaseSensitive(stats, "facts_evaluated");
      cJSON *df = cJSON_GetObjectItemCaseSensitive(stats, "derived_facts");
      cJSON *el = cJSON_GetObjectItemCaseSensitive(stats, "elapsed_ms");
      if (cJSON_IsNumber(fe))
         result_out->facts_evaluated = (int)fe->valuedouble;
      if (cJSON_IsNumber(df))
         result_out->derived_facts = (int)df->valuedouble;
      if (cJSON_IsNumber(el))
         result_out->elapsed_ms = (int)el->valuedouble;
   }

   cJSON *bindings = cJSON_GetObjectItemCaseSensitive(resp, "bindings");
   int n = bindings ? cJSON_GetArraySize(bindings) : 0;
   if (n > KB_REASONING_MAX_BINDINGS)
      n = KB_REASONING_MAX_BINDINGS;

   if (n > 0)
   {
      result_out->rows =
          (kb_reasoning_binding_t *)calloc((size_t)n, sizeof(kb_reasoning_binding_t));
      if (!result_out->rows)
      {
         cJSON_Delete(resp);
         return -1;
      }
   }

   for (int i = 0; i < n; i++)
   {
      cJSON *row = cJSON_GetArrayItem(bindings, i);
      if (!cJSON_IsObject(row))
         continue;
      kb_reasoning_binding_t *b = &result_out->rows[i];
      result_out->n_rows++;

      cJSON *vars_obj = cJSON_GetObjectItemCaseSensitive(row, "vars");
      if (cJSON_IsObject(vars_obj))
      {
         cJSON *kv = vars_obj->child;
         while (kv && b->n_vars < KB_REASONING_MAX_VARS)
         {
            snprintf(b->vars[b->n_vars].var, sizeof(b->vars[b->n_vars].var), "%s",
                     kv->string ? kv->string : "");
            snprintf(b->vars[b->n_vars].value, sizeof(b->vars[b->n_vars].value), "%s",
                     cJSON_IsString(kv) ? kv->valuestring : "");
            b->n_vars++;
            kv = kv->next;
         }
      }

      cJSON *citations = cJSON_GetObjectItemCaseSensitive(row, "citations");
      if (cJSON_IsArray(citations))
      {
         size_t pos = 0;
         int nc = cJSON_GetArraySize(citations);
         for (int ci = 0; ci < nc && pos < sizeof(b->citations) - 2; ci++)
         {
            cJSON *c = cJSON_GetArrayItem(citations, ci);
            if (!cJSON_IsString(c))
               continue;
            if (pos > 0)
               b->citations[pos++] = ',';
            size_t rem = sizeof(b->citations) - pos - 1;
            size_t vlen = strlen(c->valuestring);
            if (vlen > rem)
               vlen = rem;
            memcpy(b->citations + pos, c->valuestring, vlen);
            pos += vlen;
         }
         b->citations[pos] = '\0';
      }
   }

   cJSON_Delete(resp);
   aimee_log(LOG_DEBUG, "reasoning", "query=%s rows=%d derived=%d elapsed_ms=%d", query,
             result_out->n_rows, result_out->derived_facts, result_out->elapsed_ms);
   return 0;
}

void kb_reasoning_result_free(kb_reasoning_result_t *r)
{
   if (!r)
      return;
   free(r->rows);
   r->rows = NULL;
   r->n_rows = 0;
}

/* ---- kb_reasoning_case_write ---- */

int kb_reasoning_case_write(const char *payload_json, char *id_out, int id_out_len)
{
   if (!payload_json)
      return -1;

   char id[64];
   db2_artifact_gen_id(id, sizeof(id));

   int rc = db2_artifact_write(id, "case", "proposed", "system", "", "", 1.0, payload_json);
   if (rc != 0)
      return -1;

   if (id_out && id_out_len > 0)
      snprintf(id_out, (size_t)id_out_len, "%s", id);

   aimee_log(LOG_DEBUG, "reasoning", "wrote case artifact id=%s", id);
   return 0;
}

/* ---- kb_reasoning_seed_ruleset ---- */

void kb_reasoning_seed_ruleset(void)
{
   void *conn = db2_conn();
   if (!conn)
      return;

   char err[256] = "";
   aimee_pg_stmt_t *st =
       aimee_pg_prepare(conn,
                        "INSERT INTO reasoning_rulesets (id, version, name, content_hash, active)"
                        " VALUES ('ruleset-v1', 1, 'ruleset-v1', 'sidecar-builtin-v1', TRUE)"
                        " ON CONFLICT (id) DO NOTHING",
                        err, sizeof(err));
   if (!st)
      return;
   aimee_pg_step(st, err, sizeof(err));
   aimee_pg_finalize(st);
   aimee_log(LOG_DEBUG, "reasoning", "ruleset seed complete");
}

/* ---- kb_reasoning_case_recall ---- */

int kb_reasoning_case_recall(const char *trigger_json, const char *scope_kind, const char *scope_id,
                             kb_reasoning_case_result_t *out, int max_out)
{
   (void)trigger_json;
   if (!out || max_out <= 0)
      return -1;

   void *conn = db2_conn();
   if (!conn)
      return -1;

   /* kNN via exemplar_vectors — skip to fallback if no rows exist. */
   char err[256] = "";
   int n = 0;
   aimee_pg_stmt_t *st =
       aimee_pg_prepare(conn,
                        "SELECT a.id, coalesce(a.payload::jsonb->>'outcome',''), a.confidence"
                        "  FROM exemplar_vectors ev"
                        "  JOIN artifacts a ON a.id = ev.artifact_id"
                        " WHERE a.kind = 'case' AND a.state = 'committed'"
                        "   AND (?1 = '' OR a.scope_kind = ?1)"
                        "   AND (?2 = '' OR a.scope_id   = ?2)"
                        " ORDER BY a.confidence DESC"
                        " LIMIT ?3",
                        err, sizeof(err));
   if (st)
   {
      aimee_pg_bind_text(st, "?1", scope_kind ? scope_kind : "");
      aimee_pg_bind_text(st, "?2", scope_id ? scope_id : "");
      aimee_pg_bind_int64(st, "?3", (int64_t)max_out);
      while (n < max_out && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
      {
         memset(&out[n], 0, sizeof(out[n]));
         const char *v = aimee_pg_column_text(st, 0);
         if (v)
            snprintf(out[n].artifact_id, sizeof(out[n].artifact_id), "%s", v);
         v = aimee_pg_column_text(st, 1);
         if (v)
            snprintf(out[n].outcome, sizeof(out[n].outcome), "%s", v);
         out[n].score = aimee_pg_column_double(st, 2);
         n++;
      }
      aimee_pg_finalize(st);
   }

   if (n > 0)
      return n;

   /* Fallback: direct case query ordered by confidence. */
   st = aimee_pg_prepare(conn,
                         "SELECT id, coalesce(payload::jsonb->>'outcome',''), confidence"
                         "  FROM artifacts"
                         " WHERE kind = 'case' AND state = 'committed'"
                         "   AND (?1 = '' OR scope_kind = ?1)"
                         "   AND (?2 = '' OR scope_id   = ?2)"
                         " ORDER BY confidence DESC"
                         " LIMIT ?3",
                         err, sizeof(err));
   if (!st)
      return 0;

   aimee_pg_bind_text(st, "?1", scope_kind ? scope_kind : "");
   aimee_pg_bind_text(st, "?2", scope_id ? scope_id : "");
   aimee_pg_bind_int64(st, "?3", (int64_t)max_out);
   while (n < max_out && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      memset(&out[n], 0, sizeof(out[n]));
      const char *v = aimee_pg_column_text(st, 0);
      if (v)
         snprintf(out[n].artifact_id, sizeof(out[n].artifact_id), "%s", v);
      v = aimee_pg_column_text(st, 1);
      if (v)
         snprintf(out[n].outcome, sizeof(out[n].outcome), "%s", v);
      out[n].score = aimee_pg_column_double(st, 2);
      n++;
   }
   aimee_pg_finalize(st);
   return n;
}

/* ---- kb_reasoning_contradiction_check ---- */

int kb_reasoning_contradiction_check(const char *artifact_a_id, const char *artifact_b_id)
{
   if (!artifact_a_id || !artifact_b_id)
      return -1;
   if (!config_reasoning_datalog_command()[0])
      return -1;

   char bindings[512];
   snprintf(bindings, sizeof(bindings), "{\"%s\":\"%s\",\"%s\":\"%s\"}", "?a", artifact_a_id, "?b",
            artifact_b_id);

   kb_reasoning_result_t result;
   int rc = kb_reasoning_query("contradiction_ok(?a, ?b)", bindings, NULL, NULL, &result);
   if (rc != 0)
      return -1;

   int valid = result.n_rows > 0 ? 1 : 0;
   kb_reasoning_result_free(&result);
   return valid;
}
