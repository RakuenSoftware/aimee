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
#include "db2/entity_edges.h"
#include "db2/kb_runtime_state.h"
#include "db2/memory_health.h"
#include "db2/memory_payload.h"
#include "db2/feature_rows.h"
#include "db2/memory_promotion.h"
#include "db2/memory_query.h"
#include "memory_graph_fusion.h"
#include "kb_mdl.h"
#include "db2/memory_relations.h"
#include "db2/memory_scenes.h"
#include "db2/stopwords.h"
#include "db2/vector_index_ops.h"
#include "db2/vector_verify.h"
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
   db2_memory_l5_candidate_t candidates[20];
   int cap = (int)(sizeof(candidates) / sizeof(candidates[0]));
   int got = db2_memory_promotion_l5_pattern_candidates(candidates, cap);
   if (got < 0)
      return -1;

   int synthesized = 0;
   for (int i = 0; i < got; i++)
   {
      const db2_memory_l5_candidate_t *c = &candidates[i];
      if (!c->src_key[0] || !c->src_content[0])
         continue;

      char key[256];
      char content[1024];
      snprintf(key, sizeof(key), "pattern_%.200s", c->src_key);
      snprintf(content, sizeof(content), "Pattern observed across %d sessions: %.400s",
               c->session_count, c->src_content);

      memory_t mem = {0};
      /* L5 synthesis carries high confidence — cross-session frequency is
       * the strongest signal we have short of explicit operator approval. */
      double conf = 0.85 + 0.01 * (c->session_count - 3);
      if (conf > 0.95)
         conf = 0.95;
      if (memory_insert(TIER_L5, KIND_FACT, key, content, conf, "", &mem) != 0)
         continue;

      /* Link L5 synthesis → source memory so the provenance chain is
       * discoverable. */
      memory_link_create(mem.id, c->source_id, "synthesizes");

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
