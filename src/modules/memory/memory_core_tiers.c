#if defined(AIMEE_DB2_DISABLED)
#error "memory_core KB-real TU must not be compiled into the AIMEE_DB2_DISABLED (server) build"
#endif
#ifndef _GNU_SOURCE /* strcasestr/memmem are GNU extensions (container gcc) */
#define _GNU_SOURCE
#endif
#include "memory_core_internal.h"
/* memory_core_tiers.c: split from memory_core.c into a real translation unit
 * (was memory_core_tiers.inc, textually included only to stay under the
 * line-check ceiling). Cross-TU declarations live in the module header. */
#include "aimee.h"
#include "memory_context_internal.h"
#include "memory_rewrite_llm.h" /* weak in-process rewrite seam (KB build only) */
#include <math.h>
#include "db1_optional.h"
#include "modules/db2/c/entity_edges.h"
#include "modules/db2/c/kb_runtime_state.h"
#include "modules/db2/c/memory_health.h"
#include "modules/db2/c/memory_payload.h"
#include "modules/db2/c/feature_rows.h"
#include "modules/db2/c/memory_promotion.h"
#include "modules/db2/c/memory_query.h"
#include "memory_graph_fusion.h"
#include "kb_mdl.h"
#include "modules/db2/c/memory_relations.h"
#include "modules/db2/c/memory_scenes.h"
#include "modules/db2/c/stopwords.h"
#include "modules/db2/c/vector_index_ops.h"
#include "modules/db2/c/vector_verify.h"
#include "memory_vectors.h"
#include "lifecycle.h"
#include "platform_process.h"
#include "memory_platform.h"
#include "log.h"
#include "util.h"       /* util_now_ms — memory.search stage timing */
#include "agent_exec.h" /* agent_http_post: in-process HTTP embedding (no fork) */
#include "cJSON.h"
#include "dogfood.h"
#include <ctype.h>
#include <math.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>

static uint64_t memory_scope_identity_hash(const char *scope_type, const char *scope_value)
{
   uint64_t h = UINT64_C(1469598103934665603);
   const char *parts[2] = {scope_type ? scope_type : "", scope_value ? scope_value : ""};
   for (int p = 0; p < 2; p++)
   {
      for (const unsigned char *s = (const unsigned char *)parts[p]; *s; s++)
      {
         h ^= *s;
         h *= UINT64_C(1099511628211);
      }
      h ^= 0xff;
      h *= UINT64_C(1099511628211);
   }
   return h;
}

int memory_tier_priority(const char *tier)
{
   if (!tier)
      return 0;
   if (strcmp(tier, TIER_L5) == 0)
      return 5;
   if (strcmp(tier, TIER_L4) == 0)
      return 4;
   if (strcmp(tier, TIER_L3) == 0)
      return 3;
   if (strcmp(tier, TIER_L2) == 0)
      return 2;
   if (strcmp(tier, TIER_L1) == 0)
      return 1;
   if (strcmp(tier, TIER_L0) == 0)
      return 0;
   return 0;
}

const char *memory_functional_tier_name(const char *tier)
{
   if (!tier)
      return "Unknown";
   if (strcmp(tier, TIER_L0) == 0 || strcmp(tier, TIER_L1) == 0)
      return TIER_L0_NAME;
   if (strcmp(tier, TIER_L2) == 0)
      return TIER_L2_NAME;
   if (strcmp(tier, TIER_L3) == 0)
      return TIER_L3_NAME;
   if (strcmp(tier, TIER_L4) == 0)
      return TIER_L4_NAME;
   if (strcmp(tier, TIER_L5) == 0)
      return TIER_L5_NAME;
   return "Unknown";
}

/* Reclassify L3 directives (kind='policy' or 'workflow') to L4.
 * These are directive-type memories that should be injected at highest
 * prompt priority; functionally they are "Mental Models" not "World" context.
 * Returns count of rows reclassified, -1 on error.
 *
 * Approval gate: when `require_approval` is non-zero, only rows with a
 * `memory_promotion_approvals` entry recorded by `memory_approve_l4_promotion`
 * are eligible.  Workflows bypass the gate (they already go through the
 * explicit store_workflow MCP tool with 1.0 confidence); policies require an
 * approval row unless the gate is disabled.
 *
 * Use `memory_reclassify_directives(db)` for the explicit no-gate behaviour
 * (equivalent to `memory_reclassify_directives_ex(db, 0)`). */
int memory_reclassify_directives_ex(int require_approval)
{
   int changed = db2_memory_promotion_reclassify_directives(require_approval);
   if (changed < 0)
   {
      LOG_ERROR("memory", "reclassify_directives_ex: SQL failure");
      return -1;
   }
   if (changed > 0)
   {
      if (require_approval)
         LOG_INFO("memory",
                  "reclassify_directives_ex: %d L3 directives promoted to L4 (approval-gated)",
                  changed);
      else
         LOG_INFO("memory", "reclassify_directives: %d L3 directives promoted to L4", changed);
   }
   return changed;
}

int memory_reclassify_directives(void)
{
   return memory_reclassify_directives_ex(0);
}

/* Synthesize L5 pattern memories from L2 facts observed across multiple
 * sessions.  L5 is reserved for cross-session synthesized patterns.
 *
 * Algorithm (v1): group memory_provenance rows by memory_id and count
 * distinct sessions.  When a stable L2 fact/pattern has been observed in
 * >= 3 distinct sessions and no L5 synthesis already references it, create
 * a new L5 'pattern' memory with link `synthesizes` back to each contributing
 * source.  Content summarises the pattern; the provenance table captures which
 * sessions contributed.
 *
 * Returns the number of L5 memories synthesized, or -1 on DB error. */
int memory_synthesize_l5_patterns(void)
{
   enum
   {
      L5_SYNTHESIS_BATCH_CAP = 20
   };
   db2_memory_l5_candidate_t candidates[L5_SYNTHESIS_BATCH_CAP + 1];
   int got = db2_memory_promotion_l5_pattern_candidates(candidates, L5_SYNTHESIS_BATCH_CAP + 1);
   if (got < 0)
      return -1;
   if (got > L5_SYNTHESIS_BATCH_CAP)
   {
      LOG_WARN("memory",
               "L5 synthesis batch truncated at %d; remaining candidates defer to the next pass",
               L5_SYNTHESIS_BATCH_CAP);
      got = L5_SYNTHESIS_BATCH_CAP;
   }

   int synthesized = 0;
   for (int i = 0; i < got; i++)
   {
      const db2_memory_l5_candidate_t *c = &candidates[i];
      if (!c->src_key[0] || !c->src_content[0])
         continue;
      if (!memory_derived_sources_allowed(&c->source_id, 1))
      {
         LOG_WARN("memory", "synthesize_l5_patterns: source %lld refused by lineage gate",
                  (long long)c->source_id);
         continue;
      }
      /* An unresolved scope is an error, not a permissive default. A candidate
       * with no owning workspace has no scope to write the derived row into. */
      if (!c->scope_type[0] || !c->scope_value[0])
      {
         LOG_WARN("memory", "synthesize_l5_patterns: source %lld has unresolved scope",
                  (long long)c->source_id);
         continue;
      }

      char key[256];
      char content[1024];
      /* Scope participates in derived identity, not merely visibility. Without
       * it two identical source keys from unrelated projects merge into one
       * memory and accumulate both scope tags, recreating the global laundering
       * bug after the candidate query had correctly separated the counts. */
      snprintf(key, sizeof(key), "pattern_%s_%016llx_%.180s", c->scope_type,
               (unsigned long long)memory_scope_identity_hash(c->scope_type, c->scope_value),
               c->src_key);
      snprintf(content, sizeof(content), "Pattern observed across %d sessions in %s %.120s: %.400s",
               c->session_count, c->scope_type, c->scope_value, c->src_content);

      memory_t mem = {0};
      /* Recurrence is a reachability and salience signal, not evidence. This
       * used to scale confidence with session count to a 0.95 ceiling, which
       * converted popularity into truth and then fed itself: a higher-confidence
       * record ranks higher, is injected more, is restated more, and recurs in
       * more sessions. Exposure does not validate and time does not validate;
       * only independent evidence or explicit approval raises belief.
       *
       * A synthesized record is unproven inference, so it enters at the
       * conservative confidence for that class and stays there: this value is
       * also the provenance ceiling, because nothing raises a memory's
       * confidence -- every other path only decays it -- so a popular error
       * cannot climb into canon however often it recurs. Any future path that
       * does raise confidence has to honour provenance, or it reopens exactly
       * this hole. session_count stays on the record as the salience signal it
       * is, in the content rather than in the belief. */
      double conf = MEMORY_L5_SYNTHESIS_CONFIDENCE;
      int existed = db2_memory_key_exists(key) == 1;
      if (memory_insert(TIER_L5, KIND_FACT, key, content, conf, "", &mem) != 0)
         continue;

      /* The derived row inherits exactly the scope its recurrence was counted
       * within. Replace (rather than append to) automatic cwd/shared tags:
       * ambient scope is not part of this background derivation's evidence and
       * appending it would widen reachability after the candidate query had
       * correctly isolated the source scope. */
      if (db2_memory_scope_replace(mem.id, c->scope_type, c->scope_value) != 0)
      {
         if (!existed)
            (void)memory_delete(mem.id);
         LOG_WARN("memory", "synthesize_l5_patterns: failed to persist source scope for %lld",
                  (long long)mem.id);
         continue;
      }

      /* Link L5 synthesis → source memory so the provenance chain is
       * discoverable. */
      memory_link_create(mem.id, c->source_id, "synthesizes");
      {
         char ref[48];
         snprintf(ref, sizeof(ref), "memory:%lld", (long long)c->source_id);
         if (memory_lineage_insert("memory", mem.id, "memory", ref, conf) < 0)
         {
            if (!existed)
               (void)memory_delete(mem.id);
            continue;
         }
      }

      /* Emit MDL features for downstream ranker use. */
      kb_mdl_score_t mdl = {0};
      if (kb_mdl_score(content, c->src_content, &mdl) == 0)
      {
         char subj[32];
         snprintf(subj, sizeof(subj), "%lld", (long long)mem.id);
         char feat[256];
         snprintf(feat, sizeof(feat),
                  "{\"mdl.l_candidate\":%.2f,\"mdl.l_residual\":%.2f,"
                  "\"mdl.total\":%.2f,\"mdl.rank_in_cluster\":%d}",
                  mdl.l_candidate, mdl.l_residual, mdl.total, mdl.rank_in_cluster);
         db2_feature_row_upsert(subj, "memory_l5", "", "", "v1", feat, NULL);
      }

      synthesized++;
   }
   if (synthesized > 0)
      LOG_INFO("memory", "synthesize_l5_patterns: synthesized %d L5 patterns", synthesized);
   return synthesized;
}

int memory_activation_policy_set(int64_t memory_id, int sticky_turns, int cooldown_turns,
                                 int delay_turns, int suppressed)
{
   return db2_memory_activation_policy_set(memory_id, sticky_turns, cooldown_turns, delay_turns,
                                           suppressed);
}

/* Promote stable L2 facts/preferences to L3.  L3 is reserved for slow-changing
 * project/environment context.
 * Eligible rows satisfy all of:
 *   - tier = 'L2'
 *   - kind IN ('fact', 'preference')
 *   - confidence >= 0.95
 *   - use_count >= 5
 *   - updated_at older than 30 days (stable = hasn't mutated recently)
 *
 * Directive-like kinds (policy/workflow) are excluded — those flow L3→L4 via
 * `memory_reclassify_directives`.  Returns count promoted, -1 on error. */
int memory_promote_stable_l2_to_l3(void)
{
   char ts[32];
   now_utc(ts, sizeof(ts));
   int changed = db2_memory_promotion_promote_stable_l2_to_l3(ts);
   if (changed > 0)
      LOG_INFO("memory", "promote_stable_l2_to_l3: %d stable facts promoted to L3", changed);
   return changed;
}

/* Record an approval for an L3→L4 promotion.  Used by the approval-gated
 * `memory_reclassify_directives_ex(db, 1)` path and by the `memory approve`
 * CLI / MCP tool.  Returns 0 on success, -1 on error. */
int memory_approve_l4_promotion(int64_t memory_id, const char *approver, const char *note)
{
   return db2_memory_promotion_record_l4_approval(memory_id, approver, note);
}

/* --- Temporal as-of graph search --- */

int memory_search_graph_as_of(const char *query, const char *as_of, int limit,
                              memory_relation_t *out, int max)
{
   return db2_memory_relations_search_as_of(query, as_of, limit, out, max);
}

/* --- Lineage: insert / fetch --- */

int64_t memory_lineage_insert(const char *object_type, int64_t object_id, const char *source_kind,
                              const char *source_ref, double confidence)
{
   int64_t rowid =
       db2_memory_lineage_insert(object_type, object_id, source_kind, source_ref, confidence);
   if (rowid < 0)
      LOG_WARN("memory", "lineage_insert failed");
   return rowid;
}

int memory_lineage_get(const char *object_type, int64_t object_id, memory_lineage_t *out, int max)
{
   return db2_memory_lineage_get(object_type, object_id, out, max);
}

#define DERIVATION_MAX_DEPTH   16
#define DERIVATION_MAX_VISITED MEMORY_DERIVATION_MAX_SOURCES
#define DERIVATION_MAX_EDGES   64

static int memory_lineage_source_id(const memory_lineage_t *row, int64_t *id_out)
{
   if (!row || !id_out || strcmp(row->source_kind, "memory") != 0)
      return 0;
   const char *p = row->source_ref;
   if (strncmp(p, "memory:", 7) == 0)
      p += 7;
   char *end = NULL;
   long long id = strtoll(p, &end, 10);
   if (id <= 0 || !end || *end != '\0')
      return -1;
   *id_out = (int64_t)id;
   return 1;
}

static int memory_derived_source_walk(int64_t memory_id, int depth, int64_t *visited,
                                      unsigned char *visit_state, int *visited_count)
{
   if (depth > DERIVATION_MAX_DEPTH || !visited || !visit_state || !visited_count ||
       *visited_count >= DERIVATION_MAX_VISITED)
      return 0;
   for (int i = 0; i < *visited_count; i++)
      if (visited[i] == memory_id)
         return visit_state[i] == 2; /* active means cycle; complete means shared DAG */
   int visit_index = (*visited_count)++;
   visited[visit_index] = memory_id;
   visit_state[visit_index] = 1;

   if (db2_memory_derivation_source_refused(memory_id) != 0)
      return 0;

   memory_lineage_t rows[DERIVATION_MAX_EDGES + 1];
   int n = memory_lineage_get("memory", memory_id, rows, DERIVATION_MAX_EDGES + 1);
   if (n < 0 || n > DERIVATION_MAX_EDGES)
      return 0;
   for (int i = 0; i < n; i++)
   {
      int64_t source_id = 0;
      int parsed = memory_lineage_source_id(&rows[i], &source_id);
      if (parsed < 0)
         return 0; /* declared memory source cannot be resolved: fail closed */
      if (parsed > 0 &&
          !memory_derived_source_walk(source_id, depth + 1, visited, visit_state, visited_count))
         return 0;
   }
   visit_state[visit_index] = 2;
   return 1;
}

int memory_derived_sources_allowed(const int64_t *source_ids, int source_count)
{
   if (!source_ids || source_count <= 0 || source_count > DERIVATION_MAX_VISITED)
      return 0;
   int64_t visited[DERIVATION_MAX_VISITED];
   unsigned char visit_state[DERIVATION_MAX_VISITED] = {0};
   int visited_count = 0;
   for (int i = 0; i < source_count; i++)
      if (!memory_derived_source_walk(source_ids[i], 0, visited, visit_state, &visited_count))
         return 0;
   return 1;
}

/* --- Cite: show provenance chain for a memory ID --- */

void memory_cite(int64_t memory_id, int json_out)
{
   if (memory_id <= 0)
      return;

   /* Fetch the base memory record (key, tier, created_at) */
   char mem_key[512] = "";
   char mem_tier[16] = "";
   char mem_created[32] = "";
   {
      memory_t m;
      if (db2_memory_get(memory_id, &m) == 0)
      {
         snprintf(mem_key, sizeof(mem_key), "%s", m.key);
         snprintf(mem_tier, sizeof(mem_tier), "%s", m.tier);
         snprintf(mem_created, sizeof(mem_created), "%s", m.created_at);
      }
   }

   /* Fetch lineage rows for this memory */
   memory_lineage_t lineage[32];
   int lineage_count = memory_lineage_get("memory", memory_id, lineage, 32);

   /* Fetch session provenance from memory_provenance table */
   char prov_session[256] = "";
   char prov_action[64] = "";
   char prov_details[512] = "";
   {
      provenance_entry_t entries[1];
      if (db2_memory_provenance_list(memory_id, entries, 1) > 0)
      {
         snprintf(prov_session, sizeof(prov_session), "%s", entries[0].session_id);
         snprintf(prov_action, sizeof(prov_action), "%s", entries[0].action);
         snprintf(prov_details, sizeof(prov_details), "%s", entries[0].details);
      }
   }

   if (json_out)
   {
      cJSON *root = cJSON_CreateObject();
      cJSON_AddNumberToObject(root, "id", (double)memory_id);
      cJSON_AddStringToObject(root, "key", mem_key);
      cJSON_AddStringToObject(root, "tier", mem_tier);
      cJSON_AddStringToObject(root, "created_at", mem_created);
      if (prov_session[0])
      {
         cJSON *prov = cJSON_CreateObject();
         cJSON_AddStringToObject(prov, "session_id", prov_session);
         cJSON_AddStringToObject(prov, "action", prov_action);
         cJSON_AddStringToObject(prov, "details", prov_details);
         cJSON_AddItemToObject(root, "provenance", prov);
      }
      cJSON *larr = cJSON_CreateArray();
      for (int i = 0; i < lineage_count; i++)
      {
         cJSON *e = cJSON_CreateObject();
         cJSON_AddNumberToObject(e, "lineage_id", (double)lineage[i].id);
         cJSON_AddStringToObject(e, "source_kind", lineage[i].source_kind);
         cJSON_AddStringToObject(e, "source_ref", lineage[i].source_ref);
         cJSON_AddStringToObject(e, "ingested_at", lineage[i].ingested_at);
         cJSON_AddNumberToObject(e, "confidence", lineage[i].confidence);
         cJSON_AddItemToArray(larr, e);
      }
      cJSON_AddItemToObject(root, "lineage", larr);
      char *out_str = cJSON_PrintUnformatted(root);
      if (out_str)
      {
         printf("%s\n", out_str);
         free(out_str);
      }
      cJSON_Delete(root);
   }
   else
   {
      printf("Memory #%lld: %s  [%s]  created %s\n", (long long)memory_id,
             mem_key[0] ? mem_key : "(not found)", mem_tier, mem_created);
      if (prov_session[0])
         printf("  Provenance: session=%s action=%s details=%s\n", prov_session, prov_action,
                prov_details);
      if (lineage_count > 0)
      {
         printf("  Lineage chain:\n");
         for (int i = 0; i < lineage_count; i++)
            printf("    [%d] kind=%-12s ref=%-48s ingested=%s conf=%.2f\n", i + 1,
                   lineage[i].source_kind,
                   lineage[i].source_ref[0] ? lineage[i].source_ref : "(none)",
                   lineage[i].ingested_at, lineage[i].confidence);
      }
      else
      {
         printf("  No lineage records.\n");
      }
   }
}
