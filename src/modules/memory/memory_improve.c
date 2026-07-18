/* memory_improve.c: improve loop (dedupe, summarise, feedback re-weight) +
 * LLM-driven cognification + async cognify job queue. Extracted from
 * memory_advanced.c. */
#include "aimee.h"
#include "cJSON.h"
#include "db1_optional.h"
#if !defined(AIMEE_DB2_DISABLED)
#include "db2/entity_edges.h"
#include "db2/feedback.h"
#include "db2/memory_payload.h"
#include "db2/memory_query.h"
#include "db2/memory_relations.h"
#endif
#include "log.h"
#include "memory.h"
#include "memory_graph_fusion.h"
#include "memory_ontology.h"
#include "platform_process.h"
#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* Return the canonical episodic/semantic/procedural string for a raw kind
 * emitted by the cognifier, or NULL if the value is not recognised. */
#if !defined(AIMEE_DB2_DISABLED)
static const char *memory_cognify_canonical_kind(const char *raw)
{
   if (!raw || !raw[0])
      return NULL;
   if (strcmp(raw, MEMORY_UNIT_KIND_EPISODIC_STR) == 0)
      return MEMORY_UNIT_KIND_EPISODIC_STR;
   if (strcmp(raw, MEMORY_UNIT_KIND_SEMANTIC_STR) == 0)
      return MEMORY_UNIT_KIND_SEMANTIC_STR;
   if (strcmp(raw, MEMORY_UNIT_KIND_PROCEDURAL_STR) == 0)
      return MEMORY_UNIT_KIND_PROCEDURAL_STR;
   return NULL;
}
#endif

/* --- Memory Improve Loop --- */

/*
 * memory_improve_dedupe: find memories with identical keys across sessions and
 * mark duplicates with merged_into pointing to the highest-confidence canonical.
 * When dry_run=1, counts planned merges without writing.
 */
int memory_improve_dedupe(int dry_run)
{
#if defined(AIMEE_DB2_DISABLED)
   (void)dry_run;
   return 0;
#else
   return db2_memory_dedupe_by_key(dry_run);
#endif
}

/*
 * memory_improve_summarise: find clusters of at least min_cluster_size L1
 * memories with confidence <= max_confidence sharing a session.  Supersede the
 * cluster with a synthetic L2 observation.
 */
int memory_improve_summarise(int dry_run, int min_cluster_size, double max_confidence)
{
#if defined(AIMEE_DB2_DISABLED)
   (void)dry_run;
   (void)min_cluster_size;
   (void)max_confidence;
   return 0;
#else
   if (min_cluster_size <= 0)
      min_cluster_size = 3;
   if (max_confidence <= 0.0)
      max_confidence = 0.5;

   db2_memory_summary_cluster_t clusters[256];
   int cluster_count =
       db2_memory_summarise_clusters(max_confidence, min_cluster_size, clusters, 256);

   int summaries = 0;
   for (int i = 0; i < cluster_count; i++)
   {
      const char *session_id = clusters[i].session_id;
      int cnt = clusters[i].count;
      double avg_conf = clusters[i].avg_confidence;
      if (!session_id[0])
         continue;

      if (dry_run)
      {
         summaries++;
         continue;
      }

      char summary_content[512];
      snprintf(summary_content, sizeof(summary_content),
               "Observation summary: %d low-confidence L1 facts from session %s"
               " (avg_conf=%.2f) superseded by this L2 observation.",
               cnt, session_id, avg_conf);

      char summary_key[256];
      snprintf(summary_key, sizeof(summary_key), "summary:%s", session_id);

      memory_t summary;
      int rc = memory_insert(TIER_L2, KIND_FACT, summary_key, summary_content,
                             avg_conf + 0.1 > 1.0 ? 1.0 : avg_conf + 0.1, session_id, &summary);
      if (rc != 0)
         continue;

      db2_memory_mark_merged_into(summary.id, session_id, max_confidence);
      summaries++;
   }
   return summaries;
#endif
}

/*
 * memory_apply_feedback: update edge utility scores based on correctness.
 * On success: increment utility_score by 0.1 on entity_edges whose source/target
 * are cited in the referenced memories.
 * On failure: decrement by 0.1 and write a REL_CORRECTED_BY memory relation.
 */
int memory_apply_feedback(int success, int64_t *citation_ids, int citation_count)
{
#if defined(AIMEE_DB2_DISABLED)
   (void)success;
   (void)citation_ids;
   (void)citation_count;
   return 0;
#else
   if (!citation_ids || citation_count <= 0)
      return -1;

   double delta = success ? 0.1 : -0.1;

   for (int i = 0; i < citation_count; i++)
   {
      char key[512];
      char content[2048];
      if (db2_memory_get_key_content(citation_ids[i], key, sizeof(key), content, sizeof(content)) !=
          0)
         continue;

      /* Bump utility_score on every edge that touches this memory's key. */
      (void)db2_entity_edge_bump_utility(key, delta);

      /* On failure, write a corrected_by relation so the same mistake
       * is not repeated and the relation is queryable. */
      if (!success)
         db2_memory_relation_insert(citation_ids[i], key, "corrected_by", key,
                                    "Feedback correction");
   }
   return 0;
#endif
}

/*
 * memory_apply_feedback_path: Phase 7 path-credit feedback.
 * Distributes the feedback delta across the edges of the retrieval path so
 * the total applied credit equals the delta, weighting by relation gravity
 * and hop decay.  Each path node receives a fraction of the delta on its
 * incident entity_edges via db2_entity_edge_bump_utility (which also stamps
 * utility_touched_at and clamps stored utility).
 */
int memory_apply_feedback_path(int success, const char **path_node_keys, const char **relations,
                               const int *hops, int path_len)
{
#if defined(AIMEE_DB2_DISABLED)
   (void)success;
   (void)path_node_keys;
   (void)relations;
   (void)hops;
   (void)path_len;
   return 0;
#else
   if (!path_node_keys || path_len <= 0)
      return -1;

   double delta = success ? 0.1 : -0.1;

   /* Build the path-edge descriptor array (bounded to a sane path length). */
   if (path_len > 32)
      path_len = 32;
   memory_graph_path_edge_t edges[32];
   double credits[32];
   for (int i = 0; i < path_len; i++)
   {
      snprintf(edges[i].relation, sizeof(edges[i].relation), "%s",
               (relations && relations[i]) ? relations[i] : "");
      edges[i].hop = (hops && hops[i] > 0) ? hops[i] : (i + 1);
   }

   if (memory_graph_distribute_path_credit(delta, edges, path_len, credits) != 0)
   {
      /* Degenerate path — fall back to full delta on every node key. */
      for (int i = 0; i < path_len; i++)
         if (path_node_keys[i] && path_node_keys[i][0])
            (void)db2_entity_edge_bump_utility(path_node_keys[i], delta);
      return 0;
   }

   for (int i = 0; i < path_len; i++)
      if (path_node_keys[i] && path_node_keys[i][0])
         (void)db2_entity_edge_bump_utility(path_node_keys[i], credits[i]);
   return 0;
#endif
}

/* --- LLM-Driven Cognification --- */

/*
 * memory_cognify_parse_response: parse the JSON string returned by the cognifier
 * external command into a memory_cognify_result_t.
 *
 * Expected schema:
 * {
 *   "summary": "...",
 *   "memory_kind": "episodic",
 *   "relations": [
 *     {"subject": "Alice", "relation": "PARTICIPATED_IN", "object": "camping_2023",
 *      "fact_text": "Alice went camping in 2023"}
 *   ],
 *   "claims": [
 *     {"subject": "server", "attribute": "port", "value": "5432", "kind": "fact"},
 *     {"subject": "typescript", "attribute": "preference", "value": "avoid", "kind": "opinion"}
 *   ]
 * }
 *
 * Unknown / missing fields are tolerated; parsing is best-effort.
 */
int memory_cognify_parse_response(const char *json, memory_cognify_result_t *out)
{
   if (!json || !out)
      return -1;
   memset(out, 0, sizeof(*out));

   cJSON *j = cJSON_Parse(json);
   if (!j)
      return -1;

   cJSON *summary = cJSON_GetObjectItemCaseSensitive(j, "summary");
   if (cJSON_IsString(summary) && summary->valuestring[0])
      snprintf(out->summary, sizeof(out->summary), "%s", summary->valuestring);

   cJSON *mk = cJSON_GetObjectItemCaseSensitive(j, "memory_kind");
   if (cJSON_IsString(mk) && mk->valuestring[0])
      snprintf(out->memory_kind, sizeof(out->memory_kind), "%s", mk->valuestring);

   /* Parse relations array */
   cJSON *rels = cJSON_GetObjectItemCaseSensitive(j, "relations");
   if (cJSON_IsArray(rels))
   {
      cJSON *rel;
      cJSON_ArrayForEach(rel, rels)
      {
         if (out->relation_count >= COGNIFY_MAX_RELATIONS)
            break;
         cJSON *subj = cJSON_GetObjectItemCaseSensitive(rel, "subject");
         cJSON *rel_name = cJSON_GetObjectItemCaseSensitive(rel, "relation");
         cJSON *obj = cJSON_GetObjectItemCaseSensitive(rel, "object");
         cJSON *ft = cJSON_GetObjectItemCaseSensitive(rel, "fact_text");

         if (!cJSON_IsString(subj) || !cJSON_IsString(rel_name) || !cJSON_IsString(obj))
            continue;

         cognify_relation_t *r = &out->relations[out->relation_count++];
         snprintf(r->src_entity, sizeof(r->src_entity), "%s", subj->valuestring);
         snprintf(r->relation, sizeof(r->relation), "%s", rel_name->valuestring);
         snprintf(r->dst_entity, sizeof(r->dst_entity), "%s", obj->valuestring);
         if (cJSON_IsString(ft) && ft->valuestring[0])
            snprintf(r->fact_text, sizeof(r->fact_text), "%s", ft->valuestring);
      }
   }

   /* Parse claims array */
   cJSON *claims = cJSON_GetObjectItemCaseSensitive(j, "claims");
   if (cJSON_IsArray(claims))
   {
      cJSON *claim;
      cJSON_ArrayForEach(claim, claims)
      {
         if (out->claim_count >= COGNIFY_MAX_CLAIMS)
            break;
         cJSON *subj = cJSON_GetObjectItemCaseSensitive(claim, "subject");
         cJSON *attr = cJSON_GetObjectItemCaseSensitive(claim, "attribute");
         cJSON *val = cJSON_GetObjectItemCaseSensitive(claim, "value");
         cJSON *kind = cJSON_GetObjectItemCaseSensitive(claim, "kind");

         if (!cJSON_IsString(subj) || !cJSON_IsString(attr) || !cJSON_IsString(val))
            continue;

         cognify_claim_t *c = &out->claims[out->claim_count++];
         snprintf(c->subject, sizeof(c->subject), "%s", subj->valuestring);
         snprintf(c->attribute, sizeof(c->attribute), "%s", attr->valuestring);
         snprintf(c->value, sizeof(c->value), "%s", val->valuestring);
         if (cJSON_IsString(kind) && kind->valuestring[0])
            snprintf(c->kind, sizeof(c->kind), "%s", kind->valuestring);
         else
            snprintf(c->kind, sizeof(c->kind), "%s", KIND_FACT);
      }
   }

   /* Parse coref_bindings array */
   cJSON *cbs = cJSON_GetObjectItemCaseSensitive(j, "coref_bindings");
   if (cJSON_IsArray(cbs))
   {
      cJSON *cb;
      cJSON_ArrayForEach(cb, cbs)
      {
         if (out->coref_count >= COGNIFY_MAX_COREF)
            break;
         cJSON *pronoun = cJSON_GetObjectItemCaseSensitive(cb, "pronoun");
         cJSON *entity = cJSON_GetObjectItemCaseSensitive(cb, "entity");
         cJSON *conf = cJSON_GetObjectItemCaseSensitive(cb, "confidence");

         if (!cJSON_IsString(entity) || !entity->valuestring[0])
            continue;

         cognify_coref_binding_t *b = &out->coref_bindings[out->coref_count++];
         if (cJSON_IsString(pronoun) && pronoun->valuestring[0])
            snprintf(b->pronoun, sizeof(b->pronoun), "%s", pronoun->valuestring);
         snprintf(b->entity, sizeof(b->entity), "%s", entity->valuestring);
         b->confidence = cJSON_IsNumber(conf) ? conf->valuedouble : 0.5;
      }
   }

   cJSON_Delete(j);
   return 0;
}

/*
 * memory_cognify_unit: call the cognifier external command on a memory unit,
 * parse the result, persist relations into memory_relations, and write
 * extracted claims as new memories (kind='opinion' for subjective claims,
 * kind='fact' for evidence-backed claims).
 *
 * Returns 0 on success, -1 if disabled / command error.
 */
int memory_cognify_unit(int64_t memory_id, const char *text, const config_t *cfg,
                        memory_cognify_result_t *out)
{
   memory_cognify_result_t local;
   if (!out)
      out = &local;
   memset(out, 0, sizeof(*out));

   if (!cfg || !cfg->memory_cognify_enabled)
      return -1;
   if (!cfg->memory_cognify_command[0])
      return -1;
   if (!text || !text[0])
      return -1;

#if defined(AIMEE_DB2_DISABLED)
   if (cfg->memory_cognify_async_enabled && memory_id > 0 && db1_cognify_job_enqueue)
   {
      db1_cognify_job_enqueue(memory_id);
      return 0;
   }
   return -1;
#else
   /* When async is enabled, enqueue and return immediately; a drain worker
    * will process the job later via memory_cognify_drain(). */
   if (cfg->memory_cognify_async_enabled && memory_id > 0 && db1_cognify_job_enqueue)
   {
      db1_cognify_job_enqueue(memory_id);
      return 0;
   }

   /* Build input JSON: {unit_id, text} */
   cJSON *input = cJSON_CreateObject();
   if (!input)
      return -1;
   cJSON_AddNumberToObject(input, "unit_id", (double)memory_id);
   cJSON_AddStringToObject(input, "text", text);
   char *input_str = cJSON_PrintUnformatted(input);
   cJSON_Delete(input);
   if (!input_str)
      return -1;

   char *resp = NULL;
   size_t resp_len = 0;
   int rc = platform_exec_pipe(cfg->memory_cognify_command, input_str, strlen(input_str), &resp,
                               &resp_len);
   free(input_str);
   if (rc != 0 || !resp || resp_len == 0)
   {
      free(resp);
      aimee_log(LOG_WARN, "memory_cognify", "cognifier command failed (exit %d)", rc);
      return -1;
   }

   rc = memory_cognify_parse_response(resp, out);
   free(resp);
   if (rc != 0)
   {
      aimee_log(LOG_WARN, "memory_cognify", "cognifier returned invalid JSON");
      return -1;
   }

   /* Persist the LLM-emitted memory_kind directly onto the memory so the
    * unit-graph refresh can consume it rather than re-inferring from
    * unit_type heuristics. */
   const char *canon_kind = memory_cognify_canonical_kind(out->memory_kind);
   if (canon_kind && memory_id > 0)
      db2_memory_set_cognified_kind(memory_id, canon_kind);

   /* Persist extracted S-R-O relations into memory_relations */
   for (int i = 0; i < out->relation_count; i++)
   {
      cognify_relation_t *r = &out->relations[i];
      db2_memory_relation_insert(memory_id, r->src_entity, r->relation, r->dst_entity,
                                 r->fact_text);
   }

   /* Persist claims as memories. Opinions get stored with kind='opinion' and a
    * lower initial confidence than facts, reflecting the proposal's promotion
    * threshold distinction. Claims that look behavioural (preference / policy /
    * procedure / workflow) are mirrored into the canonical feedback-rules store
    * so the agent actually reads them on every turn, instead of leaving a
    * procedural preference stranded as a generic fact memory. */
   int procedural_memory = canon_kind && strcmp(canon_kind, MEMORY_UNIT_KIND_PROCEDURAL_STR) == 0;
   for (int i = 0; i < out->claim_count; i++)
   {
      cognify_claim_t *cl = &out->claims[i];

      /* Build a key: "subject:attribute" */
      char key[256];
      snprintf(key, sizeof(key), "%s:%s", cl->subject, cl->attribute);

      const char *kind = cl->kind[0] ? cl->kind : KIND_FACT;
      /* Opinions start at lower confidence and need more evidence to promote */
      double confidence = (strcmp(kind, KIND_OPINION) == 0) ? 0.5 : 0.8;

      memory_t m;
      memory_insert(TIER_L2, kind, key, cl->value, confidence, "cognify", &m);

      int behavioural_claim =
          (strcmp(kind, KIND_PREFERENCE) == 0 || strcmp(kind, KIND_POLICY) == 0 ||
           strcmp(kind, KIND_PROCEDURE) == 0 || strcmp(kind, KIND_WORKFLOW) == 0);
      if ((procedural_memory || behavioural_claim) && key[0] && cl->value[0])
      {
         int reinforced = 0;

         db2_feedback_record("principle", key, cl->value, -1, &reinforced);
      }
   }

   return 0;
#endif
}

/* --- Async Cognification Job Queue --- */

int memory_cognify_queue_status(memory_cognify_queue_stats_t *out)
{
   db1_cognify_job_stats_t db1_stats;
   if (!out)
      return -1;
   memset(out, 0, sizeof(*out));
   if (!db1_cognify_job_status)
      return 0;
   if (db1_cognify_job_status(&db1_stats) != 0)
      return -1;
   out->pending = db1_stats.pending;
   out->running = db1_stats.running;
   out->done = db1_stats.done;
   out->failed = db1_stats.failed;
   out->total = db1_stats.total;
   return 0;
}

/* Claim the next pending cognify job.  Returns 1 if a job was claimed
 * (job_id and memory_id set), 0 if none available, -1 on error. */
#if !defined(AIMEE_DB2_DISABLED)
static int mcj_claim_next(int64_t *job_id, int64_t *memory_id, int *attempts, int *max_attempts)
{
   if (!job_id || !memory_id || !attempts || !max_attempts)
      return -1;
   if (!db1_cognify_job_claim_next)
      return 0;
   db1_cognify_job_t job;
   int rc = db1_cognify_job_claim_next(&job);
   if (rc != 1)
      return rc;
   *job_id = job.id;
   *memory_id = job.memory_id;
   *attempts = job.attempts;
   *max_attempts = job.max_attempts;
   return 1;
}
#endif

int memory_cognify_drain(const config_t *cfg, int timeout_secs, memory_cognify_queue_stats_t *out)
{
#if defined(AIMEE_DB2_DISABLED)
   (void)cfg;
   (void)timeout_secs;
   if (out)
      return memory_cognify_queue_status(out);
   return 0;
#else
   if (!cfg)
      return -1;

   time_t started = time(NULL);
   memory_cognify_queue_stats_t stats;
   memset(&stats, 0, sizeof(stats));

   for (;;)
   {
      int64_t job_id = 0, mid = 0;
      int attempts = 0, max_att = 3;
      int claim = mcj_claim_next(&job_id, &mid, &attempts, &max_att);
      if (claim < 0)
         return -1;
      if (claim == 0)
      {
         memory_cognify_queue_status(&stats);
         if (stats.running == 0)
            break;
         if (timeout_secs > 0 && (int)(time(NULL) - started) >= timeout_secs)
            break;
         usleep(100000);
         continue;
      }

      char content[4096] = "";
      db2_memory_get_content(mid, content, sizeof(content));

      int rc = -1;
      char errbuf[256] = "";
      if (content[0])
      {
         memory_cognify_result_t result;
         rc = memory_cognify_unit(mid, content, cfg, &result);
         if (rc != 0)
            snprintf(errbuf, sizeof(errbuf), "cognify_unit failed for memory %lld", (long long)mid);
      }
      else
         snprintf(errbuf, sizeof(errbuf), "memory %lld content missing", (long long)mid);

      if (rc == 0)
      {
         if (db1_cognify_job_mark)
            db1_cognify_job_mark(job_id, "done", "");
         stats.processed++;
      }
      else
      {
         /* Retry if under max_attempts, otherwise mark permanently failed */
         if (attempts < max_att)
         {
            if (db1_cognify_job_mark)
               db1_cognify_job_mark(job_id, "pending", errbuf);
            stats.retried++;
         }
         else
         {
            if (db1_cognify_job_mark)
               db1_cognify_job_mark(job_id, "failed", errbuf);
         }
         stats.processed++;
      }

      if (timeout_secs > 0 && (int)(time(NULL) - started) >= timeout_secs)
         break;
   }

   memory_cognify_queue_status(&stats);
   if (out)
      *out = stats;
   return 0;
#endif
}
