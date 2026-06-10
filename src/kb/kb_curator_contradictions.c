/* kb_curator_contradictions.c: deep-curator detect_contradictions pass.
 *
 * Self-joins curator_claim_vectors to find claim pairs that share a
 * subject+attribute but assert different values, and that are not already
 * linked, then writes a `contradicts` artifact_link between the two claim
 * artifacts. This is the deterministic exact-facet pass; fuzzy
 * subj_attr-similar / value-different mining over the named vectors is a
 * refinement. db2_artifact_link is idempotent, so re-running is safe.
 * No DB1 access from this file. */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "kb_curator_contradictions.h"
#include "aimee.h"
#include "log.h"
#include "db2/artifacts.h"
#include "db2/db2_internal.h"
#include "db2/db_postgres.h"
#include "db2/pgvec_transport.h"

#include <string.h>

/* Max claim pairs linked per drain call (bounds work per poll). */
#define CONTRA_BATCH 32

/* Fuzzy contradiction mining (opt-in via the 4B deep tier). The exact pass above
 * only catches claims with an IDENTICAL subject+attribute text; this phase finds
 * pairs whose subject+attribute are *semantically* the same (high live subj_attr
 * cosine) but phrased differently, with a different value. Every fuzzy link is
 * REQUIRED to be confirmed in the 4B deep space — it never fires on the live
 * embedding alone — so it cannot manufacture contradictions the deep model
 * disagrees with. All thresholds are deliberately conservative; tune on a real
 * corpus before relying on it. Live dim matches the claim indexer's 384. */
#define CONTRA_CLAIM_DIM      384
#define CONTRA_FUZZY_BATCH    16   /* deep-ready claims probed per drain call */
#define CONTRA_FUZZY_K        5    /* nearest subj_attr candidates per claim */
#define CONTRA_FUZZY_LIVE_MIN 0.80 /* live subj_attr cosine to even consider a pair */
#define CONTRA_FUZZY_DEEP_MIN 0.75 /* 4B-deep cosine required to link (the guard) */

/* Fuzzy, deep-confirmed contradiction mining. Returns the number of new
 * `contradicts` links written, or 0 when the deep tier is off (the guard is
 * mandatory). Best-effort throughout: any embed/search/lookup miss just skips. */
static int mine_fuzzy_contradictions(void)
{
   config_t cfg;
   if (config_load(&cfg) != 0 || !cfg.memory_deep_embedding_enabled ||
       !cfg.memory_deep_embedding_command[0])
      return 0;
   const char *live_cmd = cfg.embedding_command[0] ? cfg.embedding_command : "builtin";
   void *conn = db2_conn();
   if (!conn)
      return 0;

   struct fuzzy_claim
   {
      int64_t pid;
      char artifact[64], subject[256], attribute[256], value[256];
   } batch[CONTRA_FUZZY_BATCH];
   int nb = 0;
   {
      static const char *bs =
          "SELECT point_id, artifact_id, subject, attribute, value FROM curator_claim_vectors"
          " WHERE subj_attr_deep_vec IS NOT NULL ORDER BY point_id DESC LIMIT 16";
      char e[256] = "";
      aimee_pg_stmt_t *st = aimee_pg_prepare(conn, bs, e, sizeof(e));
      if (st)
      {
         while (nb < CONTRA_FUZZY_BATCH && aimee_pg_step(st, e, sizeof(e)) == AIMEE_PG_ROW)
         {
            batch[nb].pid = aimee_pg_column_int64(st, 0);
            const char *a = aimee_pg_column_text(st, 1);
            const char *s = aimee_pg_column_text(st, 2);
            const char *at = aimee_pg_column_text(st, 3);
            const char *v = aimee_pg_column_text(st, 4);
            snprintf(batch[nb].artifact, sizeof(batch[nb].artifact), "%s", a ? a : "");
            snprintf(batch[nb].subject, sizeof(batch[nb].subject), "%s", s ? s : "");
            snprintf(batch[nb].attribute, sizeof(batch[nb].attribute), "%s", at ? at : "");
            snprintf(batch[nb].value, sizeof(batch[nb].value), "%s", v ? v : "");
            nb++;
         }
         aimee_pg_finalize(st);
      }
   }

   int written = 0;
   for (int i = 0; i < nb; i++)
   {
      if (!batch[i].artifact[0])
         continue;
      char subj_attr[520];
      snprintf(subj_attr, sizeof(subj_attr), "%s %s", batch[i].subject, batch[i].attribute);
      float lvec[CONTRA_CLAIM_DIM];
      if (memory_embed_text(subj_attr, live_cmd, lvec, CONTRA_CLAIM_DIM) != CONTRA_CLAIM_DIM)
         continue;

      int64_t cand[CONTRA_FUZZY_K];
      double cscore[CONTRA_FUZZY_K];
      int nc = pgvec_curator_claim_search("subj_attr", NULL, lvec, CONTRA_CLAIM_DIM, CONTRA_FUZZY_K,
                                          cand, cscore, CONTRA_FUZZY_K);
      for (int j = 0; j < nc; j++)
      {
         if (cand[j] == batch[i].pid || cscore[j] < CONTRA_FUZZY_LIVE_MIN)
            continue;
         char cart[64] = "", csubj[256] = "", cval[256] = "";
         if (pgvec_curator_claim_fields(cand[j], cart, sizeof(cart), csubj, sizeof(csubj), cval,
                                        sizeof(cval)) != 1)
            continue;
         /* Fuzzy case only: a DIFFERENT subject text (identical-subject pairs are
          * the exact pass's job) asserting a DIFFERENT value. */
         if (!cart[0] || strcmp(cart, batch[i].artifact) == 0 ||
             strcmp(csubj, batch[i].subject) == 0 || strcmp(cval, batch[i].value) == 0)
            continue;
         /* Mandatory deep confirmation: both sides deep-embedded AND deep-similar. */
         double dsim = 0.0;
         if (pgvec_curator_claim_deep_similarity(batch[i].pid, cand[j], &dsim) != 1 ||
             dsim < CONTRA_FUZZY_DEEP_MIN)
            continue;
         if (db2_artifact_link(batch[i].artifact, cart, "contradicts") == 0)
         {
            aimee_log(LOG_INFO, "kb.curator.contradict",
                      "fuzzy contradiction: '%s' vs '%s' (live=%.3f deep=%.3f)", batch[i].subject,
                      csubj, cscore[j], dsim);
            written++;
         }
      }
   }
   return written;
}

int kb_curator_detect_contradictions_one(const kb_curator_extract_opts_t *opts)
{
   (void)opts;
   void *conn = db2_conn();
   if (!conn)
      return 0;

   /* Pairs with a shared subject+attribute but a different value that are not
    * already linked as contradictions. a.point_id < b.point_id keeps each
    * unordered pair once. */
   static const char *sql =
       "SELECT a.artifact_id, b.artifact_id"
       " FROM curator_claim_vectors a"
       " JOIN curator_claim_vectors b"
       "   ON a.subject = b.subject AND a.attribute = b.attribute"
       "  AND a.subject <> '' AND a.value <> b.value AND a.point_id < b.point_id"
       " WHERE NOT EXISTS ("
       "   SELECT 1 FROM artifact_links l"
       "    WHERE l.from_id = a.artifact_id AND l.to_id = b.artifact_id"
       "      AND l.kind = 'contradicts')"
       " LIMIT 32";

   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return 0;

   char from_ids[CONTRA_BATCH][64];
   char to_ids[CONTRA_BATCH][64];
   int n = 0;
   while (n < CONTRA_BATCH && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      const char *a = aimee_pg_column_text(st, 0);
      const char *b = aimee_pg_column_text(st, 1);
      snprintf(from_ids[n], sizeof(from_ids[n]), "%s", a ? a : "");
      snprintf(to_ids[n], sizeof(to_ids[n]), "%s", b ? b : "");
      n++;
   }
   aimee_pg_finalize(st);

   int written = 0;
   for (int i = 0; i < n; i++)
   {
      if (!from_ids[i][0] || !to_ids[i][0])
         continue;
      if (db2_artifact_link(from_ids[i], to_ids[i], "contradicts") == 0)
         written++;
   }

   /* Opt-in fuzzy phase: semantic subject+attribute matches the exact join misses,
    * each mandatorily confirmed in the 4B deep space. No-op when the deep tier is off. */
   written += mine_fuzzy_contradictions();

   if (written > 0)
      aimee_log(LOG_INFO, "kb.curator.contradict", "linked %d contradicting claim pair(s)",
                written);
   return written > 0 ? 1 : 0;
}
