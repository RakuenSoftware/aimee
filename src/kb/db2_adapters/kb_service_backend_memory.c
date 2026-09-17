/* kb/db2_adapters/kb_service_backend_memory.c: caller-side memory RPC adapters for
 * aimee-kb (find_facts, list, get, insert, briefing, context_block,
 * entity_profile, entity_edges). This policy/JSON composition layer remains
 * with aimee-kb while its storage calls migrate to the generated DB2 client. */

#include "kb_service_backend.h"

#include "aimee.h"
#include "log.h" /* aimee_log — a failed recall must not read as an empty one */
#include "config.h"
#include "modules/db2/c/entity_registry.h" /* db2_entity_merge / db2_entity_unmerge */
#include "modules/db2/c/fact_ingest.h"     /* db2_typed_fact_ingress */
#include "modules/db2/c/fact_lifecycle.h"  /* db2_fact_retract, FACT_RETRACT_IMMUTABLE */
#include "modules/db2/c/fact_recall.h"     /* db2_fact_recall_in_query */
#include "modules/db2/c/kb_payload.h"      /* db2_kb_async_enqueue */
#include "modules/db2/c/decision_log.h"
#include "memory.h"
#include <aimee/memory/module_api.h> /* memory_content_dup / memory_valid_at */
#include "modules/db2/c/memory_payload.h"
#include "modules/db2/c/memory_query.h"
#include "modules/db2/c/memory_scope_query.h"
#include "session_briefing.h"
#include "modules/db2/c/tasks.h"

#include <stdlib.h>
#include <string.h>

static cJSON *kbs_memory_row_to_json(const memory_t *m)
{
   cJSON *obj = cJSON_CreateObject();
   if (!obj)
      return NULL;
   cJSON_AddNumberToObject(obj, "id", (double)m->id);
   cJSON_AddStringToObject(obj, "tier", m->tier);
   cJSON_AddStringToObject(obj, "kind", m->kind);
   cJSON_AddStringToObject(obj, "key", m->key);
   db2_memory_summary_row_t summaries[4];
   int summary_n = db2_memory_summaries_list(m->id, 4, summaries, 4);
   const char *headline = "";
   for (int i = 0; i < summary_n; i++)
      if (strcmp(summaries[i].scope, "headline") == 0 && summaries[i].summary[0])
      {
         headline = summaries[i].summary;
         break;
      }
   if (!headline[0] && summary_n > 0)
      headline = summaries[0].summary;
   cJSON_AddStringToObject(obj, "headline", headline);
   cJSON_AddStringToObject(obj, "content", m->content);
   cJSON_AddStringToObject(obj, "use_cases", m->use_cases);
   cJSON_AddNumberToObject(obj, "confidence", m->confidence);
   cJSON_AddNumberToObject(obj, "use_count", m->use_count);
   cJSON_AddStringToObject(obj, "last_used_at", m->last_used_at);
   cJSON_AddStringToObject(obj, "created_at", m->created_at);
   cJSON_AddStringToObject(obj, "updated_at", m->updated_at);
   cJSON_AddStringToObject(obj, "source_session", m->source_session);
   cJSON_AddStringToObject(obj, "provenance_category", m->provenance_category);
   cJSON_AddNumberToObject(obj, "retrieval_score", m->retrieval_score);
   cJSON_AddNumberToObject(obj, "hybrid_rank", m->hybrid_rank);
   return obj;
}

/* memory_t intentionally remains a bounded ranking/working-set value, but a
 * read-by-id response is an audit surface and must return the row verbatim.
 * Fetch content separately so this one JSON path does not inherit the
 * memory_t.content[2048] cap. The same scope predicate as db2_memory_get keeps
 * the second query from widening visibility. */
cJSON *db2_kb_service_memory_find_facts_json(const char *query, int limit)
{
   if (limit < 1)
      limit = 20;
   if (limit > 64)
      limit = 64;

   cJSON *resp = cJSON_CreateObject();
   cJSON *arr = resp ? cJSON_AddArrayToObject(resp, "facts") : NULL;
   if (!resp || !arr)
   {
      cJSON_Delete(resp);
      return NULL;
   }

   memory_t facts[64];
   db2_memory_scope_context_t scope;
   db2_memory_scope_context_get(&scope);
   int n = scope.active
               ? memory_find_facts_visible_ex(query ? query : "", scope.workspace, scope.project,
                                              scope.include_all, limit, facts, 64)
               : memory_find_facts(query ? query : "", limit, facts, 64);
   if (n < 0)
   {
      cJSON_AddStringToObject(resp, "status", "error");
      cJSON_AddStringToObject(resp, "message", "memory retrieval index unavailable");
      return resp;
   }
   cJSON_AddStringToObject(resp, "status", "ok");
   for (int i = 0; i < n; i++)
   {
      cJSON *obj = kbs_memory_row_to_json(&facts[i]);
      if (!obj)
      {
         cJSON_Delete(resp);
         return NULL;
      }
      cJSON_AddItemToArray(arr, obj);
   }
   return resp;
}

static cJSON *kbs_memory_diagnostic_to_json(const memory_diagnostic_t *d)
{
   cJSON *j = cJSON_CreateObject();
   if (!j)
      return NULL;
   cJSON *mem = kbs_memory_row_to_json(&d->memory);
   if (!mem)
   {
      cJSON_Delete(j);
      return NULL;
   }
   cJSON_AddItemToObject(j, "memory", mem);
   cJSON *parts = cJSON_AddObjectToObject(j, "parts");
   cJSON_AddNumberToObject(parts, "lexical", d->parts.lexical);
   cJSON_AddNumberToObject(parts, "coverage", d->parts.coverage);
   cJSON_AddNumberToObject(parts, "entity", d->parts.entity);
   cJSON_AddNumberToObject(parts, "temporal", d->parts.temporal);
   cJSON_AddNumberToObject(parts, "evidence", d->parts.evidence);
   cJSON_AddNumberToObject(parts, "semantic", d->parts.semantic);
   cJSON_AddNumberToObject(parts, "state", d->parts.state);
   cJSON_AddNumberToObject(parts, "intent", d->parts.intent);
   cJSON_AddNumberToObject(parts, "confidence", d->parts.confidence);
   cJSON_AddNumberToObject(parts, "salience", d->parts.salience);
   cJSON_AddNumberToObject(parts, "surprise", d->parts.surprise);
   cJSON_AddNumberToObject(parts, "pagerank", d->parts.pagerank);
   cJSON_AddNumberToObject(parts, "hybrid_total", d->parts.hybrid_total);
   cJSON_AddNumberToObject(parts, "blended_total", d->parts.blended_total);
   cJSON_AddNumberToObject(parts, "graph_score", d->parts.graph_score);
   cJSON_AddNumberToObject(parts, "graph_weight", d->parts.graph_weight);
   cJSON_AddNumberToObject(parts, "code_proximity", d->parts.code_proximity);
   cJSON_AddNumberToObject(parts, "utility", d->parts.utility);
   cJSON_AddNumberToObject(parts, "outcome", d->parts.outcome);
   cJSON_AddNumberToObject(parts, "source_fusion", d->parts.source_fusion);
   cJSON_AddNumberToObject(parts, "total", d->parts.total);
   return j;
}

cJSON *db2_kb_service_memory_diagnose_scoped_json(const char *query, const char *scope_type,
                                                  const char *scope_value, int limit)
{
   if (limit < 1)
      limit = 10;
   if (limit > 64)
      limit = 64;

   cJSON *resp = cJSON_CreateObject();
   cJSON *arr = resp ? cJSON_AddArrayToObject(resp, "rows") : NULL;
   if (!resp || !arr)
   {
      cJSON_Delete(resp);
      return NULL;
   }

   memory_diagnostic_t rows[64];
   int n = memory_diagnose_scoped(query ? query : "", scope_type, scope_value, limit, rows, 64);
   if (n < 0)
   {
      cJSON_AddStringToObject(resp, "status", "error");
      cJSON_AddStringToObject(resp, "message", "memory retrieval index unavailable");
      return resp;
   }
   cJSON_AddStringToObject(resp, "status", "ok");
   for (int i = 0; i < n; i++)
   {
      cJSON *obj = kbs_memory_diagnostic_to_json(&rows[i]);
      if (!obj)
      {
         cJSON_Delete(resp);
         return NULL;
      }
      cJSON_AddItemToArray(arr, obj);
   }
   return resp;
}

cJSON *db2_kb_service_memory_explain_match_json(const char *query, int64_t memory_id)
{
   cJSON *resp = cJSON_CreateObject();
   if (!resp)
      return NULL;
   memory_diagnostic_t row;
   memset(&row, 0, sizeof(row));
   if (memory_explain_match(query ? query : "", memory_id, &row) != 0)
   {
      cJSON_AddStringToObject(resp, "status", "error");
      cJSON_AddStringToObject(resp, "message",
                              "explain failed (memory retrieval unavailable or id missing)");
      return resp;
   }
   cJSON_AddStringToObject(resp, "status", "ok");
   cJSON *row_j = kbs_memory_diagnostic_to_json(&row);
   if (row_j)
      cJSON_AddItemToObject(resp, "row", row_j);
   return resp;
}

cJSON *db2_kb_service_memory_find_facts_visible_json(const char *query, const char *workspace,
                                                     const char *project, int limit)
{
   if (limit < 1)
      limit = 20;
   if (limit > 64)
      limit = 64;

   cJSON *resp = cJSON_CreateObject();
   cJSON *arr = resp ? cJSON_AddArrayToObject(resp, "facts") : NULL;
   if (!resp || !arr)
   {
      cJSON_Delete(resp);
      return NULL;
   }

   memory_t facts[64];
   int n = memory_find_facts_visible(query ? query : "", workspace, project, limit, facts, 64);
   if (n < 0)
   {
      cJSON_AddStringToObject(resp, "status", "error");
      cJSON_AddStringToObject(resp, "message", "memory retrieval index unavailable");
      return resp;
   }
   cJSON_AddStringToObject(resp, "status", "ok");
   for (int i = 0; i < n; i++)
   {
      cJSON *obj = kbs_memory_row_to_json(&facts[i]);
      if (!obj)
      {
         cJSON_Delete(resp);
         return NULL;
      }
      cJSON_AddItemToArray(arr, obj);
   }
   return resp;
}

cJSON *db2_kb_service_memory_find_facts_scoped_json(const char *query, const char *scope_type,
                                                    const char *scope_value, int limit)
{
   if (limit < 1)
      limit = 20;
   if (limit > 64)
      limit = 64;

   cJSON *resp = cJSON_CreateObject();
   cJSON *arr = resp ? cJSON_AddArrayToObject(resp, "facts") : NULL;
   if (!resp || !arr)
   {
      cJSON_Delete(resp);
      return NULL;
   }

   memory_t facts[64];
   int n = memory_find_facts_scoped(query ? query : "", scope_type ? scope_type : "",
                                    scope_value ? scope_value : "", limit, facts, 64);
   if (n < 0)
   {
      cJSON_AddStringToObject(resp, "status", "error");
      cJSON_AddStringToObject(resp, "message", "memory retrieval index unavailable");
      return resp;
   }
   cJSON_AddStringToObject(resp, "status", "ok");
   for (int i = 0; i < n; i++)
   {
      cJSON *obj = kbs_memory_row_to_json(&facts[i]);
      if (!obj)
      {
         cJSON_Delete(resp);
         return NULL;
      }
      cJSON_AddItemToArray(arr, obj);
   }
   return resp;
}

cJSON *db2_kb_service_session_briefing_commitments_json(int limit)
{
   cJSON *resp = cJSON_CreateObject();
   if (!resp)
      return NULL;
   char *body = session_briefing_render_commitments(limit);
   cJSON_AddStringToObject(resp, "status", "ok");
   cJSON_AddStringToObject(resp, "body", body ? body : "");
   free(body);
   return resp;
}

cJSON *db2_kb_service_session_briefing_directives_json(int limit)
{
   cJSON *resp = cJSON_CreateObject();
   if (!resp)
      return NULL;
   char *body = session_briefing_render_directives(limit);
   cJSON_AddStringToObject(resp, "status", "ok");
   cJSON_AddStringToObject(resp, "body", body ? body : "");
   free(body);
   return resp;
}

cJSON *db2_kb_service_memory_context_block_json(const char *query, const char *block_type,
                                                int limit, fact_authority_t authority)
{
   cJSON *resp = cJSON_CreateObject();
   if (!resp)
      return NULL;

   /* typed-fact ingress (§4/§6/§7), KB-side (db2 is live here). Unconditional:
    * the config.typed_facts_enabled master gate is retired. Orchestration lives
    * in fact_ingest.
    * `authority` is the caller's authenticated write authority, resolved by the
    * RPC handler — this call can retract facts, so it must not be inferred from
    * `query`, which the caller supplies. */
   char facts[2048] = "";
   (void)db2_typed_fact_ingress(query, authority, facts, sizeof(facts));

   char *block = memory_get_context_block(query ? query : "",
                                          (block_type && block_type[0]) ? block_type : "general",
                                          limit > 0 ? limit : 5);

   cJSON_AddStringToObject(resp, "status", "ok");
   if (facts[0])
   {
      const char *bl = block ? block : "";
      size_t need = strlen(bl) + strlen(facts) + 32;
      char *combined = malloc(need);
      if (combined)
      {
         snprintf(combined, need, "%s\n## Known facts\n%s", bl, facts);
         cJSON_AddStringToObject(resp, "block", combined);
         free(combined);
      }
      else
      {
         cJSON_AddStringToObject(resp, "block", bl);
      }
   }
   else
   {
      cJSON_AddStringToObject(resp, "block", block ? block : "");
   }
   free(block);
   return resp;
}

/* Read-only typed-fact recall (§7), PII-gated: the cheap path ingress_preinject
 * calls every turn. No write; facts="" when there are none. */
cJSON *db2_kb_service_memory_facts_json(const char *query)
{
   cJSON *resp = cJSON_CreateObject();
   if (!resp)
      return NULL;
   char facts[2048] = "";
   if (query && query[0])
   {
      /* A FAILED RECALL IS NOT AN EMPTY ONE. This discarded the return, so a
       * negative (db2 unavailable) produced facts="" and a "status":"ok"
       * response -- indistinguishable from "this turn has no facts", on the path
       * ingress_preinject calls EVERY turn. The model would then answer without
       * the user's facts and nothing would say why.
       *
       * db2_typed_fact_ingress() already logs this exact condition, with the
       * note that "recall affects prompt content, so a persistent failure is
       * worth surfacing". The same call on this sibling path was left silent --
       * the defect repeating where the reasoning had already been written down.
       *
       * Still a soft failure: the turn proceeds without facts rather than
       * erroring, which is the right trade for a read. It must not be silent. */
      int fr = db2_fact_recall_in_query(query, facts, sizeof(facts));
      if (fr < 0)
         aimee_log(LOG_WARN, "memory",
                   "typed-fact recall failed (db2 unavailable?); answering with no facts");
   }
   cJSON_AddStringToObject(resp, "status", "ok");
   cJSON_AddStringToObject(resp, "facts", facts);
   return resp;
}

static cJSON *kbs_task_row_to_json(const aimee_task_t *t)
{
   cJSON *obj = cJSON_CreateObject();
   if (!obj)
      return NULL;
   cJSON_AddNumberToObject(obj, "id", (double)t->id);
   cJSON_AddNumberToObject(obj, "parent_id", (double)t->parent_id);
   cJSON_AddStringToObject(obj, "title", t->title);
   cJSON_AddStringToObject(obj, "state", t->state);
   cJSON_AddNumberToObject(obj, "confidence", t->confidence);
   cJSON_AddStringToObject(obj, "created_at", t->created_at);
   cJSON_AddStringToObject(obj, "updated_at", t->updated_at);
   cJSON_AddStringToObject(obj, "session_id", t->session_id);
   return obj;
}

cJSON *db2_kb_service_task_create_json(const char *title, const char *session_id, int64_t parent_id)
{
   cJSON *resp = cJSON_CreateObject();
   if (!resp)
      return NULL;
   aimee_task_t task;
   memset(&task, 0, sizeof(task));
   if (db2_task_create(title ? title : "", session_id ? session_id : "", parent_id, &task) != 0)
   {
      cJSON_AddStringToObject(resp, "status", "error");
      cJSON_AddStringToObject(resp, "message", "failed to create task");
      return resp;
   }
   cJSON_AddStringToObject(resp, "status", "ok");
   cJSON *obj = kbs_task_row_to_json(&task);
   if (obj)
      cJSON_AddItemToObject(resp, "task", obj);
   return resp;
}

cJSON *db2_kb_service_task_update_state_json(int64_t id, const char *state)
{
   cJSON *resp = cJSON_CreateObject();
   if (!resp)
      return NULL;
   int rc = db2_task_update_state(id, state ? state : "");
   if (rc != 0)
   {
      cJSON_AddStringToObject(resp, "status", "error");
      cJSON_AddStringToObject(resp, "message", "failed to update task state");
      return resp;
   }
   cJSON_AddStringToObject(resp, "status", "ok");
   return resp;
}

cJSON *db2_kb_service_task_delete_json(int64_t id)
{
   cJSON *resp = cJSON_CreateObject();
   if (!resp)
      return NULL;
   int rc = db2_task_delete(id);
   if (rc != 0)
   {
      cJSON_AddStringToObject(resp, "status", "error");
      cJSON_AddStringToObject(resp, "message", "failed to delete task");
      return resp;
   }
   cJSON_AddStringToObject(resp, "status", "ok");
   return resp;
}

cJSON *db2_kb_service_task_add_edge_json(int64_t source, int64_t target, const char *relation)
{
   cJSON *resp = cJSON_CreateObject();
   if (!resp)
      return NULL;
   int rc = db2_task_add_edge(source, target, relation ? relation : "depends_on");
   if (rc != 0)
   {
      cJSON_AddStringToObject(resp, "status", "error");
      cJSON_AddStringToObject(resp, "message", "failed to add task edge");
      return resp;
   }
   cJSON_AddStringToObject(resp, "status", "ok");
   return resp;
}

cJSON *db2_kb_service_task_get_edges_json(int64_t task_id, int max)
{
   if (max < 1)
      max = 16;
   if (max > 64)
      max = 64;

   cJSON *resp = cJSON_CreateObject();
   cJSON *arr = resp ? cJSON_AddArrayToObject(resp, "edges") : NULL;
   if (!resp || !arr)
   {
      cJSON_Delete(resp);
      return NULL;
   }
   cJSON_AddStringToObject(resp, "status", "ok");

   task_edge_t edges[64];
   int n = db2_task_get_edges(task_id, edges, max);
   for (int i = 0; i < n; i++)
   {
      cJSON *e = cJSON_CreateObject();
      if (!e)
      {
         cJSON_Delete(resp);
         return NULL;
      }
      cJSON_AddNumberToObject(e, "id", (double)edges[i].id);
      cJSON_AddNumberToObject(e, "source_id", (double)edges[i].source_id);
      cJSON_AddNumberToObject(e, "target_id", (double)edges[i].target_id);
      cJSON_AddStringToObject(e, "relation", edges[i].relation);
      cJSON_AddItemToArray(arr, e);
   }
   return resp;
}

cJSON *db2_kb_service_task_list_json(const char *state, const char *session_id, int limit)
{
   if (limit < 1)
      limit = 16;
   if (limit > 64)
      limit = 64;

   cJSON *resp = cJSON_CreateObject();
   cJSON *arr = resp ? cJSON_AddArrayToObject(resp, "tasks") : NULL;
   if (!resp || !arr)
   {
      cJSON_Delete(resp);
      return NULL;
   }
   cJSON_AddStringToObject(resp, "status", "ok");

   aimee_task_t rows[64];
   int n = db2_task_list((state && state[0]) ? state : NULL,
                         (session_id && session_id[0]) ? session_id : NULL, limit, rows, 64);
   for (int i = 0; i < n; i++)
   {
      cJSON *obj = cJSON_CreateObject();
      if (!obj)
      {
         cJSON_Delete(resp);
         return NULL;
      }
      cJSON_AddNumberToObject(obj, "id", (double)rows[i].id);
      cJSON_AddNumberToObject(obj, "parent_id", (double)rows[i].parent_id);
      cJSON_AddStringToObject(obj, "title", rows[i].title);
      cJSON_AddStringToObject(obj, "state", rows[i].state);
      cJSON_AddNumberToObject(obj, "confidence", rows[i].confidence);
      cJSON_AddStringToObject(obj, "created_at", rows[i].created_at);
      cJSON_AddStringToObject(obj, "updated_at", rows[i].updated_at);
      cJSON_AddStringToObject(obj, "session_id", rows[i].session_id);
      cJSON_AddItemToArray(arr, obj);
   }
   return resp;
}

cJSON *db2_kb_service_memory_ask_json(const char *query, const char *scope_type,
                                      const char *scope_value, int limit)
{
   cJSON *resp = cJSON_CreateObject();
   if (!resp)
      return NULL;

   memory_answer_result_t result;
   memset(&result, 0, sizeof(result));
   int rc;
   if (scope_type && scope_type[0])
      rc = memory_ask_query_scoped(query ? query : "", scope_type, scope_value,
                                   limit > 0 ? limit : 5, &result);
   else
      rc = memory_ask_query(query ? query : "", limit > 0 ? limit : 5, &result);
   if (rc != 0)
   {
      cJSON_AddStringToObject(resp, "status", "error");
      cJSON_AddStringToObject(resp, "message",
                              result.error[0] ? result.error : "memory_ask failed");
      return resp;
   }
   cJSON_AddStringToObject(resp, "status", "ok");
   cJSON_AddStringToObject(resp, "answer", result.answer);
   cJSON_AddNumberToObject(resp, "confidence", result.confidence);
   cJSON_AddStringToObject(resp, "evidence_mode", result.evidence_mode);
   cJSON_AddBoolToObject(resp, "no_answer", result.no_answer);
   cJSON_AddBoolToObject(resp, "low_confidence", result.low_confidence);
   cJSON_AddNumberToObject(resp, "retrieval_count", result.retrieval_count);
   cJSON *citations = cJSON_AddArrayToObject(resp, "citation_ids");
   for (int i = 0; i < result.citation_count; i++)
      cJSON_AddItemToArray(citations, cJSON_CreateNumber((double)result.citation_ids[i]));
   cJSON *trace = cJSON_AddObjectToObject(resp, "evidence_trace");
   if (trace)
   {
      cJSON_AddStringToObject(trace, "decision",
                              memory_answer_evidence_decision_str(&result.evidence));
      cJSON_AddStringToObject(trace, "reason", memory_answer_evidence_reason_str(&result.evidence));
      cJSON *ids = cJSON_AddArrayToObject(trace, "candidate_ids");
      for (int i = 0; ids && i < result.evidence.candidate_id_count; i++)
         cJSON_AddItemToArray(ids, cJSON_CreateNumber((double)result.evidence.candidate_ids[i]));
      cJSON_AddNumberToObject(trace, "ranked_count", result.evidence.ranked_count);
      cJSON_AddNumberToObject(trace, "anchor_id", (double)result.evidence.anchor_id);
      cJSON_AddNumberToObject(trace, "anchor_rank", result.evidence.anchor_rank);
      cJSON_AddNumberToObject(trace, "topk_grounding", result.evidence.topk_grounding);
      cJSON_AddNumberToObject(trace, "anchor_coverage", result.evidence.anchor_coverage);
      cJSON_AddNumberToObject(trace, "cluster_coverage", result.evidence.cluster_coverage);
      cJSON_AddNumberToObject(trace, "threshold", result.evidence.threshold);
      cJSON_AddNumberToObject(trace, "chunk_floor", result.evidence.chunk_floor);
      cJSON_AddBoolToObject(trace, "structural", result.evidence.structural);
      cJSON_AddBoolToObject(trace, "exempt", result.evidence.exempt);
      cJSON_AddBoolToObject(trace, "trace_truncated", result.evidence.trace_truncated);
   }
   return resp;
}

/* --- Typed-fact §4 retraction and §3 entity merge/unmerge ---
 *
 * These three primitives were built, tested, and left with no production caller:
 * a wrong entity merge was recorded and reversible in principle, with no way to
 * reverse one outside a test, and retraction was reachable only from the
 * pattern-extraction path in fact_ingest. They are the correction half of the
 * typed-fact layer — without a surface, the layer can learn a wrong fact but
 * cannot be told that it is wrong. */

cJSON *db2_kb_service_facts_retract_json(const char *source, const char *relation,
                                         const char *target, const char *authority)
{
   cJSON *resp = cJSON_CreateObject();
   if (!resp)
      return NULL;

   /* §4/§5 authority guard: only an explicit "user" authority may retract a
    * user-stated Class A fact. Anything else — including an absent or
    * unrecognised value — is treated as model authority, the conservative
    * reading, since a caller that cannot name its authority must not inherit the
    * user's.
    *
    * `authority` reaches here ALREADY RESOLVED against the caller's
    * authentication by the request boundary that has it — kb_handle_facts_retract
    * (authenticated actor) and facts_retract_command (attested transport). It is
    * not a field a client can set on the way in; do not add a path that forwards
    * a request body's value here unresolved. */
   fact_authority_t auth =
       (authority && strcmp(authority, "user") == 0) ? FACT_AUTHORITY_USER : FACT_AUTHORITY_MODEL;

   int rc = db2_fact_retract(source ? source : "", relation ? relation : "",
                             (target && target[0]) ? target : NULL, auth);
   if (rc == FACT_RETRACT_IMMUTABLE)
   {
      /* Distinct from a plain failure: the relation is immutable and this caller
       * lacks the authority to override it. Naming that lets a client explain the
       * refusal instead of retrying a request that can never succeed. */
      cJSON_AddStringToObject(resp, "status", "error");
      cJSON_AddStringToObject(resp, "reason", "immutable");
      cJSON_AddStringToObject(resp, "message",
                              "this relation is immutable; only a user authority may retract it");
      return resp;
   }
   if (rc < 0)
   {
      cJSON_AddStringToObject(resp, "status", "error");
      cJSON_AddStringToObject(resp, "message", "fact retraction failed");
      return resp;
   }

   cJSON_AddStringToObject(resp, "status", "ok");
   /* rc == 0 is success with nothing matched, NOT an error: retracting a fact
    * that is already gone leaves the caller in exactly the state they asked for.
    * The count is what distinguishes the two cases, so it is always reported. */
   cJSON_AddNumberToObject(resp, "retracted", rc);
   return resp;
}

cJSON *db2_kb_service_entities_merge_json(int64_t from_id, int64_t into_id)
{
   cJSON *resp = cJSON_CreateObject();
   if (!resp)
      return NULL;
   int64_t merge_id = db2_entity_merge(from_id, into_id);
   if (merge_id <= 0)
   {
      cJSON_AddStringToObject(resp, "status", "error");
      cJSON_AddStringToObject(resp, "message",
                              "merge refused: both ids must be distinct active entities");
      return resp;
   }
   cJSON_AddStringToObject(resp, "status", "ok");
   /* The audit id is the only handle a caller can later unmerge with, so it is
    * the one field this response must always carry. */
   cJSON_AddNumberToObject(resp, "merge_id", (double)merge_id);
   return resp;
}

cJSON *db2_kb_service_entities_unmerge_json(int64_t merge_id)
{
   cJSON *resp = cJSON_CreateObject();
   if (!resp)
      return NULL;
   if (db2_entity_unmerge(merge_id) != 0)
   {
      cJSON_AddStringToObject(resp, "status", "error");
      cJSON_AddStringToObject(resp, "message", "no such merge, or it was already undone");
      return resp;
   }
   cJSON_AddStringToObject(resp, "status", "ok");
   cJSON_AddNumberToObject(resp, "merge_id", (double)merge_id);
   return resp;
}
