/* kb_store/lessons.c: see lessons.h. The only writer of the append-only, memory-graph-
 * isolated retrieval-outcome ledger (graph-feedback §3). Postgres via libpq. */
#include "lessons.h"
#include "kb_store_internal.h"
#include "db_postgres.h"

#include <stdio.h>
#include <string.h>

#define LES_ERR 256

int64_t kb_store_lessons_record_outcome(const char *session_id, const char *turn_id,
                                        const char *project_id, int64_t generation_id,
                                        const char *answer_outcome, const char *correction_text,
                                        const char *finding_id, const char *actor_id,
                                        const char *actor_source, int confirmed)
{
   void *conn = kb_store_conn();
   if (!conn || !answer_outcome || !answer_outcome[0])
      return -1;
   /* Defend the CHECK constraints in the C layer so a bad caller gets -1, not a
    * SQL error mid-transaction. */
   if (strcmp(answer_outcome, "useful") != 0 && strcmp(answer_outcome, "dead_end") != 0 &&
       strcmp(answer_outcome, "corrected") != 0)
      return -1;
   const char *src = (actor_source && actor_source[0]) ? actor_source : "agent";
   if (strcmp(src, "user") != 0 && strcmp(src, "reviewer") != 0 && strcmp(src, "agent") != 0)
      return -1;

   char err[LES_ERR] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       conn,
       "INSERT INTO lessons_outcome_ledger"
       " (session_id, turn_id, project_id, generation_id, answer_outcome, correction_text,"
       "  finding_id, actor_id, actor_source, confirmed)"
       " VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10) RETURNING id",
       err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", session_id ? session_id : "");
   aimee_pg_bind_text(st, "?2", turn_id ? turn_id : "");
   aimee_pg_bind_text(st, "?3", project_id ? project_id : "");
   aimee_pg_bind_int64(st, "?4", generation_id);
   aimee_pg_bind_text(st, "?5", answer_outcome);
   aimee_pg_bind_text(st, "?6", correction_text ? correction_text : "");
   aimee_pg_bind_text(st, "?7", finding_id ? finding_id : "");
   aimee_pg_bind_text(st, "?8", actor_id ? actor_id : "");
   aimee_pg_bind_text(st, "?9", src);
   aimee_pg_bind_int(st, "?10", confirmed ? 1 : 0);
   int64_t id = -1;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
      id = aimee_pg_column_int64(st, 0);
   aimee_pg_finalize(st);
   return id;
}

int kb_store_lessons_record_citation(int64_t outcome_id, const char *node_id,
                                     const char *disposition)
{
   void *conn = kb_store_conn();
   if (!conn || outcome_id <= 0 || !node_id || !node_id[0])
      return -1;
   const char *disp = (disposition && disposition[0]) ? disposition : "unused";
   if (strcmp(disp, "useful") != 0 && strcmp(disp, "stale") != 0 && strcmp(disp, "unused") != 0)
      return -1;
   char err[LES_ERR] = "";
   /* ON CONFLICT: the (outcome_id, node_id) pair is the PK; a repeat is a no-op. */
   aimee_pg_stmt_t *st =
       aimee_pg_prepare(conn,
                        "INSERT INTO lessons_outcome_citations (outcome_id, node_id, disposition)"
                        " VALUES (?1, ?2, ?3) ON CONFLICT (outcome_id, node_id) DO NOTHING",
                        err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_int64(st, "?1", outcome_id);
   aimee_pg_bind_text(st, "?2", node_id);
   aimee_pg_bind_text(st, "?3", disp);
   int rc = (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_DONE) ? 0 : -1;
   aimee_pg_finalize(st);
   return rc;
}

int kb_store_lessons_list_outcomes(const char *project_id, int64_t community_gen,
                                   kb_store_lessons_outcome_row_t *out, int max)
{
   void *conn = kb_store_conn();
   if (!conn || !out || max <= 0)
      return -1;
   char err[LES_ERR] = "";
   /* One row per (outcome, cited node), joined to the generation's community
    * partition for grouping. ts is stored ISO text; ::date - epoch gives a day
    * ordinal for the reflection's time-decay. */
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       conn,
       /* ts is stored as UTC 'YYYY-MM-DD HH24:MI:SS' TEXT; take the leading date
        * substring so the day ordinal is a pure calendar parse — no session-TZ
        * dependence, keeping the reflection byte-stable across servers. */
       "SELECT c.node_id, COALESCE(cpc.community_id, ''), l.answer_outcome, l.actor_source,"
       "       (SUBSTRING(l.ts, 1, 10)::date - DATE '1970-01-01'), l.confirmed"
       " FROM lessons_outcome_ledger l"
       " JOIN lessons_outcome_citations c ON c.outcome_id = l.id"
       " LEFT JOIN code_projection_communities cpc"
       "        ON cpc.node_id = c.node_id AND cpc.generation_id = ?2"
       " WHERE l.project_id = ?1"
       " ORDER BY c.node_id, l.ts, l.id"
       " LIMIT ?3",
       err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", project_id ? project_id : "");
   aimee_pg_bind_int64(st, "?2", community_gen);
   aimee_pg_bind_int64(st, "?3", max);
   int n = 0;
   while (n < max && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      const char *node = aimee_pg_column_text(st, 0);
      const char *comm = aimee_pg_column_text(st, 1);
      const char *oc = aimee_pg_column_text(st, 2);
      const char *actor = aimee_pg_column_text(st, 3);
      snprintf(out[n].node_id, sizeof(out[n].node_id), "%s", node ? node : "");
      snprintf(out[n].community, sizeof(out[n].community), "%s", comm ? comm : "");
      snprintf(out[n].answer_outcome, sizeof(out[n].answer_outcome), "%s", oc ? oc : "");
      snprintf(out[n].actor_source, sizeof(out[n].actor_source), "%s", actor ? actor : "");
      out[n].ts_days = (long)aimee_pg_column_int64(st, 4);
      out[n].confirmed = (int)aimee_pg_column_int64(st, 5);
      n++;
   }
   aimee_pg_finalize(st);
   return n;
}
