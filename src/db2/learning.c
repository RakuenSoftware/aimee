/* db2/learning.c: learning_signals + learning_proposals primitives —
 * Postgres via libpq. */

#include "db2_learning.h"
#include "db2.h"
#include "db2_internal.h"
#include "db_postgres.h"
#include "log.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LRN_ERRBUF 256

static void lrn_copy_text(char *dst, size_t dstsz, const char *src, const char *fallback)
{
   const char *s = src ? src : (fallback ? fallback : "");
   snprintf(dst, dstsz, "%s", s);
}

static void lrn_load_proposal_row(aimee_pg_stmt_t *st, learning_proposal_t *out)
{
   memset(out, 0, sizeof(*out));
   out->id = aimee_pg_column_int(st, 0);
   out->signal_id = aimee_pg_column_int(st, 1);
   lrn_copy_text(out->sink, sizeof(out->sink), aimee_pg_column_text(st, 2), "");
   lrn_copy_text(out->state, sizeof(out->state), aimee_pg_column_text(st, 3), "");
   lrn_copy_text(out->target_key, sizeof(out->target_key), aimee_pg_column_text(st, 4), "");
   out->target_memory_id = aimee_pg_column_int64(st, 5);
   lrn_copy_text(out->action_json, sizeof(out->action_json), aimee_pg_column_text(st, 6), "{}");
   lrn_copy_text(out->evidence_refs, sizeof(out->evidence_refs), aimee_pg_column_text(st, 7), "[]");
   out->corroboration_count = aimee_pg_column_int(st, 8);
   lrn_copy_text(out->expires_at, sizeof(out->expires_at), aimee_pg_column_text(st, 9), "");
   lrn_copy_text(out->committed_at, sizeof(out->committed_at), aimee_pg_column_text(st, 10), "");
   lrn_copy_text(out->archive_reason, sizeof(out->archive_reason), aimee_pg_column_text(st, 11),
                 "");
   lrn_copy_text(out->created_at, sizeof(out->created_at), aimee_pg_column_text(st, 12), "");
   lrn_copy_text(out->updated_at, sizeof(out->updated_at), aimee_pg_column_text(st, 13), "");
}

static void lrn_load_observation_row(aimee_pg_stmt_t *st, learning_observation_t *out)
{
   memset(out, 0, sizeof(*out));
   lrn_copy_text(out->observation_id, sizeof(out->observation_id), aimee_pg_column_text(st, 0),
                 "");
   lrn_copy_text(out->scope_kind, sizeof(out->scope_kind), aimee_pg_column_text(st, 1), "");
   lrn_copy_text(out->scope_id, sizeof(out->scope_id), aimee_pg_column_text(st, 2), "");
   lrn_copy_text(out->observation_type, sizeof(out->observation_type),
                 aimee_pg_column_text(st, 3), "");
   lrn_copy_text(out->title, sizeof(out->title), aimee_pg_column_text(st, 4), "");
   lrn_copy_text(out->summary, sizeof(out->summary), aimee_pg_column_text(st, 5), "");
   lrn_copy_text(out->status, sizeof(out->status), aimee_pg_column_text(st, 6), "");
   out->confidence = aimee_pg_column_double(st, 7);
   lrn_copy_text(out->evidence_window_start, sizeof(out->evidence_window_start),
                 aimee_pg_column_text(st, 8), "");
   lrn_copy_text(out->evidence_window_end, sizeof(out->evidence_window_end),
                 aimee_pg_column_text(st, 9), "");
   lrn_copy_text(out->synthesis_policy_version, sizeof(out->synthesis_policy_version),
                 aimee_pg_column_text(st, 10), "");
   out->evidence_count = aimee_pg_column_int(st, 11);
   out->independent_session_count = aimee_pg_column_int(st, 12);
   lrn_copy_text(out->supersedes, sizeof(out->supersedes), aimee_pg_column_text(st, 13), "");
   lrn_copy_text(out->superseded_by, sizeof(out->superseded_by), aimee_pg_column_text(st, 14), "");
   lrn_copy_text(out->created_at, sizeof(out->created_at), aimee_pg_column_text(st, 15), "");
   lrn_copy_text(out->refreshed_at, sizeof(out->refreshed_at), aimee_pg_column_text(st, 16), "");
   lrn_copy_text(out->retired_at, sizeof(out->retired_at), aimee_pg_column_text(st, 17), "");
}

#define LRN_OBSERVATION_COLS                                                                     \
   "observation_id,scope_kind,scope_id,observation_type,title,summary,status,confidence,"         \
   "evidence_window_start,evidence_window_end,synthesis_policy_version,evidence_count,"          \
   "independent_session_count,supersedes,superseded_by,created_at,refreshed_at,retired_at"

static int lrn_observation_type_valid(const char *type)
{
   static const char *const types[] = {"recurring_failure", "failed_strategy",
                                       "successful_recovery", "missing_precondition",
                                       "tool_misuse", "environment_mismatch",
                                       "unstable_procedure", NULL};
   if (!type)
      return 0;
   for (int i = 0; types[i]; i++)
      if (strcmp(type, types[i]) == 0)
         return 1;
   return 0;
}

static int lrn_outcome_valid(const char *outcome)
{
   return outcome && (strcmp(outcome, "unknown") == 0 || strcmp(outcome, "success") == 0 ||
                       strcmp(outcome, "failure") == 0 || strcmp(outcome, "corrected") == 0 ||
                       strcmp(outcome, "abandoned") == 0);
}

static int lrn_event_in_scope(void *conn, int64_t source_event_id, const char *scope_kind,
                              const char *scope_id)
{
   char err[LRN_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       conn,
       "SELECT 1 FROM interaction_event_embeddings"
       " WHERE source_event_id=?1 AND scope_kind=?2 AND scope_id=?3",
       err, sizeof(err));
   if (!st)
      return 0;
   aimee_pg_bind_int64(st, "?1", source_event_id);
   aimee_pg_bind_text(st, "?2", scope_kind);
   aimee_pg_bind_text(st, "?3", scope_id);
   int found = aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW;
   aimee_pg_finalize(st);
   return found;
}

static int lrn_recompute_observation(void *conn, const char *observation_id)
{
   char err[LRN_ERRBUF] = "";
   static const char *sql =
       "UPDATE learning_observations SET"
       " evidence_count=(SELECT COUNT(*) FROM learning_observation_evidence oe"
       "   WHERE oe.observation_id=?1),"
       " independent_session_count=(SELECT COUNT(DISTINCT ie.session_id)"
       "   FROM learning_observation_evidence oe JOIN interaction_event_embeddings ie"
       "     ON ie.source_event_id=oe.source_event_id"
       "   WHERE oe.observation_id=?1 AND oe.stance='supports'),"
       " evidence_window_start=COALESCE((SELECT MIN(observed_at)"
       "   FROM learning_observation_evidence oe WHERE oe.observation_id=?1),''),"
       " evidence_window_end=COALESCE((SELECT MAX(observed_at)"
       "   FROM learning_observation_evidence oe WHERE oe.observation_id=?1),''),"
       " confidence=CASE"
       "   WHEN (0.5 + 0.1*(SELECT COUNT(*) FROM learning_observation_evidence oe"
       "      WHERE oe.observation_id=?1 AND stance='supports')"
       "      - 0.15*(SELECT COUNT(*) FROM learning_observation_evidence oe"
       "      WHERE oe.observation_id=?1 AND stance='contradicts')) < 0.0 THEN 0.0"
       "   WHEN (0.5 + 0.1*(SELECT COUNT(*) FROM learning_observation_evidence oe"
       "      WHERE oe.observation_id=?1 AND stance='supports')"
       "      - 0.15*(SELECT COUNT(*) FROM learning_observation_evidence oe"
       "      WHERE oe.observation_id=?1 AND stance='contradicts')) > 0.95 THEN 0.95"
       "   ELSE (0.5 + 0.1*(SELECT COUNT(*) FROM learning_observation_evidence oe"
       "      WHERE oe.observation_id=?1 AND stance='supports')"
       "      - 0.15*(SELECT COUNT(*) FROM learning_observation_evidence oe"
       "      WHERE oe.observation_id=?1 AND stance='contradicts')) END,"
       " status=CASE WHEN status='rejected' THEN 'rejected'"
       "   WHEN observation_type='unstable_procedure' AND EXISTS(SELECT 1"
       "     FROM learning_observation_evidence oe WHERE oe.observation_id=?1"
       "       AND stance='contradicts') THEN 'active'"
       "   WHEN observation_type='successful_recovery'"
       "     AND (SELECT COUNT(*) FROM learning_observation_evidence oe"
       "       WHERE oe.observation_id=?1 AND stance='supports')>=2"
       "     AND (SELECT COUNT(DISTINCT ie.session_id)"
       "       FROM learning_observation_evidence oe JOIN interaction_event_embeddings ie"
       "       ON ie.source_event_id=oe.source_event_id WHERE oe.observation_id=?1"
       "       AND oe.stance='supports')>=2 THEN 'active'"
       "   WHEN (SELECT COUNT(*) FROM learning_observation_evidence oe"
       "       WHERE oe.observation_id=?1 AND stance='supports')>=3"
       "     AND (SELECT COUNT(DISTINCT ie.session_id)"
       "       FROM learning_observation_evidence oe JOIN interaction_event_embeddings ie"
       "       ON ie.source_event_id=oe.source_event_id WHERE oe.observation_id=?1"
       "       AND oe.stance='supports')>=2 THEN 'active'"
       "   WHEN status IN ('active','retired') THEN 'retired' ELSE 'candidate' END,"
       " retired_at=CASE"
       "   WHEN status='rejected' THEN retired_at"
       "   WHEN ((observation_type='unstable_procedure' AND EXISTS(SELECT 1"
       "       FROM learning_observation_evidence oe WHERE oe.observation_id=?1"
       "       AND stance='contradicts'))"
       "     OR (observation_type='successful_recovery'"
       "       AND (SELECT COUNT(*) FROM learning_observation_evidence oe"
       "         WHERE oe.observation_id=?1 AND stance='supports')>=2"
       "       AND (SELECT COUNT(DISTINCT ie.session_id)"
       "         FROM learning_observation_evidence oe JOIN interaction_event_embeddings ie"
       "         ON ie.source_event_id=oe.source_event_id WHERE oe.observation_id=?1"
       "         AND oe.stance='supports')>=2)"
       "     OR ((SELECT COUNT(*) FROM learning_observation_evidence oe"
       "         WHERE oe.observation_id=?1 AND stance='supports')>=3)) THEN ''"
       "   WHEN status IN ('active','retired') THEN CASE WHEN retired_at=''"
       "      THEN pg_now_text() ELSE retired_at END ELSE '' END,"
       " refreshed_at=pg_now_text() WHERE observation_id=?1";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", observation_id);
   int rc = aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_DONE ? 0 : -1;
   aimee_pg_finalize(st);
   return rc;
}

int db2_learning_observation_refresh(
    const char *observation_id, const char *scope_kind, const char *scope_id,
    const char *observation_type, const char *title, const char *summary,
    const char *policy_version, const learning_observation_evidence_input_t *evidence,
    int evidence_count, const char *supersedes)
{
   const char *sk = scope_kind && scope_kind[0] ? scope_kind : "workspace";
   const char *si = scope_id ? scope_id : "";
   if (!observation_id || !observation_id[0] || !lrn_observation_type_valid(observation_type) ||
       !policy_version || !policy_version[0] || evidence_count < 0 ||
       (evidence_count > 0 && !evidence))
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;
   for (int i = 0; i < evidence_count; i++)
      if (evidence[i].source_event_id <= 0 ||
          (strcmp(evidence[i].stance ? evidence[i].stance : "", "supports") != 0 &&
           strcmp(evidence[i].stance ? evidence[i].stance : "", "contradicts") != 0) ||
          !lrn_event_in_scope(conn, evidence[i].source_event_id, sk, si))
         return -1;

   char err[LRN_ERRBUF] = "";
   if (aimee_pg_exec(conn, "BEGIN", err, sizeof(err)) != 0)
      return -1;
   int rc = -1;
   static const char *upsert =
       "INSERT INTO learning_observations"
       " (observation_id,scope_kind,scope_id,observation_type,title,summary,status,confidence,"
       " synthesis_policy_version,supersedes,created_at,refreshed_at)"
       " VALUES (?1,?2,?3,?4,?5,?6,'candidate',0.0,?7,?8,pg_now_text(),pg_now_text())"
       " ON CONFLICT (observation_id) DO UPDATE SET"
       " title=EXCLUDED.title,summary=EXCLUDED.summary,"
       " synthesis_policy_version=EXCLUDED.synthesis_policy_version,"
       " supersedes=CASE WHEN EXCLUDED.supersedes<>'' THEN EXCLUDED.supersedes"
       "                 ELSE learning_observations.supersedes END,"
       " refreshed_at=pg_now_text()"
       " WHERE learning_observations.scope_kind=EXCLUDED.scope_kind"
       "   AND learning_observations.scope_id=EXCLUDED.scope_id"
       "   AND learning_observations.observation_type=EXCLUDED.observation_type";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, upsert, err, sizeof(err));
   if (!st)
      goto done;
   aimee_pg_bind_text(st, "?1", observation_id);
   aimee_pg_bind_text(st, "?2", sk);
   aimee_pg_bind_text(st, "?3", si);
   aimee_pg_bind_text(st, "?4", observation_type);
   aimee_pg_bind_text(st, "?5", title ? title : "");
   aimee_pg_bind_text(st, "?6", summary ? summary : "");
   aimee_pg_bind_text(st, "?7", policy_version);
   aimee_pg_bind_text(st, "?8", supersedes ? supersedes : "");
   if (aimee_pg_step(st, err, sizeof(err)) != AIMEE_PG_DONE)
   {
      aimee_pg_finalize(st);
      goto done;
   }
   aimee_pg_finalize(st);

   static const char *attach =
       "INSERT INTO learning_observation_evidence"
       " (observation_id,evidence_kind,source_event_id,source_span,stance,observed_at)"
       " SELECT ?1,'interaction_event',ie.source_event_id,?3,?4,ie.created_at"
       " FROM interaction_event_embeddings ie WHERE ie.source_event_id=?2"
       " ON CONFLICT (observation_id,evidence_kind,source_event_id,source_span,stance) DO NOTHING";
   for (int i = 0; i < evidence_count; i++)
   {
      st = aimee_pg_prepare(conn, attach, err, sizeof(err));
      if (!st)
         goto done;
      aimee_pg_bind_text(st, "?1", observation_id);
      aimee_pg_bind_int64(st, "?2", evidence[i].source_event_id);
      aimee_pg_bind_text(st, "?3", evidence[i].source_span ? evidence[i].source_span : "");
      aimee_pg_bind_text(st, "?4", evidence[i].stance);
      if (aimee_pg_step(st, err, sizeof(err)) != AIMEE_PG_DONE)
      {
         aimee_pg_finalize(st);
         goto done;
      }
      aimee_pg_finalize(st);
   }
   if (supersedes && supersedes[0])
   {
      st = aimee_pg_prepare(conn,
                            "UPDATE learning_observations SET superseded_by=?2,status='retired',"
                            " retired_at=CASE WHEN retired_at='' THEN pg_now_text() ELSE retired_at END"
                            " WHERE observation_id=?1 AND observation_id<>?2",
                            err, sizeof(err));
      if (!st)
         goto done;
      aimee_pg_bind_text(st, "?1", supersedes);
      aimee_pg_bind_text(st, "?2", observation_id);
      if (aimee_pg_step(st, err, sizeof(err)) != AIMEE_PG_DONE)
      {
         aimee_pg_finalize(st);
         goto done;
      }
      aimee_pg_finalize(st);
   }
   rc = lrn_recompute_observation(conn, observation_id);
done:
   if (rc == 0)
      (void)aimee_pg_exec(conn, "COMMIT", err, sizeof(err));
   else
   {
      (void)aimee_pg_exec(conn, "ROLLBACK", err, sizeof(err));
      LOG_WARN("db2.learning", "observation refresh: %s", err);
   }
   return rc;
}

int db2_learning_observation_add_evidence(const char *observation_id, int64_t source_event_id,
                                          const char *source_span, const char *stance)
{
   learning_observation_t obs;
   if (db2_learning_observation_get(observation_id, &obs) != 0)
      return -1;
   learning_observation_evidence_input_t ev = {source_event_id, source_span, stance};
   return db2_learning_observation_refresh(observation_id, obs.scope_kind, obs.scope_id,
                                           obs.observation_type, obs.title, obs.summary,
                                           obs.synthesis_policy_version, &ev, 1, obs.supersedes);
}

int db2_learning_observation_refresh_recurrence(const char *observation_id, const char *role,
                                                const char *failure_mode, const char *title,
                                                const char *summary, int64_t max_event_id)
{
   if (!observation_id || !observation_id[0] || !failure_mode || !failure_mode[0] ||
       max_event_id <= 0)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;
   char err[LRN_ERRBUF] = "";
   if (aimee_pg_exec(conn, "BEGIN", err, sizeof(err)) != 0)
      return -1;
   int rc = -1;
   static const char *upsert =
       "INSERT INTO learning_observations"
       " (observation_id,scope_kind,scope_id,observation_type,title,summary,status,confidence,"
       "  synthesis_policy_version,created_at,refreshed_at)"
       " VALUES (?1,'workspace','','recurring_failure',?2,?3,'candidate',0.0,"
       "         'recurrence-v1',pg_now_text(),pg_now_text())"
       " ON CONFLICT (observation_id) DO UPDATE SET title=EXCLUDED.title,summary=EXCLUDED.summary,"
       " synthesis_policy_version=EXCLUDED.synthesis_policy_version,refreshed_at=pg_now_text(),"
       " status=CASE WHEN learning_observations.status='rejected' THEN 'rejected' ELSE 'candidate' END,"
       " retired_at=''";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, upsert, err, sizeof(err));
   if (!st)
      goto done;
   aimee_pg_bind_text(st, "?1", observation_id);
   aimee_pg_bind_text(st, "?2", title ? title : "");
   aimee_pg_bind_text(st, "?3", summary ? summary : "");
   if (aimee_pg_step(st, err, sizeof(err)) != AIMEE_PG_DONE)
   {
      aimee_pg_finalize(st);
      goto done;
   }
   aimee_pg_finalize(st);

   static const char *attach =
       "INSERT INTO learning_observation_evidence"
       " (observation_id,evidence_kind,source_event_id,source_span,stance,observed_at)"
       " SELECT ?1,'interaction_event',source_event_id,'','supports',created_at"
       " FROM interaction_event_embeddings"
       " WHERE event_type='delegate_exit' AND role=?2 AND failure_mode=?3 AND source_event_id<=?4"
       " ON CONFLICT (observation_id,evidence_kind,source_event_id,source_span,stance) DO NOTHING";
   st = aimee_pg_prepare(conn, attach, err, sizeof(err));
   if (!st)
      goto done;
   aimee_pg_bind_text(st, "?1", observation_id);
   aimee_pg_bind_text(st, "?2", role ? role : "");
   aimee_pg_bind_text(st, "?3", failure_mode);
   aimee_pg_bind_int64(st, "?4", max_event_id);
   if (aimee_pg_step(st, err, sizeof(err)) != AIMEE_PG_DONE)
   {
      aimee_pg_finalize(st);
      goto done;
   }
   aimee_pg_finalize(st);

   static const char *recompute =
       "UPDATE learning_observations SET"
       " evidence_count=(SELECT COUNT(*) FROM learning_observation_evidence oe"
       "   WHERE oe.observation_id=?1),"
       " independent_session_count=(SELECT COUNT(DISTINCT ie.session_id)"
       "   FROM learning_observation_evidence oe JOIN interaction_event_embeddings ie"
       "     ON ie.source_event_id=oe.source_event_id WHERE oe.observation_id=?1),"
       " evidence_window_start=COALESCE((SELECT MIN(observed_at)"
       "   FROM learning_observation_evidence oe WHERE oe.observation_id=?1),''),"
       " evidence_window_end=COALESCE((SELECT MAX(observed_at)"
       "   FROM learning_observation_evidence oe WHERE oe.observation_id=?1),''),"
       " confidence=CASE WHEN (SELECT COUNT(*) FROM learning_observation_evidence oe"
       "   WHERE oe.observation_id=?1)>=5 THEN 0.95 ELSE 0.5+0.1*(SELECT COUNT(*)"
       "   FROM learning_observation_evidence oe WHERE oe.observation_id=?1) END,"
       " status=CASE WHEN status='rejected' THEN 'rejected'"
       "   WHEN (SELECT COUNT(*) FROM learning_observation_evidence oe WHERE oe.observation_id=?1)>=3"
       "    AND (SELECT COUNT(DISTINCT ie.session_id) FROM learning_observation_evidence oe"
       "      JOIN interaction_event_embeddings ie ON ie.source_event_id=oe.source_event_id"
       "      WHERE oe.observation_id=?1)>=2 THEN 'active' ELSE 'retired' END,"
       " retired_at=CASE WHEN (SELECT COUNT(*) FROM learning_observation_evidence oe"
       "   WHERE oe.observation_id=?1)<3 THEN pg_now_text() ELSE '' END,refreshed_at=pg_now_text()"
       " WHERE observation_id=?1";
   st = aimee_pg_prepare(conn, recompute, err, sizeof(err));
   if (!st)
      goto done;
   aimee_pg_bind_text(st, "?1", observation_id);
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_DONE)
      rc = 0;
   aimee_pg_finalize(st);
done:
   if (rc == 0)
      (void)aimee_pg_exec(conn, "COMMIT", err, sizeof(err));
   else
   {
      (void)aimee_pg_exec(conn, "ROLLBACK", err, sizeof(err));
      LOG_WARN("db2.learning", "observation refresh: %s", err);
   }
   return rc;
}

int db2_learning_observation_get(const char *observation_id, learning_observation_t *out)
{
   if (!observation_id || !observation_id[0] || !out)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;
   static const char *sql = "SELECT " LRN_OBSERVATION_COLS
                            " FROM learning_observations WHERE observation_id=?1";
   char err[LRN_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", observation_id);
   int rc = -1;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      lrn_load_observation_row(st, out);
      rc = 0;
   }
   aimee_pg_finalize(st);
   return rc;
}

int db2_learning_observation_list(const char *status, const char *scope_kind,
                                  const char *scope_id, int limit, learning_observation_t *out,
                                  int max)
{
   if (!out || max <= 0)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;
   if (limit <= 0 || limit > max)
      limit = max;
   static const char *sql = "SELECT " LRN_OBSERVATION_COLS
                            " FROM learning_observations"
                            " WHERE (?1='' OR status=?1) AND (?2='' OR scope_kind=?2)"
                            " AND (?3='' OR scope_id=?3) ORDER BY refreshed_at DESC LIMIT ?4";
   char err[LRN_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", status ? status : "");
   aimee_pg_bind_text(st, "?2", scope_kind ? scope_kind : "");
   aimee_pg_bind_text(st, "?3", scope_id ? scope_id : "");
   aimee_pg_bind_int(st, "?4", limit);
   int n = 0;
   while (n < max && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
      lrn_load_observation_row(st, &out[n++]);
   aimee_pg_finalize(st);
   return n;
}

int db2_learning_observation_evidence_ids(const char *observation_id, int64_t *out, int max)
{
   if (!observation_id || !observation_id[0] || !out || max <= 0)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;
   static const char *sql = "SELECT source_event_id FROM learning_observation_evidence"
                            " WHERE observation_id=?1 ORDER BY source_event_id LIMIT ?2";
   char err[LRN_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", observation_id);
   aimee_pg_bind_int(st, "?2", max);
   int n = 0;
   while (n < max && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
      out[n++] = aimee_pg_column_int64(st, 0);
   aimee_pg_finalize(st);
   return n;
}

int db2_learning_observation_set_status(const char *observation_id, const char *status)
{
   if (!observation_id || !observation_id[0] || !status ||
       (strcmp(status, "candidate") != 0 && strcmp(status, "active") != 0 &&
        strcmp(status, "retired") != 0 && strcmp(status, "rejected") != 0))
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;
   static const char *sql =
       "UPDATE learning_observations SET status=?2,refreshed_at=pg_now_text(),"
       " retired_at=CASE WHEN ?2='retired' THEN pg_now_text() ELSE '' END"
       " WHERE observation_id=?1";
   char err[LRN_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", observation_id);
   aimee_pg_bind_text(st, "?2", status);
   int rc = aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_DONE ? 0 : -1;
   aimee_pg_finalize(st);
   return rc;
}

int db2_learning_observations_reconcile(void)
{
   void *conn = db2_conn();
   if (!conn)
      return -1;
   char err[LRN_ERRBUF] = "";
   aimee_pg_stmt_t *st =
       aimee_pg_prepare(conn, "SELECT COUNT(*) FROM learning_observations", err, sizeof(err));
   if (!st)
      return -1;
   int count = 0;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
      count = aimee_pg_column_int(st, 0);
   aimee_pg_finalize(st);
   if (count <= 0)
      return 0;
   char(*ids)[64] = calloc((size_t)count, sizeof(*ids));
   if (!ids)
      return -1;
   st = aimee_pg_prepare(conn, "SELECT observation_id FROM learning_observations",
                         err, sizeof(err));
   if (!st)
   {
      free(ids);
      return -1;
   }
   int n = 0;
   while (n < count && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      lrn_copy_text(ids[n], sizeof(ids[n]), aimee_pg_column_text(st, 0), "");
      n++;
   }
   aimee_pg_finalize(st);
   int rc = 0;
   for (int i = 0; i < n; i++)
      if (lrn_recompute_observation(conn, ids[i]) != 0)
         rc = -1;
   free(ids);
   return rc;
}

int db2_learning_application_record(const learning_application_event_t *event)
{
   if (!event || !event->application_id[0] || event->source_event_id <= 0 ||
       !lrn_outcome_valid(event->outcome) || event->latency_ms < 0 || event->tool_count < 0 ||
       event->turn_count < 0 || event->token_count < 0 ||
       (event->rendered && !event->retrieved) || (event->selected && !event->rendered) ||
       (event->applied && !event->selected))
      return -1;
   const char *sk = event->scope_kind[0] ? event->scope_kind : "workspace";
   if (!lrn_event_in_scope(db2_conn(), event->source_event_id, sk, event->scope_id))
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;
   static const char *sql =
       "INSERT INTO learning_application_events"
       " (application_id,source_event_id,session_id,scope_kind,scope_id,task_family,"
       " observation_id,procedure_artifact_id,proposal_id,retrieved,rendered,selected,applied,"
       " outcome,failure_class,human_correction,latency_ms,tool_count,turn_count,token_count,"
       " retrieved_refs,rendered_refs,selected_refs,applied_refs,created_at)"
       " VALUES (?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,?15,?16,?17,?18,"
       " ?19,?20,?21,?22,?23,?24,pg_now_text())"
       " ON CONFLICT (application_id) DO UPDATE SET"
       " source_event_id=EXCLUDED.source_event_id,session_id=EXCLUDED.session_id,"
       " scope_kind=EXCLUDED.scope_kind,scope_id=EXCLUDED.scope_id,"
       " task_family=EXCLUDED.task_family,observation_id=EXCLUDED.observation_id,"
       " procedure_artifact_id=EXCLUDED.procedure_artifact_id,proposal_id=EXCLUDED.proposal_id,"
       " retrieved=EXCLUDED.retrieved,rendered=EXCLUDED.rendered,selected=EXCLUDED.selected,"
       " applied=EXCLUDED.applied,outcome=EXCLUDED.outcome,"
       " failure_class=EXCLUDED.failure_class,human_correction=EXCLUDED.human_correction,"
       " latency_ms=EXCLUDED.latency_ms,tool_count=EXCLUDED.tool_count,"
       " turn_count=EXCLUDED.turn_count,token_count=EXCLUDED.token_count,"
       " retrieved_refs=EXCLUDED.retrieved_refs,rendered_refs=EXCLUDED.rendered_refs,"
       " selected_refs=EXCLUDED.selected_refs,applied_refs=EXCLUDED.applied_refs";
   char err[LRN_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", event->application_id);
   aimee_pg_bind_int64(st, "?2", event->source_event_id);
   aimee_pg_bind_text(st, "?3", event->session_id);
   aimee_pg_bind_text(st, "?4", sk);
   aimee_pg_bind_text(st, "?5", event->scope_id);
   aimee_pg_bind_text(st, "?6", event->task_family);
   aimee_pg_bind_text(st, "?7", event->observation_id);
   aimee_pg_bind_text(st, "?8", event->procedure_artifact_id);
   aimee_pg_bind_int(st, "?9", event->proposal_id);
   aimee_pg_bind_int(st, "?10", event->retrieved ? 1 : 0);
   aimee_pg_bind_int(st, "?11", event->rendered ? 1 : 0);
   aimee_pg_bind_int(st, "?12", event->selected ? 1 : 0);
   aimee_pg_bind_int(st, "?13", event->applied ? 1 : 0);
   aimee_pg_bind_text(st, "?14", event->outcome);
   aimee_pg_bind_text(st, "?15", event->failure_class);
   aimee_pg_bind_text(st, "?16", event->human_correction);
   aimee_pg_bind_int64(st, "?17", event->latency_ms);
   aimee_pg_bind_int(st, "?18", event->tool_count);
   aimee_pg_bind_int(st, "?19", event->turn_count);
   aimee_pg_bind_int64(st, "?20", event->token_count);
   aimee_pg_bind_text(st, "?21", event->retrieved_refs[0] ? event->retrieved_refs : "[]");
   aimee_pg_bind_text(st, "?22", event->rendered_refs[0] ? event->rendered_refs : "[]");
   aimee_pg_bind_text(st, "?23", event->selected_refs[0] ? event->selected_refs : "[]");
   aimee_pg_bind_text(st, "?24", event->applied_refs[0] ? event->applied_refs : "[]");
   int rc = aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_DONE ? 0 : -1;
   if (rc != 0)
      LOG_WARN("db2.learning", "application record: %s", err);
   aimee_pg_finalize(st);
   return rc;
}

int db2_learning_application_get(const char *application_id, learning_application_event_t *out)
{
   if (!application_id || !application_id[0] || !out)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;
   static const char *sql =
       "SELECT application_id,source_event_id,session_id,scope_kind,scope_id,task_family,"
       " observation_id,procedure_artifact_id,proposal_id,retrieved,rendered,selected,applied,"
       " outcome,failure_class,human_correction,latency_ms,tool_count,turn_count,token_count,"
       " retrieved_refs,rendered_refs,selected_refs,applied_refs"
       " FROM learning_application_events WHERE application_id=?1";
   char err[LRN_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", application_id);
   if (aimee_pg_step(st, err, sizeof(err)) != AIMEE_PG_ROW)
   {
      aimee_pg_finalize(st);
      return -1;
   }
   memset(out, 0, sizeof(*out));
   lrn_copy_text(out->application_id, sizeof(out->application_id), aimee_pg_column_text(st, 0), "");
   out->source_event_id = aimee_pg_column_int64(st, 1);
   lrn_copy_text(out->session_id, sizeof(out->session_id), aimee_pg_column_text(st, 2), "");
   lrn_copy_text(out->scope_kind, sizeof(out->scope_kind), aimee_pg_column_text(st, 3), "");
   lrn_copy_text(out->scope_id, sizeof(out->scope_id), aimee_pg_column_text(st, 4), "");
   lrn_copy_text(out->task_family, sizeof(out->task_family), aimee_pg_column_text(st, 5), "");
   lrn_copy_text(out->observation_id, sizeof(out->observation_id), aimee_pg_column_text(st, 6), "");
   lrn_copy_text(out->procedure_artifact_id, sizeof(out->procedure_artifact_id),
                 aimee_pg_column_text(st, 7), "");
   out->proposal_id = aimee_pg_column_int(st, 8);
   out->retrieved = aimee_pg_column_int(st, 9);
   out->rendered = aimee_pg_column_int(st, 10);
   out->selected = aimee_pg_column_int(st, 11);
   out->applied = aimee_pg_column_int(st, 12);
   lrn_copy_text(out->outcome, sizeof(out->outcome), aimee_pg_column_text(st, 13), "unknown");
   lrn_copy_text(out->failure_class, sizeof(out->failure_class), aimee_pg_column_text(st, 14), "");
   lrn_copy_text(out->human_correction, sizeof(out->human_correction), aimee_pg_column_text(st, 15), "");
   out->latency_ms = aimee_pg_column_int64(st, 16);
   out->tool_count = aimee_pg_column_int(st, 17);
   out->turn_count = aimee_pg_column_int(st, 18);
   out->token_count = aimee_pg_column_int64(st, 19);
   lrn_copy_text(out->retrieved_refs, sizeof(out->retrieved_refs), aimee_pg_column_text(st, 20), "[]");
   lrn_copy_text(out->rendered_refs, sizeof(out->rendered_refs), aimee_pg_column_text(st, 21), "[]");
   lrn_copy_text(out->selected_refs, sizeof(out->selected_refs), aimee_pg_column_text(st, 22), "[]");
   lrn_copy_text(out->applied_refs, sizeof(out->applied_refs), aimee_pg_column_text(st, 23), "[]");
   aimee_pg_finalize(st);
   return 0;
}

void db2_learning_proposals_archive_expired(void)
{
   void *conn = db2_conn();
   if (!conn)
      return;

   static const char *sql =
       "UPDATE learning_proposals"
       " SET state = 'archived', archive_reason = 'expired', updated_at = pg_now_text()"
       " WHERE state = 'pending' AND expires_at != '' AND expires_at < pg_now_text()";
   char err[LRN_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return;
   if (aimee_pg_step(st, err, sizeof(err)) != AIMEE_PG_DONE)
      LOG_WARN("db2.learning", "archive_expired: %s", err);
   aimee_pg_finalize(st);
}

int db2_learning_proposal_archive(int id, const char *reason)
{
   if (id <= 0)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;

   static const char *sql =
       "UPDATE learning_proposals"
       " SET state = 'archived', archive_reason = ?2, updated_at = pg_now_text()"
       " WHERE id = ?1";
   char err[LRN_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_int(st, "?1", id);
   aimee_pg_bind_text(st, "?2", reason ? reason : "");
   int rc = (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_DONE) ? 0 : -1;
   aimee_pg_finalize(st);
   return rc;
}

int db2_learning_proposal_bump_corroboration(int id)
{
   if (id <= 0)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;

   static const char *sql =
       "UPDATE learning_proposals"
       " SET corroboration_count = corroboration_count + 1, updated_at = pg_now_text()"
       " WHERE id = ?1 AND state = 'pending'";
   char err[LRN_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_int(st, "?1", id);
   int rc = (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_DONE) ? 0 : -1;
   aimee_pg_finalize(st);
   return rc;
}

int db2_learning_proposal_refresh_evidence(int id, const char *evidence_refs_json)
{
   if (id <= 0 || !evidence_refs_json)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;
   static const char *sql =
       "UPDATE learning_proposals SET evidence_refs=?2,updated_at=pg_now_text()"
       " WHERE id=?1 AND state='pending'";
   char err[LRN_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_int(st, "?1", id);
   aimee_pg_bind_text(st, "?2", evidence_refs_json);
   int rc = aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_DONE ? 0 : -1;
   aimee_pg_finalize(st);
   return rc;
}

int db2_learning_commits_in_last_7_days(const char *sink)
{
   if (!sink || !*sink)
      return 0;
   return db2_scalar_int_text("SELECT COUNT(*) FROM learning_proposals"
                              " WHERE sink = ?1 AND state = 'committed'"
                              "   AND committed_at >= pg_now_text('-7 days')",
                              sink, 0);
}

int db2_learning_signal_insert(const learning_signal_input_t *input, const char *source_session)
{
   if (!input)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;

   static const char *sql =
       "INSERT INTO learning_signals ("
       " signal_type, source, polarity, title, description, target_key, target_memory_id,"
       " correction_text, workflow_project, workflow_signal_type, evidence_refs, source_session,"
       " created_at)"
       " VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, pg_now_text())"
       " RETURNING id";
   char err[LRN_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", input->signal_type);
   aimee_pg_bind_text(st, "?2", input->source[0] ? input->source : "explicit");
   aimee_pg_bind_text(st, "?3", input->polarity);
   aimee_pg_bind_text(st, "?4", input->title);
   aimee_pg_bind_text(st, "?5", input->description);
   aimee_pg_bind_text(st, "?6", input->target_key);
   aimee_pg_bind_int64(st, "?7", input->target_memory_id);
   aimee_pg_bind_text(st, "?8", input->correction_text);
   aimee_pg_bind_text(st, "?9", input->workflow_project);
   aimee_pg_bind_text(st, "?10", input->workflow_signal_type);
   aimee_pg_bind_text(st, "?11", input->evidence_refs_json ? input->evidence_refs_json : "[]");
   aimee_pg_bind_text(st, "?12", source_session ? source_session : "");

   int id = -1;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
      id = aimee_pg_column_int(st, 0);
   aimee_pg_finalize(st);
   return id;
}

int db2_learning_proposal_find_pending(const char *sink, const char *target_key,
                                       int64_t target_memory_id)
{
   if (!sink || !*sink)
      return 0;
   void *conn = db2_conn();
   if (!conn)
      return 0;

   static const char *sql =
       "SELECT id FROM learning_proposals"
       " WHERE sink = ?1 AND state = 'pending' AND target_key = ?2 AND target_memory_id = ?3"
       " ORDER BY id DESC LIMIT 1";
   char err[LRN_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return 0;
   aimee_pg_bind_text(st, "?1", sink);
   aimee_pg_bind_text(st, "?2", target_key ? target_key : "");
   aimee_pg_bind_int64(st, "?3", target_memory_id);
   int id = 0;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
      id = aimee_pg_column_int(st, 0);
   aimee_pg_finalize(st);
   return id;
}

int db2_learning_proposal_insert(int signal_id, const char *sink, const char *target_key,
                                 int64_t target_memory_id, const char *action_json,
                                 const char *evidence_refs, const char *expires_at)
{
   if (!sink || !*sink || !action_json)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;

   static const char *sql =
       "INSERT INTO learning_proposals ("
       " signal_id, sink, state, target_key, target_memory_id, action_json, evidence_refs,"
       " corroboration_count, expires_at, created_at, updated_at)"
       " VALUES (?1, ?2, 'pending', ?3, ?4, ?5, ?6, 1, ?7, pg_now_text(), pg_now_text())"
       " RETURNING id";
   char err[LRN_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_int(st, "?1", signal_id);
   aimee_pg_bind_text(st, "?2", sink);
   aimee_pg_bind_text(st, "?3", target_key ? target_key : "");
   aimee_pg_bind_int64(st, "?4", target_memory_id);
   aimee_pg_bind_text(st, "?5", action_json);
   aimee_pg_bind_text(st, "?6", evidence_refs ? evidence_refs : "[]");
   aimee_pg_bind_text(st, "?7", expires_at ? expires_at : "");

   int id = -1;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
      id = aimee_pg_column_int(st, 0);
   aimee_pg_finalize(st);
   return id;
}

int db2_learning_proposal_mark_committed(int id)
{
   if (id <= 0)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;

   static const char *sql =
       "UPDATE learning_proposals"
       " SET state = 'committed', committed_at = pg_now_text(), updated_at = pg_now_text()"
       " WHERE id = ?1";
   char err[LRN_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_int(st, "?1", id);
   int rc = (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_DONE) ? 0 : -1;
   if (rc != 0)
      LOG_ERROR("db2.learning", "mark_committed: %s", err);
   aimee_pg_finalize(st);
   return rc;
}

int db2_learning_proposal_get(int id, learning_proposal_t *out)
{
   if (!out || id <= 0)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;

   static const char *sql =
       "SELECT id, signal_id, sink, state, target_key, target_memory_id, action_json,"
       " evidence_refs, corroboration_count, expires_at, committed_at, archive_reason,"
       " created_at, updated_at"
       " FROM learning_proposals WHERE id = ?1";
   char err[LRN_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_int(st, "?1", id);
   int rc = -1;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      lrn_load_proposal_row(st, out);
      rc = 0;
   }
   aimee_pg_finalize(st);
   return rc;
}

int db2_learning_proposals_settled_counts(int window_days, int64_t *committed, int64_t *terminal)
{
   if (window_days <= 0)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;

   char window_expr[32];
   snprintf(window_expr, sizeof(window_expr), "-%d days", window_days);

   static const char *sql =
       "SELECT"
       " SUM(CASE WHEN state = 'committed' THEN 1 ELSE 0 END),"
       " SUM(CASE WHEN state IN ('committed', 'archived') THEN 1 ELSE 0 END)"
       " FROM learning_proposals"
       " WHERE COALESCE(committed_at, updated_at, created_at) >= pg_now_text(?1)";
   char err[LRN_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", window_expr);
   int rc = -1;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      if (committed)
         *committed = aimee_pg_column_int64(st, 0);
      if (terminal)
         *terminal = aimee_pg_column_int64(st, 1);
      rc = 0;
   }
   aimee_pg_finalize(st);
   return rc;
}

int db2_learning_proposal_list(const char *state, const char *sink, int limit,
                               learning_proposal_t *out, int max)
{
   if (!out || max <= 0)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;
   if (limit <= 0 || limit > max)
      limit = max;

   static const char *sql =
       "SELECT id, signal_id, sink, state, target_key, target_memory_id, action_json,"
       " evidence_refs, corroboration_count, expires_at, committed_at, archive_reason,"
       " created_at, updated_at"
       " FROM learning_proposals"
       " WHERE (?1 = '' OR state = ?2) AND (?3 = '' OR sink = ?4)"
       " ORDER BY id DESC LIMIT ?5";
   char err[LRN_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", state ? state : "");
   aimee_pg_bind_text(st, "?2", state ? state : "");
   aimee_pg_bind_text(st, "?3", sink ? sink : "");
   aimee_pg_bind_text(st, "?4", sink ? sink : "");
   aimee_pg_bind_int(st, "?5", limit);

   int count = 0;
   while (count < max && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      lrn_load_proposal_row(st, &out[count]);
      count++;
   }
   aimee_pg_finalize(st);
   return count;
}
