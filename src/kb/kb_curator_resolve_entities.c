/* kb_curator_resolve_entities.c: deep-curator resolve_entities pass.
 *
 * Claims one proposed `entity` mention artifact, embeds its name+context via the
 * configured embedding_command, runs an NN search against existing canonical
 * entities, and either resolves the mention to a near-identical match
 * (>= the merge threshold; no duplicate vector written) or commits it as a new
 * canonical entity and upserts its vector into curator_entity_vectors. The
 * ambiguous [0.70, 0.85) band is adjudicated by an optional LLM judge sidecar
 * (kb_curator_judge). The search covers the mention's own scope and then each
 * BROADER scope up the lattice (project -> workspace -> global), so a narrow
 * mention resolves onto a pre-existing broader-scope entity rather than forking
 * a near-duplicate. No DB1 access from this file. */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "kb_curator_resolve_entities.h"
#include "kb_curator_judge.h"
#include "kb_curator_promote.h" /* kb_curator_broaden_scope (scope lattice) */
#include "aimee.h"
#include "config.h"
#include "cJSON.h"
#include "log.h"
#include "memory.h"
#include "db2/artifacts.h"
#include "db2/db2_internal.h"
#include "db2/db_postgres.h"
#include "db2/pgvec_transport.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define CURATOR_ENTITY_DIM 384

/* Cosine score (1 - distance) at/above which a mention is treated as the same
 * canonical entity as an existing one (no new vector written). */
#define CURATOR_ENTITY_MERGE_THRESHOLD 0.85

/* Lower bound of the ambiguous band: a nearest match scoring in
 * [CURATOR_ENTITY_JUDGE_LOW, CURATOR_ENTITY_MERGE_THRESHOLD) is neither a
 * confident merge nor a confident create, so an LLM judge (when configured)
 * decides. Below this, always create. */
#define CURATOR_ENTITY_JUDGE_LOW 0.70

/* Deep-tier precision guard (config memory_deep_embedding_enabled): when a confident
 * live-embedding merge (>= MERGE_THRESHOLD) is also checkable in the 4B deep space
 * — both the mention and the matched entity have a deep embedding — a clearly-low
 * deep cosine means the live embedding over-estimated similarity; reject the merge
 * and create a distinct entity instead. Conservative threshold: true matches are
 * deep-similar (well above this), so a real merge is never blocked; a no-op when
 * the deep tier is off or the matched entity has no deep embedding yet. Deep can
 * only PREVENT a merge, never force one — it cannot collapse distinct entities. */
#define CURATOR_ENTITY_DEEP_CONFIRM 0.60

void kb_curator_entity_embed_text(const char *name, const char *context, char *out, size_t out_len)
{
   if (!out || out_len == 0)
      return;
   const char *n = name ? name : "";
   const char *c = context ? context : "";
   if (n[0] && c[0])
      snprintf(out, out_len, "%s - %s", n, c);
   else
      snprintf(out, out_len, "%s", n);
}

int64_t kb_curator_entity_point_id(const char *artifact_id)
{
   uint64_t h = 1469598103934665603ULL; /* FNV-1a 64-bit offset basis */
   if (artifact_id)
   {
      for (const unsigned char *p = (const unsigned char *)artifact_id; *p; p++)
      {
         h ^= (uint64_t)*p;
         h *= 1099511628211ULL; /* FNV-1a 64-bit prime */
      }
   }
   return (int64_t)(h & 0x7FFFFFFFFFFFFFFFULL);
}

/* Search one scope for a near-identical existing canonical entity. Returns 1 if
 * the mention should resolve onto an existing entity (merge — no new vector),
 * 0 to keep searching / create. Three outcomes by cosine score:
 *   >= 0.85          confident merge
 *   [0.70, 0.85)     ambiguous — ask the LLM judge when one is configured
 *   <  0.70          no match in this scope.
 * A judge error is treated as "not the same" (create), so a flaky sidecar can
 * only over-create, never collapse distinct entities. */
static int resolve_try_match(const char *scope_kind, const char *scope_id, const float *vec,
                             int dim, const char *name, const char *context, const config_t *cfg,
                             const float *deep_vec, int deep_dim)
{
   int64_t match_ids[1];
   double match_scores[1];
   int nmatch =
       pgvec_curator_entity_search(scope_kind, scope_id, vec, dim, 1, match_ids, match_scores, 1);
   if (nmatch < 1)
      return 0;
   if (match_scores[0] >= CURATOR_ENTITY_MERGE_THRESHOLD)
   {
      /* Deep-tier guard: reject a confident live merge only when the 4B deep space
       * clearly disagrees (both sides deep-embedded; cosine below the floor). */
      double dsim = 0.0;
      if (deep_dim > 0 &&
          pgvec_curator_entity_deep_similarity(match_ids[0], deep_vec, deep_dim, &dsim) == 1 &&
          dsim < CURATOR_ENTITY_DEEP_CONFIRM)
      {
         aimee_log(LOG_INFO, "kb.curator.resolve",
                   "entity '%s' live-matched in %s:%s (score=%.3f) but deep guard rejects "
                   "(deep=%.3f); creating distinct entity",
                   name, scope_kind, scope_id, match_scores[0], dsim);
         return 0;
      }
      aimee_log(LOG_INFO, "kb.curator.resolve",
                "entity '%s' resolves to existing in %s:%s (score=%.3f); skipping duplicate vector",
                name, scope_kind, scope_id, match_scores[0]);
      return 1;
   }
   if (match_scores[0] >= CURATOR_ENTITY_JUDGE_LOW && cfg->kb_curator_judge_command[0])
   {
      char cand_name[256];
      if (pgvec_curator_entity_lookup(match_ids[0], NULL, 0, cand_name, sizeof(cand_name)) == 1)
      {
         int same = 0;
         char jerr[256];
         int jrc =
             kb_curator_judge_same_entity(cfg->kb_curator_judge_command, name, context, cand_name,
                                          match_scores[0], &same, jerr, sizeof(jerr));
         if (jrc == 0 && same)
         {
            aimee_log(LOG_INFO, "kb.curator.resolve",
                      "entity '%s' judged same as '%s' in %s:%s (score=%.3f); skipping duplicate "
                      "vector",
                      name, cand_name, scope_kind, scope_id, match_scores[0]);
            return 1;
         }
         if (jrc != 0)
            aimee_log(LOG_WARN, "kb.curator.resolve",
                      "judge error for '%s' (%s); creating new entity", name, jerr);
      }
   }
   return 0;
}

int kb_curator_resolve_entities_one(const kb_curator_extract_opts_t *opts)
{
   (void)opts;
   void *conn = db2_conn();
   if (!conn)
      return 0;

   static const char *sql = "SELECT id, payload, scope_kind, scope_id"
                            " FROM artifacts"
                            " WHERE kind = 'entity' AND state = 'proposed'"
                            " ORDER BY id LIMIT 1";
   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return 0;

   char id[64] = "", scope_kind[64] = "", scope_id[128] = "";
   char *payload = NULL;
   int found = 0;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      const char *c_id = aimee_pg_column_text(st, 0);
      const char *c_pl = aimee_pg_column_text(st, 1);
      const char *c_sk = aimee_pg_column_text(st, 2);
      const char *c_si = aimee_pg_column_text(st, 3);
      snprintf(id, sizeof(id), "%s", c_id ? c_id : "");
      payload = strdup(c_pl ? c_pl : "{}");
      snprintf(scope_kind, sizeof(scope_kind), "%s", c_sk ? c_sk : "");
      snprintf(scope_id, sizeof(scope_id), "%s", c_si ? c_si : "");
      found = 1;
   }
   aimee_pg_finalize(st);
   if (!found)
   {
      free(payload);
      return 0;
   }

   char name[256] = "", context[512] = "";
   cJSON *pj = payload ? cJSON_Parse(payload) : NULL;
   if (pj)
   {
      const cJSON *jn = cJSON_GetObjectItemCaseSensitive(pj, "name");
      const cJSON *jc = cJSON_GetObjectItemCaseSensitive(pj, "context");
      if (cJSON_IsString(jn) && jn->valuestring[0])
         snprintf(name, sizeof(name), "%s", jn->valuestring);
      if (cJSON_IsString(jc) && jc->valuestring[0])
         snprintf(context, sizeof(context), "%s", jc->valuestring);
   }

   if (!name[0])
   {
      /* Nothing embeddable — commit so the mention is not reprocessed. */
      cJSON_Delete(pj);
      db2_artifact_set_state(id, "committed");
      free(payload);
      return 1;
   }

   char embed_text[768];
   kb_curator_entity_embed_text(name, context, embed_text, sizeof(embed_text));

   config_t cfg;
   config_load(&cfg);
   const char *embed_cmd = cfg.embedding_command[0] ? cfg.embedding_command : "builtin";
   float vec[CURATOR_ENTITY_DIM];
   int dim = memory_embed_text(embed_text, embed_cmd, vec, CURATOR_ENTITY_DIM);
   /* Deep tier: embed the mention once with the deep model (when enabled) — reused
    * for the merge-confirmation guard and, if it becomes a new entity, stored. */
   float deep_vec[DEEP_EMBED_MAX_DIM];
   int deep_dim = 0;
   if (cfg.memory_deep_embedding_enabled && cfg.memory_deep_embedding_command[0])
      deep_dim = memory_embed_text(embed_text, cfg.memory_deep_embedding_command, deep_vec,
                                   DEEP_EMBED_MAX_DIM);
   if (dim == CURATOR_ENTITY_DIM)
   {
      /* Resolve to an existing canonical entity in this scope, or — per the
       * charter's scope lattice — any BROADER scope (project -> workspace ->
       * global): a project-scoped mention that matches an existing
       * workspace/global entity resolves onto it instead of creating a
       * near-duplicate. The searches run before the upsert, so they only see
       * previously-committed entities. */
      int merged = resolve_try_match(scope_kind, scope_id, vec, dim, name, context, &cfg, deep_vec,
                                     deep_dim);
      if (!merged)
      {
         void *conn = db2_conn();
         char ck[64], cid[128], wk[64], wid[128];
         snprintf(ck, sizeof(ck), "%s", scope_kind);
         snprintf(cid, sizeof(cid), "%s", scope_id);
         while (!merged && conn &&
                kb_curator_broaden_scope(conn, ck, cid, wk, sizeof(wk), wid, sizeof(wid)))
         {
            merged = resolve_try_match(wk, wid, vec, dim, name, context, &cfg, deep_vec, deep_dim);
            snprintf(ck, sizeof(ck), "%s", wk);
            snprintf(cid, sizeof(cid), "%s", wid);
         }
      }
      if (!merged)
      {
         int64_t pid = kb_curator_entity_point_id(id);
         if (pgvec_curator_entity_upsert(pid, vec, dim, scope_kind, scope_id, name, id,
                                         payload ? payload : "{}") != 0)
            aimee_log(LOG_WARN, "kb.curator.resolve", "entity vector upsert failed for %s", id);
         else if (deep_dim > 0)
            pgvec_curator_entity_deep_update(pid, deep_vec, deep_dim); /* deep precision layer */
      }
   }
   else
   {
      aimee_log(LOG_WARN, "kb.curator.resolve", "embed failed (dim=%d) for entity %s", dim, id);
   }

   db2_artifact_set_state(id, "committed");
   aimee_log(LOG_INFO, "kb.curator.resolve", "committed entity '%s' (%s)", name, id);

   cJSON_Delete(pj);
   free(payload);
   return 1;
}
