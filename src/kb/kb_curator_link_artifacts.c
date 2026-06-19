/* kb_curator_link_artifacts.c: deep-curator link_artifacts (doc<->code bridge).
 *
 * Picks one committed code_unit not yet processed (reflected_at unset — code
 * units are not in the reflection flow, so the field is free as a "linked"
 * marker), parses its domain_concepts, finds the canonical entity artifacts
 * those concepts name (by normalized string match, then by embedding-similarity
 * NN search over the entity vectors), and writes a `mentions` artifact_link from
 * the code_unit to each. Entities are cited to their source document, so
 * doc -> entity -> code_unit becomes a graph traversal (POST /v1/implements).
 * No DB1 access from this file. */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "kb_curator_link_artifacts.h"
#include "aimee.h"
#include "cJSON.h"
#include "config.h"
#include "log.h"
#include "memory.h"
#include "db2/artifacts.h"
#include "db2/db2_internal.h"
#include "db2/db_postgres.h"
#include "db2/pgvec_transport.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Max domain_concepts scanned from a single code_unit per call. */
#define LINK_MAX_CONCEPTS 32

/* Distinct entities linked from one code_unit per call (dedup bound: exact and
 * semantic passes share this set so an entity is linked at most once). */
#define LINK_MAX_LINKED 128

/* Dimension of the curator entity embedding space (matches resolve_entities). */
#define CURATOR_LINK_ENTITY_DIM 384

/* Top-K nearest entity vectors fetched per concept for semantic linking. */
#define CURATOR_LINK_SEMANTIC_TOPK 5

/* Cosine score (1 - distance) at/above which a code_unit domain_concept is
 * linked to a committed entity by embedding similarity. This catches the
 * matches the normalized string compare cannot: extra/reordered words
 * (`code_unit_extraction` vs entity "code unit extraction pipeline") and, with
 * a real embedding_command model rather than the lexical builtin embedder,
 * paraphrases/synonyms. Conservative to keep `mentions` links high-precision;
 * a compile-time constant like the resolve_entities merge/judge thresholds. */
#define CURATOR_LINK_SEMANTIC_THRESHOLD 0.75

/* Entity ids already linked to the current code_unit, so the exact-name and
 * embedding passes don't double-link (and the count stays honest). */
typedef struct
{
   char ids[LINK_MAX_LINKED][64];
   int n;
} linked_set_t;

/* Link code_unit -> entity once. Returns 1 if a new link was written, 0 if the
 * entity was already linked this call or the write failed. */
static int try_link(const char *code_id, const char *entity_id, linked_set_t *seen)
{
   if (!entity_id || !entity_id[0])
      return 0;
   for (int i = 0; i < seen->n; i++)
      if (strcmp(seen->ids[i], entity_id) == 0)
         return 0;
   if (seen->n < LINK_MAX_LINKED)
      snprintf(seen->ids[seen->n++], 64, "%s", entity_id);
   return db2_artifact_link(code_id, entity_id, "mentions") == 0 ? 1 : 0;
}

/* Link the code_unit to every committed entity whose payload name matches
 * `concept`. Returns the number of links written. */
static int link_concept_to_entities(void *conn, const char *code_id, const char *concept,
                                    linked_set_t *seen)
{
   if (!concept || !concept[0])
      return 0;

   /* Match the entity name via the JSON operator, not a LIKE on the raw
    * payload: artifacts.payload is JSONB, which Postgres normalizes to
    * "name": "X" (space after the colon), so a no-space `"name":"X"` LIKE
    * pattern never matches on Postgres (it only matched on the sqlite shim).
    *
    * Normalize both sides before comparing: code extraction emits snake_case
    * identifiers (`workspace_provider`, `entity_mention_link`) while doc
    * extraction emits prose (`workspace provider`, `Entity-Mention-Link`), so
    * an exact case-sensitive equality almost never links the same concept
    * across the two passes. Lowercase and fold `_`/`-` to spaces so
    * format-only differences match. (Wording that diverges beyond format —
    * e.g. extra/reordered words, or synonyms with a real embedding model — is
    * caught by the embedding-similarity pass in link_concept_semantic().)
    * lower()/replace() are supported by both Postgres and the sqlite test shim. */
   static const char *sql = "SELECT id FROM artifacts"
                            " WHERE kind = 'entity' AND state = 'committed'"
                            "   AND lower(replace(replace(payload->>'name', '_', ' '), '-', ' '))"
                            "     = lower(replace(replace(:concept, '_', ' '), '-', ' ')) LIMIT 5";
   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return 0;
   aimee_pg_bind_text(st, "concept", concept);

   char entity_ids[5][64];
   int n = 0;
   while (n < 5 && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      const char *eid = aimee_pg_column_text(st, 0);
      snprintf(entity_ids[n], sizeof(entity_ids[n]), "%s", eid ? eid : "");
      n++;
   }
   aimee_pg_finalize(st);

   int written = 0;
   for (int i = 0; i < n; i++)
      written += try_link(code_id, entity_ids[i], seen);
   return written;
}

/* Embedding-similarity fallback: embed the concept with the configured
 * embedder, NN-search committed entity vectors, and link every hit at/above
 * CURATOR_LINK_SEMANTIC_THRESHOLD. A no-op where vectors/embeddings are
 * unavailable (e.g. the sqlite test shim), so it only adds links, never
 * removes the exact-match behaviour. */
static int link_concept_semantic(const char *code_id, const char *concept, const char *embed_cmd,
                                 linked_set_t *seen)
{
   if (!concept || !concept[0])
      return 0;

   /* Entities with no context were embedded from their bare name, so embedding
    * the concept string directly keeps both sides in the same space. */
   float vec[CURATOR_LINK_ENTITY_DIM];
   int dim = memory_embed_text(concept, embed_cmd, vec, CURATOR_LINK_ENTITY_DIM);
   if (dim != CURATOR_LINK_ENTITY_DIM)
      return 0;

   int64_t ids[CURATOR_LINK_SEMANTIC_TOPK];
   double scores[CURATOR_LINK_SEMANTIC_TOPK];
   int nm = pgvec_curator_entity_search(NULL, NULL, vec, dim, CURATOR_LINK_SEMANTIC_TOPK, ids,
                                        scores, CURATOR_LINK_SEMANTIC_TOPK);
   int written = 0;
   for (int i = 0; i < nm; i++)
   {
      if (scores[i] < CURATOR_LINK_SEMANTIC_THRESHOLD)
         continue;
      char aid[64] = "";
      if (pgvec_curator_entity_lookup(ids[i], aid, sizeof(aid), NULL, 0) == 1)
         written += try_link(code_id, aid, seen);
   }
   return written;
}

int kb_curator_link_artifacts_one(const kb_curator_extract_opts_t *opts)
{
   (void)opts;
   void *conn = db2_conn();
   if (!conn)
      return 0;

   config_t cfg;
   config_load(&cfg);
   const char *embed_cmd = config_embedding_command(&cfg, NULL);

   static const char *sql = "SELECT id, payload FROM artifacts"
                            " WHERE kind = 'code_unit' AND state = 'committed'"
                            "   AND (reflected_at IS NULL OR reflected_at = '')"
                            " ORDER BY id LIMIT 1";
   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return 0;

   char id[64] = "";
   char *payload = NULL;
   int found = 0;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      const char *c_id = aimee_pg_column_text(st, 0);
      const char *c_pl = aimee_pg_column_text(st, 1);
      snprintf(id, sizeof(id), "%s", c_id ? c_id : "");
      payload = strdup(c_pl ? c_pl : "{}");
      found = 1;
   }
   aimee_pg_finalize(st);
   if (!found)
   {
      free(payload);
      return 0;
   }

   int total = 0;
   cJSON *pj = payload ? cJSON_Parse(payload) : NULL;
   const cJSON *concepts = pj ? cJSON_GetObjectItemCaseSensitive(pj, "domain_concepts") : NULL;
   if (cJSON_IsArray(concepts))
   {
      int n = cJSON_GetArraySize(concepts);
      if (n > LINK_MAX_CONCEPTS)
         n = LINK_MAX_CONCEPTS;
      linked_set_t seen = {0};
      for (int i = 0; i < n; i++)
      {
         const cJSON *c = cJSON_GetArrayItem(concepts, i);
         if (cJSON_IsString(c) && c->valuestring[0])
         {
            total += link_concept_to_entities(conn, id, c->valuestring, &seen);
            total += link_concept_semantic(id, c->valuestring, embed_cmd, &seen);
         }
      }
   }
   cJSON_Delete(pj);

   /* Stamp processed so the code_unit is not re-scanned (idempotent links mean
    * re-runs would be harmless but wasteful). */
   db2_artifact_stamp_reflected(id);
   if (total > 0)
      aimee_log(LOG_INFO, "kb.curator.link", "linked code_unit %s to %d entity mention(s)", id,
                total);

   free(payload);
   return 1;
}
